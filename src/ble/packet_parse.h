/**
 * Pure BLE data-packet parsing core.
 *
 * Deliberately free of Arduino dependencies (only the C++ standard library)
 * so the exact parsing logic the firmware runs can be exercised by host-side
 * unit and fuzz tests (`pio test -e native`, test/test_packet_parse).
 * The firmware-facing wrappers in packet.cpp add debug logging and the
 * malformed-packet counters around these functions.
 *
 * Parsing is STRICT and all-or-nothing: a parser returns Complete only when
 * the entire payload was consumed exactly and every record was structurally
 * valid. On any defect it returns Malformed with the offending offset and a
 * static reason string, and the output is left EMPTY — a corrupted packet can
 * therefore never apply a partial state update (drift after radio corruption
 * or protocol deviations). The protocol is reverse-engineered; if a legitimate
 * device variant trips a strict check, the wrapper's hexdump plus the diag
 * reason identify it so the rule can be adjusted deliberately.
 *
 * 0x06 status broadcast — one record per changed unit:
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
 *   [0x80]           — only if cap == 0x03 exactly (value verified)
 *   [stored_level]   — only if flags bit 4 set (previous brightness)
 *   Brightness       — current output level 0-255
 *   [Aux1]           — if aux_count >= 1 (vertical or temp, device-dependent)
 *   [Aux2]           — if aux_count >= 2 (temp)
 *
 * Record length = 3 + has_const + has_prev + 1 + aux_count
 */

#ifndef PACKET_PARSE_H
#define PACKET_PARSE_H

#include <cstddef>
#include <cstdint>
#include <vector>

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

