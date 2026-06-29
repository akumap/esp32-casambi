/**
 * Casambi Encryption Implementation
 */

#include "encryption.h"
#include <mbedtls/aes.h>

CasambiEncryption::CasambiEncryption(const uint8_t* key) {
    memcpy(_key, key, AES_KEY_SIZE);
}

CasambiEncryption::~CasambiEncryption() {
    // Clear sensitive data
    memset(_key, 0, AES_KEY_SIZE);
}

std::vector<uint8_t> CasambiEncryption::encryptThenMac(
    const std::vector<uint8_t>& packet,
    const std::vector<uint8_t>& nonce,
    size_t headerLen
) {
    if (nonce.size() != NONCE_SIZE) {
        Serial.println("Encrypt: Invalid nonce size");
        return std::vector<uint8_t>();
    }

    if (packet.size() < headerLen) {
        Serial.println("Encrypt: Packet too small");
        return std::vector<uint8_t>();
    }

    // Split packet into header and payload
    std::vector<uint8_t> header(packet.begin(), packet.begin() + headerLen);
    std::vector<uint8_t> payload(packet.begin() + headerLen, packet.end());

    // Encrypt payload
    std::vector<uint8_t> encrypted_payload = _encryptInternal(payload, nonce);

    // Combine header + encrypted payload
    std::vector<uint8_t> ciphertext = header;
    ciphertext.insert(ciphertext.end(), encrypted_payload.begin(), encrypted_payload.end());

    // Compute CMAC over ciphertext
    std::vector<uint8_t> mac = _computeCMAC(ciphertext);

    // Append CMAC
    ciphertext.insert(ciphertext.end(), mac.begin(), mac.end());

    return ciphertext;
}

std::vector<uint8_t> CasambiEncryption::decryptAndVerify(
    const std::vector<uint8_t>& packet,
    const std::vector<uint8_t>& nonce,
    size_t headerLen
) {
    if (nonce.size() != NONCE_SIZE) {
        Serial.println("Decrypt: Invalid nonce size");
        return std::vector<uint8_t>();
    }

    if (packet.size() < headerLen + CMAC_SIZE) {
        Serial.println("Decrypt: Packet too small");
        return std::vector<uint8_t>();
    }

    // Split packet: ciphertext (header + encrypted payload) and MAC
    size_t ciphertext_len = packet.size() - CMAC_SIZE;
    std::vector<uint8_t> ciphertext(packet.begin(), packet.begin() + ciphertext_len);
    std::vector<uint8_t> received_mac(packet.begin() + ciphertext_len, packet.end());

    // Verify CMAC
    std::vector<uint8_t> computed_mac = _computeCMAC(ciphertext);

    // Constant-time compare: OR all byte differences instead of breaking on the
    // first mismatch, so the time taken does not leak how many leading MAC bytes
    // matched (which would be a forgery oracle).
    uint8_t mac_diff = 0;
    for (size_t i = 0; i < CMAC_SIZE; i++) {
        mac_diff |= received_mac[i] ^ computed_mac[i];
    }
    bool mac_valid = (mac_diff == 0);

    if (!mac_valid) {
        if (bleDebugEnabled) {
            Serial.println("Decrypt: CMAC verification failed!");
            Serial.print("Expected: ");
            for (size_t i = 0; i < 8; i++) Serial.printf("%02x ", received_mac[i]);
            Serial.print("\nComputed: ");
            for (size_t i = 0; i < 8; i++) Serial.printf("%02x ", computed_mac[i]);
            Serial.println();
        }
        return std::vector<uint8_t>();
    }

    if (bleDebugEnabled) {
        Serial.println("Decrypt: CMAC verified OK");
    }

    // Extract encrypted payload (skip header)
    std::vector<uint8_t> encrypted_payload(ciphertext.begin() + headerLen, ciphertext.end());

    // Decrypt (AES-CTR is symmetric, so decrypt = encrypt)
    std::vector<uint8_t> plaintext = _encryptInternal(encrypted_payload, nonce);

    return plaintext;
}

std::vector<uint8_t> CasambiEncryption::_encryptInternal(
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& nonce
) {
    if (nonce.size() != NONCE_SIZE) {
        Serial.println("_encryptInternal: Invalid nonce size");
        return std::vector<uint8_t>();
    }

    std::vector<uint8_t> result;
    result.reserve(data.size());

    // Initialize AES context for ECB mode
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_enc(&aes_ctx, _key, AES_KEY_SIZE * 8);

    // Process data in 16-byte blocks
    uint32_t counter = 0;
    size_t offset = 0;

    while (offset < data.size()) {
        // Build counter block: nonce with last 4 bytes as little-endian counter
        uint8_t counter_block[16];
        memcpy(counter_block, nonce.data(), 12); // First 12 bytes from nonce

        // Last 4 bytes: counter in little-endian
        counter_block[12] = counter & 0xFF;
        counter_block[13] = (counter >> 8) & 0xFF;
        counter_block[14] = (counter >> 16) & 0xFF;
        counter_block[15] = (counter >> 24) & 0xFF;

        // Encrypt counter block
        uint8_t encrypted_block[16];
        mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, counter_block, encrypted_block);

        // XOR with plaintext/ciphertext
        size_t remaining = data.size() - offset;
        size_t block_size = (remaining < 16) ? remaining : 16;

        for (size_t i = 0; i < block_size; i++) {
            result.push_back(data[offset + i] ^ encrypted_block[i]);
        }

        offset += block_size;
        counter++;
    }

    mbedtls_aes_free(&aes_ctx);
    return result;
}

