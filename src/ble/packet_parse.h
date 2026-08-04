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
 *               the payload was not understood (truncated tail record,
 *               trailing bytes) and is dropped. Diag says where and why.
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
 * 0x06 status broadcast — one record per changed unit. This framing replaced
 * an earlier "capability descriptor" reading of byte 2 (aux-channel count +
 * a special-cased 0x80 constant byte) that was reverse-engineered ad hoc from
 * a handful of captures. That heuristic and the framing below compute the
 * IDENTICAL record length for every packet ever captured on the reference
 * network (56 real records across 5 fixture types, full dimmer/vertical/
 * temperature sweeps, and genuine mains power-cycle/offline transitions —
 * see docs/captures/2026-08-04-0x06-framing/), so the rewrite carried no
 * framing risk; it was done because the two readings disagree on *meaning*
 * (notably `online`, where the old low-nibble-nonzero heuristic is
 * demonstrably wrong during a real offline transition — it reported
 * `online: true` for a unit that had just gone dark, twice, in that capture).
 *
 *   Byte 0: Unit-ID
 *   Byte 1: Flags
 *            Bit 0: on      — HARDWARE-VERIFIED as decoupled from live
 *                             brightness (stays 1 while a unit is dimmed to
 *                             exactly 0 while online) but always identical to
 *                             bit 1 in every capture so far (56/56 records) —
 *                             carries no information beyond `online` on this
 *                             network. Passed through verbatim regardless;
 *                             this firmware does not decide what "on" means
 *                             for a dashboard, callers do (see UnitStateRecord).
 *            Bit 1: online  — HARDWARE-VERIFIED more correct than the old
 *                             low-nibble heuristic for real offline
 *                             transitions (see above).
 *            Bit 2: con     — optional byte present (+1 byte). UNVERIFIED
 *                             distinct from `sid`/`extra` — on this hardware
 *                             it only ever co-occurs with single-dimmer
 *                             fixtures (type 1422) and always carries 0x80,
 *                             same value/position the old "constant byte"
 *                             heuristic already skipped.
 *            Bit 3: sid     — optional byte present (+1 byte). UNVERIFIED —
 *                             never observed set in any capture; implemented
 *                             per the documented protocol regardless.
 *            Bit 4: extra   — optional byte present (+1 byte). Same wire
 *                             position as the old "stored_level" byte; this
 *                             framing does not assume what it contains.
 *            Bit 5:         — UNVERIFIED, never observed set.
 *            Bit 6-7: padding length (0-3 trailing bytes). UNVERIFIED —
 *                             never observed nonzero.
 *   Byte 2: b8
 *            Bits 4-7: state_len - 1  (state_len = 1-16 raw state bytes)
 *            Bits 0-3: priority. UNVERIFIED beyond "always 3" — every one of
 *                             56 real captures had priority == 3 regardless
 *                             of fixture type, change source, or online/
 *                             offline transition; implemented per the
 *                             documented 0-15 range regardless.
 *   [con]            — present iff flags bit 2
 *   [sid]            — present iff flags bit 3
 *   [extra]          — present iff flags bit 4
 *   state[state_len] — raw per-unit state blob. NO fixture semantics here —
 *                      which byte means "dimmer" vs "vertical" vs
 *                      "temperature" comes from the cloud fixture's controls
 *                      (offset/length), applied by the caller, not this parser.
 *   padding[padding_len]
 *
 * Record length = 3 + has_con + has_sid + has_extra + state_len + padding_len
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

// Upper bounds on a 0x06 record's variable-length parts, straight from the
// wire framing: state_len is a 4-bit nibble + 1 (1-16), padding_len is a
// 2-bit field (0-3). Distinct from state_codec::MAX_STATE_BYTES, which
// bounds the OUTGOING SetState encoder (8) — that limit does not apply here.
constexpr size_t UNIT_STATE_MAX_LEN     = 16;
constexpr size_t UNIT_STATE_MAX_PADDING = 3;

