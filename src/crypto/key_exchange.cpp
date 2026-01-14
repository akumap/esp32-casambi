/**
 * ECDH Key Exchange Implementation
 */

#include "key_exchange.h"
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/bignum.h>

ECDHKeyExchange::ECDHKeyExchange()
    : _ecdh_context(nullptr), _keyPairGenerated(false), _deviceKeySet(false) {
    // Allocate and initialize ECDH context
    _ecdh_context = new mbedtls_ecdh_context;
    mbedtls_ecdh_init(static_cast<mbedtls_ecdh_context*>(_ecdh_context));
}

ECDHKeyExchange::~ECDHKeyExchange() {
    if (_ecdh_context) {
        mbedtls_ecdh_free(static_cast<mbedtls_ecdh_context*>(_ecdh_context));
        delete static_cast<mbedtls_ecdh_context*>(_ecdh_context);
        _ecdh_context = nullptr;
    }
}

bool ECDHKeyExchange::generateKeyPair() {
    if (!_ecdh_context) return false;

    mbedtls_ecdh_context* ctx = static_cast<mbedtls_ecdh_context*>(_ecdh_context);

    // Initialize entropy and RNG
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    const char* pers = "ecdh_keygen";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        Serial.printf("ECDH: RNG seed failed: %d\n", ret);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    // Setup SECP256R1 curve
    ret = mbedtls_ecp_group_load(&ctx->grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        Serial.printf("ECDH: Group load failed: %d\n", ret);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    // Generate key pair
    ret = mbedtls_ecdh_gen_public(&ctx->grp, &ctx->d, &ctx->Q,
                                   mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        Serial.printf("ECDH: Key generation failed: %d\n", ret);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    _keyPairGenerated = true;
    if (debugEnabled) {
        Serial.println("ECDH: Key pair generated");
    }
    return true;
}

bool ECDHKeyExchange::setDevicePublicKey(const uint8_t* x, const uint8_t* y) {
    if (!_ecdh_context || !_keyPairGenerated) return false;

    mbedtls_ecdh_context* ctx = static_cast<mbedtls_ecdh_context*>(_ecdh_context);

    // Initialize device public key point
    mbedtls_ecp_point Qp;
    mbedtls_ecp_point_init(&Qp);

    int ret = 0;

    // Import X coordinate (little-endian to big-endian)
    mbedtls_mpi x_mpi;
    mbedtls_mpi_init(&x_mpi);
    ret = mbedtls_mpi_read_binary_le(&x_mpi, x, ECDH_KEY_SIZE);
    if (ret != 0) {
        Serial.printf("ECDH: Failed to read X: %d\n", ret);
        mbedtls_mpi_free(&x_mpi);
        mbedtls_ecp_point_free(&Qp);
        return false;
    }

    // Import Y coordinate (little-endian to big-endian)
    mbedtls_mpi y_mpi;
    mbedtls_mpi_init(&y_mpi);
    ret = mbedtls_mpi_read_binary_le(&y_mpi, y, ECDH_KEY_SIZE);
    if (ret != 0) {
        Serial.printf("ECDH: Failed to read Y: %d\n", ret);
        mbedtls_mpi_free(&x_mpi);
        mbedtls_mpi_free(&y_mpi);
        mbedtls_ecp_point_free(&Qp);
        return false;
    }

    // Set Z coordinate to 1 (affine coordinates)
    mbedtls_mpi_lset(&Qp.Z, 1);

    // Copy coordinates
    mbedtls_mpi_copy(&Qp.X, &x_mpi);
    mbedtls_mpi_copy(&Qp.Y, &y_mpi);

    // Verify point is on curve
    ret = mbedtls_ecp_check_pubkey(&ctx->grp, &Qp);
    if (ret != 0) {
        Serial.printf("ECDH: Invalid public key: %d\n", ret);
        mbedtls_mpi_free(&x_mpi);
        mbedtls_mpi_free(&y_mpi);
        mbedtls_ecp_point_free(&Qp);
        return false;
    }

    // Store device public key
    mbedtls_ecp_copy(&ctx->Qp, &Qp);

    mbedtls_mpi_free(&x_mpi);
    mbedtls_mpi_free(&y_mpi);
    mbedtls_ecp_point_free(&Qp);

    _deviceKeySet = true;
    if (debugEnabled) {
        Serial.println("ECDH: Device public key set");
    }
    return true;
}

std::vector<uint8_t> ECDHKeyExchange::getPublicKeyX() const {
    std::vector<uint8_t> result(ECDH_KEY_SIZE, 0);

    if (!_keyPairGenerated || !_ecdh_context) return result;

    mbedtls_ecdh_context* ctx = static_cast<mbedtls_ecdh_context*>(_ecdh_context);

    // Export X coordinate in little-endian format
    mbedtls_mpi_write_binary_le(&ctx->Q.X, result.data(), ECDH_KEY_SIZE);

    return result;
}

std::vector<uint8_t> ECDHKeyExchange::getPublicKeyY() const {
    std::vector<uint8_t> result(ECDH_KEY_SIZE, 0);

    if (!_keyPairGenerated || !_ecdh_context) return result;

    mbedtls_ecdh_context* ctx = static_cast<mbedtls_ecdh_context*>(_ecdh_context);

    // Export Y coordinate in little-endian format
    mbedtls_mpi_write_binary_le(&ctx->Q.Y, result.data(), ECDH_KEY_SIZE);

    return result;
}

std::vector<uint8_t> ECDHKeyExchange::deriveTransportKey() {
    std::vector<uint8_t> result(AES_KEY_SIZE, 0);

    if (!_keyPairGenerated || !_deviceKeySet || !_ecdh_context) {
        Serial.println("ECDH: Cannot derive key - not ready");
        return result;
    }

    mbedtls_ecdh_context* ctx = static_cast<mbedtls_ecdh_context*>(_ecdh_context);

    // Compute shared secret
    mbedtls_mpi shared_secret;
    mbedtls_mpi_init(&shared_secret);

    int ret = mbedtls_ecdh_compute_shared(&ctx->grp, &shared_secret,
                                           &ctx->Qp, &ctx->d,
                                           nullptr, nullptr);
    if (ret != 0) {
        Serial.printf("ECDH: Compute shared secret failed: %d\n", ret);
        mbedtls_mpi_free(&shared_secret);
        return result;
    }

    // Export shared secret to bytes (big-endian)
    uint8_t secret_bytes[ECDH_KEY_SIZE];
    mbedtls_mpi_write_binary(&shared_secret, secret_bytes, ECDH_KEY_SIZE);
    mbedtls_mpi_free(&shared_secret);

    // Python code reverses the bytes before hashing
    std::vector<uint8_t> secret_reversed(ECDH_KEY_SIZE);
    for (int i = 0; i < ECDH_KEY_SIZE; i++) {
        secret_reversed[i] = secret_bytes[ECDH_KEY_SIZE - 1 - i];
    }

    // SHA256 hash
    uint8_t hash[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); // 0 = SHA256 (not SHA224)
    mbedtls_sha256_update(&sha_ctx, secret_reversed.data(), ECDH_KEY_SIZE);
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    // XOR fold: transport_key[i] = hash[i] ^ hash[i+16]
    for (int i = 0; i < AES_KEY_SIZE; i++) {
        result[i] = hash[i] ^ hash[16 + i];
    }

    if (debugEnabled) {
        Serial.println("ECDH: Transport key derived");
    }
    return result;
}

