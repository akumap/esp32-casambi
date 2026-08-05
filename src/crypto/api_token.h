/**
 * Shared derivation of the API token from the Casambi network password.
 *
 *   apiToken = hex( SHA-256( API_TOKEN_PREFIX || casambiPassword ) )
 *
 * Used by the REST/WebSocket auth in web/webserver.cpp AND by the telnet
 * console login (net/telnet_console.cpp) — extracted here so the derivation
 * and the constant-time comparison exist in exactly one place instead of
 * two copies of security-sensitive code drifting apart.
 */

#ifndef CRYPTO_API_TOKEN_H
#define CRYPTO_API_TOKEN_H

#include <Arduino.h>

namespace ApiToken {

// Empty when `casambiPassword` is empty (no password stored yet — the
// caller decides what "no token" means for its own auth-disabled case).
String derive(const String& casambiPassword);

// Constant-time comparison: does not early-exit on the first mismatching
// character, so timing does not reveal how many leading characters matched.
// A length mismatch still short-circuits (lengths are not secret).
bool constantTimeEquals(const String& a, const String& b);

}  // namespace ApiToken

#endif // CRYPTO_API_TOKEN_H
