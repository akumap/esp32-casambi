/**
 * Host-side unit tests for the INVOCATION-frame semantic layer
 * (invocation_events.h): button-stream/NotifyInput classification and the
 * packet-local dedup rules. Runs via `pio test -e native`, no BLE/Arduino
 * dependency.
 *
 * UNVERIFIED, like the layer it tests — these tests pin down the documented
 * behavior, not hardware-confirmed behavior (0x07 has never been captured on
 * the reference network; see packet_parse.h's parseInvocationStream doc
 * comment).
 */

#include <unity.h>
#include <cstdint>
#include <vector>

#include "ble/invocation_events.h"

using invocation_events::CasambiInputEvent;
using invocation_events::EventDedupTable;
using invocation_events::InputEventType;
using invocation_events::classifyButtonFrame;
using invocation_events::classifyInvocationEvent;
using invocation_events::classifyNotifyInputFrame;
using invocation_events::decodeInvocationEvents;
using invocation_events::mapInputCode;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Frame builders — construct InvocationFrame values directly (this layer
// operates on already-framed InvocationFrame, not raw bytes; the wire-level
// framing is covered separately in test_packet_parse).
// ---------------------------------------------------------------------------

static InvocationFrame makeButtonFrame(uint8_t opcode, uint8_t targetId, uint8_t payload0) {
    InvocationFrame f;
    f.opcode = opcode;
    f.target = static_cast<uint16_t>((static_cast<uint16_t>(targetId) << 8) | 0x06);
    f.payloadLen = 1;
    f.payload[0] = payload0;
    return f;
}

static InvocationFrame makeNotifyFrame(uint8_t opcode, uint8_t targetId, uint8_t code, uint8_t b1,
                                       bool withValue16 = false, uint16_t value16 = 0) {
    InvocationFrame f;
    f.opcode = opcode;
    f.target = static_cast<uint16_t>((static_cast<uint16_t>(targetId) << 8) | 0x12);
    f.payload[0] = code;
    f.payload[1] = b1;
    if (withValue16) {
        f.payloadLen = 4;
        f.payload[2] = static_cast<uint8_t>(value16 & 0xFF);
        f.payload[3] = static_cast<uint8_t>((value16 >> 8) & 0xFF);
    } else {
        f.payloadLen = 2;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Button-stream classification (targetType 0x06, opcode 29-36)
// ---------------------------------------------------------------------------

void test_button_press_classified(void) {
    auto frame = makeButtonFrame(29, 5, 0x80);   // bit7 set = pressed
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyButtonFrame(frame, ev));
    TEST_ASSERT_EQUAL(InputEventType::ButtonPress, ev.type);
    TEST_ASSERT_TRUE(ev.pressed);
    TEST_ASSERT_EQUAL(5, ev.unitId);
    TEST_ASSERT_EQUAL(0, ev.index);
    TEST_ASSERT_TRUE(ev.isButtonStream);
}

void test_button_release_classified(void) {
    auto frame = makeButtonFrame(29, 5, 0x00);   // bit7 clear = released
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyButtonFrame(frame, ev));
    TEST_ASSERT_EQUAL(InputEventType::ButtonRelease, ev.type);
    TEST_ASSERT_FALSE(ev.pressed);
}

void test_button_p_s_bits_extracted(void) {
    // 0xD3 = 1101_0011: bit7=1 (pressed), bits3-6=0b1010=10 (p), bits0-2=0b011=3 (s)
    auto frame = makeButtonFrame(29, 1, 0xD3);
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyButtonFrame(frame, ev));
    TEST_ASSERT_TRUE(ev.pressed);
    TEST_ASSERT_EQUAL(10, ev.p);
    TEST_ASSERT_EQUAL(3, ev.s);
}

