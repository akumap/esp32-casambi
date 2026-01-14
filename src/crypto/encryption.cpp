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

    bool mac_valid = true;
    for (size_t i = 0; i < CMAC_SIZE; i++) {
        if (received_mac[i] != computed_mac[i]) {
            mac_valid = false;
            break;
        }
    }

    if (!mac_valid) {
        if (debugEnabled) {
            Serial.println("Decrypt: CMAC verification failed!");
            Serial.print("Expected: ");
            for (size_t i = 0; i < 8; i++) Serial.printf("%02x ", received_mac[i]);
            Serial.print("\nComputed: ");
            for (size_t i = 0; i < 8; i++) Serial.printf("%02x ", computed_mac[i]);
            Serial.println();
        }
        return std::vector<uint8_t>();
    }

    if (debugEnabled) {
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

    // Process all but last block
    for (size_t i = 0; i < num_blocks - 1; i++) {
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
