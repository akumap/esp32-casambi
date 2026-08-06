/**
 * Which command lines the telnet console refuses to dispatch (decision E3:
 * `setup` and `wifi set` stay serial-only).
 *
 * Its own header because matching the raw input line is not good enough. The
 * dispatcher in serial_console.cpp normalises before it decides — `cmdWifi()`
 * takes `cmd.substring(5)` and trims it — so `wifi<SP><SP>set ssid pw` reaches
 * the WiFi handler while a `startsWith("wifi set")` guard on the raw line does
 * not fire. The same mismatch defeats the credential masking in
 * handleCommand(), which is keyed on the same literal: the password ends up in
 * the command echo, in the console ring buffer and thus on the very telnet
 * connection E3 exists to keep credentials off.
 *
 * The guard therefore runs on a normalised copy (leading/trailing whitespace
 * removed, every internal whitespace run collapsed to one space) and must be a
 * SUPERSET of what the dispatcher can be talked into executing. The line that
 * is actually executed stays the trimmed original — only the decision uses the
 * normalised form.
 *
 * Arduino-free and templated on the string type, same pattern as
 * serial_args.h; host tests live in test/test_telnet_policy.
 */

#ifndef NET_TELNET_POLICY_H
#define NET_TELNET_POLICY_H

namespace telnetpolicy {

// True for space, tab, CR, LF, VT, FF — the parser strips CR/LF, but a tab
// between 'wifi' and 'set' reaches us intact.
inline bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// Copies `in` into `out` (capacity `outCap`, always NUL-terminated) with
// leading/trailing whitespace removed and internal whitespace runs collapsed
// to a single space. Truncates rather than overflowing; truncation only ever
// makes the result shorter, which cannot turn a blocked command into an
// allowed one (the prefixes checked below are short).
inline void normalize(const char* in, char* out, unsigned outCap) {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!in) return;
    unsigned w = 0;
    bool pendingSpace = false;
    for (const char* p = in; *p && w + 1 < outCap; ++p) {
        if (isSpace(*p)) {
            if (w > 0) pendingSpace = true;   // never emit a leading space
            continue;
        }
        if (pendingSpace) {
            out[w++] = ' ';
            pendingSpace = false;
            if (w + 1 >= outCap) break;
        }
        out[w++] = *p;
    }
    out[w] = '\0';   // trailing whitespace is dropped: pendingSpace stays unwritten
}

inline bool startsWith(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

inline bool equals(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// True if `normalized` (output of normalize()) must not run over telnet.
//
//  - "setup"     — drives the blocking BLE scan and the five Serial.available()
//                  wait loops of the wizard, and prints credentials.
//  - "wifi set …" — would hand the WiFi password to the console echo and pull
//                  the network out from under the session; "wifi status" stays
//                  allowed.
inline bool isSerialOnly(const char* normalized) {
    if (!normalized) return false;
    if (equals(normalized, "setup")) return true;
    if (equals(normalized, "wifi set")) return true;      // usage line, still refused
    if (startsWith(normalized, "wifi set ")) return true;
    return false;
}

}  // namespace telnetpolicy

#endif // NET_TELNET_POLICY_H
