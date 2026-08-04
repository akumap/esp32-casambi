/**
 * Host-side unit tests for the pure full-state encoder (state_codec.h).
 * These run via `pio test -e native` and pin down exactly the encoding the
 * firmware sends as an OpCode::SetState payload (/api/units/:id/state):
 * byte placement per control, little-endian 16-bit values, zero-fill, and
 * the explicit rejection of layouts that were never verified on hardware.
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "cloud/state_codec.h"

using statecodec::ControlSpec;
using statecodec::EncodeResult;
using statecodec::DecodeResult;
using statecodec::encodeState;
using statecodec::decodeControl;
using statecodec::maxControlValue;
using statecodec::MAX_STATE_BYTES;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The verified real-world case: Oligo Grace — two independent 8-bit dimmers
// (byte 0 = first dimmer, byte 1 = second dimmer) plus a temperature byte.
// ---------------------------------------------------------------------------

static void test_grace_layout_full_state() {
    ControlSpec c[3] = {
        { /*offset*/ 0,  /*length*/ 8, /*value*/ 255 },   // dimmer0
        { /*offset*/ 8,  /*length*/ 8, /*value*/ 128 },   // dimmer1
        { /*offset*/ 16, /*length*/ 8, /*value*/ 46  },   // temperature
    };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_OK, encodeState(c, 3, 3, out));
    TEST_ASSERT_EQUAL_UINT8(255, out[0]);
    TEST_ASSERT_EQUAL_UINT8(128, out[1]);
    TEST_ASSERT_EQUAL_UINT8(46,  out[2]);
}

// The non-destructive-write property: a control that is NOT overridden keeps
// its current value in the blob — nothing is implicitly zeroed as long as the
// caller passes the current values (a zeroed byte resets the control on the
// fixture, observed for the temperature byte).
static void test_current_values_preserved() {
    ControlSpec c[3] = {
        { 0,  8, 0   },    // dimmer0 turned off by the caller
        { 8,  8, 200 },    // dimmer1 untouched: caller passed its current value
        { 16, 8, 46  },    // temperature untouched
    };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_OK, encodeState(c, 3, 3, out));
    TEST_ASSERT_EQUAL_UINT8(0,   out[0]);
    TEST_ASSERT_EQUAL_UINT8(200, out[1]);
    TEST_ASSERT_EQUAL_UINT8(46,  out[2]);
}

static void test_single_dimmer() {
    ControlSpec c[1] = { { 0, 8, 77 } };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_OK, encodeState(c, 1, 1, out));
    TEST_ASSERT_EQUAL_UINT8(77, out[0]);
}

static void test_16bit_little_endian() {
    // 16-bit control at byte 1 — encoded LSB first (matches the hue bytes of
    // OpCode::SetColor, the only multi-byte value verified on the wire).
    ControlSpec c[2] = {
        { 0, 8,  9      },
        { 8, 16, 0xABCD },
    };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_OK, encodeState(c, 2, 3, out));
    TEST_ASSERT_EQUAL_UINT8(9,    out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xCD, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, out[2]);
}

static void test_unclaimed_bytes_zero() {
    ControlSpec c[1] = { { 8, 8, 5 } };   // only byte 1 claimed
    uint8_t out[MAX_STATE_BYTES];
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_EQUAL(statecodec::ENCODE_OK, encodeState(c, 1, 3, out));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);
    TEST_ASSERT_EQUAL_UINT8(5, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0, out[2]);
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

static void test_no_layout() {
    ControlSpec c[1] = { { 0, 8, 1 } };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_NO_LAYOUT, encodeState(c, 0, 3, out));
    TEST_ASSERT_EQUAL(statecodec::ENCODE_NO_LAYOUT, encodeState(c, 1, 0, out));
}

static void test_state_too_long() {
    ControlSpec c[1] = { { 0, 8, 1 } };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_STATE_TOO_LONG,
                      encodeState(c, 1, MAX_STATE_BYTES + 1, out));
}

static void test_unaligned_offset_rejected() {
    ControlSpec c[1] = { { 4, 8, 1 } };   // bit offset 4 — never observed/verified
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_UNSUPPORTED_LAYOUT, encodeState(c, 1, 2, out));
}

static void test_odd_length_rejected() {
    ControlSpec c[1] = { { 0, 12, 1 } };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_UNSUPPORTED_LAYOUT, encodeState(c, 1, 2, out));
}

static void test_control_past_blob_rejected() {
    ControlSpec c[1] = { { 16, 16, 1 } };   // needs bytes 2-3, blob has 3 bytes
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_UNSUPPORTED_LAYOUT, encodeState(c, 1, 3, out));
}

static void test_value_out_of_range() {
    ControlSpec c[1] = { { 0, 8, 256 } };
    uint8_t out[MAX_STATE_BYTES];
    TEST_ASSERT_EQUAL(statecodec::ENCODE_VALUE_RANGE, encodeState(c, 1, 1, out));
}

