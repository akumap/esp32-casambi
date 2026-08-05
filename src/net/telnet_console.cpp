/**
 * TelnetConsole — see telnet_console.h.
 */

#include "telnet_console.h"

#include <sys/select.h>

#include "../config.h"
#include "../app_state.h"
#include "../console_out.h"
#include "../crypto/api_token.h"
#include "../log/event_log.h"
#include "../serial_console.h"

namespace {

// True when the socket can accept at least one byte RIGHT NOW.
//
// Deliberately not WiFiClient::availableForWrite(): WiFiClient does not
// override it on the ESP32 Arduino core, so it inherits Print's default
// implementation, which unconditionally returns 0. Gating a send on
// "availableForWrite() > 0" therefore never sends anything -- that bug
// silently swallowed ALL command output over telnet (the banner, echo and
// prompt still appeared because those are written straight to the client and
// never went through the gated path), and it disabled the E6b liveness probe
// the same way.
//
// A zero-timeout select() answers the question the gate was actually asking,
// and answers it without ever waiting -- which is the point: the loop task
// must never stall on a client that stopped reading (E7 / watchdog).
bool socketWritable(int fd) {
    if (fd < 0) return false;
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = {0, 0};   // poll, never block
    return select(fd + 1, nullptr, &wset, nullptr, &tv) > 0 && FD_ISSET(fd, &wset);
}

}  // namespace

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
        if (socketWritable(_client.fd())) {
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
    _pendingLen = _pendingSent = 0;   // no leftovers from a previous session
    _lastOutByte = '\n';
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
    _pendingLen = _pendingSent = 0;
}

void TelnetConsole::_handleLine() {
    // Deliberately no _parser.reset() here: the parser starts a fresh line by
    // itself once new content arrives (see telnet_line.h). Resetting would
    // clear its "just saw CR" state, so the LF of a client's CR LF would no
    // longer be recognised as the second half of that pair and would fire a
    // second, EMPTY line -- one spurious command, and one extra prompt, after
    // every single command. reset() belongs at session start/end only.
    String line(_parser.line());

    if (_state == State::AwaitingPassword) {
        line.trim();
        // What the user types IS the token (the 64-char hex digest), compared
        // as-is against the stored one — NOT hashed again. Deriving from the
        // input and comparing digests would instead require typing the raw
        // Casambi password, which is exactly the cloud credential decision E1
        // exists to keep off an unencrypted Telnet connection.
        String expected = ApiToken::derive(networkConfig.casambiPassword);
        if (!expected.isEmpty() && ApiToken::constantTimeEquals(line, expected)) {
            _state = State::Authenticated;
            _passwordMode = false;
            // Scrollback replay: start at the oldest byte still buffered so a
            // freshly logged-in client immediately sees recent history, not
            // just output produced from now on (docs/konzept-tcp-konsole.md,
            // 4.2 point 3). Gated behind auth so nothing leaks pre-login.
            _outCursor = consoleRingOldestAvailable();
            EventLog::log(LOG_INFO, "Telnet: login from %s",
                          _client.remoteIP().toString().c_str());
            // 'exit' is advertised here rather than in cmdHelp(): that help
            // text is shared with the serial console, where there is no
            // session to leave.
            _client.print("\r\nWelcome. Type 'help' for commands, 'exit' to disconnect.\r\n> ");
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
    if (line == "exit" || line == "quit") {
        // Telnet-only, and handled before handleCommand() on purpose: there is
        // nothing to exit on the serial console, so this never reaches the
        // shared command table. Without it the only ways out are killing the
        // client or waiting out the idle timeout -- and since just one session
        // is allowed at a time (E6), a session nobody can close cleanly blocks
        // the next login from another machine.
        _client.print("Bye.\r\n");
        EventLog::log(LOG_INFO, "Telnet: logout from %s",
                      _client.remoteIP().toString().c_str());
        _endSession();
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

    // Drain in chunks for as long as the socket keeps taking them, bounded so
    // a chatty moment cannot monopolise this loop() iteration. A single chunk
    // per iteration would not keep up: one 'status' or 'log' produces several
    // kB at once, so the ring buffer would wrap before the client had seen it
    // -- reported as dropped bytes, but lost all the same.
    size_t budget = CONSOLE_RING_BUFFER_SIZE;

    while (budget > 0) {
        // Don't start a send the socket cannot take right now: a write() into
        // a full send buffer stalls the loop task and, at worst, trips the
        // watchdog (docs/konzept-tcp-konsole.md, 4.2 point 2 / E7).
        if (!socketWritable(_client.fd())) return;

        // Finish the previous chunk first. write() reports what the socket
        // actually accepted (it sends with MSG_DONTWAIT under the hood), which
        // can be less than requested even right after select() said "writable".
        if (_pendingSent < _pendingLen) {
            _pendingSent += _client.write(_pending + _pendingSent,
                                          _pendingLen - _pendingSent);
            if (_pendingSent < _pendingLen) return;   // still full; resume later
        }
        _pendingLen = _pendingSent = 0;

        uint8_t buf[128];
        uint64_t dropped = 0;
        size_t n = consoleRingRead(&_outCursor, buf, sizeof(buf), &dropped);
        if (dropped > 0) {
            _totalDropped += dropped;
            _client.printf("\r\n[%lu Bytes verworfen -- Client war zu langsam]\r\n",
                           (unsigned long)dropped);
        }
        if (n == 0) return;   // ring buffer drained

        // Expand bare \n to \r\n. Telnet NVT ends a line with CR LF, and most
        // of this firmware's output is printf("...\n") -- only Print::println()
        // already emits both. Sent verbatim, every such line staircases on a
        // terminal that takes LF literally. _lastOutByte carries the previous
        // byte across chunk boundaries so an existing \r\n is not turned into
        // \r\r\n when the pair happens to straddle two chunks.
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];
            if (c == '\n' && _lastOutByte != '\r') _pending[_pendingLen++] = '\r';
            _pending[_pendingLen++] = c;
            _lastOutByte = c;
        }

        budget -= (budget < n) ? budget : n;
    }
}
