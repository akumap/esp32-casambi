/**
 * Pure encoder for a unit's full state vector (OpCode::SetState payload).
 *
 * Deliberately free of Arduino dependencies (only the C++ standard library)
 * so the exact encoding the firmware sends can be exercised by host-side
 * unit tests (`pio test -e native`, test/test_state_codec) — same pattern as
 * config_invariants.h and serial_args.h.
 *
 * WRITE SEMANTICS (verified against an Oligo Grace dual-dimmer fixture, see
 * docs/casambi-protocol-reference.md): a SetState write carries the COMPLETE
 * state blob of the unit — every control's raw value at the bit position the
 * cloud fixture definition (/fixture/{type}) declares. Bytes that are sent
 * as zero RESET the corresponding control on the fixture (observed: a zeroed
 * temperature byte resets the colour temperature). Callers must therefore
 * always encode from the unit's CURRENT control values and apply their
 * changes as overrides — never send a partially filled blob.
 *
 * Layout support is limited to what has been observed in real fixture
 * definitions and verified on hardware: byte-aligned controls of 8 or 16
 * bits (16-bit values little-endian, matching the LSB-first hue encoding of
 * OpCode::SetColor). Anything else fails the encode explicitly instead of
 * guessing a bit order that was never tested.
 */

#ifndef STATE_CODEC_H
#define STATE_CODEC_H

#include <cstddef>
#include <cstdint>

namespace statecodec {

// Upper bound of the state blob (bytes) and of the per-unit control count the
// firmware supports for full-state WRITES (SetState/`POST .../state`). Real
// fixtures observed so far use 1-3 bytes; 8 bounds the BleCommand queue entry
// without heap allocation. This is an OUTGOING-only limit: incoming 0x06
// records (UnitStateRecord in packet_parse.h) carry a state_len of 1-16
// bytes per the documented wire framing, and decodeControl() below accepts
// that full range — do not reuse MAX_STATE_BYTES to bound incoming decode.
constexpr size_t MAX_STATE_BYTES = 8;
constexpr size_t MAX_CONTROLS    = 8;

// One control's bit layout plus the raw value to encode. Filled from
// UnitControl (offset/length in bits, straight from the cloud fixture).
struct ControlSpec {
    uint8_t  offset;   // bit offset into the state blob
    uint8_t  length;   // bit length (8 or 16 supported)
    uint16_t value;    // raw value (0 .. 2^length-1)
};

enum EncodeResult : uint8_t {
    ENCODE_OK = 0,
    ENCODE_NO_LAYOUT,           // no controls or stateLen == 0
    ENCODE_STATE_TOO_LONG,      // stateLen exceeds MAX_STATE_BYTES
    ENCODE_UNSUPPORTED_LAYOUT,  // control not byte-aligned / not 8/16 bit /
                                // extends past the state blob
    ENCODE_VALUE_RANGE,         // value exceeds 2^length-1
};

inline const char* encodeResultName(EncodeResult r) {
    switch (r) {
        case ENCODE_OK:                 return "ok";
        case ENCODE_NO_LAYOUT:          return "no fixture control layout";
        case ENCODE_STATE_TOO_LONG:     return "state blob too long";
        case ENCODE_UNSUPPORTED_LAYOUT: return "unsupported control layout";
        case ENCODE_VALUE_RANGE:        return "value out of range";
    }
    return "unknown";
}

// Largest raw value a control of `length` bits can carry.
inline uint16_t maxControlValue(uint8_t length) {
    if (length >= 16) return 0xFFFF;
    return static_cast<uint16_t>((1u << length) - 1u);
}

// Encode the full state blob from the given control specs. `out` must hold at
// least stateLen bytes (<= MAX_STATE_BYTES); unclaimed bytes are zero — with
// complete control coverage (as cloud fixture definitions provide) every byte
// is claimed, so nothing is unintentionally reset.
inline EncodeResult encodeState(const ControlSpec* controls, size_t nControls,
                                size_t stateLen, uint8_t* out) {
    if (nControls == 0 || stateLen == 0) return ENCODE_NO_LAYOUT;
    if (stateLen > MAX_STATE_BYTES)      return ENCODE_STATE_TOO_LONG;

    for (size_t i = 0; i < stateLen; i++) out[i] = 0;

    for (size_t i = 0; i < nControls; i++) {
        const ControlSpec& c = controls[i];
        if ((c.offset % 8) != 0 || (c.length != 8 && c.length != 16)) {
            return ENCODE_UNSUPPORTED_LAYOUT;
        }
        const size_t idx   = c.offset / 8;
        const size_t bytes = c.length / 8;
        if (idx + bytes > stateLen)              return ENCODE_UNSUPPORTED_LAYOUT;
        if (c.value > maxControlValue(c.length)) return ENCODE_VALUE_RANGE;

        out[idx] = static_cast<uint8_t>(c.value & 0xFF);
        if (bytes == 2) out[idx + 1] = static_cast<uint8_t>(c.value >> 8);
    }
    return ENCODE_OK;
}

enum DecodeResult : uint8_t {
    DECODE_OK = 0,
    DECODE_UNSUPPORTED_LAYOUT,  // control not byte-aligned / not 8/16 bit
    DECODE_TRUNCATED,           // control extends past the state blob
};

inline const char* decodeResultName(DecodeResult r) {
    switch (r) {
        case DECODE_OK:                 return "ok";
        case DECODE_UNSUPPORTED_LAYOUT: return "unsupported control layout";
        case DECODE_TRUNCATED:          return "control extends past state blob";
    }
    return "unknown";
}

// Decode one control's raw value out of a unit's raw state blob (the `state`
// array of a UnitStateRecord, packet_parse.h) — the read-side mirror of
// encodeState() above, same scope: byte-aligned 8- or 16-bit controls only
// (16-bit little-endian, matching encodeState's write order and the LSB-first
// hue encoding of OpCode::SetColor). No fixture has been observed with a
// sub-byte or bit-packed control, so — exactly as encodeState does on the
// write side — this fails explicitly rather than guessing a bit order that
// was never tested.
//
// `stateLen` is the RECEIVED record's state_len (1-16, see packet_parse.h's
// UnitStateRecord), not MAX_STATE_BYTES — that constant bounds the outgoing
// SetState encoder only and must not gate incoming decode.
inline DecodeResult decodeControl(const uint8_t* state, size_t stateLen,
                                  uint8_t offsetBits, uint8_t lengthBits,
                                  uint16_t& value) {
    if ((offsetBits % 8) != 0 || (lengthBits != 8 && lengthBits != 16)) {
        return DECODE_UNSUPPORTED_LAYOUT;
    }
    const size_t idx   = offsetBits / 8;
    const size_t bytes = lengthBits / 8;
    if (idx + bytes > stateLen) return DECODE_TRUNCATED;

    value = state[idx];
    if (bytes == 2) {
        value = static_cast<uint16_t>(value | (static_cast<uint16_t>(state[idx + 1]) << 8));
    }
    return DECODE_OK;
}

}  // namespace statecodec

#endif // STATE_CODEC_H
