/**
 * Pure BLE parsing core: the handshake device-info record and the data
 * packets (0x06/0x07/0x08).
 *
 * Deliberately free of Arduino dependencies (only the C++ standard library)
 * so the exact parsing logic the firmware runs can be exercised by host-side
 * unit and fuzz tests (`pio test -e native`, test/test_packet_parse).
 * The firmware-facing wrappers in packet.cpp add debug logging and the
 * partial/malformed counters around these functions.
 *
 * ENDIANNESS — the protocol mixes both, per layer. Getting this wrong is
 * silent (the affected fields are diagnostics), so the rule is written down
 * here and every multi-byte field below states which layer it belongs to:
 *
 *   Transport layer  — LITTLE-endian: the 4-byte packet counter in the packet
 *                      header and in the nonce (docs B.7/B.8), and the
 *                      AES-CTR block counter.
 *   Handshake + operation layer — BIG-endian: the device-info record
 *                      (unit ID, flags — casambi-bt's `>BHH16s`, docs B.4)
 *                      and the operation header (flags, origin, target —
 *                      docs B.9/D.4, byte-identical to casambi-bt).
 *
 * docs/casambi-protokoll-referenz.md D.6 tabulates every multi-byte field
 * with the evidence for its byte order.
 *
 * The data-packet parsers below are three-state, TOLERANT-BUT-HONEST:
 *
 *   Complete  — the whole payload was consumed and understood.
 *   Partial   — a well-formed prefix was parsed and IS returned; the rest of
 *               the payload was not understood (unknown capability, truncated
 *               tail record, trailing bytes) and is dropped. Diag says where
 *               and why.
 *   Malformed — nothing usable; the output is empty.
 *
 * Rationale for tolerance: these payloads arrive decrypted AND CMAC-verified,
 * so radio corruption never reaches the parser — bytes we do not understand
 * are almost certainly protocol elements the reverse-engineering has not
 * covered (yet) or a newer protocol revision. The known parts must keep
 * working then: the understood prefix is applied, the unknown tail is dropped
 * and counted (never guessed at), and a packet is Malformed only when it
 * yields nothing usable at all. Callers may apply results for Complete AND
 * Partial; a parser never fabricates state from bytes it did not understand.
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
 *   [0x80]           — only if cap == 0x03 exactly (value observed as 0x80,
 *                      tolerated if different — possibly an undecoded flag)
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

// ============================================================================
// HANDSHAKE — device-info record
// ============================================================================
/*
 * The first GATT read on the auth characteristic, before any encryption:
 *
 *   Byte 0:      type = 0x01
 *   Byte 1:      version — LOW NIBBLE is the protocol version, the upper bits
 *                are undecoded flags
 *   Byte 2:      MTU
 *   Byte 3-4:    unit ID  (uint16, BIG-endian)
 *   Byte 5-6:    flags    (uint16, BIG-endian)
 *   Byte 7-22:   nonce (16 bytes) — the base for every later packet nonce
 *
 * Both 16-bit fields are big-endian, matching casambi-bt's
 * `struct.unpack_from(">BHH16s", firstResp, 2)` (docs B.4) and the outgoing
 * operation header. Reading the unit ID little-endian produced values like
 * 2816 (= 0x0B00) where the network's real unit ID is 11 (= 0x000B) — every
 * observed value decoded to a real unit ID once read big-endian (issue #49).
 *
 * The version byte likewise carries more than the version: protocol v11 is
 * reported as 0x2B, i.e. version 11 in the low nibble plus an undecoded 0x2
 * in the upper bits. Comparing the whole byte against the configured version
 * made every single connection log a bogus mismatch ("device reports 43,
 * config has 11"). casambi-bt papers over the same byte with a special case
 * for 0x2B; masking is the same fix generalised. The mask is the narrowest
 * one consistent with the observation — should a network ever report a
 * version >= 16 it would need widening, hence `versionRaw` is kept so the
 * undecoded bits stay visible instead of being silently dropped.
 *
 * Not three-state: the record is a fixed layout with no optional tail, so
 * there is no "understood prefix" worth keeping — it parses or it does not.
 */

