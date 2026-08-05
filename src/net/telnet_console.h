/**
 * TelnetConsole: the serial console, reachable over the network.
 *
 * A single-session Telnet server (PORT: TELNET_PORT) that logs in with the
 * same derived API token as the REST/WebSocket auth (crypto/api_token.h) —
 * never the raw Casambi network password — and then dispatches lines to the
 * exact same handleCommand() the serial console uses, on the loop task, so
 * the concurrency invariant in serial_console.h (only the loop task may call
 * handleCommand()) holds unchanged. 'setup' and 'wifi set' are refused here;
 * they remain serial-only (docs/konzept-tcp-konsole.md, decision E3).
 *
 * Output: every Console.print/println/printf/write call in the firmware is
 * mirrored into a ring buffer (console_out.h); this class only drains that
 * buffer into the connected client, non-blocking, and never touches Serial
 * or any print call site directly.
 *
 * Polled from loop() like SetupPortal — begin() once at boot, loop() every
 * iteration. Started only when a Casambi network password (and therefore a
 * token) is configured (E8); WiFi must already be connected.
 */

#ifndef NET_TELNET_CONSOLE_H
#define NET_TELNET_CONSOLE_H

#include <WiFi.h>
#include "telnet_line.h"

class TelnetConsole {
public:
    TelnetConsole();

    // Starts listening on TELNET_PORT. Returns false (and does not listen)
    // if no Casambi network password is stored yet.
    bool begin();

    // Accept/serve the single session. Call every loop() iteration.
    void loop();

    bool sessionActive() const { return _state != State::Idle; }

    // Cumulative bytes lost because the client could not keep up with the
    // ring buffer (see console_ring_buffer.h) since this session type first
    // dropped any — i.e. across reconnects, not just the current session.
    uint64_t droppedBytes() const { return _totalDropped; }

private:
    enum class State { Idle, AwaitingPassword, Authenticated };

    void _beginSession(WiFiClient client);
    void _endSession();
    void _handleLine();
    void _drainOutput();

    WiFiServer _server;
    WiFiClient _client;
    bool       _began = false;
    State      _state = State::Idle;
    uint8_t    _loginAttempts = 0;
    bool       _passwordMode = false;

    char             _lineBuf[TELNET_LINE_MAX_LEN];
    TelnetLineParser _parser;

    uint64_t _outCursor = 0;
    uint64_t _totalDropped = 0;

    unsigned long _lastActivityMs = 0;   // last complete command line (E6a)
    unsigned long _lastNopMs = 0;        // last liveness probe sent (E6b)
};

#endif // NET_TELNET_CONSOLE_H
