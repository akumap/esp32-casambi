/**
 * Host-side unit and fuzz tests for the strict BLE packet parsers (P2 #8).
 *
 * These run on the build host via `pio test -e native` — no ESP32 required.
 * They pin down the strict all-or-nothing contract of packet_parse.h: a
 * parser returns Complete only when the whole payload was consumed exactly,
 * and on Malformed the output is empty (a corrupted packet can never apply a
 * partial state update).
 */

#include <unity.h>
#include <vector>
#include <cstdint>
#include <cstring>

#include "ble/packet_parse.h"

using packetparse::ParseStatus;
using packetparse::ParseDiag;

static std::vector<UnitStateInfo> states;
static OperationEcho echo;

void setUp(void) {
    states.clear();
    echo = OperationEcho();
}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 0x06 status broadcast
// ---------------------------------------------------------------------------

// cap 0x00, source 0x3 (software): [unitId][flags][cap][level]
void test_06_single_simple_record(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x00, 200 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(5, states[0].unitId);
    TEST_ASSERT_EQUAL(200, states[0].level);
    TEST_ASSERT_TRUE(states[0].hasLevel);
    TEST_ASSERT_TRUE(states[0].on);
    TEST_ASSERT_TRUE(states[0].online);
    TEST_ASSERT_FALSE(states[0].hasVertical);
    TEST_ASSERT_FALSE(states[0].hasColorTemp);
}

// source nibble 0x0 = physical/offline
void test_06_offline_source(void) {
    const uint8_t pkt[] = { 7, 0x00, 0x00, 0 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_FALSE(states[0].online);
    TEST_ASSERT_FALSE(states[0].on);
}

// flags bit 4: stored_level byte precedes the current level
void test_06_stored_level_skipped(void) {
    const uint8_t pkt[] = { 5, 0x13, 0x00, 99 /*stored*/, 200 /*current*/ };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(200, states[0].level);
}

// cap 0x03 carries the 0x80 constant byte
void test_06_cap03_constant_ok(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x03, 0x80, 128 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(128, states[0].level);
}

// NEW strict rule: the constant byte must actually be 0x80
void test_06_cap03_constant_wrong_rejected(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x03, 0x7F, 128 };
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states, &diag));
    TEST_ASSERT_TRUE(states.empty());
    TEST_ASSERT_EQUAL(3, diag.offset);
}

// cap 0x13: one aux channel
void test_06_one_aux(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x13, 200, 60 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_TRUE(states[0].hasVertical);
    TEST_ASSERT_EQUAL(60, states[0].vertical);
    TEST_ASSERT_FALSE(states[0].hasColorTemp);
}

// cap 0x23: two aux channels
void test_06_two_aux(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x23, 200, 60, 30 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_TRUE(states[0].hasVertical);
    TEST_ASSERT_TRUE(states[0].hasColorTemp);
    TEST_ASSERT_EQUAL(60, states[0].vertical);
    TEST_ASSERT_EQUAL(30, states[0].colorTemp);
}

void test_06_multiple_records(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,          // simple
        2, 0x03, 0x23, 200, 60, 30,  // 2 aux
    };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(2, states.size());
    TEST_ASSERT_EQUAL(1, states[0].unitId);
    TEST_ASSERT_EQUAL(2, states[1].unitId);
}

// NEW strict rule: a valid record followed by garbage rejects the WHOLE
// packet — no partial state update.
void test_06_valid_then_truncated_rejected(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,     // valid record
        2, 0x03, 0x23, 200,     // truncated 2-aux record
    };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_TRUE(states.empty());
}

void test_06_valid_then_bad_cap_rejected(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,     // valid record
        2, 0x03, 0x44, 200,     // invalid capability 0x44
    };
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states, &diag));
    TEST_ASSERT_TRUE(states.empty());
    TEST_ASSERT_EQUAL(6, diag.offset);
}

// NEW strict rule: trailing bytes (< header size) reject the packet
void test_06_trailing_bytes_rejected(void) {
    const uint8_t pkt[] = { 1, 0x03, 0x00, 100, 0xAA };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_TRUE(states.empty());
}

void test_06_too_short_rejected(void) {
    const uint8_t pkt[] = { 1, 0x03, 0x00 };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, 0, states));
}

