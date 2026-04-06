/**
 * Packet utilities and data packet parsing
 *
 * 0x06 Unit State Change Event (reverse-engineered):
 *
 * Sent whenever one or more units change state (via app, power, scene, etc.).
 * Contains one record per changed unit, concatenated.
 * Each record:
 *
 *   Byte 0: Unit-ID
 *   Byte 1: Flags
 *            Bit 4:   stored_level byte present (+1 byte)
 *            Bit 0-3: Change source
 *                     0x0 = physical (power cycle, offline)
 *                     0x3 = software (app, ESP32) — devices with aux
 *                     0x7 = software (simple devices / type 1422)
 *   Byte 2: Capability descriptor
 *            Upper nibble = number of aux channels (0,1,2)
 *            Lower nibble: 0x00 or 0x03
 *            0x00 = simple device, 0 aux, NO constant byte
 *            0x03 = simple device, 0 aux, HAS constant byte (0x80)
 *            0x13 = 1 aux (brightness + temp OR vertical)
 *            0x23 = 2 aux (brightness + vertical + temp)
 *   [0x80]           — only if cap == 0x03 exactly
 *   [stored_level]   — only if flags bit 4 set (previous brightness)
 *   Brightness       — current output level 0-255
 *   [Aux1]           — if aux_count >= 1 (vertical or temp, device-dependent)
 *   [Aux2]           — if aux_count >= 2 (temp)
 *
 * Record length = 3 + has_const + has_prev + 1 + aux_count
 *   where has_const = (cap == 0x03) ? 1 : 0
 *         has_prev  = (flags & 0x10) ? 1 : 0
 *         aux_count = cap >> 4
 */

#include "packet.h"
#include "../config.h"

void rgbToHS(uint8_t r, uint8_t g, uint8_t b, uint16_t& hue, uint8_t& sat) {
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float max = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float min = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float delta = max - min;

    float s = (max == 0.0f) ? 0.0f : (delta / max);

    float h = 0.0f;
    if (delta != 0.0f) {
        if (max == rf) {
            h = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (max == gf) {
            h = (bf - rf) / delta + 2.0f;
        } else {
            h = (rf - gf) / delta + 4.0f;
        }
        h /= 6.0f;
    }

    hue = static_cast<uint16_t>(h * 1023.0f);
    sat = static_cast<uint8_t>(s * 255.0f);
}

const char* targetTypeName(uint8_t type) {
    switch (type) {
        case TARGET_TYPE_UNIT:  return "Unit";
        case TARGET_TYPE_GROUP: return "Group";
        case TARGET_TYPE_SCENE: return "Scene";
        default: return "Unknown";
    }
}

const char* opcodeName(uint8_t opcode) {
    switch (opcode) {
        case 0:  return "Response";
        case 1:  return "SetLevel";
        case 3:  return "SetTemperature";
        case 4:  return "SetVertical";
        case 5:  return "SetWhite";
        case 7:  return "SetColor";
        case 12: return "SetSlider";
        case 48: return "SetState";
        case 54: return "SetColorXY";
        default: return "Unknown";
    }
}

void hexDump(const char* label, const uint8_t* data, size_t len, size_t maxBytes) {
    Serial.printf("%s (%d bytes): ", label, len);
    size_t printLen = (len < maxBytes) ? len : maxBytes;
    for (size_t i = 0; i < printLen; i++) {
        Serial.printf("%02x ", data[i]);
    }
    if (len > maxBytes) {
        Serial.print("...");
    }
    Serial.println();
}

// ============================================================================
// 0x06 - Status Broadcast Parsing
// ============================================================================

/**
 * Check if a capability byte is valid.
 * Known values: 0x00, 0x03, 0x13, 0x23
 * Lower nibble must be 0x00 or 0x03, upper nibble 0-2.
 */
static bool isValidCap(uint8_t cap) {
    uint8_t low = cap & 0x0F;
    uint8_t auxCount = (cap >> 4) & 0x0F;
    return (low == 0x00 || low == 0x03) && (auxCount <= 2);
}

/**
 * Calculate the expected record length for one unit in a 0x06 packet.
 */
static size_t calcRecordLength(uint8_t flags, uint8_t cap) {
    uint8_t auxCount = (cap >> 4) & 0x0F;
    uint8_t hasConst = (cap == 0x03) ? 1 : 0;   // only exact 0x03 has the 0x80 byte
    uint8_t hasPrev  = (flags & 0x10) ? 1 : 0;

    return 3 + hasConst + hasPrev + 1 + auxCount;
}

bool parseStatusBroadcast(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states) {
    states.clear();

    if (len < 4) {
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x06: Packet too short (%d bytes)\n", len);
        }
        return false;
    }

    if (bleDebugEnabled) {
        hexDump("PARSE 0x06 raw", data, len);
    }

    size_t offset = 0;

    while (offset + 3 <= len) {
        uint8_t unitId = data[offset];
        uint8_t flags  = data[offset + 1];
        uint8_t cap    = data[offset + 2];

        if (!isValidCap(cap)) {
            if (bleDebugEnabled) {
                Serial.printf("PARSE 0x06: Unknown cap=0x%02x at offset %d, stopping\n",
                              cap, offset);
            }
            break;
        }

        size_t recordLen = calcRecordLength(flags, cap);

        if (offset + recordLen > len) {
            if (bleDebugEnabled) {
                Serial.printf("PARSE 0x06: Truncated record at offset %d (need %d, have %d)\n",
                              offset, recordLen, len - offset);
            }
            break;
        }

        uint8_t auxCount = (cap >> 4) & 0x0F;
        bool hasConst = (cap == 0x03);
        bool hasPrev  = (flags & 0x10) != 0;

        UnitStateInfo info;
        info.unitId = unitId;
        info.online = true;

        // Change source from lower nibble
        uint8_t source = flags & 0x0F;
        if (source == 0x00) {
            info.online = false;
        }

        size_t pos = offset + 3;

        // Skip constant byte (0x80) — only for cap == 0x03
        if (hasConst) {
            pos++;
        }

        // Skip stored_level (previous brightness)
        if (hasPrev) {
            pos++;
        }

        // Current brightness
        info.level = data[pos];
        info.on = (info.level > 0);
        info.hasLevel = true;
        pos++;

        // Aux channels
        if (auxCount >= 1) {
            info.vertical = data[pos];
            info.hasVertical = true;
            pos++;
        }

        if (auxCount >= 2) {
            info.colorTemp = data[pos];
            info.hasColorTemp = true;
            pos++;
        }

        states.push_back(info);
        offset += recordLen;
    }

    if (bleDebugEnabled && !states.empty()) {
        Serial.printf("PARSE 0x06: Parsed %d unit state(s)\n", states.size());
        for (const auto& s : states) {
            Serial.printf("  Unit %d: level=%d online=%d on=%d",
                          s.unitId, s.level, s.online, s.on);
            if (s.hasVertical) Serial.printf(" aux1=%d", s.vertical);
            if (s.hasColorTemp) Serial.printf(" aux2=%d", s.colorTemp);
            Serial.println();
        }
    }

    if (parseDebugEnabled && !states.empty()) {
        Serial.print("P06:");
        for (const auto& s : states) {
            Serial.printf(" U%d=%d", s.unitId, s.level);
            if (!s.online) Serial.print("(offline)");
            if (s.hasVertical)  Serial.printf(" v=%d", s.vertical);
            if (s.hasColorTemp) Serial.printf(" t=%d", s.colorTemp);
        }
        Serial.println();
    }

    return !states.empty();
}

