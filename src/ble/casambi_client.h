/**
 * Casambi BLE Client
 *
 * Handles BLE connection, ECDH key exchange, authentication, and packet communication
 */

#ifndef CASAMBI_CLIENT_H
#define CASAMBI_CLIENT_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <vector>
#include "../config.h"
#include "../cloud/network_config.h"
#include "../crypto/encryption.h"
#include "../crypto/key_exchange.h"

// ============================================================================
// CONNECTION STATES
// ============================================================================

enum class ConnectionState {
    None = 0,
    Connected = 1,
    KeyExchanged = 2,
    Authenticated = 3,
    Error = 99
};

// ============================================================================
// CASAMBI BLE CLIENT
// ============================================================================

class CasambiClient {
public:
    CasambiClient(NetworkConfig* config);
    ~CasambiClient();

    /**
     * Connect to Casambi device
     * @param address BLE MAC address
     * @return true on success
     */
    bool connect(const String& address);

    /**
     * Disconnect from device
     */
    void disconnect();

    /**
     * Check if connected and authenticated
     */
    bool isAuthenticated() const {
        return _state == ConnectionState::Authenticated;
    }

    /**
     * Get current connection state
     */
    ConnectionState getState() const { return _state; }

    // ========================================================================
    // CONTROL FUNCTIONS
    // ========================================================================

    /**
     * Set scene level
     * @param sceneId Scene ID (0-255)
     * @param level Level 0-255 (0xFF = restore last level)
     */
    void setSceneLevel(uint8_t sceneId, uint8_t level);

    /**
     * Set unit level
     * @param unitId Unit ID (0-255)
     * @param level Level 0-255
     */
    void setUnitLevel(uint8_t unitId, uint8_t level);

    /**
     * Set group level
     * @param groupId Group ID (0-255)
     * @param level Level 0-255
     */
    void setGroupLevel(uint8_t groupId, uint8_t level);

    /**
     * Set unit vertical (light distribution balance)
     * @param unitId Unit ID (0-255)
     * @param vertical Light balance 0-255 (0=top only, 127=both, 255=bottom only)
     */
    void setUnitVertical(uint8_t unitId, uint8_t vertical);

    /**
     * Set group vertical (light distribution balance)
     * @param groupId Group ID (0-255)
     * @param vertical Light balance 0-255 (0=top only, 127=both, 255=bottom only)
     */
    void setGroupVertical(uint8_t groupId, uint8_t vertical);

    /**
     * Set unit color temperature
     * @param unitId Unit ID
     * @param kelvin Color temperature in Kelvin (divided by 50 for protocol)
     */
    void setUnitTemperature(uint8_t unitId, uint16_t kelvin);

    /**
     * Set unit RGB color
     * @param unitId Unit ID
     * @param r Red 0-255
     * @param g Green 0-255
     * @param b Blue 0-255
     */
    void setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b);

    /**
     * Set unit slider (motor position control)
     * @param unitId Unit ID
     * @param value Motor position 0-255 (0=up, 255=down)
     */
    void setUnitSlider(uint8_t unitId, uint8_t value);

    /**
     * Set group slider (motor position control)
     * @param groupId Group ID
     * @param value Motor position 0-255 (0=up, 255=down)
     */
    void setGroupSlider(uint8_t groupId, uint8_t value);

private:
    NetworkConfig* _config;
    BLEClient* _bleClient;
    BLERemoteCharacteristic* _authChar;

    ConnectionState _state;
    ECDHKeyExchange* _keyExchange;
    CasambiEncryption* _encryption;

    uint8_t _nonce[NONCE_SIZE];
    uint8_t _mtu;
    uint16_t _unitId;
    uint16_t _flags;

    uint32_t _outPacketCount;
    uint32_t _inPacketCount;
    uint16_t _origin;

    // Thread safety for concurrent command line + web server access
    SemaphoreHandle_t _mutex;

    // ========================================================================
    // CONNECTION FLOW
    // ========================================================================

    /**
     * Read initial device info and start notifications
     */
    bool _readDeviceInfo();

    /**
     * Perform ECDH key exchange
     */
    bool _performKeyExchange();

    /**
     * Authenticate using session key
     */
    bool _authenticate();

    // ========================================================================
    // PACKET HANDLING
    // ========================================================================

    /**
     * Send operation packet
     * @param opcode Operation code
     * @param target Target encoding (deviceId << 8 | type)
     * @param payload Operation payload
     */
    void _sendOperation(uint8_t opcode, uint16_t target, const std::vector<uint8_t>& payload);

    /**
     * Build operation packet
     */
    std::vector<uint8_t> _buildOperation(uint8_t opcode, uint16_t target,
                                         const std::vector<uint8_t>& payload);

    /**
     * Send encrypted packet
     */
    void _sendEncryptedPacket(const std::vector<uint8_t>& packet, uint32_t counter);

    /**
     * Get nonce for packet encryption
     */
    std::vector<uint8_t> _getNonce(uint32_t counter);

    // ========================================================================
    // BLE CALLBACKS
    // ========================================================================

    static void _notifyCallback(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);
    void _handleNotification(uint8_t* data, size_t len);
    void _handleKeyExchangeNotification(uint8_t* data, size_t len);
    void _handleAuthNotification(uint8_t* data, size_t len);
    void _handleDataNotification(uint8_t* data, size_t len);
};

#endif // CASAMBI_CLIENT_H
