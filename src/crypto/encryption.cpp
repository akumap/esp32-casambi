/**
 * Casambi Encryption Implementation
 *
 * Allocation-free by design — see the ALLOCATION POLICY note in encryption.h
 * for why this matters on the BLE notification task.
 */

#include "encryption.h"
#include "../console_out.h"
#include <mbedtls/aes.h>

CasambiEncryption::CasambiEncryption(const uint8_t* key) {
    memcpy(_key, key, AES_KEY_SIZE);
}

CasambiEncryption::~CasambiEncryption() {
    // Clear sensitive data
    memset(_key, 0, AES_KEY_SIZE);
}

bool CasambiEncryption::encryptThenMac(const uint8_t* packet, size_t pktLen,
                                       const uint8_t* nonce,
                                       uint8_t* out, size_t outCap, size_t& outLen,
                                       size_t headerLen) {
    outLen = 0;

    if (pktLen < headerLen) {
        Console.println("Encrypt: Packet too small");
        return false;
    }
    if (outCap < pktLen + CMAC_SIZE) {
        Console.printf("Encrypt: Output buffer too small (%u < %u)\n",
                      (unsigned)outCap, (unsigned)(pktLen + CMAC_SIZE));
        return false;
    }

    // Header travels in the clear, the payload is AES-CTR encrypted straight
    // into the destination — the ciphertext is assembled in `out` so the CMAC
    // below can run over it without a second copy.
    memcpy(out, packet, headerLen);
    _ctrXcrypt(packet + headerLen, pktLen - headerLen, nonce, out + headerLen);

    // CMAC over header || ciphertext, appended.
    _computeCMAC(out, pktLen, out + pktLen);

    outLen = pktLen + CMAC_SIZE;
    return true;
}

bool CasambiEncryption::decryptAndVerify(const uint8_t* packet, size_t pktLen,
                                         const uint8_t* nonce,
                                         uint8_t* out, size_t outCap, size_t& outLen,
                                         size_t headerLen) {
    outLen = 0;

    if (pktLen < headerLen + CMAC_SIZE) {
        Console.println("Decrypt: Packet too small");
        return false;
    }

    const size_t ciphertextLen = pktLen - CMAC_SIZE;
    const size_t plainLen      = ciphertextLen - headerLen;

    if (outCap < plainLen) {
        Console.printf("Decrypt: Output buffer too small (%u < %u)\n",
                      (unsigned)outCap, (unsigned)plainLen);
        return false;
    }

    // Verify the MAC over header || ciphertext BEFORE decrypting anything.
    const uint8_t* receivedMac = packet + ciphertextLen;
    uint8_t computedMac[CMAC_SIZE];
    _computeCMAC(packet, ciphertextLen, computedMac);

    // Constant-time compare: OR all byte differences instead of breaking on the
    // first mismatch, so the time taken does not leak how many leading MAC bytes
    // matched (which would be a forgery oracle).
    uint8_t mac_diff = 0;
    for (size_t i = 0; i < CMAC_SIZE; i++) {
        mac_diff |= receivedMac[i] ^ computedMac[i];
    }

    if (mac_diff != 0) {
        if (bleDebugEnabled) {
            Console.println("Decrypt: CMAC verification failed!");
            Console.print("Expected: ");
            for (size_t i = 0; i < 8; i++) Console.printf("%02x ", receivedMac[i]);
            Console.print("\nComputed: ");
            for (size_t i = 0; i < 8; i++) Console.printf("%02x ", computedMac[i]);
            Console.println();
        }
        return false;
    }

    if (bleDebugEnabled) {
        Console.println("Decrypt: CMAC verified OK");
    }

    // AES-CTR is symmetric, so decrypt == encrypt.
    _ctrXcrypt(packet + headerLen, plainLen, nonce, out);

    outLen = plainLen;
    return true;
}

void CasambiEncryption::_ctrXcrypt(const uint8_t* in, size_t len,
                                   const uint8_t* nonce, uint8_t* out) {
    // Initialize AES context for ECB mode
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_enc(&aes_ctx, _key, AES_KEY_SIZE * 8);

    // Process data in 16-byte blocks
    uint32_t counter = 0;
    size_t offset = 0;

    while (offset < len) {
        // Build counter block: nonce with last 4 bytes as little-endian counter
        uint8_t counter_block[16];
        memcpy(counter_block, nonce, 12); // First 12 bytes from nonce

        // Last 4 bytes: counter in little-endian
        counter_block[12] = counter & 0xFF;
        counter_block[13] = (counter >> 8) & 0xFF;
        counter_block[14] = (counter >> 16) & 0xFF;
        counter_block[15] = (counter >> 24) & 0xFF;

        // Encrypt counter block
        uint8_t keystream[16];
        mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, counter_block, keystream);

        // XOR with plaintext/ciphertext
        size_t remaining = len - offset;
        size_t block_size = (remaining < 16) ? remaining : 16;

        for (size_t i = 0; i < block_size; i++) {
            out[offset + i] = in[offset + i] ^ keystream[i];
        }

        offset += block_size;
        counter++;
    }

    mbedtls_aes_free(&aes_ctx);
}

void CasambiEncryption::_computeCMAC(const uint8_t* data, size_t len,
                                     uint8_t mac[CMAC_SIZE]) {
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
    bool complete_block = (len > 0 && len % 16 == 0);

    if (complete_block) {
        // M_last = M_n XOR K1
        size_t last_block_offset = len - 16;
        for (int i = 0; i < 16; i++) {
            M_last[i] = data[last_block_offset + i] ^ K1[i];
        }
    } else {
        // M_last = (M_n || 10^j) XOR K2
        size_t remaining = len % 16;
        size_t last_block_offset = len - remaining;

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
    size_t num_blocks = (len + 15) / 16;

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
    memcpy(mac, X, CMAC_SIZE);

    mbedtls_aes_free(&aes_ctx);
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
        uint8_t mac[CMAC_SIZE];
        enc._computeCMAC(M, v.len, mac);
        if (memcmp(mac, v.mac, CMAC_SIZE) != 0) {
            Console.printf("CMAC self-test FAILED for message length %u\n",
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