std::vector<uint8_t> CasambiEncryption::_computeCMAC(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> mac(CMAC_SIZE);

    // Manual CMAC-AES implementation (RFC 4493)
    // Since mbedTLS CMAC is not compiled in, we implement it using AES-ECB

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_enc(&aes_ctx, _key, AES_KEY_SIZE * 8);

    // Generate subkeys K1 and K2
    uint8_t L[16] = {0};
    uint8_t K1[16], K2[16];

    // L = AES(K, 0^128)
    mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, L, L);

    // K1 = L << 1
    _leftShift(L, K1);
    if (L[0] & 0x80) {
        K1[15] ^= 0x87;  // XOR with Rb
    }

    // K2 = K1 << 1
    _leftShift(K1, K2);
    if (K1[0] & 0x80) {
        K2[15] ^= 0x87;  // XOR with Rb
    }

    // Process message
    uint8_t M_last[16] = {0};
    bool complete_block = (data.size() > 0 && data.size() % 16 == 0);

    if (complete_block) {
        // M_last = M_n XOR K1
        size_t last_block_offset = data.size() - 16;
        for (int i = 0; i < 16; i++) {
            M_last[i] = data[last_block_offset + i] ^ K1[i];
        }
    } else {
        // M_last = (M_n || 10^j) XOR K2
        size_t remaining = data.size() % 16;
        size_t last_block_offset = data.size() - remaining;

        for (size_t i = 0; i < remaining; i++) {
            M_last[i] = data[last_block_offset + i];
        }
        M_last[remaining] = 0x80;  // Padding

        for (int i = 0; i < 16; i++) {
            M_last[i] ^= K2[i];
        }
    }

    // CBC-MAC
    uint8_t X[16] = {0};
    size_t num_blocks = (data.size() + 15) / 16;

    // Process all but last block (i + 1 < num_blocks avoids size_t underflow when num_blocks == 0)
    for (size_t i = 0; i + 1 < num_blocks; i++) {
        for (int j = 0; j < 16; j++) {
            X[j] ^= data[i * 16 + j];
        }
        mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, X, X);
    }

    // Process last block
    for (int i = 0; i < 16; i++) {
        X[i] ^= M_last[i];
    }
    mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, X, X);

    // Copy result
    memcpy(mac.data(), X, CMAC_SIZE);

    mbedtls_aes_free(&aes_ctx);
    return mac;
}

bool CasambiEncryption::selfTestRFC4493() {
    // RFC 4493 Appendix D test vectors (AES-128 CMAC).
    // Key K = 2b7e1516 28aed2a6 abf71588 09cf4f3c
    static const uint8_t K[16] = {
        0x2b,0x7e,0x15,0x16, 0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88, 0x09,0xcf,0x4f,0x3c
    };
    // Full 64-byte example message M (cases use the first 0/16/40/64 bytes).
    static const uint8_t M[64] = {
        0x6b,0xc1,0xbe,0xe2, 0x2e,0x40,0x9f,0x96, 0xe9,0x3d,0x7e,0x11, 0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57, 0x1e,0x03,0xac,0x9c, 0x9e,0xb7,0x6f,0xac, 0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46, 0xa3,0x5c,0xe4,0x11, 0xe5,0xfb,0xc1,0x19, 0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45, 0xdf,0x4f,0x9b,0x17, 0xad,0x2b,0x41,0x7b, 0xe6,0x6c,0x37,0x10
    };
    struct Vec { size_t len; uint8_t mac[16]; };
    static const Vec vecs[] = {
        { 0,  {0xbb,0x1d,0x69,0x29, 0xe9,0x59,0x37,0x28, 0x7f,0xa3,0x7d,0x12, 0x9b,0x75,0x67,0x46} },
        { 16, {0x07,0x0a,0x16,0xb4, 0x6b,0x4d,0x41,0x44, 0xf7,0x9b,0xdd,0x9d, 0xd0,0x4a,0x28,0x7c} },
        { 40, {0xdf,0xa6,0x67,0x47, 0xde,0x9a,0xe6,0x30, 0x30,0xca,0x32,0x61, 0x14,0x97,0xc8,0x27} },
        { 64, {0x51,0xf0,0xbe,0xbf, 0x7e,0x3b,0x9d,0x92, 0xfc,0x49,0x74,0x17, 0x79,0x36,0x3c,0xfe} },
    };

    CasambiEncryption enc(K);
    for (const auto& v : vecs) {
        std::vector<uint8_t> msg(M, M + v.len);
        std::vector<uint8_t> mac = enc._computeCMAC(msg);
        if (mac.size() != CMAC_SIZE || memcmp(mac.data(), v.mac, CMAC_SIZE) != 0) {
            Serial.printf("CMAC self-test FAILED for message length %u\n",
                          (unsigned)v.len);
            return false;
        }
    }
    return true;
}

void CasambiEncryption::_leftShift(const uint8_t* input, uint8_t* output) {
    uint8_t overflow = 0;
    for (int i = 15; i >= 0; i--) {
        output[i] = (input[i] << 1) | overflow;
        overflow = (input[i] & 0x80) ? 1 : 0;
    }
}

std::vector<uint8_t> CasambiEncryption::_xor(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b
) {
    std::vector<uint8_t> result(a.size());
    for (size_t i = 0; i < a.size(); i++) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}
