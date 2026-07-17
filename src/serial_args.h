/**
 * Pure argument parsing for the serial control commands.
 *
 * Deliberately free of Arduino dependencies so the exact validation the
 * firmware runs can be exercised by host-side unit tests
 * (`pio test -e native`, test/test_serial_args) — same pattern as
 * config_validation.h and packet_parse.h.
 *
 * Templated on the string type instead of using Arduino String directly.
 * TStr must provide: copy construction/assignment, trim(), length(),
 * operator[](unsigned) and substring(unsigned from, unsigned to) — Arduino
 * String qualifies; the host tests use a small std::string-backed shim.
 */

#ifndef SERIAL_ARGS_H
#define SERIAL_ARGS_H

namespace serialargs {

// Parse a decimal integer strictly: the whole trimmed string must be digits
// (one optional leading '-') and the value must lie within [minV, maxV].
// Unlike String::toInt(), "300", "-1" and "foo" are rejected instead of
// silently truncating to some other value after a uint8 cast.
// Magnitudes above 100000 are rejected outright (bound before overflow
// could occur) — every command range in this firmware is far below that.
template <typename TStr>
inline bool parseInt(const TStr& raw, long minV, long maxV, long& out) {
    TStr s = raw;
    s.trim();
    if (s.length() == 0) return false;
    unsigned i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.length() == 1) return false;
    }
    long v = 0;
    for (; i < s.length(); i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > 100000L) return false;   // bound before overflow could occur
    }
    if (neg) v = -v;
    if (v < minV || v > maxV) return false;
    out = v;
    return true;
}

// Split a command's argument tail into whitespace-separated tokens.
// Returns the token count, or -1 when there are more than maxTok tokens.
template <typename TStr>
inline int splitArgs(const TStr& tail, TStr* tok, int maxTok) {
    int n = 0;
    unsigned i = 0;
    const unsigned len = tail.length();
    while (i < len) {
        while (i < len && tail[i] == ' ') i++;
        if (i >= len) break;
        if (n == maxTok) return -1;
        unsigned start = i;
        while (i < len && tail[i] != ' ') i++;
        tok[n++] = tail.substring(start, i);
    }
    return n;
}

}  // namespace serialargs

#endif  // SERIAL_ARGS_H
