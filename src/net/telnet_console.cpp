/**
 * TelnetConsole — see telnet_console.h.
 */

#include "telnet_console.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <lwip/sockets.h>   // send()/MSG_DONTWAIT — same header the core uses

#include "../config.h"
#include "../app_state.h"
#include "../console_out.h"
#include "../crypto/api_token.h"
#include "../log/event_log.h"
#include "../serial_console.h"
#include "telnet_policy.h"

static_assert(TELNET_OUT_BUFFER_SIZE >= 2 * TELNET_RING_CHUNK_BYTES + 64,
              "outbound buffer must hold a worst-case expanded ring chunk "
              "(every byte doubled by CR-LF/IAC escaping) plus a drop notice");

namespace {

// True when the socket can accept at least one byte RIGHT NOW.
//
// Deliberately not WiFiClient::availableForWrite(): WiFiClient does not
// override it on the ESP32 Arduino core, so it inherits Print's default
// implementation, which unconditionally returns 0. Gating a send on
// "availableForWrite() > 0" therefore never sends anything -- that bug
// silently swallowed ALL command output over telnet (the banner, echo and
// prompt still appeared because those were written straight to the client and
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

// One non-blocking send attempt. Returns bytes accepted (0 = would block),
// or -1 on a fatal socket error.
//
// Raw send() instead of WiFiClient::write() on purpose. WiFiClient::write() is
// NOT non-blocking: on a full send buffer it runs its own retry loop
// (WIFI_CLIENT_MAX_WRITE_RETRY attempts, each with a one-second select()), so a
// single call can park the loop task for ~10 s. select()-then-write() does not
// save us either -- select() only promises that ONE byte fits, and the core
// re-enters that retry loop for the remainder. Since a stalled telnet client is
// the expected failure mode (laptop suspended mid-session, E7), the console
// talks to the descriptor directly and decides for itself what to do when the
// socket is full: drop, and account for it.
int nonBlockingSend(int fd, const uint8_t* data, size_t len) {
    if (fd < 0 || len == 0) return 0;
    // Unqualified, like the Arduino core's own socket calls: lwIP's compat
    // layer may define send() as a macro over lwip_send().
    int n = send(fd, data, len, MSG_DONTWAIT);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

// Best-effort one-shot message to a client we are about to drop (busy/lockout
// rejections). Never retries: the peer has just connected, so its window is
// open; if it is not, the rejection is not worth a stalled loop task.
void sendRejection(WiFiClient& client, const char* text) {
    nonBlockingSend(client.fd(), (const uint8_t*)text, strlen(text));
}

// millis()-rollover-safe "deadline reached?".
bool timeReached(unsigned long deadline) {
    return (long)(millis() - deadline) >= 0;
}

}  // namespace

TelnetConsole::TelnetConsole()
    : _server(TELNET_PORT), _parser(_lineBuf, sizeof(_lineBuf)) {}

bool TelnetConsole::begin() {
    if (_began) return false;                                   // already listening
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
            sendRejection(incoming, "Busy: another telnet session is already active.\r\n");
            incoming.stop();
        } else if (_lockoutUntilMs != 0 && !timeReached(_lockoutUntilMs)) {
            // Login lockout: refuse without taking the session slot, so the
            // legitimate user is not additionally locked out by the attempts.
            sendRejection(incoming, "Locked: too many failed logins, try again later.\r\n");
            incoming.stop();
        } else {
            // A session whose client is gone but that loop() has not reaped
            // yet: close it properly instead of overwriting _client and
            // leaving its state half-carried into the new session.
            if (_state != State::Idle) _endSession();
            _beginSession(incoming);
        }
    }

    if (_state == State::Idle) return;

    if (_sendFailed || !_client.connected()) {
        _endSession();
        return;
    }

    // Unauthenticated connections get a short leash of their own: the idle
    // timeout is measured in minutes (and can be disabled entirely), so
    // without this a peer that connects and says nothing owns the single
    // session slot for 15 minutes — or forever with 'telnet timeout 0'.
    if (_state == State::AwaitingPassword &&
        (millis() - _lastActivityMs) > TELNET_LOGIN_TIMEOUT_MS) {
        _queueStr("\r\nLogin timeout -- closing session.\r\n");
        _endSession();
        return;
    }

    uint32_t timeoutS = networkConfig.telnetTimeoutSeconds;
    if (timeoutS > TELNET_TIMEOUT_MAX_SECONDS) timeoutS = TELNET_TIMEOUT_MAX_SECONDS;  // no ms overflow
    if (timeoutS > 0 && (millis() - _lastActivityMs) > timeoutS * 1000UL) {
        _queueStr("\r\nIdle timeout -- closing session.\r\n");
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
        _queue(nop, sizeof(nop));
    }

    // Re-prompt after a failed login, once the backoff has elapsed.
    if (_repromptAtMs != 0 && timeReached(_repromptAtMs)) {
        _repromptAtMs = 0;
        _queueStr("Password: ");
    }

    _readInput();

    if (_state != State::Idle) _pumpOutput();
}

