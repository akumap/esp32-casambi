/**
 * ECDH Key Exchange for Casambi
 *
 * SECP256R1 elliptic curve Diffie-Hellman key exchange
 */

#ifndef KEY_EXCHANGE_H
#define KEY_EXCHANGE_H

#include <Arduino.h>
#include <vector>
#include "../config.h"

class ECDHKeyExchange {
public:
    ECDHKeyExchange();
    ~ECDHKeyExchange();

    /**
     * Generate new SECP256R1 key pair
     * @return true on success
     */
    bool generateKeyPair();

    /**
     * Set device's public key from coordinates
     * @param x X coordinate (32 bytes, little-endian)
     * @param y Y coordinate (32 bytes, little-endian)
     * @return true on success
     */
    bool setDevicePublicKey(const uint8_t* x, const uint8_t* y);

    /**
     * Get our public key X coordinate
     * @return 32-byte X coordinate (little-endian)
     */
    std::vector<uint8_t> getPublicKeyX() const;

    /**
     * Get our public key Y coordinate
     * @return 32-byte Y coordinate (little-endian)
     */
    std::vector<uint8_t> getPublicKeyY() const;

    /**
     * Derive 16-byte transport key from shared secret
     * Uses: SHA256(shared_secret) then XOR fold to 16 bytes
     * @return 16-byte transport key
     */
    std::vector<uint8_t> deriveTransportKey();

private:
    void* _ecdh_context;  // mbedtls_ecdh_context pointer
    bool _keyPairGenerated;
    bool _deviceKeySet;
};

#endif // KEY_EXCHANGE_H