// ---------------------------------------------------------------------------
// 0x07 operation echo
// ---------------------------------------------------------------------------

// Build a well-formed echo: header declares `declared`, buffer carries `actual`
// payload bytes.
static std::vector<uint8_t> makeEcho(uint16_t declared, size_t actual,
                                     uint8_t opcode = 1,
                                     uint8_t targetId = 9, uint8_t targetType = 0x01) {
    std::vector<uint8_t> pkt;
    uint16_t flags = (uint16_t)((5u << 11) | (declared & 0x07FF));
    pkt.push_back((flags >> 8) & 0xFF);
    pkt.push_back(flags & 0xFF);
    pkt.push_back(opcode);
    pkt.push_back(0); pkt.push_back(1);        // origin
    pkt.push_back(targetId); pkt.push_back(targetType);
    pkt.push_back(0); pkt.push_back(0);        // reserved
    for (size_t i = 0; i < actual; i++) pkt.push_back((uint8_t)(0x40 + i));
    return pkt;
}

void test_07_exact_length_ok(void) {
    auto pkt = makeEcho(2, 2);
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo));
    TEST_ASSERT_EQUAL(1, echo.opcode);
    TEST_ASSERT_EQUAL(9, echo.targetId);
    TEST_ASSERT_EQUAL(0x01, echo.targetType);
    TEST_ASSERT_EQUAL(2, echo.payload.size());
    TEST_ASSERT_EQUAL(0x40, echo.payload[0]);
}

void test_07_empty_payload_ok(void) {
    auto pkt = makeEcho(0, 0);
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo));
    TEST_ASSERT_TRUE(echo.payload.empty());
}

// NEW strict rule: declared length > received payload → Malformed (was:
// copy what is there and report success)
void test_07_declared_longer_rejected(void) {
    auto pkt = makeEcho(5, 2);
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo));
    TEST_ASSERT_TRUE(echo.payload.empty());
}

// NEW strict rule: extra bytes beyond the declared length → Malformed
void test_07_declared_shorter_rejected(void) {
    auto pkt = makeEcho(1, 3);
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo));
}

void test_07_header_truncated_rejected(void) {
    auto pkt = makeEcho(0, 0);
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseOperationEcho(pkt.data(), 8, echo));
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseOperationEcho(pkt.data(), 0, echo));
}

// ---------------------------------------------------------------------------
// 0x08 unit state update
// ---------------------------------------------------------------------------

void test_08_pairs_ok(void) {
    const uint8_t pkt[] = { 1, 100, 2, 200 };
    // data[2] == 2 is not a valid cap (low nibble 2) → pair fallback
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(2, states.size());
    TEST_ASSERT_EQUAL(1, states[0].unitId);
    TEST_ASSERT_EQUAL(100, states[0].level);
    TEST_ASSERT_EQUAL(2, states[1].unitId);
    TEST_ASSERT_EQUAL(200, states[1].level);
}

// NEW strict rule: an odd trailing byte rejects the packet (was: silently
// ignored)
void test_08_odd_trailing_byte_rejected(void) {
    const uint8_t pkt[] = { 1, 100, 2 };
    // len 3 < 4 → cannot be 0x06 format, and odd → Malformed
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states));
    TEST_ASSERT_TRUE(states.empty());
}

void test_08_too_short_rejected(void) {
    const uint8_t pkt[] = { 1 };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states));
}

// byte 2 announcing a valid cap routes into the strict 0x06 record parser
void test_08_delegates_to_06_format(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x13, 200, 60 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_TRUE(states[0].hasVertical);
}

// ...and a packet that announces the 0x06 format but violates it is rejected
// as a whole (no silent fall back to pair interpretation)
void test_08_bad_06_format_rejected(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x23, 200 };  // 2-aux record truncated
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states));
    TEST_ASSERT_TRUE(states.empty());
}

// ---------------------------------------------------------------------------
// Fuzz: random, truncated and mutated packets must never crash and must
// honor the all-or-nothing contract.
// ---------------------------------------------------------------------------

// Deterministic 32-bit LCG (Numerical Recipes constants) — reproducible runs.
static uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