void TelnetConsole::_beginSession(WiFiClient client) {
    _client = client;
    _client.setNoDelay(true);
    _state = State::AwaitingPassword;
    _loginAttempts = 0;
    _passwordMode = true;
    _sendFailed = false;
    _promptPending = false;
    _repromptAtMs = 0;
    _parser.reset();
    _outHead = _outTail = 0;   // no leftovers from a previous session
    _lastOutByte = '\n';
    _lastActivityMs = millis();
    _lastNopMs = millis();

    // Negotiate ECHO + SUPPRESS-GO-AHEAD so this side controls local echo
    // (needed to hide the password) instead of relying on the client's own
    // "local echo" setting — see docs/konzept-tcp-konsole.md, decision E4.
    static const uint8_t neg[] = {255, 251, 1, 255, 251, 3};  // IAC WILL ECHO, IAC WILL SGA
    _queue(neg, sizeof(neg));

    _queuePrintf("\r\nESP32 Casambi Controller (build %d)\r\n", FIRMWARE_BUILD);
    _queueStr("Password: ");
    _flushOut();
}

void TelnetConsole::_endSession() {
    _flushOut();     // hand whatever is queued to lwIP before the FIN
    _client.stop();
    _state = State::Idle;
    _passwordMode = false;
    _promptPending = false;
    _repromptAtMs = 0;
    _sendFailed = false;
    _parser.reset();
    _outHead = _outTail = 0;
    _lastOutByte = '\n';
}

void TelnetConsole::notifyReboot(const char* message) {
    if (_state == State::Idle) return;
    if (_state == State::Authenticated && message && *message) {
        _queueStr("\r\n");
        _queueStr(message);
        _queueStr("\r\n");
    }
    _endSession();
}

void telnetNotifyReboot(const char* message) {
    if (telnetConsole) telnetConsole->notifyReboot(message);
}

// ============================================================================
// INPUT
// ============================================================================

void TelnetConsole::_readInput() {
    if (_repromptAtMs != 0) return;   // login backoff: leave the bytes in the socket

    // Bounded on purpose. The watchdog is fed once per loop() (main.cpp), so an
    // unbounded "while (available())" hands the loop task to whoever sends
    // fastest: a peer streaming data faster than the console drains it keeps
    // available() non-zero indefinitely and the WDT fires after 45 s — with no
    // authentication required to try it. The socket buffer keeps the rest; the
    // next iteration continues where this one stopped.
    size_t budget = TELNET_INPUT_BUDGET_BYTES;

    while (budget > 0 && _client.available()) {
        int b = _client.read();
        if (b < 0) break;
        budget--;

        switch (_parser.feed((uint8_t)b)) {
            case TelnetLineParser::Result::Consumed: {
                if (!_passwordMode) {
                    const uint8_t echo = (uint8_t)b;
                    _queue(&echo, 1);
                }
                break;
            }
            case TelnetLineParser::Result::Backspaced:
                if (!_passwordMode) _queueStr("\b \b");
                break;
            case TelnetLineParser::Result::LineTooLong:
                // Deliberately NOT counted as activity (E6a measures complete
                // command lines), so an endless stream of overlong lines
                // cannot hold the session past the login/idle timeout.
                _queuePrintf("\r\nLine too long (max %d characters) -- ignored.\r\n",
                             (int)(TELNET_LINE_MAX_LEN - 1));
                if (_state == State::AwaitingPassword) _queueStr("Password: ");
                else _promptPending = true;
                return;
            case TelnetLineParser::Result::LineReady:
                _queueStr("\r\n");
                _lastActivityMs = millis();
                _handleLine();
                // One command per loop() iteration. handleCommand() can run for
                // seconds ('scan', 'connect'), and a pasted script would
                // otherwise run every one of its lines between two watchdog
                // feeds. Remaining input waits in the socket buffer.
                return;
            case TelnetLineParser::Result::Ignored:
                break;
        }
    }
}

