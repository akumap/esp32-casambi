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
#include "../console_out.h"

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
    Console.printf("%s (%d bytes): ", label, len);
    size_t printLen = (len < limit) ? len : limit;
    for (size_t i = 0; i < printLen; i++) {
        Console.printf("%02x ", data[i]);
    }
    if (len > limit) {
        Console.print("...");
    }
    Console.println();
}

// ============================================================================
// 0x06 - Status Broadcast Parsing
// ============================================================================

bool parseStatusBroadcast(const uint8_t* data, size_t len, std::vector<UnitStateRecord>& records) {
    // All PARSE output belongs to `debug parse` (not `debug ble`): raw hex,
    // parse diagnostics, and the positional per-record view. The named,
    // cloud-derived per-unit line ("Casambi: Unit ...") is emitted separately by
    // _applyUnitStates and gated by `debug ble`/`debug casambi`.
    if (parseDebugEnabled) {
        hexDump("PARSE 0x06 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseStatusBroadcast(data, len, records, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed06++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x06: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial06++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x06: partial — %u record(s) applied, tail dropped at offset %u: %s\n",
                          (unsigned)records.size(), (unsigned)diag.offset, diag.reason);
        }
    }

    if (parseDebugEnabled && !records.empty()) {
        Console.printf("PARSE 0x06: %d record(s)\n", records.size());
        for (const auto& r : records) {
            Console.printf("  Unit %d: on=%d online=%d priority=%d state[0..%d]=",
                          r.unitId, r.on, r.online, r.priority, r.stateLen - 1);
            for (uint8_t i = 0; i < r.stateLen; i++) Console.printf("%d ", r.state[i]);
            Console.println();
        }
    }

    return true;
}

// ============================================================================
// 0x07 - INVOCATION Frame Stream Parsing
// ============================================================================

bool parseInvocationStream(const uint8_t* data, size_t len, std::vector<InvocationFrame>& frames) {
    // PARSE output → `debug parse`.
    if (parseDebugEnabled) {
        hexDump("PARSE 0x07 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseInvocationStream(data, len, frames, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed07++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x07: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial07++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x07: partial — %u frame(s) applied, tail dropped at offset %u: %s\n",
                          (unsigned)frames.size(), (unsigned)diag.offset, diag.reason);
        }
    }

    if (parseDebugEnabled && !frames.empty()) {
        Console.printf("PARSE 0x07: %d frame(s)\n", frames.size());
        for (const auto& f : frames) {
            Console.printf("  op=%s(%d) target=%s[%d] origin=%d age=%d payload=%d bytes",
                          opcodeName(f.opcode), f.opcode,
                          targetTypeName(f.targetType()), f.targetId(),
                          f.origin, f.age, f.payloadLen);
            if (f.hasOriginHandle) {
                Console.printf(" originHandle=%d", f.originHandle);
            }
            Console.println();
            if (f.payloadLen > 0) {
                hexDump("    payload", f.payload, f.payloadLen, 16);
            }
        }
    }

    return true;
}

// ============================================================================
// 0x08 - Unit State Update Parsing
// ============================================================================

bool parseUnitStateUpdate(const uint8_t* data, size_t len, std::vector<UnitStateInfo>& states) {
    // PARSE output → `debug parse`.
    if (parseDebugEnabled) {
        hexDump("PARSE 0x08 raw", data, len);
    }

    packetparse::ParseDiag diag;
    packetparse::ParseStatus st = packetparse::parseUnitStateUpdate(data, len, states, &diag);
    if (st == packetparse::ParseStatus::Malformed) {
        g_parseStats.malformed08++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x08: malformed at offset %u: %s (packet dropped)\n",
                          (unsigned)diag.offset, diag.reason);
        }
        return false;
    }
    if (st == packetparse::ParseStatus::Partial) {
        g_parseStats.partial08++;
        if (parseDebugEnabled) {
            Console.printf("PARSE 0x08: partial — %u state(s) applied, tail dropped at offset %u: %s\n",
                          (unsigned)states.size(), (unsigned)diag.offset, diag.reason);
        }
    }

    if (parseDebugEnabled && !states.empty()) {
        Console.printf("PARSE 0x08: %d record(s)\n", states.size());
        for (const auto& s : states) {
            Console.printf("  Unit %d: on=%d state[0]=%d", s.unitId, s.on, s.level);
            if (s.hasVertical)  Console.printf(" state[1]=%d", s.vertical);
            if (s.hasColorTemp) Console.printf(" state[2]=%d", s.colorTemp);
            Console.println();
        }
    }

    return true;
}
