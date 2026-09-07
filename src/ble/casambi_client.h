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
#include "packet_parse.h"      // InvocationFrame
#include "invocation_events.h" // CasambiInputEvent, EventDedupTable

// NimBLE types are an implementation detail of src/ble/ — forward-declare here
// and include <NimBLEDevice.h> only in the .cpp so the BLE stack does not leak
// into the rest of the firmware (see docs/concept-ble-nimble-migration.md, 5.1).
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

// Human-readable reason for traces and the event log ("link-loss", "auth-failed",
// …). Static string, never nullptr — a bare enum number in a serial log is
// unusable for anyone reporting a problem.
const char* disconnectReasonName(DisconnectReason reason);

// ============================================================================
// CALLBACK TYPES
// ============================================================================

// Called when a unit state changes (from incoming data packets). `on`/
// `online` are the device's own flags bits, passed through verbatim (see
// packet_parse.h's UnitStateRecord) — not a level-derived re-interpretation.
using UnitStateCallback = std::function<void(uint8_t unitId, uint8_t level, bool online, bool on)>;

// Called when connection state changes
using ConnectionStateCallback = std::function<void(ConnectionState newState, DisconnectReason reason)>;

// Called for each classified button/NotifyInput event decoded from an
// incoming 0x07 INVOCATION frame stream, after dedup — see
// invocation_events.h. UNVERIFIED (see packet_parse.h's parseInvocationStream
// doc comment): 0x07 has never been observed on the reference network.
using InputEventCallback = std::function<void(const invocation_events::CasambiInputEvent&)>;

// Called for EVERY frame in an incoming 0x07 stream, classified or not —
// full-fidelity access for diagnostics/analysis of unrecognized opcodes,
// with no dedup applied.
using RawInvocationCallback = std::function<void(const InvocationFrame&)>;

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
     * How far the LAST connection attempt got before it failed (or "ready" if
     * it succeeded). One of: "idle", "link", "service", "characteristic",
     * "keypair", "devinfo", "keyexchange", "auth", "ready". A plain
     * DisconnectReason cannot tell "peer never answered the advertisement"
     * from "GATT was fine but auth was rejected" — this can.
     * Static string, never nullptr.
     */
    const char* getLastConnectPhase() const { return _lastConnectPhase.load(); }

    /**
     * NimBLE return code of the last failed link-up (0 = none/success). Carries
     * the real reason a connect() attempt failed (timeout, no such peer,
     * controller busy …) which is otherwise swallowed by the bool return.
     */
    int getLastConnectError() const { return _lastConnectRc.load(); }

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

    /**
     * Set callback for classified, deduplicated button/NotifyInput events
     * decoded from incoming 0x07 INVOCATION frames. UNVERIFIED, see
     * invocation_events.h.
     */
    void setInputEventCallback(InputEventCallback cb) { _inputEventCallback = cb; }

    /**
     * Set callback for every raw INVOCATION frame from an incoming 0x07
     * stream, classified or not, no dedup applied.
     */
    void setRawInvocationCallback(RawInvocationCallback cb) { _rawInvocationCallback = cb; }

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
    bool setGroupLevel(uint8_t groupId, uint8_t level);
    // Full-state write (OpCode::SetState): `state` carries the unit's COMPLETE
    // state blob (`len` = fixture stateLength, one raw value per control at
    // its fixture bit position — see state_codec.h). This is the only way to
    // set several controls atomically (e.g. both dimmers of a dual-dimmer
    // fixture). Callers MUST encode unchanged controls at their current
    // values: a zeroed byte resets that control on the fixture.
    bool setUnitState(uint8_t unitId, const uint8_t* state, uint8_t len);
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

    // Connection tracking. Everything below except _connectedAddress and
    // _connectTime is written on one task and read on another (NimBLE host
    // task writes the notification counter/timestamp, the loop task polls
    // them in wait loops, the async_tcp task reads the diagnostics for
    // /api/status) — atomics make those hand-offs well-defined instead of
    // relying on delay() forcing a reload.
    String _connectedAddress;            // guarded by g_configMutex
    unsigned long _connectTime;          // millis() when connected (loop task only)
    std::atomic<unsigned long> _lastNotificationTime; // millis() at last notification
    std::atomic<uint32_t> _totalReceivedPackets;
    std::atomic<DisconnectReason> _lastDisconnectReason;
    std::atomic<const char*> _lastDisconnectSource;  // detector of the last disconnect (static string)
    std::atomic<const char*> _lastConnectPhase;      // phase the last connect attempt reached
    std::atomic<int> _lastConnectRc;     // NimBLE rc of the last failed link-up (0 = none)
    std::atomic<int> _lastRssi;          // last known link RSSI in dBm (0 = unknown)

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
    InputEventCallback _inputEventCallback;
    RawInvocationCallback _rawInvocationCallback;
    // Dedup state for decodeInvocationEvents() — persists across 0x07 packets.
    invocation_events::EventDedupTable _invocationDedupTable;

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
    // CONNECT DIAGNOSTICS
    // ========================================================================

    /**
     * Record the connect phase we are entering (see getLastConnectPhase()) and
     * trace it when BLE debug is on.
     */
    void _setPhase(const char* phase);

    /**
     * Dump the GATT topology actually discovered on the peer (services, their
     * characteristics and properties) to Serial. Called when the expected
     * Casambi service/characteristic is missing — the enumeration says whether
     * we reached the wrong device or a device with an unexpected profile.
     */
    void _dumpGattTopology();

    /**
     * After a failed link-up: scan briefly and report whether the peer is
     * advertising at all, with which address type and RSSI. Separates "light is
     * off / out of range" from "advertises, but under a different address type
     * than the one we connect with". Debug-gated (costs a blocking scan).
     */
    void _probePeer(const String& address);

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
     * Append an operation block to the packet under construction.
     * Advances `outLen` and, on success, the _origin counter. Returns false if
     * the block would not fit, leaving `out`/`outLen`/_origin unchanged.
     */
    bool _buildOperation(uint8_t opcode, uint16_t target,
                         const std::vector<uint8_t>& payload,
                         uint8_t* out, size_t outCap, size_t& outLen);

    /**
     * Send encrypted packet
     */
    // Returns true only if writeValue() handed the packet to the GATT stack.
    // Returns false on mutex timeout, missing encryption/characteristic, a
    // failed encryption or a failed GATT write.
    bool _sendEncryptedPacket(const uint8_t* packet, size_t pktLen, uint32_t counter);

    /**
     * Fill the caller's buffer with the nonce for the given packet counter.
     */
    void _getNonce(uint32_t counter, uint8_t nonce[NONCE_SIZE]);

    // ========================================================================
    // BLE CALLBACKS
    // ========================================================================

    static void _notifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);
    void _handleNotification(uint8_t* data, size_t len);
    void _handleKeyExchangeNotification(uint8_t* data, size_t len);
    void _handleAuthNotification(uint8_t* data, size_t len);
    void _handleDataNotification(uint8_t* data, size_t len);

    /**
     * Apply parsed unit state records to NetworkConfig and fire callbacks
     */
    void _applyUnitStates(const std::vector<struct UnitStateRecord>& records);

    /**
     * Classify/dedup an incoming 0x07 INVOCATION frame stream and fire the
     * raw + input-event callbacks. Never touches NetworkConfig/unit state —
     * see the parseInvocationStream doc comment in packet_parse.h for why.
     */
    void _applyInvocationFrames(const std::vector<InvocationFrame>& frames);
};

#endif // CASAMBI_CLIENT_H