// BUTTON_LABELS = {4,1,2,3} — only for index 0-3 (opcode 29-32).
void test_button_label_mapping_index_0_to_3(void) {
    const uint8_t expectedLabels[4] = {4, 1, 2, 3};
    for (uint8_t index = 0; index < 4; index++) {
        auto frame = makeButtonFrame(static_cast<uint8_t>(29 + index), 1, 0x80);
        CasambiInputEvent ev;
        TEST_ASSERT_TRUE(classifyButtonFrame(frame, ev));
        TEST_ASSERT_EQUAL(index, ev.index);
        TEST_ASSERT_EQUAL(expectedLabels[index], ev.label);
    }
}

// Indices 4-7 (opcode 33-36) exist on the wire but have no observed label —
// never guessed at, label stays 0.
void test_button_label_zero_for_index_4_plus(void) {
    for (uint8_t index = 4; index < 8; index++) {
        auto frame = makeButtonFrame(static_cast<uint8_t>(29 + index), 1, 0x80);
        CasambiInputEvent ev;
        TEST_ASSERT_TRUE(classifyButtonFrame(frame, ev));
        TEST_ASSERT_EQUAL(index, ev.index);
        TEST_ASSERT_EQUAL(0, ev.label);
    }
}

void test_button_wrong_target_type_not_classified(void) {
    InvocationFrame frame = makeButtonFrame(29, 5, 0x80);
    frame.target = static_cast<uint16_t>((5 << 8) | 0x12);   // NotifyInput's type, not 0x06
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyButtonFrame(frame, ev));
}

void test_button_opcode_out_of_range_not_classified(void) {
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyButtonFrame(makeButtonFrame(28, 5, 0x80), ev));
    TEST_ASSERT_FALSE(classifyButtonFrame(makeButtonFrame(37, 5, 0x80), ev));
}

void test_button_empty_payload_not_classified(void) {
    InvocationFrame frame = makeButtonFrame(29, 5, 0x80);
    frame.payloadLen = 0;
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyButtonFrame(frame, ev));
}

// ---------------------------------------------------------------------------
// NotifyInput classification (targetType 0x12, opcode 64-71)
// ---------------------------------------------------------------------------

void test_notifyinput_code_mapping(void) {
    TEST_ASSERT_EQUAL(InputEventType::ButtonPress,          mapInputCode(0x01));
    TEST_ASSERT_EQUAL(InputEventType::ButtonRelease,        mapInputCode(0x02));
    TEST_ASSERT_EQUAL(InputEventType::ButtonHold,            mapInputCode(0x09));
    TEST_ASSERT_EQUAL(InputEventType::ButtonReleaseAfterHold, mapInputCode(0x0C));
    TEST_ASSERT_EQUAL(InputEventType::RawInput,              mapInputCode(0xFF));
}

void test_notifyinput_channel_extraction(void) {
    auto frame = makeNotifyFrame(64, 7, 0x01, 0xFD);   // 0xFD & 0x07 = 5
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyNotifyInputFrame(frame, ev));
    TEST_ASSERT_EQUAL(7, ev.unitId);
    TEST_ASSERT_EQUAL(0, ev.index);
    TEST_ASSERT_EQUAL(0x01, ev.inputCode);
    TEST_ASSERT_EQUAL(5, ev.channel);
    TEST_ASSERT_EQUAL(InputEventType::ButtonPress, ev.type);
    TEST_ASSERT_FALSE(ev.isButtonStream);
}

void test_notifyinput_value16_little_endian_when_4_bytes(void) {
    auto frame = makeNotifyFrame(65, 7, 0x01, 0x00, /*withValue16=*/true, 0x1234);
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyNotifyInputFrame(frame, ev));
    TEST_ASSERT_EQUAL(1, ev.index);
    TEST_ASSERT_TRUE(ev.hasValue16);
    TEST_ASSERT_EQUAL(0x1234, ev.value16);
}

void test_notifyinput_no_value16_when_2_bytes(void) {
    auto frame = makeNotifyFrame(64, 7, 0x01, 0x00);
    CasambiInputEvent ev;
    TEST_ASSERT_TRUE(classifyNotifyInputFrame(frame, ev));
    TEST_ASSERT_FALSE(ev.hasValue16);
}

