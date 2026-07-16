/**
 * Casambi Packet Operations
 *
 * Operation codes, data packet types, and packet formatting
 */

#ifndef PACKET_H
#define PACKET_H

#include <Arduino.h>
#include <vector>
#include <atomic>
#include "packet_parse.h"   // UnitStateInfo, OperationEcho, pure parser core

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
    StatusBroadcast = 0x06,  // Unit state change event, one record per changed unit (data context)
    OperationEcho   = 0x07,  // Echo of operations from other controllers
    UnitState       = 0x08,  // Individual unit state update (not yet observed in practice)
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
// UnitStateInfo and OperationEcho live in packet_parse.h (pure header, shared
// with the host-side parser tests) and are re-exported via the include above.

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

// Firmware-facing parser wrappers around the strict pure core in
// packet_parse.h. Each returns true only when the ENTIRE payload parsed
// cleanly (ParseStatus::Complete); on any structural defect the output is
// empty, the matching malformed counter is bumped and — with BLE debug
// enabled — offset and reason are printed. Callers must not apply any state
// from a false return (there is none).

/**
 * Parse a 0x06 status broadcast packet (unit state change event).
 * @param data  Decrypted payload (starting AFTER the type byte)
 * @param len   Length of payload
 * @param states Output vector (empty unless true is returned)
 */
bool parseStatusBroadcast(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states);

/**
 * Parse a 0x07 operation echo packet (operations from other controllers).
 */
bool parseOperationEcho(const uint8_t* data, size_t len, OperationEcho& echo);

/**
 * Parse a 0x08 unit state update packet.
 */
bool parseUnitStateUpdate(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states);

/**
 * Counters of packets rejected as malformed, per packet type. Written on the
 * NimBLE host task, read for diagnostics (serial `status`) — atomics so the
 * cross-task reads are well-defined.
 */
struct PacketParseStats {
    std::atomic<uint32_t> malformed06{0};
    std::atomic<uint32_t> malformed07{0};
    std::atomic<uint32_t> malformed08{0};
};
const PacketParseStats& packetParseStats();

/**
 * Hex dump helper for debug output
 */
void hexDump(const char* label, const uint8_t* data, size_t len, size_t maxBytes = 32);

#endif // PACKET_H