void TelnetConsole::_handleLine() {
    // Deliberately no _parser.reset() here: the parser starts a fresh line by
    // itself once new content arrives (see telnet_line.h). Resetting would
    // clear its "just saw CR" state, so the LF of a client's CR LF would no
    // longer be recognised as the second half of that pair and would fire a
    // second, EMPTY line -- one spurious command, and one extra prompt, after
    // every single command. reset() belongs at session start/end only.
    if (_state == State::AwaitingPassword) {
        String line(_parser.line());
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
            _loginAttempts = 0;
            _consecutiveFailures = 0;
            _lockoutUntilMs = 0;
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
            _queueStr("\r\nWelcome. Type 'help' for commands, 'exit' to disconnect.\r\n");
            _promptPending = true;   // emitted after the scrollback replay
        } else {
            _noteLoginFailure();
        }
        return;
    }

    _handleAuthenticatedLine(_parser.line());
}

void TelnetConsole::_noteLoginFailure() {
    _loginAttempts++;
    if (_consecutiveFailures < 255) _consecutiveFailures++;
    EventLog::log(LOG_WARN, "Telnet: failed login attempt %u from %s",
                  _loginAttempts, _client.remoteIP().toString().c_str());

    // Per-connection attempts are no obstacle to a script that just reconnects:
    // count failures across connections too and close the port for a while.
    // The token is a 64-char hex digest, so this is not about guessing it —
    // it caps the flash wear (one event-log entry per attempt) and the loop
    // time any LAN host can spend on the login path.
    if (_consecutiveFailures >= TELNET_LOCKOUT_FAILURES) {
        _lockoutUntilMs = millis() + TELNET_LOCKOUT_MS;
        if (_lockoutUntilMs == 0) _lockoutUntilMs = 1;   // 0 means "no lockout"
        _consecutiveFailures = 0;                        // next window starts fresh
        EventLog::log(LOG_WARN, "Telnet: login locked for %d s after %d failures",
                      TELNET_LOCKOUT_MS / 1000, TELNET_LOCKOUT_FAILURES);
        _queueStr("\r\nToo many failed logins -- try again later.\r\n");
        _endSession();
        return;
    }

    if (_loginAttempts >= TELNET_MAX_LOGIN_ATTEMPTS) {
        _queueStr("\r\nToo many attempts.\r\n");
        _endSession();
        return;
    }

    // Small deliberate pause before the next prompt — as a deadline, not a
    // delay(): blocking here would stop BLE reconnect, keepalive and the web
    // server's deferred work for half a second per wrong token.
    _repromptAtMs = millis() + TELNET_LOGIN_RETRY_DELAY_MS;
    if (_repromptAtMs == 0) _repromptAtMs = 1;
}

void TelnetConsole::_handleAuthenticatedLine(const char* raw) {
    // Trim like the serial path does (main.cpp trims before handleCommand()),
    // so a trailing space does not turn 'status' into an unknown command.
    String line(raw);
    line.trim();

    if (line.length() == 0) {
        _promptPending = true;
        return;
    }

    if (line == "exit" || line == "quit") {
        // Telnet-only, and handled before handleCommand() on purpose: there is
        // nothing to exit on the serial console, so this never reaches the
        // shared command table. Without it the only ways out are killing the
        // client or waiting out the idle timeout -- and since just one session
        // is allowed at a time (E6), a session nobody can close cleanly blocks
        // the next login from another machine.
        _queueStr("Bye.\r\n");
        EventLog::log(LOG_INFO, "Telnet: logout from %s",
                      _client.remoteIP().toString().c_str());
        _endSession();
        return;
    }

    // The serial-only guard runs on a normalised copy, not on the raw line:
    // the dispatcher normalises too, so 'wifi<SP><SP>set ssid pw' would slip
    // past a literal startsWith() check and leak the WiFi password into the
    // command echo (telnet_policy.h explains this in full).
    char norm[TELNET_LINE_MAX_LEN];
    telnetpolicy::normalize(line.c_str(), norm, sizeof(norm));
    if (telnetpolicy::isSerialOnly(norm)) {
        // Both would leak WiFi/Casambi credentials over a Telnet session and
        // 'setup' drives BLE scanning the same way 'connect' does; keeping
        // them serial-only avoids reworking their blocking Serial.available()
        // wait loops for a network client (decision E3).
        Console.println("Telnet: 'setup' and 'wifi set' are only available on the serial console.");
    } else {
        handleCommand(line);
    }
    _promptPending = true;
}