namespace packetparse {

enum class ParseStatus : uint8_t {
    Complete,   // whole payload consumed, all records valid, output filled
    Malformed,  // structural defect — output cleared, see ParseDiag
};

// Where and why a parse failed (valid only when Malformed is returned).
// `reason` is always a static string, never nullptr.
struct ParseDiag {
    size_t offset;
    const char* reason;
    ParseDiag() : offset(0), reason("") {}
};

namespace detail {

// Known capability values: 0x00, 0x03, 0x13, 0x23.
// Lower nibble must be 0x00 or 0x03, upper nibble 0-2.
inline bool isValidCap(uint8_t cap) {
    uint8_t low = cap & 0x0F;
    uint8_t auxCount = (cap >> 4) & 0x0F;
    return (low == 0x00 || low == 0x03) && (auxCount <= 2);
}

// Expected record length for one unit in a 0x06 packet.
inline size_t recordLength(uint8_t flags, uint8_t cap) {
    size_t auxCount = (cap >> 4) & 0x0F;
    size_t hasConst = (cap == 0x03) ? 1 : 0;   // only exact 0x03 has the 0x80 byte
    size_t hasPrev  = (flags & 0x10) ? 1 : 0;
    return 3 + hasConst + hasPrev + 1 + auxCount;
}

inline ParseStatus fail(std::vector<UnitStateInfo>* states, ParseDiag* diag,
                        size_t offset, const char* reason) {
    if (states) states->clear();
    if (diag) { diag->offset = offset; diag->reason = reason; }
    return ParseStatus::Malformed;
}

}  // namespace detail

/**
 * Parse a 0x06 status broadcast (unit state change event).
 * `data`/`len` is the decrypted payload AFTER the type byte.
 * Complete ⇒ `states` holds at least one record and `len` was consumed
 * exactly; Malformed ⇒ `states` is empty.
 */
inline ParseStatus parseStatusBroadcast(const uint8_t* data, size_t len,
                                        std::vector<UnitStateInfo>& states,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    states.clear();

    // Shortest possible record (cap 0x00, no prev): 4 bytes.
    if (len < 4) return fail(&states, diag, 0, "packet shorter than one record");

    size_t offset = 0;
    while (offset < len) {
        if (offset + 3 > len) {
            return fail(&states, diag, offset, "truncated record header");
        }
        uint8_t unitId = data[offset];
        uint8_t flags  = data[offset + 1];
        uint8_t cap    = data[offset + 2];

        if (!isValidCap(cap)) {
            return fail(&states, diag, offset + 2, "unknown capability byte");
        }

        size_t recordLen = recordLength(flags, cap);
        if (offset + recordLen > len) {
            return fail(&states, diag, offset, "truncated record");
        }

        uint8_t auxCount = (cap >> 4) & 0x0F;
        bool hasConst = (cap == 0x03);
        bool hasPrev  = (flags & 0x10) != 0;

        UnitStateInfo info;
        info.unitId = unitId;
        info.online = true;

        // Change source from lower nibble: 0x0 = physical/offline
        if ((flags & 0x0F) == 0x00) {
            info.online = false;
        }

        size_t pos = offset + 3;

        // Constant byte — documented as always 0x80; anything else marks a
        // record layout we do not actually understand.
        if (hasConst) {
            if (data[pos] != 0x80) {
                return fail(&states, diag, pos, "constant byte != 0x80");
            }
            pos++;
        }

        // stored_level (previous brightness) — skipped
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

    // The loop exits only at offset == len (every over-run is caught above),
    // so the payload is consumed exactly and at least one record was parsed.
    return ParseStatus::Complete;
}

/**
 * Parse a 0x07 operation echo.
 * Structure (after type byte):
 *   [flags:2 BE, low 11 bits = payload length] [opcode:1] [origin:2 BE]
 *   [target:2 BE] [reserved:2] [payload...]
 * The declared payload length must match the received length exactly.
 */
inline ParseStatus parseOperationEcho(const uint8_t* data, size_t len,
                                      OperationEcho& echo,
                                      ParseDiag* diag = nullptr) {
    using namespace detail;
    echo.payload.clear();

    if (len < 9) {
        if (diag) { diag->offset = 0; diag->reason = "header truncated (need 9 bytes)"; }
        return ParseStatus::Malformed;
    }

    uint16_t flags = (uint16_t)((data[0] << 8) | data[1]);
    size_t declaredLen = flags & 0x07FF;
    size_t actualLen   = len - 9;

    if (declaredLen != actualLen) {
        if (diag) {
            diag->offset = 9;
            diag->reason = (declaredLen > actualLen)
                               ? "payload shorter than declared"
                               : "payload longer than declared";
        }
        return ParseStatus::Malformed;
    }

    echo.opcode = data[2];
    echo.target = (uint16_t)((data[5] << 8) | data[6]);
    echo.targetId = (echo.target >> 8) & 0xFF;
    echo.targetType = echo.target & 0xFF;
    if (declaredLen > 0) {
        echo.payload.assign(data + 9, data + 9 + declaredLen);
    }
    return ParseStatus::Complete;
}

/**
 * Parse a 0x08 unit state update.
 * When byte 2 looks like a valid 0x06 capability the packet is parsed with
 * the 0x06 record format (strictly, no fallback on failure — a packet that
 * announces the record format must honor it). Otherwise the payload must be
 * an exact sequence of [unitId][level] pairs.
 */
inline ParseStatus parseUnitStateUpdate(const uint8_t* data, size_t len,
                                        std::vector<UnitStateInfo>& states,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    states.clear();

    if (len < 2) return fail(&states, diag, 0, "packet shorter than one pair");

    if (len >= 4 && isValidCap(data[2])) {
        return parseStatusBroadcast(data, len, states, diag);
    }

    if (len % 2 != 0) {
        return fail(&states, diag, len - 1, "trailing byte in pair list");
    }

    for (size_t offset = 0; offset < len; offset += 2) {
        UnitStateInfo info;
        info.unitId = data[offset];
        info.level = data[offset + 1];
        info.on = (info.level > 0);
        info.online = true;
        info.hasLevel = true;
        states.push_back(info);
    }
    return ParseStatus::Complete;
}

}  // namespace packetparse

#endif  // PACKET_PARSE_H