static void test_max_control_value() {
    TEST_ASSERT_EQUAL_UINT16(255,    maxControlValue(8));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, maxControlValue(16));
}

static void test_result_names_nonempty() {
    for (int r = 0; r <= statecodec::ENCODE_VALUE_RANGE; r++) {
        const char* n = statecodec::encodeResultName(static_cast<EncodeResult>(r));
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_TRUE(strlen(n) > 0);
    }
}

// ---------------------------------------------------------------------------
// decodeControl — the read-side mirror of encodeState, used to interpret an
// incoming 0x06 record's raw state[] blob (UnitStateRecord, packet_parse.h)
// once fixture controls (offset/length) are known.
// ---------------------------------------------------------------------------

static void test_decode_grace_layout() {
    // Same blob encodeState would produce for the Oligo Grace layout.
    const uint8_t state[3] = { 255, 128, 46 };
    uint16_t value = 0xFFFF;
    TEST_ASSERT_EQUAL(statecodec::DECODE_OK, decodeControl(state, 3, 0, 8, value));
    TEST_ASSERT_EQUAL_UINT16(255, value);
    TEST_ASSERT_EQUAL(statecodec::DECODE_OK, decodeControl(state, 3, 8, 8, value));
    TEST_ASSERT_EQUAL_UINT16(128, value);
    TEST_ASSERT_EQUAL(statecodec::DECODE_OK, decodeControl(state, 3, 16, 8, value));
    TEST_ASSERT_EQUAL_UINT16(46, value);
}

static void test_decode_16bit_little_endian() {
    // Byte 1-2 = 0xABCD little-endian, matching encodeState's write order.
    const uint8_t state[3] = { 9, 0xCD, 0xAB };
    uint16_t value = 0;
    TEST_ASSERT_EQUAL(statecodec::DECODE_OK, decodeControl(state, 3, 8, 16, value));
    TEST_ASSERT_EQUAL_UINT16(0xABCD, value);
}

static void test_decode_unaligned_offset_rejected() {
    const uint8_t state[2] = { 1, 2 };
    uint16_t value = 0;
    TEST_ASSERT_EQUAL(statecodec::DECODE_UNSUPPORTED_LAYOUT,
                      decodeControl(state, 2, 4, 8, value));   // bit offset 4
}

static void test_decode_odd_length_rejected() {
    const uint8_t state[2] = { 1, 2 };
    uint16_t value = 0;
    TEST_ASSERT_EQUAL(statecodec::DECODE_UNSUPPORTED_LAYOUT,
                      decodeControl(state, 2, 0, 12, value));
}

static void test_decode_control_past_blob_rejected() {
    const uint8_t state[3] = { 1, 2, 3 };
    uint16_t value = 0;
    // 16-bit control at byte offset 2 needs bytes 2-3, blob only has 3 bytes.
    TEST_ASSERT_EQUAL(statecodec::DECODE_TRUNCATED,
                      decodeControl(state, 3, 16, 16, value));
}

// The write-up's correct catch: MAX_STATE_BYTES (8) is an outgoing-only
// limit. A state_len of 16 (the largest a 0x06 record's b8 high nibble can
// declare) must decode fine — nothing here is bounded by MAX_STATE_BYTES.
static void test_decode_beyond_max_state_bytes() {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) state[i] = (uint8_t)(i + 1);
    uint16_t value = 0;
    TEST_ASSERT_EQUAL(statecodec::DECODE_OK, decodeControl(state, 16, 15 * 8, 8, value));
    TEST_ASSERT_EQUAL_UINT16(16, value);
}

static void test_decode_result_names_nonempty() {
    for (int r = 0; r <= statecodec::DECODE_TRUNCATED; r++) {
        const char* n = statecodec::decodeResultName(static_cast<DecodeResult>(r));
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_TRUE(strlen(n) > 0);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_grace_layout_full_state);
    RUN_TEST(test_current_values_preserved);
    RUN_TEST(test_single_dimmer);
    RUN_TEST(test_16bit_little_endian);
    RUN_TEST(test_unclaimed_bytes_zero);
    RUN_TEST(test_no_layout);
    RUN_TEST(test_state_too_long);
    RUN_TEST(test_unaligned_offset_rejected);
    RUN_TEST(test_odd_length_rejected);
    RUN_TEST(test_control_past_blob_rejected);
    RUN_TEST(test_value_out_of_range);
    RUN_TEST(test_max_control_value);
    RUN_TEST(test_result_names_nonempty);

    RUN_TEST(test_decode_grace_layout);
    RUN_TEST(test_decode_16bit_little_endian);
    RUN_TEST(test_decode_unaligned_offset_rejected);
    RUN_TEST(test_decode_odd_length_rejected);
    RUN_TEST(test_decode_control_past_blob_rejected);
    RUN_TEST(test_decode_beyond_max_state_bytes);
    RUN_TEST(test_decode_result_names_nonempty);
    return UNITY_END();
}