void test_notifyinput_wrong_target_type_not_classified(void) {
    InvocationFrame frame = makeNotifyFrame(64, 7, 0x01, 0x00);
    frame.target = static_cast<uint16_t>((7 << 8) | 0x06);   // button's type, not 0x12
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyNotifyInputFrame(frame, ev));
}

void test_notifyinput_opcode_out_of_range_not_classified(void) {
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyNotifyInputFrame(makeNotifyFrame(63, 7, 0x01, 0x00), ev));
    TEST_ASSERT_FALSE(classifyNotifyInputFrame(makeNotifyFrame(72, 7, 0x01, 0x00), ev));
}

void test_notifyinput_too_short_payload_not_classified(void) {
    InvocationFrame frame = makeNotifyFrame(64, 7, 0x01, 0x00);
    frame.payloadLen = 1;
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyNotifyInputFrame(frame, ev));
}

void test_classify_unrecognized_frame_returns_false(void) {
    // Not a parse error (see packet_parse.h's tolerant-but-honest contract) —
    // just "not one of the two known input-event shapes".
    InvocationFrame frame;
    frame.opcode = 1;
    frame.target = static_cast<uint16_t>((1 << 8) | 0x01);   // TARGET_TYPE_UNIT, unrelated
    frame.payloadLen = 2;
    CasambiInputEvent ev;
    TEST_ASSERT_FALSE(classifyInvocationEvent(frame, ev));
}

// ---------------------------------------------------------------------------
// Dedup (invocation_events::decodeInvocationEvents)
// ---------------------------------------------------------------------------

void test_dedup_repeated_press_suppressed(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    std::vector<InvocationFrame> pressOnce = {makeButtonFrame(29, 5, 0x80)};
    decodeInvocationEvents(pressOnce, table, events);
    TEST_ASSERT_EQUAL(1, events.size());

    decodeInvocationEvents(pressOnce, table, events);   // same pressed state again
    TEST_ASSERT_EQUAL(0, events.size());
}

void test_dedup_state_transition_emitted(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    decodeInvocationEvents({makeButtonFrame(29, 5, 0x80)}, table, events);   // press
    TEST_ASSERT_EQUAL(1, events.size());

    decodeInvocationEvents({makeButtonFrame(29, 5, 0x00)}, table, events);   // release: transition
    TEST_ASSERT_EQUAL(1, events.size());
    TEST_ASSERT_EQUAL(InputEventType::ButtonRelease, events[0].type);
}

void test_dedup_repeated_notifyinput_code_suppressed(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    std::vector<InvocationFrame> codeOnce = {makeNotifyFrame(66, 7, 0x01, 0x00)};
    decodeInvocationEvents(codeOnce, table, events);
    TEST_ASSERT_EQUAL(1, events.size());

    decodeInvocationEvents(codeOnce, table, events);   // same code again
    TEST_ASSERT_EQUAL(0, events.size());
}

// Button stream takes precedence: a button-stream frame in the SAME packet
// suppresses a matching NotifyInput Press(0x01)/Release(0x02) for the same
// (unitId, index) — both frames must resolve to the same key. Button index 0
// = opcode 29; NotifyInput index 0 = opcode 64; both use targetId 8 here.
void test_dedup_button_stream_suppresses_matching_notifyinput_same_packet(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    std::vector<InvocationFrame> packet = {
        makeButtonFrame(29, 8, 0x80),        // press, unit 8 index 0
        makeNotifyFrame(64, 8, 0x01, 0x00),  // Press code, unit 8 index 0 — same key
    };
    decodeInvocationEvents(packet, table, events);

    TEST_ASSERT_EQUAL(1, events.size());
    TEST_ASSERT_EQUAL(InputEventType::ButtonPress, events[0].type);
    TEST_ASSERT_TRUE(events[0].isButtonStream);
}

