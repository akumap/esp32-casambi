/**
 * TelnetLineParser: turns a Telnet client's raw byte stream into complete
 * command lines.
 *
 * Strips Telnet IAC negotiation (`IAC WILL/WONT/DO/DONT <option>`) and
 * subnegotiation (`IAC SB ... IAC SE`) sequences — PuTTY sends these
 * unprompted on connect; without stripping them the first command line would
 * start with garbage. Collapses CR, LF, CRLF and CR-NUL line endings into a
 * single line-complete signal (different Telnet clients use different ones).
 * Handles backspace/DEL for local line editing.
 *
 * Arduino-free and single-threaded by design — see test/test_telnet_line.
 * The caller (net/telnet_console.cpp) owns echoing bytes back to the client;
 * this class only tracks buffer/line state.
 */

#ifndef TELNET_LINE_H
#define TELNET_LINE_H

#include <cstddef>
#include <cstdint>

class TelnetLineParser {
public:
    enum class Result {
        Consumed,    // byte appended to the in-progress line
        Backspaced,  // backspace/DEL removed the previous character
        LineReady,   // line() now holds a complete, NUL-terminated line
        Ignored,     // absorbed (IAC sequence, or backspace on an empty line)
    };

    // `buf` must stay valid for the parser's lifetime and have room for at
    // least 1 byte (a line is always truncated to leave room for the NUL).
    TelnetLineParser(char* buf, size_t capacity)
        : _buf(buf), _capacity(capacity) {
        reset();
    }

    // Full reset: abandons any in-progress line and IAC parsing state.
    // Not needed between ordinary lines — feed() itself starts a fresh line
    // automatically the moment new content arrives after a LineReady result.
    void reset() {
        _len = 0;
        _state = State::Normal;
        _afterCr = false;
        _lineComplete = false;
        _terminate();
    }

    const char* line() const { return _buf; }
    size_t length() const { return _len; }

    Result feed(uint8_t b) {
        switch (_state) {
            case State::Iac:
                _state = (b == kIacSb) ? State::IacSb : State::IacOpt;
                return Result::Ignored;
            case State::IacOpt:
                _state = State::Normal;
                return Result::Ignored;
            case State::IacSb:
                if (b == kIac) _state = State::IacSbIac;
                return Result::Ignored;
            case State::IacSbIac:
                _state = (b == kIacSe) ? State::Normal : State::IacSb;
                return Result::Ignored;
            case State::Normal:
                break;
        }

        if (b == kIac) {
            _state = State::Iac;
            return Result::Ignored;
        }

        // A CR just seen may be the first half of CRLF or CR-NUL; the second
        // half carries no information of its own once the line already ended.
        if (_afterCr) {
            _afterCr = false;
            if (b == '\n' || b == '\0') return Result::Ignored;
            // Anything else starts the next line — fall through below.
        }

        if (b == '\r' || b == '\n') {
            _afterCr = (b == '\r');
            _terminate();
            _lineComplete = true;
            return Result::LineReady;
        }

        _startNewLineIfNeeded();

        if (b == kBackspace || b == kDel) {
            if (_len > 0) {
                _len--;
                _terminate();
                return Result::Backspaced;
            }
            return Result::Ignored;
        }

        if (_len + 1 < _capacity) {
            _buf[_len++] = (char)b;
            _terminate();
            return Result::Consumed;
        }
        return Result::Ignored;  // line too long; drop extra bytes, keep scanning for line end
    }

private:
    enum class State { Normal, Iac, IacOpt, IacSb, IacSbIac };

    static constexpr uint8_t kIac       = 255;
    static constexpr uint8_t kIacSb     = 250;
    static constexpr uint8_t kIacSe     = 240;
    static constexpr uint8_t kBackspace = 0x08;
    static constexpr uint8_t kDel       = 0x7f;

    // Keeps `_buf` a valid, NUL-terminated C string at every point, not just
    // after a LineReady result — a caller inspecting line()/length() mid-line
    // (e.g. to redraw a prompt) must never see stale bytes from a previous,
    // longer line.
    void _terminate() {
        if (_capacity > 0) _buf[_len < _capacity ? _len : _capacity - 1] = '\0';
    }

    void _startNewLineIfNeeded() {
        if (_lineComplete) {
            _len = 0;
            _lineComplete = false;
            _terminate();
        }
    }

    char*  _buf;
    size_t _capacity;
    size_t _len = 0;
    State  _state = State::Normal;
    bool   _afterCr = false;
    bool   _lineComplete = false;
};

#endif // TELNET_LINE_H