void test_fuzz_random_buffers(void) {
    uint32_t seed = 0xC0FFEE01;
    uint8_t buf[64];
    for (int iter = 0; iter < 20000; iter++) {
        size_t len = lcg(seed) % (sizeof(buf) + 1);
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(lcg(seed) >> 24);

        ParseStatus st = packetparse::parseStatusBroadcast(buf, len, states);
        if (st == ParseStatus::Complete) TEST_ASSERT_FALSE(states.empty());
        else                             TEST_ASSERT_TRUE(states.empty());

        st = packetparse::parseUnitStateUpdate(buf, len, states);
        if (st == ParseStatus::Complete) TEST_ASSERT_FALSE(states.empty());
        else                             TEST_ASSERT_TRUE(states.empty());

        st = packetparse::parseOperationEcho(buf, len, echo);
        if (st == ParseStatus::Complete) {
            // declared length must have matched exactly
            TEST_ASSERT_EQUAL(len - 9, echo.payload.size());
        } else {
            TEST_ASSERT_TRUE(echo.payload.empty());
        }
    }
}

// Fuzz around valid packets: build a well-formed 0x06 packet, then truncate
// or mutate single bytes. Every truncation must be rejected; mutations must
// never produce a partial result (either Complete with records or empty).
void test_fuzz_truncated_and_mutated_valid_packets(void) {
    const uint8_t base[] = {
        1, 0x03, 0x00, 100,
        2, 0x13, 0x03, 0x80, 55, 128,   // cap 0x03 + stored_level
        3, 0x03, 0x23, 200, 60, 30,
    };
    const size_t baseLen = sizeof(base);

    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(base, baseLen, states));
    TEST_ASSERT_EQUAL(3, states.size());

    // Every proper prefix must be rejected outright (records end exactly at
    // offsets 4, 10 and 16 — but a strict parser rejects even those cuts'
    // siblings; prefixes that ARE valid record sequences parse Complete).
    for (size_t cut = 0; cut < baseLen; cut++) {
        ParseStatus st = packetparse::parseStatusBroadcast(base, cut, states);
        if (cut == 4 || cut == 10) {
            TEST_ASSERT_EQUAL(ParseStatus::Complete, st);   // valid record boundary
        } else {
            TEST_ASSERT_EQUAL(ParseStatus::Malformed, st);
            TEST_ASSERT_TRUE(states.empty());
        }
    }

    // Single-byte mutations: all-or-nothing must hold for every outcome.
    uint32_t seed = 0xDEADBEEF;
    uint8_t buf[sizeof(base)];
    for (int iter = 0; iter < 5000; iter++) {
        memcpy(buf, base, baseLen);
        buf[lcg(seed) % baseLen] = (uint8_t)(lcg(seed) >> 24);
        ParseStatus st = packetparse::parseStatusBroadcast(buf, baseLen, states);
        if (st == ParseStatus::Complete) TEST_ASSERT_FALSE(states.empty());
        else                             TEST_ASSERT_TRUE(states.empty());
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_06_single_simple_record);
    RUN_TEST(test_06_offline_source);
    RUN_TEST(test_06_stored_level_skipped);
    RUN_TEST(test_06_cap03_constant_ok);
    RUN_TEST(test_06_cap03_constant_wrong_rejected);
    RUN_TEST(test_06_one_aux);
    RUN_TEST(test_06_two_aux);
    RUN_TEST(test_06_multiple_records);
    RUN_TEST(test_06_valid_then_truncated_rejected);
    RUN_TEST(test_06_valid_then_bad_cap_rejected);
    RUN_TEST(test_06_trailing_bytes_rejected);
    RUN_TEST(test_06_too_short_rejected);

    RUN_TEST(test_07_exact_length_ok);
    RUN_TEST(test_07_empty_payload_ok);
    RUN_TEST(test_07_declared_longer_rejected);
    RUN_TEST(test_07_declared_shorter_rejected);
    RUN_TEST(test_07_header_truncated_rejected);

    RUN_TEST(test_08_pairs_ok);
    RUN_TEST(test_08_odd_trailing_byte_rejected);
    RUN_TEST(test_08_too_short_rejected);
    RUN_TEST(test_08_delegates_to_06_format);
    RUN_TEST(test_08_bad_06_format_rejected);

    RUN_TEST(test_fuzz_random_buffers);
    RUN_TEST(test_fuzz_truncated_and_mutated_valid_packets);

    return UNITY_END();
}
