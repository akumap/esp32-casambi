/**
 * Casambi Encryption
 *
 * AES-CTR encryption with CMAC authentication for Casambi BLE protocol
 *
 * ALLOCATION POLICY
 * -----------------
 * Every entry point here works on caller-supplied buffers and allocates
 * nothing. That is deliberate and extends the rule the WebSocket broadcast
 * path already follows (see webserver.cpp: "the BLE task never allocates"):
 * decryptAndVerify() runs on the NimBLE host task, which on the dual-core
 * targets executes in parallel with the async_tcp task (pinned to core 1 via
 * CONFIG_ASYNC_TCP_RUNNING_CORE). A burst of 0x06 notifications is exactly the
 * event that also makes async_tcp allocate — one incoming packet becomes one
 * WebSocket broadcast — so short-lived crypto blocks and longer-lived TCP/WS
 * structures would otherwise interleave in the heap at precisely the same
 * moment, leaving small durable blocks that stop free space from coalescing.
 * Keeping this path allocation-free removes that coupling.
 *
 * All buffers are bounded by CRYPTO_MAX_PACKET_LEN; callers size their stack
 * buffers from the same constant.
 */

#ifndef ENCRYPTION_H
#define ENCRYPTION_H

#include <Arduino.h>
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
     * Encrypt a packet into `out` and append the CMAC.
     *
     * The first `headerLen` bytes are copied verbatim (they travel in the
     * clear); the remainder is AES-CTR encrypted, then a CMAC over
     * header||ciphertext is appended.
     *
     * @param packet    Plaintext (header + payload)
     * @param pktLen    Length of `packet`, must be >= headerLen
     * @param nonce     NONCE_SIZE-byte nonce
     * @param out       Destination, needs room for pktLen + CMAC_SIZE bytes
     * @param outCap    Capacity of `out`
     * @param outLen    Set to the number of bytes written on success
     * @param headerLen Length of the unencrypted header
     * @return false if a length check fails; `out` is then meaningless
     */
    bool encryptThenMac(const uint8_t* packet, size_t pktLen,
                        const uint8_t* nonce,
                        uint8_t* out, size_t outCap, size_t& outLen,
                        size_t headerLen = PACKET_HEADER_LEN);

    /**
     * Verify the trailing CMAC and decrypt the payload into `out`.
     *
     * The MAC is checked before anything is decrypted, and the comparison is
     * constant time so a mismatch does not reveal how many leading MAC bytes
     * matched (which would be a forgery oracle).
     *
     * @param packet    Encrypted packet (header + ciphertext + CMAC)
     * @param pktLen    Length of `packet`
     * @param nonce     NONCE_SIZE-byte nonce used for encryption
     * @param out       Destination for the plaintext payload, needs room for
     *                  pktLen - headerLen - CMAC_SIZE bytes
     * @param outCap    Capacity of `out`
     * @param outLen    Set to the number of plaintext bytes on success
     * @param headerLen Length of the unencrypted header
     * @return false on a length check or CMAC mismatch; `out` is then
     *         meaningless and must not be parsed
     */
    bool decryptAndVerify(const uint8_t* packet, size_t pktLen,
                          const uint8_t* nonce,
                          uint8_t* out, size_t outCap, size_t& outLen,
                          size_t headerLen = PACKET_HEADER_LEN);

    /**
     * Validate the AES-CMAC implementation against the official RFC 4493
     * Appendix D test vectors (subkey generation + MAC over the 0/16/40/64-byte
     * example messages). Returns true if all vectors match. Intended as a
     * one-shot boot self-test; logs a message on failure.
     */
    static bool selfTestRFC4493();

private:
    uint8_t _key[AES_KEY_SIZE];

    /**
     * AES-CTR keystream XOR (symmetric: encrypt == decrypt). `in` and `out`
     * may overlap only if identical. The counter starts at 0 and occupies the
     * last 4 counter-block bytes in little-endian order.
     */
    void _ctrXcrypt(const uint8_t* in, size_t len, const uint8_t* nonce, uint8_t* out);

    /**
     * Compute the AES-CMAC (RFC 4493) over `data` into the caller's buffer.
     */
    void _computeCMAC(const uint8_t* data, size_t len, uint8_t mac[CMAC_SIZE]);

    /**
     * Left shift operation for CMAC subkey generation
     */
    static void _leftShift(const uint8_t* input, uint8_t* output);
};

#endif // ENCRYPTION_H