/**
 * One raw 0x06 status-broadcast record, decoded strictly per the documented
 * wire framing (see the header comment above) with NO fixture semantics
 * applied — `state` is the raw per-unit state blob exactly as received;
 * which byte means "dimmer"/"vertical"/"temperature"/... comes from the
 * unit's cloud fixture controls (offset/length), applied by the caller.
 * `con`/`sid`/`extraByte` are likewise passed through uninterpreted — see
 * the header comment for what is and is not hardware-verified about them.
 */
struct UnitStateRecord {
    uint8_t unitId;
    uint8_t flags;
    bool    on;             // flags bit 0, verbatim — see header comment
    bool    online;         // flags bit 1, verbatim — see header comment
    uint8_t priority;       // b8 low nibble (0-15)

    bool    hasCon;         // flags bit 2
    bool    hasSid;         // flags bit 3
    bool    hasExtra;       // flags bit 4
    uint8_t con;            // valid iff hasCon
    uint8_t sid;            // valid iff hasSid
    uint8_t extraByte;      // valid iff hasExtra

    uint8_t stateLen;                          // 1-16, b8 high nibble + 1
    uint8_t state[UNIT_STATE_MAX_LEN];         // state[0..stateLen-1] valid

    uint8_t paddingLen;                        // 0-3, flags bits 6-7
    uint8_t padding[UNIT_STATE_MAX_PADDING];   // padding[0..paddingLen-1] valid

    UnitStateRecord()
        : unitId(0), flags(0), on(false), online(false), priority(0),
          hasCon(false), hasSid(false), hasExtra(false), con(0), sid(0), extraByte(0),
          stateLen(0), state(), paddingLen(0), padding() {}
};

// Upper bounds on an INVOCATION frame per the documented wire framing:
// payloadLen is a 6-bit field (flags & 0x003F), so 0-63 bytes.
constexpr size_t INVOCATION_HEADER_LEN      = 9;
constexpr size_t INVOCATION_MAX_PAYLOAD_LEN = 63;

/**
 * One INVOCATION frame from an incoming 0x07 payload, which is a STREAM of
 * zero or more of these back to back — not a single record (see the header
 * comment above for why the previous single-frame "operation echo" reading
 * is superseded). `targetType()`/`targetId()` decode `target` exactly like
 * the OUTGOING operation encoder's `encodeTarget()` (packet.h) — see the
 * header comment for whether that symmetry actually holds here.
 */
struct InvocationFrame {
    uint16_t flags;
    uint8_t  opcode;
    uint16_t origin;
    uint16_t target;
    uint16_t age;

    bool    hasOriginHandle;   // flags bit 0x0200
    uint8_t originHandle;      // valid iff hasOriginHandle

    uint8_t payloadLen;                             // flags & 0x003F, 0-63
    uint8_t payload[INVOCATION_MAX_PAYLOAD_LEN];    // payload[0..payloadLen-1] valid

    InvocationFrame()
        : flags(0), opcode(0), origin(0), target(0), age(0),
          hasOriginHandle(false), originHandle(0),
          payloadLen(0), payload() {}

    uint8_t targetType() const { return static_cast<uint8_t>(target & 0x00FF); }
    uint8_t targetId()   const { return static_cast<uint8_t>((target >> 8) & 0x00FF); }
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

inline void setDiag(ParseDiag* diag, size_t offset, const char* reason) {
    if (diag) { diag->offset = offset; diag->reason = reason; }
}

// Stop parsing at `offset` for `reason`: keep the records understood so far
// (Partial) — or report Malformed when there is nothing usable to keep.
// Templated: used for both UnitStateInfo (0x08 pair list) and
// UnitStateRecord (0x06) output vectors.
template <typename T>
inline ParseStatus stop(std::vector<T>& records, ParseDiag* diag,
                        size_t offset, const char* reason) {
    setDiag(diag, offset, reason);
    return records.empty() ? ParseStatus::Malformed : ParseStatus::Partial;
}

}  // namespace detail

