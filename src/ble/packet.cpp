/**
 * Packet utilities and data packet parsing.
 *
 * The parsing logic itself lives in packet_parse.h (pure, host-testable, and
 * the record-format documentation lives there too). This file provides the
 * firmware-facing wrappers: debug/trace output and the malformed counters.
 * Parsing is strict all-or-nothing — see packet_parse.h.
 */

#include "packet.h"
#include "../config.h"

static PacketParseStats g_parseStats;

const PacketParseStats& packetParseStats() {
    return g_parseStats;
}

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
    // maxBytes == 0 means "no limit": dump the complete packet. Raw BLE packet
    // dumps use this so a status/network payload is never silently cut off at a
    // fixed length (only explicit previews pass a positive cap).
    size_t limit = (maxBytes == 0) ? len : maxBytes;
    Serial.printf("%s (%d bytes): ", label, len);
    size_t printLen = (len < limit) ? len : limit;
    for (size_t i = 0; i < printLen; i++) {
        Serial.printf("%02x ", data[i]);
    }
    if (len > limit) {
        Serial.print("...");
    }
    Serial.println();
}

// ============================================================================
// 0x06 - Status Broadcast Parsing
// ============================================================================

bool parseStatusBroadcast(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states) {
    if (bleDebugEnabled) {
        hexDump("PARSE 0x06 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseStatusBroadcast(data, len, states, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed06++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x06: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial06++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x06: partial — %u record(s) applied, tail dropped at offset %u: %s\n",
                          (unsigned)states.size(), (unsigned)diag.offset, diag.reason);
        }
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

    if (parseDebugEnabled) {
        Serial.printf("P06 raw (%d):", len);
        for (size_t i = 0; i < len; i++) Serial.printf(" %02x", data[i]);
        Serial.println();
        if (!states.empty()) {
            Serial.print("P06:");
            for (const auto& s : states) {
                Serial.printf(" U%d=%d", s.unitId, s.level);
                if (!s.online) Serial.print("(offline)");
                if (s.hasVertical)  Serial.printf(" v=%d", s.vertical);
                if (s.hasColorTemp) Serial.printf(" t=%d", s.colorTemp);
            }
            Serial.println();
        }
    }

    return true;
}

// ============================================================================
// 0x07 - Operation Echo Parsing
// ============================================================================

bool parseOperationEcho(const uint8_t* data, size_t len, OperationEcho& echo) {
    if (bleDebugEnabled) {
        hexDump("PARSE 0x07 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseOperationEcho(data, len, echo, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed07++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x07: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial07++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x07: partial — %s at offset %u\n",
                          diag.reason, (unsigned)diag.offset);
        }
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
        Serial.printf("P07 raw (%d):", len);
        for (size_t i = 0; i < len; i++) Serial.printf(" %02x", data[i]);
        Serial.println();
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
    if (bleDebugEnabled) {
        hexDump("PARSE 0x08 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseUnitStateUpdate(data, len, states, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed08++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x08: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial08++;
        if (bleDebugEnabled) {
            Serial.printf("PARSE 0x08: partial — %u state(s) applied, tail dropped at offset %u: %s\n",
                          (unsigned)states.size(), (unsigned)diag.offset, diag.reason);
        }
    }

    if (bleDebugEnabled && !states.empty()) {
        Serial.printf("PARSE 0x08: Parsed %d unit states\n", states.size());
        for (const auto& s : states) {
            Serial.printf("  Unit %d: level=%d on=%d\n", s.unitId, s.level, s.on);
        }
    }

    if (parseDebugEnabled) {
        Serial.printf("P08 raw (%d):", len);
        for (size_t i = 0; i < len; i++) Serial.printf(" %02x", data[i]);
        Serial.println();
        if (!states.empty()) {
            Serial.print("P08:");
            for (const auto& s : states) {
                Serial.printf(" U%d=%d", s.unitId, s.level);
                if (!s.online) Serial.print("(offline)");
                if (s.hasVertical)  Serial.printf(" v=%d", s.vertical);
                if (s.hasColorTemp) Serial.printf(" t=%d", s.colorTemp);
            }
            Serial.println();
        }
    }

    return true;
}
