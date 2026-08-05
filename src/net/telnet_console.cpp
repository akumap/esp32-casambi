/**
 * TelnetConsole — see telnet_console.h.
 */

#include "telnet_console.h"

#include "../config.h"
#include "../app_state.h"
#include "../console_out.h"
#include "../crypto/api_token.h"
#include "../log/event_log.h"
#include "../serial_console.h"

TelnetConsole::TelnetConsole()
    : _server(TELNET_PORT), _parser(_lineBuf, sizeof(_lineBuf)) {}

bool TelnetConsole::begin() {
    if (networkConfig.casambiPassword.isEmpty()) return false;  // E8: no token, no listener
    _server.begin();
    _began = true;
    Console.printf("Telnet: console listening on port %d\n", TELNET_PORT);
    return true;
}

void TelnetConsole::loop() {
    if (!_began) return;

    if (_server.hasClient()) {
        WiFiClient incoming = _server.available();
        if (_client.connected()) {
            // Only one session at a time (E6) — two operators racing a
            // blocking command like 'connect' would be worse than a reject.
            incoming.print("Busy: another telnet session is already active.\r\n");
            incoming.stop();
        } else {
            _beginSession(incoming);
        }
    }

    if (_state == State::Idle) return;

    if (!_client.connected()) {
        _endSession();
        return;
    }

    const uint32_t timeoutS = networkConfig.telnetTimeoutSeconds;
    if (timeoutS > 0 && (millis() - _lastActivityMs) > timeoutS * 1000UL) {
        _client.print("\r\nIdle timeout -- closing session.\r\n");
        _endSession();
        return;
    }

    // Liveness probe (E6b): invisible to a real terminal, but a dead peer's
    // TCP stack will eventually RST/timeout on it, which connected() below
    // then notices -- freeing the single session slot even with the idle
    // timeout disabled for an overnight capture.
    if (millis() - _lastNopMs > TELNET_NOP_INTERVAL_MS) {
        _lastNopMs = millis();
        static const uint8_t nop[] = {255, 241};  // IAC NOP
        if (_client.availableForWrite() >= (int)sizeof(nop)) {
            _client.write(nop, sizeof(nop));
        }
    }

    while (_client.available()) {
        int b = _client.read();
        if (b < 0) break;
        switch (_parser.feed((uint8_t)b)) {
            case TelnetLineParser::Result::Consumed:
                if (!_passwordMode) _client.write((uint8_t)b);
                break;
            case TelnetLineParser::Result::Backspaced:
                if (!_passwordMode) _client.print("\b \b");
                break;
            case TelnetLineParser::Result::LineReady:
                _client.print("\r\n");
                _lastActivityMs = millis();
                _handleLine();
                break;
            case TelnetLineParser::Result::Ignored:
                break;
        }
    }

    _drainOutput();
}

void TelnetConsole::_beginSession(WiFiClient client) {
    _client = client;
    _client.setNoDelay(true);
    _state = State::AwaitingPassword;
    _loginAttempts = 0;
    _passwordMode = true;
    _parser.reset();
    _lastActivityMs = millis();
    _lastNopMs = millis();

    // Negotiate ECHO + SUPPRESS-GO-AHEAD so this side controls local echo
    // (needed to hide the password) instead of relying on the client's own
    // "local echo" setting — see docs/konzept-tcp-konsole.md, decision E4.
    static const uint8_t neg[] = {255, 251, 1, 255, 251, 3};  // IAC WILL ECHO, IAC WILL SGA
    _client.write(neg, sizeof(neg));

    _client.printf("\r\nESP32 Casambi Controller (build %d)\r\n", FIRMWARE_BUILD);
    _client.print("Password: ");
}

void TelnetConsole::_endSession() {
    _client.stop();
    _state = State::Idle;
    _passwordMode = false;
    _parser.reset();
}

void TelnetConsole::_handleLine() {
    String line(_parser.line());
    _parser.reset();

    if (_state == State::AwaitingPassword) {
        line.trim();
        // Login uses the SAME derived token as REST/WebSocket auth, never
        // the raw Casambi password — see decision E1.
        String expected = ApiToken::derive(networkConfig.casambiPassword);
        if (!expected.isEmpty() && ApiToken::constantTimeEquals(ApiToken::derive(line), expected)) {
            _state = State::Authenticated;
            _passwordMode = false;
            // Scrollback replay: start at the oldest byte still buffered so a
            // freshly logged-in client immediately sees recent history, not
            // just output produced from now on (docs/konzept-tcp-konsole.md,
            // 4.2 point 3). Gated behind auth so nothing leaks pre-login.
            _outCursor = consoleRingOldestAvailable();
            EventLog::log(LOG_INFO, "Telnet: login from %s",
                          _client.remoteIP().toString().c_str());
            _client.print("\r\nWelcome. Type 'help' for commands.\r\n> ");
        } else {
            _loginAttempts++;
            EventLog::log(LOG_WARN, "Telnet: failed login attempt %u from %s",
                          _loginAttempts, _client.remoteIP().toString().c_str());
            if (_loginAttempts >= TELNET_MAX_LOGIN_ATTEMPTS) {
                _client.print("\r\nToo many attempts.\r\n");
                _endSession();
            } else {
                delay(500);  // small deliberate delay against fast brute force
                _client.print("Password: ");
            }
        }
        return;
    }

    if (line.length() == 0) {
        _client.print("> ");
        return;
    }
    if (line == "setup" || line.startsWith("wifi set")) {
        // Both would leak WiFi/Casambi credentials over a Telnet session and
        // 'setup' drives BLE scanning the same way 'connect' does; keeping
        // them serial-only avoids reworking their blocking Serial.available()
        // wait loops for a network client (decision E3).
        Console.println("Telnet: 'setup' and 'wifi set' are only available on the serial console.");
    } else {
        handleCommand(line);
    }
    _client.print("> ");
}

void TelnetConsole::_drainOutput() {
    if (_state != State::Authenticated) return;  // nothing leaks pre-auth

    uint8_t buf[128];
    uint64_t dropped = 0;
    size_t n = consoleRingRead(&_outCursor, buf, sizeof(buf), &dropped);
    if (dropped > 0) {
        _totalDropped += dropped;
        _client.printf("\r\n[%lu Bytes verworfen -- Client war zu langsam]\r\n",
                       (unsigned long)dropped);
    }
    if (n == 0) return;

    // Never ask the socket for more than it can currently accept — a
    // blocking client.write() here would stall the loop task and, at worst,
    // trip the watchdog (docs/konzept-tcp-konsole.md, 4.2 point 2 / E7).
    int avail = _client.availableForWrite();
    if (avail <= 0) return;
    size_t toSend = ((size_t)avail < n) ? (size_t)avail : n;
    _client.write(buf, toSend);

    // Unsent tail stays in the ring buffer: rewind the cursor so it is
    // retried next loop() iteration instead of silently skipped.
    if (toSend < n) _outCursor -= (n - toSend);
}