constexpr size_t  DEVICE_INFO_HEADER_LEN  = 7;
constexpr size_t  DEVICE_INFO_NONCE_LEN   = 16;   // == NONCE_SIZE in config.h
constexpr size_t  DEVICE_INFO_MIN_LEN     = DEVICE_INFO_HEADER_LEN + DEVICE_INFO_NONCE_LEN;
constexpr uint8_t DEVICE_INFO_TYPE        = 0x01;
constexpr uint8_t DEVICE_INFO_VERSION_MASK = 0x0F;

struct DeviceInfo {
    uint8_t  type;
    uint8_t  version;      // protocol version — byte 1 with the flag bits masked off
    uint8_t  versionRaw;   // byte 1 verbatim, so the undecoded upper bits stay visible
    uint8_t  mtu;
    uint16_t unitId;       // BIG-endian
    uint16_t flags;        // BIG-endian
    const uint8_t* nonce;  // into the caller's buffer, DEVICE_INFO_NONCE_LEN bytes

    DeviceInfo() : type(0), version(0), versionRaw(0), mtu(0),
                   unitId(0), flags(0), nonce(nullptr) {}
};

enum class DeviceInfoStatus : uint8_t {
    Ok,
    TooShort,    // fewer than DEVICE_INFO_MIN_LEN bytes (a 0-byte GATT read is the common case)
    WrongType,   // byte 0 != 0x01 — not a device-info record
};

// `out` is only filled for Ok; `out.nonce` then points into `data`, so it
// stays valid exactly as long as the caller's buffer does.
inline DeviceInfoStatus parseDeviceInfo(const uint8_t* data, size_t len,
                                        DeviceInfo& out) {
    if (data == nullptr || len < DEVICE_INFO_MIN_LEN) return DeviceInfoStatus::TooShort;
    if (data[0] != DEVICE_INFO_TYPE)                  return DeviceInfoStatus::WrongType;

    out.type       = data[0];
    out.versionRaw = data[1];
    out.version    = static_cast<uint8_t>(data[1] & DEVICE_INFO_VERSION_MASK);
    out.mtu        = data[2];
    out.unitId     = static_cast<uint16_t>((data[3] << 8) | data[4]);
    out.flags      = static_cast<uint16_t>((data[5] << 8) | data[6]);
    out.nonce      = data + DEVICE_INFO_HEADER_LEN;
    return DeviceInfoStatus::Ok;
}

// ============================================================================
// DATA PACKETS
// ============================================================================

enum class ParseStatus : uint8_t {
    Complete,   // whole payload consumed and understood, output filled
    Partial,    // understood prefix returned, unknown tail dropped (see diag)
    Malformed,  // nothing usable — output empty, see diag
};

// Where and why parsing stopped (set for Partial and Malformed).
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

inline void setDiag(ParseDiag* diag, size_t offset, const char* reason) {
    if (diag) { diag->offset = offset; diag->reason = reason; }
}

// Stop parsing at `offset` for `reason`: keep the records understood so far
// (Partial) — or report Malformed when there is nothing usable to keep.
inline ParseStatus stop(std::vector<UnitStateInfo>& states, ParseDiag* diag,
                        size_t offset, const char* reason) {
    setDiag(diag, offset, reason);
    return states.empty() ? ParseStatus::Malformed : ParseStatus::Partial;
}

}  // namespace detail

/**
 * Parse a 0x06 status broadcast (unit state change event).
 * `data`/`len` is the decrypted payload AFTER the type byte.
 * Complete ⇒ `len` consumed exactly, ≥1 record. Partial ⇒ `states` holds the
 * well-formed leading records; the tail (unknown capability = unknown record
 * length, truncated record, leftover bytes) was dropped. Malformed ⇒ empty.
 */