/**
 * Parse a 0x06 status broadcast (unit state change event).
 * `data`/`len` is the decrypted payload AFTER the type byte.
 * Complete ⇒ `len` consumed exactly, ≥1 record. Partial ⇒ `records` holds the
 * well-formed leading records; the tail (truncated record, leftover bytes)
 * was dropped. Malformed ⇒ empty.
 *
 * Unlike the previous capability-heuristic parser, every possible `b8` value
 * now maps to a valid state_len/priority pair — there is no "unknown
 * capability" rejection anymore, so Malformed/Partial here only ever mean
 * outright truncation, never an unrecognized byte-2 value.
 */
inline ParseStatus parseStatusBroadcast(const uint8_t* data, size_t len,
                                        std::vector<UnitStateRecord>& records,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    records.clear();

    // Shortest possible record (state_len 1, no optionals/padding): 4 bytes.
    if (len < 4) return stop(records, diag, 0, "packet shorter than one record");

    size_t offset = 0;
    while (offset < len) {
        if (offset + 3 > len) {
            return stop(records, diag, offset, "trailing bytes (no room for a record header)");
        }

        UnitStateRecord rec;
        rec.unitId = data[offset];
        rec.flags  = data[offset + 1];
        uint8_t b8 = data[offset + 2];

        rec.on       = (rec.flags & 0x01) != 0;
        rec.online   = (rec.flags & 0x02) != 0;
        rec.hasCon   = (rec.flags & 0x04) != 0;
        rec.hasSid   = (rec.flags & 0x08) != 0;
        rec.hasExtra = (rec.flags & 0x10) != 0;
        rec.paddingLen = (rec.flags >> 6) & 0x03;

        rec.priority = b8 & 0x0F;
        rec.stateLen = static_cast<uint8_t>(((b8 >> 4) & 0x0F) + 1);

        size_t optionalLen = (rec.hasCon ? 1 : 0) + (rec.hasSid ? 1 : 0) + (rec.hasExtra ? 1 : 0);
        size_t recordLen = 3 + optionalLen + rec.stateLen + rec.paddingLen;

        if (offset + recordLen > len) {
            return stop(records, diag, offset, "truncated record");
        }

        size_t pos = offset + 3;
        if (rec.hasCon)   rec.con       = data[pos++];
        if (rec.hasSid)   rec.sid       = data[pos++];
        if (rec.hasExtra) rec.extraByte = data[pos++];

        for (uint8_t i = 0; i < rec.stateLen; i++) rec.state[i] = data[pos + i];
        pos += rec.stateLen;

        for (uint8_t i = 0; i < rec.paddingLen; i++) rec.padding[i] = data[pos + i];
        pos += rec.paddingLen;

        records.push_back(rec);
        offset += recordLen;
    }

    // The loop exits only at offset == len (every over-run stops above), so
    // the payload was consumed exactly and at least one record was parsed.
    return ParseStatus::Complete;
}

/**
 * Parse a 0x07 payload as a STREAM of INVOCATION frames (zero or more, back
 * to back) — not a single "operation echo" record. This supersedes an
 * earlier reading that treated 0x07 as one frame with an 11-bit payload
 * length (`flags & 0x07FF`) taken by symmetry with the OUTGOING operation
 * encoder (docs B.9). That symmetry assumption is UNVERIFIED and now
 * believed wrong: 0x07 has never been observed on the reference network at
 * all (no captures exist), and it directly conflicts with an alternative,
 * equally unverified theory documented in docs/casambi-protokoll-referenz.md
 * B.12 (ported from casambi-bt), which reads incoming 0x07 as a stream of
 * switch/sensor events framed with a 3-byte-per-message header — structurally
 * incompatible with the 9-byte header below. Neither theory can be confirmed
 * without a real capture; this parser implements the newer, more detailed
 * INVOCATION-frame theory (see docs B.12a) because it is internally more
 * consistent (its payload-length field agrees with the "payload: max. 63
 * byte" note already documented for the OUTGOING operation packet, B.9) and
 * because it explains an empirical observation: operating lights via the
 * Casambi/Occhio app produces only 0x06 traffic, never 0x07 — consistent
 * with 0x07 (incoming) being tied to physical button/sensor input rather
 * than being a generic echo of any operation, since the app is not a
 * physical switch.
 *
 * Per-frame structure (9-byte header, then optional/variable tail):
 *   Byte 0-1: flags, BIG-endian
 *              Bits 0-5:  payloadLen (0-63)
 *              Bit  9 (0x0200): hasOriginHandle — 1 extra byte present
 *              other bits: undecoded
 *   Byte 2:   opcode
 *   Byte 3-4: origin, BIG-endian
 *   Byte 5-6: target, BIG-endian — decode via targetType()/targetId()
 *   Byte 7-8: age, BIG-endian
 *   [originHandle]         — present iff hasOriginHandle
 *   payload[payloadLen]    — 0-63 bytes
 *
 * Frame length = 9 + has_origin_handle + payloadLen. A critical regression
 * this framing must get right: flags=0x0204 means hasOriginHandle=true,
 * payloadLen=4 — the OLD `flags & 0x07FF` reading would instead compute
 * declaredLen=516 and reject or misparse the packet.
 *
 * Same tolerant-but-honest contract as 0x06 (see the file header comment):
 * Complete when the whole payload is consumed as whole frames, Partial when
 * a truncated trailing frame is dropped after >=1 good frame, Malformed when
 * nothing usable was parsed at all.
 */
