/**
 * Casambi Packet Operations
 *
 * Operation codes, data packet types, and packet formatting
 */

#ifndef PACKET_H
#define PACKET_H

#include <Arduino.h>
#include <vector>

// ============================================================================
// OPERATION CODES (outgoing)
// ============================================================================

enum class OpCode : uint8_t {
    Response = 0,
    SetLevel = 1,
    SetTemperature = 3,
    SetVertical = 4,
    SetWhite = 5,
    SetColor = 7,
    SetSlider = 12,
    SetState = 48,
    SetColorXY = 54
};

// ============================================================================
// DATA PACKET TYPES (incoming, after decryption)
// ============================================================================

enum class DataPacketType : uint8_t {
    AuthSuccess     = 0x05,  // Authentication accepted
    AuthReject      = 0x06,  // Authentication rejected (auth context only)
    StatusBroadcast = 0x06,  // Unit/scene status broadcast (data context)
    OperationEcho   = 0x07,  // Echo of operations from other controllers
    UnitState       = 0x08,  // Individual unit state update
    NetworkState    = 0x09,  // Full network state snapshot
    TimeSync        = 0x0A,  // Time synchronization
    Keepalive       = 0x0C,  // Keepalive / heartbeat
};

// ============================================================================
// TARGET TYPE FLAGS
// ============================================================================

#define TARGET_TYPE_UNIT    0x01
#define TARGET_TYPE_GROUP   0x02
#define TARGET_TYPE_SCENE   0x04

// ============================================================================
// PARSED DATA STRUCTURES
// ============================================================================

/**
 * Parsed unit state from incoming packets
 */
struct UnitStateInfo {
    uint8_t unitId;
    uint8_t level;         // 0-255
    bool online;
    bool on;
    uint8_t vertical;      // 0-255 (light balance)
    uint16_t colorTemp;    // Kelvin
    uint8_t colorR, colorG, colorB;
    bool hasLevel;
    bool hasVertical;
    bool hasColorTemp;
    bool hasColor;

    UnitStateInfo() : unitId(0), level(0), online(false), on(false),
                      vertical(127), colorTemp(0),
                      colorR(0), colorG(0), colorB(0),
                      hasLevel(false), hasVertical(false),
                      hasColorTemp(false), hasColor(false) {}
};

/**
 * Parsed operation echo from incoming packets
 */
struct OperationEcho {
    uint8_t opcode;
    uint16_t target;
    uint8_t targetType;    // TARGET_TYPE_*
    uint8_t targetId;
    std::vector<uint8_t> payload;
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Encode target for operation packet
 * @param id Unit/Group/Scene ID
 * @param type TARGET_TYPE_*
 * @return Encoded target value
 */
inline uint16_t encodeTarget(uint8_t id, uint8_t type) {
    return (static_cast<uint16_t>(id) << 8) | type;
}

/**
 * Decode target from operation packet
 */
inline void decodeTarget(uint16_t target, uint8_t& id, uint8_t& type) {
    id = (target >> 8) & 0xFF;
    type = target & 0xFF;
}

/**
 * Get human-readable name for target type
 */
const char* targetTypeName(uint8_t type);

/**
 * Get human-readable name for opcode
 */
const char* opcodeName(uint8_t opcode);

/**
 * RGB to Hue/Saturation conversion
 * @param r Red 0-255
 * @param g Green 0-255
 * @param b Blue 0-255
 * @param hue Output: Hue 0-1023
 * @param sat Output: Saturation 0-255
 */
void rgbToHS(uint8_t r, uint8_t g, uint8_t b, uint16_t& hue, uint8_t& sat);

/**
 * Parse a 0x06 status broadcast packet.
 * These packets contain a variable-length list of unit states
 * describing the current state of units/scenes in the network.
 *
 * Known sub-structure (per unit, within the broadcast):
 *   [unitId:1] [flags:1] [level:1] [optional extended data...]
 *
 * @param data  Decrypted payload (starting AFTER the type byte)
 * @param len   Length of payload
 * @param states Output vector
 * @return true if at least one unit state was parsed
 */
bool parseStatusBroadcast(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states);

/**
 * Parse a 0x07 operation echo packet.
 * These are operations from other controllers (apps, other ESP32s)
 * echoed over the mesh network.
 *
 * Structure (after type byte):
 *   [flags:2 big-endian] [opcode:1] [origin:2 big-endian]
 *   [target:2 big-endian] [reserved:2] [payload...]
 */
bool parseOperationEcho(const uint8_t* data, size_t len, OperationEcho& echo);

/**
 * Parse a 0x08 unit state update packet.
 * Sent when individual unit states change.
 */
bool parseUnitStateUpdate(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states);

/**
 * Hex dump helper for debug output
 */
void hexDump(const char* label, const uint8_t* data, size_t len, size_t maxBytes = 32);

#endif // PACKET_H