// ============================================================================
// OUTPUT (nothing below ever blocks the loop task — decision E7)
// ============================================================================

bool TelnetConsole::_queue(const uint8_t* data, size_t len) {
    if (len == 0) return true;
    if (_state == State::Idle || len > sizeof(_outBuf)) return false;

    if (_outTail + len > sizeof(_outBuf) && _outHead > 0) {
        const size_t pending = _outPendingBytes();
        memmove(_outBuf, _outBuf + _outHead, pending);
        _outHead = 0;
        _outTail = pending;
    }
    if (_outTail + len > sizeof(_outBuf)) return false;   // client stalled: drop

    memcpy(_outBuf + _outTail, data, len);
    _outTail += len;
    return true;
}

bool TelnetConsole::_queueStr(const char* s) {
    return s ? _queue((const uint8_t*)s, strlen(s)) : true;
}

void TelnetConsole::_queuePrintf(const char* fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) _queue((const uint8_t*)tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

void TelnetConsole::_flushOut() {
    while (_outHead < _outTail) {
        const int fd = _client.fd();
        if (fd < 0) break;
        if (!socketWritable(fd)) return;   // socket full: try again next iteration

        const int n = nonBlockingSend(fd, _outBuf + _outHead, _outPendingBytes());
        if (n > 0) {
            _outHead += (size_t)n;
            continue;
        }
        if (n < 0) _sendFailed = true;     // loop() tears the session down
        return;
    }
    _outHead = _outTail = 0;
}

void TelnetConsole::_pumpOutput() {
    _flushOut();
    if (_state != State::Authenticated) return;  // nothing leaks pre-auth

    // Drain in chunks for as long as the buffer keeps taking them, bounded so
    // a chatty moment cannot monopolise this loop() iteration. A single chunk
    // per iteration would not keep up: one 'status' or 'log' produces several
    // kB at once, so the ring buffer would wrap before the client had seen it
    // -- reported as dropped bytes, but lost all the same.
    size_t budget = CONSOLE_RING_BUFFER_SIZE;
    bool ringDrained = false;

    while (budget > 0) {
        if (_sendFailed) return;

        // Only pull from the ring when a whole chunk is guaranteed to fit in
        // its worst-case expanded form (plus a drop notice). Otherwise the
        // cursor would advance over bytes that _queue() then has to drop —
        // data lost without appearing in the drop counter.
        if (_outFree() < 2 * TELNET_RING_CHUNK_BYTES + 64) return;

        uint8_t buf[TELNET_RING_CHUNK_BYTES];
        uint64_t dropped = 0;
        size_t n = consoleRingRead(&_outCursor, buf, sizeof(buf), &dropped);
        if (dropped > 0) {
            _totalDropped += dropped;
            _queuePrintf("\r\n[%lu bytes dropped -- client too slow]\r\n",
                         (unsigned long)dropped);
        }
        if (n == 0) {
            ringDrained = true;
            break;
        }

        // Expand bare \n to \r\n and escape IAC. Telnet NVT ends a line with
        // CR LF, and most of this firmware's output is printf("...\n") -- only
        // Print::println() already emits both. Sent verbatim, every such line
        // staircases on a terminal that takes LF literally. _lastOutByte
        // carries the previous byte across chunk boundaries so an existing
        // \r\n is not turned into \r\r\n when the pair straddles two chunks.
        // A literal 0xFF must be doubled (RFC 854): unescaped, the client
        // reads it as IAC and swallows the byte after it.
        uint8_t expanded[2 * TELNET_RING_CHUNK_BYTES];
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];
            if (c == '\n' && _lastOutByte != '\r') expanded[m++] = '\r';
            else if (c == 255) expanded[m++] = 255;
            expanded[m++] = c;
            _lastOutByte = c;
        }
        _queue(expanded, m);
        _flushOut();

        budget -= (budget < n) ? budget : n;
    }

    // The prompt is owed to the client, but only after everything the command
    // printed has been queued ahead of it — the output exists in the ring
    // buffer at that point, so writing "> " when the command returns puts the
    // prompt in front of its own output.
    if (ringDrained && _promptPending) {
        _promptPending = false;
        _queueStr("> ");
        _flushOut();
    }
}