inline ParseStatus parseStatusBroadcast(const uint8_t* data, size_t len,
                                        std::vector<UnitStateInfo>& states,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    states.clear();

    // Shortest possible record (cap 0x00, no prev): 4 bytes.
    if (len < 4) return stop(states, diag, 0, "packet shorter than one record");

    size_t offset = 0;
    while (offset < len) {
        if (offset + 3 > len) {
            return stop(states, diag, offset, "trailing bytes (no room for a record)");
        }
        uint8_t unitId = data[offset];
        uint8_t flags  = data[offset + 1];
        uint8_t cap    = data[offset + 2];

        // An unknown capability means an unknown record LENGTH — nothing
        // after this point can be located reliably, so keep what was
        // understood and drop the rest (never guess at record boundaries).
        if (!isValidCap(cap)) {
            return stop(states, diag, offset + 2, "unknown capability byte");
        }

        size_t recordLen = recordLength(flags, cap);
        if (offset + recordLen > len) {
            return stop(states, diag, offset, "truncated record");
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

        // Constant byte — observed as 0x80 so far. The record length is known
        // regardless of its value, so a different value is tolerated (it may
        // simply be a flag byte the reverse-engineering has not decoded yet).
        if (hasConst) {
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

    // The loop exits only at offset == len (every over-run stops above), so
    // the payload was consumed exactly and at least one record was parsed.
    return ParseStatus::Complete;
}

/**
 * Parse a 0x07 operation echo.
 * Structure (after type byte):
 *   [flags:2 BE, low 11 bits = payload length] [opcode:1] [origin:2 BE]
 *   [target:2 BE] [reserved:2] [payload...]
 * Exactly the DECLARED payload length is used. Extra trailing bytes beyond it
 * are tolerated and dropped (Partial) — a newer protocol revision may append
 * fields. A payload SHORTER than declared is Malformed: the packet arrived
 * MAC-verified, so the length-field interpretation does not fit this packet,
 * and a truncated operation payload must not be acted on.
 */
inline ParseStatus parseOperationEcho(const uint8_t* data, size_t len,
                                      OperationEcho& echo,
                                      ParseDiag* diag = nullptr) {
    using namespace detail;
    echo.payload.clear();

    if (len < 9) {
        setDiag(diag, 0, "header truncated (need 9 bytes)");
        return ParseStatus::Malformed;
    }

    uint16_t flags = (uint16_t)((data[0] << 8) | data[1]);
    size_t declaredLen = flags & 0x07FF;
    size_t actualLen   = len - 9;

    if (declaredLen > actualLen) {
        setDiag(diag, 9, "payload shorter than declared");
        return ParseStatus::Malformed;
    }

    echo.opcode = data[2];
    echo.target = (uint16_t)((data[5] << 8) | data[6]);
    echo.targetId = (echo.target >> 8) & 0xFF;
    echo.targetType = echo.target & 0xFF;
    if (declaredLen > 0) {
        echo.payload.assign(data + 9, data + 9 + declaredLen);
    }

    if (declaredLen < actualLen) {
        setDiag(diag, 9 + declaredLen, "trailing bytes beyond declared payload");
        return ParseStatus::Partial;
    }
    return ParseStatus::Complete;
}

/**
 * Parse a 0x08 unit state update.
 * When byte 2 looks like a valid 0x06 capability the packet is parsed with
 * the (tolerant) 0x06 record format — no fall-back to pair interpretation on
 * failure, record boundaries would be guesswork then. Otherwise the payload
 * is read as [unitId][level] pairs; a trailing odd byte is tolerated and
 * dropped (Partial).
 */
inline ParseStatus parseUnitStateUpdate(const uint8_t* data, size_t len,
                                        std::vector<UnitStateInfo>& states,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    states.clear();

    if (len < 2) return stop(states, diag, 0, "packet shorter than one pair");

    if (len >= 4 && isValidCap(data[2])) {
        return parseStatusBroadcast(data, len, states, diag);
    }

    for (size_t offset = 0; offset + 1 < len; offset += 2) {
        UnitStateInfo info;
        info.unitId = data[offset];
        info.level = data[offset + 1];
        info.on = (info.level > 0);
        info.online = true;
        info.hasLevel = true;
        states.push_back(info);
    }

    if (len % 2 != 0) {
        return stop(states, diag, len - 1, "trailing odd byte in pair list");
    }
    return ParseStatus::Complete;
}

}  // namespace packetparse

#endif  // PACKET_PARSE_H
