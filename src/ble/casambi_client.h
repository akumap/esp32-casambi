/**
 * Casambi BLE Client
 *
 * Handles BLE connection, ECDH key exchange, authentication, and packet communication.
 * Includes auto-reconnect, connection monitoring, and data packet parsing.
 */

#ifndef CASAMBI_CLIENT_H
#define CASAMBI_CLIENT_H

#include <Arduino.h>
#include <vector>
#include <functional>
#include <atomic>
#include "../config.h"
#include "../cloud/network_config.h"
#include "../crypto/encryption.h"
#include "../crypto/key_exchange.h"

// NimBLE types are an implementation detail of src/ble/ — forward-declare here
// and include <NimBLEDevice.h> only in the .cpp so the BLE stack does not leak
// into the rest of the firmware (see docs/konzept-ble-nimble-migration.md, 5.1).
class NimBLEClient;
class NimBLERemoteCharacteristic;

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
     * Which detector triggered the last disconnect ("silent" = health check,
     * "keepalive" = GATT read got no response, "send" = link dead on write,
     * "connect" = failure during connection setup, "user" = requested).
     * Static string, never nullptr.
     */
    const char* getLastDisconnectSource() const { return _lastDisconnectSource; }

    /**
     * Last known RSSI of the gateway link in dBm (0 = not measured yet).
     * Updated on connect and by every health check (~10 s); cached so it can
     * be read from any task without a BLE stack call.
     */
    int getLastRssi() const { return _lastRssi; }

    /**
     * Get the address we're connected (or were last connected) to.
     * Copies under g_configMutex — safe to call from the async_tcp task while
     * the loop task is reconnecting (see the definition for the race note).
     */
    String getConnectedAddress() const;

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

    // Control setters. Return true only when the command was successfully
    // handed to the BLE GATT stack. A false return means the command was NOT
    // transmitted (not authenticated, link lost, mutex timeout, encryption
    // failure or a failed GATT write); callers must surface this rather than
    // reporting success. Packet/origin counters are only advanced on success.
    bool setSceneLevel(uint8_t sceneId, uint8_t level);
    bool setUnitLevel(uint8_t unitId, uint8_t level);
    bool setUnitChannels(uint8_t unitId, uint8_t up, uint8_t down); // dual-dimmer fixtures (e.g. Oligo Grace Uplight/Downlight)
    bool setGroupLevel(uint8_t groupId, uint8_t level);
    bool setUnitVertical(uint8_t unitId, uint8_t vertical);
    bool setGroupVertical(uint8_t groupId, uint8_t vertical);
    bool setUnitTemperature(uint8_t unitId, uint16_t kelvin);
    bool setUnitColor(uint8_t unitId, uint8_t r, uint8_t g, uint8_t b);
    bool setUnitSlider(uint8_t unitId, uint8_t value);
    bool setGroupSlider(uint8_t groupId, uint8_t value);

private:
    NetworkConfig* _config;
    NimBLEClient* _bleClient;
    NimBLERemoteCharacteristic* _authChar;

    // Written by the NimBLE host task (notification handlers) and polled by the
    // loop task (connect wait loops) — atomic so the cross-task hand-off is
    // well-defined instead of relying on delay() forcing a reload.
    std::atomic<ConnectionState> _state;
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
    const char* _lastDisconnectSource;   // detector of the last disconnect (static string)
    int _lastRssi;                       // last known link RSSI in dBm (0 = unknown)

    // Thread safety for concurrent command line + web server access
    SemaphoreHandle_t _mutex;

    // Protects _encryption lifetime across main-loop and BLE-stack tasks.
    // Lock order: always _mutex before _encMutex (never the other way around).
    SemaphoreHandle_t _encMutex;

    // Cached transport key — set by _performKeyExchange, reused by _authenticate
    // so deriveTransportKey() is only called once per connection attempt.
    std::vector<uint8_t> _transportKey;

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
     * Connect flow body. Assumes _mutex is held (public connect() takes it so
     * the reconnect path cannot free _bleClient/_authChar under a control
     * command that is mid-write).
     */
    bool _connectLocked(const String& address);

    /**
     * Internal disconnect with reason tracking. Takes _mutex; if the mutex is
     * busy the disconnect is skipped (the next health check retries) rather
     * than racing a concurrent sender. `source` names the detector for
     * diagnostics (static string, e.g. "keepalive"); see getLastDisconnectSource().
     */
    void _disconnectInternal(DisconnectReason reason, const char* source = nullptr);

    /**
     * Disconnect body. Assumes _mutex is held.
     */
    void _disconnectLocked(DisconnectReason reason, const char* source = nullptr);

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
    // Returns true only if the packet was accepted by the GATT stack. On any
    // failure the packet and origin counters are left unchanged (rolled back)
    // so the nonce sequence does not drift on a dropped send.
    bool _sendOperation(uint8_t opcode, uint16_t target, const std::vector<uint8_t>& payload);

    /**
     * Build operation packet
     */
    std::vector<uint8_t> _buildOperation(uint8_t opcode, uint16_t target,
                                         const std::vector<uint8_t>& payload);

    /**
     * Send encrypted packet
     */
    // Returns true only if writeValue() handed the packet to the GATT stack.
    // Returns false on mutex timeout, missing encryption/characteristic, empty
    // ciphertext or a failed GATT write.
    bool _sendEncryptedPacket(const std::vector<uint8_t>& packet, uint32_t counter);

    /**
     * Get nonce for packet encryption
     */
    std::vector<uint8_t> _getNonce(uint32_t counter);

    // ========================================================================
    // BLE CALLBACKS
    // ========================================================================

    static void _notifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);
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
