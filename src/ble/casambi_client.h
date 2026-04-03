/**
 * Casambi BLE Client
 *
 * Handles BLE connection, ECDH key exchange, authentication, and packet communication.
 * Includes auto-reconnect, connection monitoring, and data packet parsing.
 */

#ifndef CASAMBI_CLIENT_H
#define CASAMBI_CLIENT_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <vector>
#include <functional>
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
// DISCONNECT REASON (for diagnostics)
// ============================================================================

enum class DisconnectReason {
    None = 0,
    UserRequested,
    BLELinkLoss,
    AuthFailed,
    KeyExchangeFailed,
    Timeout,
    InternalError
};

// ============================================================================
// CALLBACK TYPES
// ============================================================================

// Called when a unit state changes (from incoming data packets)
using UnitStateCallback = std::function<void(uint8_t unitId, uint8_t level, bool online)>;

// Called when connection state changes
using ConnectionStateCallback = std::function<void(ConnectionState newState, DisconnectReason reason)>;

// ============================================================================
// CASAMBI BLE CLIENT
// ============================================================================

class CasambiClient {
public:
    CasambiClient(NetworkConfig* config);
    ~CasambiClient();

    // ========================================================================
    // CONNECTION MANAGEMENT
    // ========================================================================

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
     * Check if BLE link is still alive (low-level check)
     */
    bool isBLEConnected() const;


    /**
     * Send "keep-alive" messages to network, check fo response
     */
    bool sendKeepalive();

    /**
     * Get current connection state
     */
    ConnectionState getState() const { return _state; }

    /**
     * Get last disconnect reason
     */
    DisconnectReason getLastDisconnectReason() const { return _lastDisconnectReason; }

    /**
     * Get the address we're connected (or were last connected) to
     */
    String getConnectedAddress() const { return _connectedAddress; }

    /**
     * Get uptime of current connection in milliseconds
     */
    unsigned long getConnectionUptime() const;

    /**
     * Get count of packets received since connection
     */
    uint32_t getReceivedPacketCount() const { return _totalReceivedPackets; }

    /**
     * Check connection health and detect silent disconnects.
     * Call this periodically from loop().
     * @return true if connection is healthy
     */
    bool checkConnectionHealth();

    // ========================================================================
    // CALLBACKS
    // ========================================================================

    /**
     * Set callback for unit state changes
     */
    void setUnitStateCallback(UnitStateCallback cb) { _unitStateCallback = cb; }

    /**
     * Set callback for connection state changes
     */
    void setConnectionStateCallback(ConnectionStateCallback cb) { _connStateCallback = cb; }

    // ========================================================================
    // CONTROL FUNCTIONS
    // ========================================================================

    void setSceneLevel(uint8_t sceneId, uint8_t level);
    void setUnitLevel(uint8_t unitId, uint8_t level);
    void setGroupLevel(uint8_t groupId, uint8_t level);
    void setUnitVertical(uint8_t unitId, uint8_t vertical);
    void setGroupVertical(uint8_t groupId, uint8_t vertical);
    void setUnitTemperature(uint8_t unitId, uint16_t kelvin);
    void setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b);
    void setUnitSlider(uint8_t unitId, uint8_t value);
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

    // Connection tracking
    String _connectedAddress;
    unsigned long _connectTime;          // millis() when connected
    unsigned long _lastNotificationTime; // millis() when last notification received
    uint32_t _totalReceivedPackets;
    DisconnectReason _lastDisconnectReason;

    // Thread safety for concurrent command line + web server access
    SemaphoreHandle_t _mutex;

    // Callbacks
    UnitStateCallback _unitStateCallback;
    ConnectionStateCallback _connStateCallback;

    // ========================================================================
    // CONNECTION FLOW
    // ========================================================================

    bool _readDeviceInfo();
    bool _performKeyExchange();
    bool _authenticate();

    /**
     * Internal disconnect with reason tracking
     */
    void _disconnectInternal(DisconnectReason reason);

    /**
     * Update state and fire callback
     */
    void _setState(ConnectionState newState, DisconnectReason reason = DisconnectReason::None);

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

    /**
     * Apply parsed unit states to NetworkConfig and fire callbacks
     */
    void _applyUnitStates(const std::vector<struct UnitStateInfo>& states);
};

#endif // CASAMBI_CLIENT_H
