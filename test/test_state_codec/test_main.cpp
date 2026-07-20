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
using statecodec::encodeState;
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
    return UNITY_END();
}
