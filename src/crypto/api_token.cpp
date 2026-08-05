/**
 * ApiToken — see api_token.h.
 */

#include "api_token.h"

#include <cstdio>
#include <cstring>
#include <mbedtls/sha256.h>
#include "../config.h"

namespace ApiToken {

String derive(const String& casambiPassword) {
    if (casambiPassword.isEmpty()) return "";

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const uint8_t*)API_TOKEN_PREFIX, strlen(API_TOKEN_PREFIX));
    mbedtls_sha256_update(&ctx, (const uint8_t*)casambiPassword.c_str(),
                          casambiPassword.length());
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[sizeof(hash) * 2 + 1];
    for (size_t i = 0; i < sizeof(hash); i++) sprintf(hex + i * 2, "%02x", hash[i]);
    hex[sizeof(hash) * 2] = '\0';
    return String(hex);
}

bool constantTimeEquals(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return diff == 0;
}

}  // namespace ApiToken
