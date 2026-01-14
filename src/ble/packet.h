/**
 * Casambi Packet Operations
 *
 * Operation codes and packet formatting
 */

#ifndef PACKET_H
#define PACKET_H

#include <Arduino.h>

// ============================================================================
// OPERATION CODES
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
// TARGET TYPE FLAGS
// ============================================================================

#define TARGET_TYPE_UNIT    0x01
#define TARGET_TYPE_GROUP   0x02
#define TARGET_TYPE_SCENE   0x04

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
 * RGB to Hue/Saturation conversion
 * @param r Red 0-255
 * @param g Green 0-255
 * @param b Blue 0-255
 * @param hue Output: Hue 0-1023
 * @param sat Output: Saturation 0-255
 */
void rgbToHS(uint8_t r, uint8_t g, uint8_t b, uint16_t& hue, uint8_t& sat);

#endif // PACKET_H