inline ParseStatus parseInvocationStream(const uint8_t* data, size_t len,
                                         std::vector<InvocationFrame>& frames,
                                         ParseDiag* diag = nullptr) {
    using namespace detail;
    frames.clear();

    if (len < INVOCATION_HEADER_LEN) {
        return stop(frames, diag, 0, "packet shorter than one invocation header");
    }

    size_t offset = 0;
    while (offset < len) {
        if (offset + INVOCATION_HEADER_LEN > len) {
            return stop(frames, diag, offset, "trailing bytes (no room for a frame header)");
        }

        InvocationFrame f;
        f.flags = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        f.payloadLen = static_cast<uint8_t>(f.flags & 0x003F);
        f.hasOriginHandle = (f.flags & 0x0200) != 0;

        size_t frameLen = INVOCATION_HEADER_LEN + (f.hasOriginHandle ? 1 : 0) + f.payloadLen;
        if (offset + frameLen > len) {
            return stop(frames, diag, offset, "truncated invocation frame");
        }

        f.opcode = data[offset + 2];
        f.origin = (uint16_t)((data[offset + 3] << 8) | data[offset + 4]);
        f.target = (uint16_t)((data[offset + 5] << 8) | data[offset + 6]);
        f.age    = (uint16_t)((data[offset + 7] << 8) | data[offset + 8]);

        size_t pos = offset + INVOCATION_HEADER_LEN;
        if (f.hasOriginHandle) f.originHandle = data[pos++];
        for (uint8_t i = 0; i < f.payloadLen; i++) f.payload[i] = data[pos + i];

        frames.push_back(f);
        offset += frameLen;
    }

    // The loop exits only at offset == len (every over-run stops above), so
    // the payload was consumed exactly as whole frames.
    return ParseStatus::Complete;
}

/**
 * Parse a 0x08 unit state update: [unitId][level] pairs; a trailing odd byte
 * is tolerated and dropped (Partial).
 *
 * The previous version of this parser would delegate to the 0x06 record
 * format when byte 2 "looked like a valid capability" — that heuristic relied
 * on the old capability scheme having invalid byte-2 values to detect on.
 * Under the corrected 0x06 framing (see packet_parse.h header comment) EVERY
 * byte-2 value is a valid state_len/priority pair, so there is no principled
 * way left to distinguish "0x08 in 0x06 record format" from a plain pair
 * list by inspection — and 0x08 has never been observed in practice at all
 * on the reference network (see DataPacketType::UnitState in packet.h), so
 * there is no real capture to derive a better heuristic from. Always parsing
 * as a pair list is therefore the honest choice; revisit with a real 0x08
 * capture if one is ever seen (it would show up as a `malformed08`/oddly
 * shaped pair-list count anomaly).
 */
inline ParseStatus parseUnitStateUpdate(const uint8_t* data, size_t len,
                                        std::vector<UnitStateInfo>& states,
                                        ParseDiag* diag = nullptr) {
    using namespace detail;
    states.clear();

    if (len < 2) return stop(states, diag, 0, "packet shorter than one pair");

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
