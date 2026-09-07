/**
 * TelnetConsole: the serial console, reachable over the network.
 *
 * A single-session Telnet server (PORT: TELNET_PORT) that logs in with the
 * same derived API token as the REST/WebSocket auth (crypto/api_token.h) —
 * never the raw Casambi network password — and then dispatches lines to the
 * exact same handleCommand() the serial console uses, on the loop task, so
 * the concurrency invariant in serial_console.h (only the loop task may call
 * handleCommand()) holds unchanged. 'setup' and 'wifi set' are refused here;
 * they remain serial-only (docs/concept-tcp-console.md, decision E3, and
 * telnet_policy.h for why the check normalises the line first).
 *
 * Output: every Console.print/println/printf/write call in the firmware is
 * mirrored into a ring buffer (console_out.h); this class only drains that
 * buffer into the connected client, non-blocking, and never touches Serial
 * or any print call site directly.
 *
 * NOTHING IS EVER WRITTEN TO THE SOCKET DIRECTLY (decision E7). Banner, echo,
 * prompt and drained ring output are all queued in _outBuf and pushed with a
 * non-blocking send(); a client that stopped reading costs dropped bytes, not
 * a stalled loop task. The same applies in the other direction: the read loop
 * is bounded per iteration and runs at most one command line, because the
 * watchdog is only fed once per loop() (main.cpp).
 *
 * Polled from loop() like SetupPortal — begin() once at boot, loop() every
 * iteration. Started only when a Casambi network password (and therefore a
 * token) is configured (E8); WiFi must already be connected — wifi_manager
 * starts it late if WiFi only came up after setup().
 */

#ifndef NET_TELNET_CONSOLE_H
#define NET_TELNET_CONSOLE_H

#include <WiFi.h>
#include "../config.h"   // TELNET_PORT, TELNET_LINE_MAX_LEN, buffer sizes
#include "telnet_line.h"

class TelnetConsole {
public:
    TelnetConsole();

    // Starts listening on TELNET_PORT. Returns false (and does not listen)
    // if no Casambi network password is stored yet. Idempotent: a second call
    // on an already-listening console returns false without re-binding, so the
    // WiFi-reconnect path can call it unconditionally.
    bool begin();

    // Accept/serve the single session. Call every loop() iteration.
    void loop();

    // True once begin() actually bound the listening socket. 'telnet status'
    // reports this rather than "the object exists" — begin() legitimately
    // fails on a config that predates the persisted network password.
    bool listening() const { return _began; }

    bool sessionActive() const { return _state != State::Idle; }

    // Tell a connected client why the device is about to reboot, then close
    // the session cleanly (FIN) — E6 requires reboot commands to end the
    // session. Without this the message only reaches the ring buffer, is
    // never drained, and the connection just dies on the reset.
    void notifyReboot(const char* message);

    // Cumulative bytes lost because the client could not keep up with the
    // ring buffer (see console_ring_buffer.h) since this session type first
    // dropped any — i.e. across reconnects, not just the current session.
    uint64_t droppedBytes() const { return _totalDropped; }

private:
    enum class State { Idle, AwaitingPassword, Authenticated };

    void _beginSession(WiFiClient client);
    void _endSession();
    void _readInput();
    void _handleLine();
    void _handleAuthenticatedLine(const char* raw);
    void _noteLoginFailure();

    // --- outbound staging (never blocks) ---
    void _pumpOutput();
    void _flushOut();
    bool _queue(const uint8_t* data, size_t len);
    bool _queueStr(const char* s);
    void _queuePrintf(const char* fmt, ...);
    size_t _outPendingBytes() const { return _outTail - _outHead; }
    size_t _outFree() const { return sizeof(_outBuf) - _outPendingBytes(); }

    WiFiServer _server;
    WiFiClient _client;
    bool       _began = false;
    State      _state = State::Idle;
    uint8_t    _loginAttempts = 0;
    bool       _passwordMode = false;

    // Set when a send() failed for a reason other than "would block"; loop()
    // tears the session down at a defined point instead of inside a helper.
    bool _sendFailed = false;

    // "> " is owed to the client but must wait until the ring buffer is fully
    // drained — otherwise the prompt overtakes the output of the command that
    // was just executed (that output only exists in the ring at that moment).
    bool _promptPending = false;

    // Deadline for re-showing 'Password: ' after a failed attempt, replacing a
    // delay() that would have stalled the loop task. 0 = nothing pending.
    unsigned long _repromptAtMs = 0;

    char             _lineBuf[TELNET_LINE_MAX_LEN];
    TelnetLineParser _parser;

    uint64_t _outCursor = 0;
    uint64_t _totalDropped = 0;

    uint8_t _outBuf[TELNET_OUT_BUFFER_SIZE];
    size_t  _outHead = 0;   // next byte to send
    size_t  _outTail = 0;   // one past the last queued byte
    uint8_t _lastOutByte = '\n';   // for CR-LF collapsing across chunks

    unsigned long _lastActivityMs = 0;   // last complete command line (E6a)
    unsigned long _lastNopMs = 0;        // last liveness probe sent (E6b)

    // Login-failure state that deliberately OUTLIVES a session: per-connection
    // attempt counting alone is no obstacle to a script that reconnects.
    uint8_t       _consecutiveFailures = 0;
    unsigned long _lockoutUntilMs = 0;
};

// Close a live telnet session before a reboot, wherever that reboot is
// triggered from (serial 'restart'/'clearconfig'/'wifi set', cloud refresh,
// POST /api/reboot). Safe to call when no console exists or no client is
// connected.
void telnetNotifyReboot(const char* message);

#endif // NET_TELNET_CONSOLE_H
