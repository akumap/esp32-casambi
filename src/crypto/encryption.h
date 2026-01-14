/**
 * Casambi Encryption
 *
 * AES-CTR encryption with CMAC authentication for Casambi BLE protocol
 */

#ifndef ENCRYPTION_H
#define ENCRYPTION_H

#include <Arduino.h>
#include <vector>
#include "../config.h"

class CasambiEncryption {
public:
    /**
     * Initialize encryption with AES key
     * @param key 16-byte AES key
     */
    CasambiEncryption(const uint8_t* key);
    ~CasambiEncryption();

    /**
     * Encrypt packet and append CMAC
     * @param packet Data to encrypt (header + payload)
     * @param nonce 16-byte nonce for encryption
     * @param headerLen Length of unencrypted header (default 4)
     * @return Encrypted packet with appended CMAC
     */
    std::vector<uint8_t> encryptThenMac(
        const std::vector<uint8_t>& packet,
        const std::vector<uint8_t>& nonce,
        size_t headerLen = PACKET_HEADER_LEN
    );

    /**
     * Verify CMAC and decrypt packet
     * @param packet Encrypted packet with CMAC
     * @param nonce 16-byte nonce used for encryption
     * @param headerLen Length of unencrypted header (default 4)
     * @return Decrypted payload (without header or CMAC)
     * @throws std::runtime_error if CMAC verification fails
     */
    std::vector<uint8_t> decryptAndVerify(
        const std::vector<uint8_t>& packet,
        const std::vector<uint8_t>& nonce,
        size_t headerLen = PACKET_HEADER_LEN
    );

private:
    uint8_t _key[AES_KEY_SIZE];

    /**
     * AES-CTR encryption/decryption (symmetric operation)
     */
    std::vector<uint8_t> _encryptInternal(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& nonce
    );

    /**
     * Compute CMAC over data
     */
    std::vector<uint8_t> _computeCMAC(const std::vector<uint8_t>& data);

    /**
     * Left shift operation for CMAC subkey generation
     */
    static void _leftShift(const uint8_t* input, uint8_t* output);

    /**
     * XOR two byte arrays of equal length
     */
    static std::vector<uint8_t> _xor(
        const std::vector<uint8_t>& a,
        const std::vector<uint8_t>& b
    );
};

#endif // ENCRYPTION_H