// ============================================================================
// 0x07 - Operation Echo Parsing
// ============================================================================

bool parseOperationEcho(const uint8_t* data, size_t len, OperationEcho& echo) {
    if (len < 9) {
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x07: Too short (%d bytes, need >= 9)\n", len);
        }
        return false;
    }

    if (bleDebugEnabled) {
        hexDump("PARSE 0x07 raw", data, len);
    }

    uint16_t flags = (data[0] << 8) | data[1];
    uint8_t payloadLen = flags & 0x07FF;

    echo.opcode = data[2];

    echo.target = (data[5] << 8) | data[6];
    echo.targetId = (echo.target >> 8) & 0xFF;
    echo.targetType = echo.target & 0xFF;

    echo.payload.clear();
    size_t actualPayloadLen = len - 9;
    if (actualPayloadLen > 0) {
        size_t copyLen = (payloadLen < actualPayloadLen) ? payloadLen : actualPayloadLen;
        echo.payload.assign(data + 9, data + 9 + copyLen);
    }

    if (bleDebugEnabled) {
        Serial.printf("PARSE 0x07: op=%s(%d) target=%s[%d] payload=%d bytes\n",
                      opcodeName(echo.opcode), echo.opcode,
                      targetTypeName(echo.targetType), echo.targetId,
                      echo.payload.size());
        if (!echo.payload.empty()) {
            hexDump("  payload", echo.payload.data(), echo.payload.size(), 16);
        }
    }

    if (parseDebugEnabled) {
        Serial.printf("P07: %s %s[%d]",
                      opcodeName(echo.opcode),
                      targetTypeName(echo.targetType),
                      echo.targetId);
        for (size_t i = 0; i < echo.payload.size(); i++) {
            Serial.printf(" %02x", echo.payload[i]);
        }
        Serial.println();
    }

    return true;
}

// ============================================================================
// 0x08 - Unit State Update Parsing
// ============================================================================

bool parseUnitStateUpdate(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states) {
    states.clear();

    if (len < 2) {
        if (bleDebugEnabled) {
            Serial.println("PARSE 0x08: Packet too short");
        }
        return false;
    }

    if (bleDebugEnabled) {
        hexDump("PARSE 0x08 raw", data, len);
    }

    // Try parsing as 0x06 format — check if byte 2 looks like a valid cap
    if (len >= 4 && isValidCap(data[2])) {
        return parseStatusBroadcast(data, len, states);
    }

    // Fallback: raw [unitId][level] pairs
    size_t offset = 0;
    while (offset + 1 < len) {
        UnitStateInfo info;
        info.unitId = data[offset];
        info.level = data[offset + 1];
        info.on = (info.level > 0);
        info.online = true;
        info.hasLevel = true;
        offset += 2;
        states.push_back(info);
    }

    if (bleDebugEnabled && !states.empty()) {
        Serial.printf("PARSE 0x08: Parsed %d unit states (fallback)\n", states.size());
        for (const auto& s : states) {
            Serial.printf("  Unit %d: level=%d on=%d\n", s.unitId, s.level, s.on);
        }
    }

    if (parseDebugEnabled) {
        if (!states.empty()) {
            Serial.print("P08:");
            for (const auto& s : states) {
                Serial.printf(" U%d=%d", s.unitId, s.level);
                if (!s.online) Serial.print("(offline)");
                if (s.hasVertical)  Serial.printf(" v=%d", s.vertical);
                if (s.hasColorTemp) Serial.printf(" t=%d", s.colorTemp);
            }
            Serial.println();
        } else {
            // Unbekanntes Format — Rohbytes ausgeben
            Serial.printf("P08-raw (%d):", len);
            for (size_t i = 0; i < len; i++) Serial.printf(" %02x", data[i]);
            Serial.println();
        }
    }

    return !states.empty();
}
