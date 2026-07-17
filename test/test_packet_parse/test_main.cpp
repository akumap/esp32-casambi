/**
 * Host-side unit and fuzz tests for the tolerant BLE packet parsers (P2 #8).
 *
 * These run on the build host via `pio test -e native` — no ESP32 required.
 * They pin down the three-state contract of packet_parse.h: Complete = whole
 * payload understood; Partial = the well-formed prefix IS returned and the
 * undecoded tail dropped (payloads are CMAC-verified, so unknown bytes are
 * protocol elements we do not decode yet — the known parts must keep
 * working); Malformed = nothing usable, output empty. A parser never
 * fabricates state from bytes it did not understand.
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

// TOLERANCE: an unexpected constant-byte value does not change the record
// length, so the record still parses — the byte may be an undecoded flag.
void test_06_cap03_constant_other_value_tolerated(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x03, 0x7F, 128 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(128, states[0].level);
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

// TOLERANCE: a valid record followed by a truncated one keeps the valid
// prefix (Partial) — the tail is dropped, never guessed at.
void test_06_valid_then_truncated_partial(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,     // valid record
        2, 0x03, 0x23, 200,     // truncated 2-aux record
    };
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(1, states[0].unitId);
    TEST_ASSERT_EQUAL(100, states[0].level);
}

// TOLERANCE: an unknown capability means an unknown record length — keep the
// understood prefix, drop the rest.
void test_06_valid_then_unknown_cap_partial(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,     // valid record
        2, 0x03, 0x44, 200,     // unknown capability 0x44
    };
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states, &diag));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(1, states[0].unitId);
    TEST_ASSERT_EQUAL(6, diag.offset);
}

// ...but when already the FIRST record is not understood there is nothing to
// keep: Malformed, empty.
void test_06_unknown_cap_first_record_malformed(void) {
    const uint8_t pkt[] = { 2, 0x03, 0x44, 200 };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_TRUE(states.empty());
}

// TOLERANCE: trailing bytes after valid records keep the records (Partial)
void test_06_trailing_bytes_partial(void) {
    const uint8_t pkt[] = { 1, 0x03, 0x00, 100, 0xAA };
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), states));
    TEST_ASSERT_EQUAL(1, states.size());
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

// Declared length > received payload stays Malformed: the packet arrived
// MAC-verified, so our length-field interpretation does not fit it — a
// truncated operation payload must not be acted on (was: copy what is there
// and report success).
void test_07_declared_longer_rejected(void) {
    auto pkt = makeEcho(5, 2);
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo));
    TEST_ASSERT_TRUE(echo.payload.empty());
}

// TOLERANCE: extra bytes beyond the declared length are dropped (Partial) —
// a newer protocol revision may append fields after the payload.
void test_07_trailing_bytes_partial(void) {
    auto pkt = makeEcho(1, 3);
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseOperationEcho(pkt.data(), pkt.size(), echo, &diag));
    TEST_ASSERT_EQUAL(1, echo.payload.size());   // exactly the declared length
    TEST_ASSERT_EQUAL(0x40, echo.payload[0]);
    TEST_ASSERT_EQUAL(10, diag.offset);          // 9-byte header + 1 declared
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

// TOLERANCE: an odd trailing byte is dropped, the complete pairs are kept
// (Partial) — and no longer silently, the diag/counter records it.
void test_08_odd_trailing_byte_partial(void) {
    const uint8_t pkt[] = { 1, 100, 2 };
    // len 3 < 4 → cannot be 0x06 format → pair list with one trailing byte
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states, &diag));
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_EQUAL(1, states[0].unitId);
    TEST_ASSERT_EQUAL(100, states[0].level);
    TEST_ASSERT_EQUAL(2, diag.offset);
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
// honor the three-state contract: Complete/Partial ⇒ usable non-empty
// output, Malformed ⇒ empty output. A parser never returns records it did
// not fully understand.
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
        if (st == ParseStatus::Malformed) TEST_ASSERT_TRUE(states.empty());
        else                              TEST_ASSERT_FALSE(states.empty());

        st = packetparse::parseUnitStateUpdate(buf, len, states);
        if (st == ParseStatus::Malformed) TEST_ASSERT_TRUE(states.empty());
        else                              TEST_ASSERT_FALSE(states.empty());

        st = packetparse::parseOperationEcho(buf, len, echo);
        if (st == ParseStatus::Complete) {
            TEST_ASSERT_EQUAL(len - 9, echo.payload.size());   // declared == actual
        } else if (st == ParseStatus::Partial) {
            TEST_ASSERT_TRUE(echo.payload.size() < len - 9);   // declared < actual
        } else {
            TEST_ASSERT_TRUE(echo.payload.empty());
        }
    }
}

// Fuzz around valid packets: build a well-formed 0x06 packet, then truncate
// or mutate single bytes. Truncations degrade gracefully: the intact leading
// records are kept (Partial), never guessed-at fragments; cuts at record
// boundaries parse Complete.
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

    // Record boundaries lie at offsets 4, 10 and 16. Cuts on a boundary are
    // Complete; cuts inside record 1 (0-3) leave nothing usable (Malformed);
    // cuts inside later records keep the intact leading records (Partial).
    for (size_t cut = 0; cut < baseLen; cut++) {
        ParseStatus st = packetparse::parseStatusBroadcast(base, cut, states);
        if (cut == 4 || cut == 10) {
            TEST_ASSERT_EQUAL(ParseStatus::Complete, st);
            TEST_ASSERT_EQUAL(cut == 4 ? 1 : 2, states.size());
        } else if (cut < 4) {
            TEST_ASSERT_EQUAL(ParseStatus::Malformed, st);
            TEST_ASSERT_TRUE(states.empty());
        } else {
            TEST_ASSERT_EQUAL(ParseStatus::Partial, st);
            TEST_ASSERT_EQUAL(cut < 10 ? 1 : 2, states.size());
        }
    }

    // Single-byte mutations: whatever the outcome, the contract must hold —
    // Malformed is empty, everything else carries only fully-parsed records.
    uint32_t seed = 0xDEADBEEF;
    uint8_t buf[sizeof(base)];
    for (int iter = 0; iter < 5000; iter++) {
        memcpy(buf, base, baseLen);
        buf[lcg(seed) % baseLen] = (uint8_t)(lcg(seed) >> 24);
        ParseStatus st = packetparse::parseStatusBroadcast(buf, baseLen, states);
        if (st == ParseStatus::Malformed) TEST_ASSERT_TRUE(states.empty());
        else                              TEST_ASSERT_FALSE(states.empty());
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_06_single_simple_record);
    RUN_TEST(test_06_offline_source);
    RUN_TEST(test_06_stored_level_skipped);
    RUN_TEST(test_06_cap03_constant_ok);
    RUN_TEST(test_06_cap03_constant_other_value_tolerated);
    RUN_TEST(test_06_one_aux);
    RUN_TEST(test_06_two_aux);
    RUN_TEST(test_06_multiple_records);
    RUN_TEST(test_06_valid_then_truncated_partial);
    RUN_TEST(test_06_valid_then_unknown_cap_partial);
    RUN_TEST(test_06_unknown_cap_first_record_malformed);
    RUN_TEST(test_06_trailing_bytes_partial);
    RUN_TEST(test_06_too_short_rejected);

    RUN_TEST(test_07_exact_length_ok);
    RUN_TEST(test_07_empty_payload_ok);
    RUN_TEST(test_07_declared_longer_rejected);
    RUN_TEST(test_07_trailing_bytes_partial);
    RUN_TEST(test_07_header_truncated_rejected);

    RUN_TEST(test_08_pairs_ok);
    RUN_TEST(test_08_odd_trailing_byte_partial);
    RUN_TEST(test_08_too_short_rejected);
    RUN_TEST(test_08_delegates_to_06_format);
    RUN_TEST(test_08_bad_06_format_rejected);

    RUN_TEST(test_fuzz_random_buffers);
    RUN_TEST(test_fuzz_truncated_and_mutated_valid_packets);

    return UNITY_END();
}