// Hold/ReleaseAfterHold are NOT press/release edges, so they are still
// emitted even when a button-stream frame for the same key is in the packet.
void test_dedup_notifyinput_hold_still_emitted_with_button_stream(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    std::vector<InvocationFrame> packet = {
        makeButtonFrame(29, 8, 0x80),        // press, unit 8 index 0
        makeNotifyFrame(64, 8, 0x09, 0x00),  // Hold code, unit 8 index 0 — same key
    };
    decodeInvocationEvents(packet, table, events);

    TEST_ASSERT_EQUAL(2, events.size());
}

// Regression: "button stream seen" must be scoped to a single packet, not
// remembered forever — otherwise a button-stream frame observed once would
// permanently suppress all later NotifyInput press/release for that key.
void test_dedup_button_stream_not_sticky_across_packets(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    decodeInvocationEvents({makeButtonFrame(29, 3, 0x80)}, table, events);   // packet 1: button only
    TEST_ASSERT_EQUAL(1, events.size());

    decodeInvocationEvents({makeNotifyFrame(64, 3, 0x01, 0x00)}, table, events);  // packet 2: NotifyInput only
    TEST_ASSERT_EQUAL(1, events.size());   // must NOT be suppressed
}

// Table is fixed-size (32 entries); beyond capacity, dedup fails OPEN —
// events for pairs with no free slot still pass through unfiltered rather
// than being dropped or evicting an existing entry.
void test_dedup_table_fail_open_beyond_capacity(void) {
    EventDedupTable table;
    std::vector<CasambiInputEvent> events;

    std::vector<InvocationFrame> fillPacket;
    for (uint8_t unitId = 0; unitId < EventDedupTable::CAPACITY; unitId++) {
        fillPacket.push_back(makeButtonFrame(29, unitId, 0x80));
    }
    decodeInvocationEvents(fillPacket, table, events);
    TEST_ASSERT_EQUAL(EventDedupTable::CAPACITY, events.size());   // table now full

    decodeInvocationEvents({makeButtonFrame(29, 99, 0x80)}, table, events);
    TEST_ASSERT_EQUAL(1, events.size());   // no slot available, but not dropped
    TEST_ASSERT_EQUAL(99, events[0].unitId);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_button_press_classified);
    RUN_TEST(test_button_release_classified);
    RUN_TEST(test_button_p_s_bits_extracted);
    RUN_TEST(test_button_label_mapping_index_0_to_3);
    RUN_TEST(test_button_label_zero_for_index_4_plus);
    RUN_TEST(test_button_wrong_target_type_not_classified);
    RUN_TEST(test_button_opcode_out_of_range_not_classified);
    RUN_TEST(test_button_empty_payload_not_classified);

    RUN_TEST(test_notifyinput_code_mapping);
    RUN_TEST(test_notifyinput_channel_extraction);
    RUN_TEST(test_notifyinput_value16_little_endian_when_4_bytes);
    RUN_TEST(test_notifyinput_no_value16_when_2_bytes);
    RUN_TEST(test_notifyinput_wrong_target_type_not_classified);
    RUN_TEST(test_notifyinput_opcode_out_of_range_not_classified);
    RUN_TEST(test_notifyinput_too_short_payload_not_classified);

    RUN_TEST(test_classify_unrecognized_frame_returns_false);

    RUN_TEST(test_dedup_repeated_press_suppressed);
    RUN_TEST(test_dedup_state_transition_emitted);
    RUN_TEST(test_dedup_repeated_notifyinput_code_suppressed);
    RUN_TEST(test_dedup_button_stream_suppresses_matching_notifyinput_same_packet);
    RUN_TEST(test_dedup_notifyinput_hold_still_emitted_with_button_stream);
    RUN_TEST(test_dedup_button_stream_not_sticky_across_packets);
    RUN_TEST(test_dedup_table_fail_open_beyond_capacity);

    return UNITY_END();
}
