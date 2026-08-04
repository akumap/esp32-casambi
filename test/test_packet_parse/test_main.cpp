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

// 0x06 tests use UnitStateRecord (the protocol-level record, no fixture
// semantics). 0x08 tests use UnitStateInfo — the pair-list format has no
// flags/framing ambiguity, so it keeps the older, simpler struct.
static std::vector<UnitStateRecord> records;
static std::vector<UnitStateInfo> states;
static OperationEcho echo;

void setUp(void) {
    records.clear();
    states.clear();
    echo = OperationEcho();
}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// 0x06 status broadcast
// ---------------------------------------------------------------------------

// b8=0x00: state_len=1, priority=0. Minimal record.
void test_06_single_simple_record(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x00, 200 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(5, records[0].unitId);
    TEST_ASSERT_EQUAL(1, records[0].stateLen);
    TEST_ASSERT_EQUAL(0, records[0].priority);
    TEST_ASSERT_EQUAL(200, records[0].state[0]);
    TEST_ASSERT_TRUE(records[0].on);
    TEST_ASSERT_TRUE(records[0].online);
}

// flags=0x00: on/online both false — hardware-verified as the genuine
// offline/power-cycle pattern (docs/captures/2026-08-04-0x06-framing/).
void test_06_offline_flags(void) {
    const uint8_t pkt[] = { 7, 0x00, 0x00, 0 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_FALSE(records[0].online);
    TEST_ASSERT_FALSE(records[0].on);
}

// flags bit 4: an optional "extra" byte precedes the state — captured
// verbatim, not interpreted (unlike the old "stored_level" reading, this
// parser does not assume what the byte means).
void test_06_extra_byte_present(void) {
    const uint8_t pkt[] = { 5, 0x13, 0x00, 99 /*extra*/, 200 /*state[0]*/ };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_TRUE(records[0].hasExtra);
    TEST_ASSERT_EQUAL(99, records[0].extraByte);
    TEST_ASSERT_EQUAL(200, records[0].state[0]);
}

// flags bit 2: an optional "con" byte precedes the state.
void test_06_con_byte_present(void) {
    const uint8_t pkt[] = { 1, 0x04, 0x00, 0x11 /*con*/, 0x22 /*state[0]*/ };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_TRUE(records[0].hasCon);
    TEST_ASSERT_EQUAL(0x11, records[0].con);
    TEST_ASSERT_EQUAL(0x22, records[0].state[0]);
}

// flags bit 3: an optional "sid" byte precedes the state. UNVERIFIED on real
// hardware (never observed set) — implemented per the documented spec.
void test_06_sid_byte_present(void) {
    const uint8_t pkt[] = { 1, 0x08, 0x00, 0x33 /*sid*/, 0x44 /*state[0]*/ };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_TRUE(records[0].hasSid);
    TEST_ASSERT_EQUAL(0x33, records[0].sid);
    TEST_ASSERT_EQUAL(0x44, records[0].state[0]);
}

// All three optional fields plus padding together, in wire order
// con, sid, extra, state, padding. UNVERIFIED as a combination on real
// hardware — implemented per the documented spec regardless.
void test_06_all_optional_fields_and_padding(void) {
    const uint8_t pkt[] = {
        7,                  // unit
        0xDF,               // on,online,con,sid,extra set; padding_len=3 (bits 6-7 = 11)
        0x23,               // state_len=3, priority=3
        0x11, 0x22, 0x33,   // con, sid, extra
        0x40, 0x50, 0x60,   // state
        0xAA, 0xBB, 0xCC,   // padding
    };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    const UnitStateRecord& r = records[0];
    TEST_ASSERT_EQUAL(7, r.unitId);
    TEST_ASSERT_TRUE(r.on);
    TEST_ASSERT_TRUE(r.online);
    TEST_ASSERT_EQUAL(3, r.priority);
    TEST_ASSERT_EQUAL(3, r.stateLen);
    TEST_ASSERT_TRUE(r.hasCon);   TEST_ASSERT_EQUAL(0x11, r.con);
    TEST_ASSERT_TRUE(r.hasSid);   TEST_ASSERT_EQUAL(0x22, r.sid);
    TEST_ASSERT_TRUE(r.hasExtra); TEST_ASSERT_EQUAL(0x33, r.extraByte);
    TEST_ASSERT_EQUAL(0x40, r.state[0]);
    TEST_ASSERT_EQUAL(0x50, r.state[1]);
    TEST_ASSERT_EQUAL(0x60, r.state[2]);
    TEST_ASSERT_EQUAL(3, r.paddingLen);
    TEST_ASSERT_EQUAL(0xAA, r.padding[0]);
    TEST_ASSERT_EQUAL(0xBB, r.padding[1]);
    TEST_ASSERT_EQUAL(0xCC, r.padding[2]);
}

// state_len spans 1-16 (b8 high nibble 0-15). 3 is the largest seen on real
// hardware so far (Occhio Mito sospeso); 16 is UNVERIFIED but must not
// overflow the fixed-size state[] array.
void test_06_state_len_16(void) {
    uint8_t pkt[3 + 16];
    pkt[0] = 9; pkt[1] = 0x03; pkt[2] = 0xF3;   // state_len=16, priority=3
    for (int i = 0; i < 16; i++) pkt[3 + i] = (uint8_t)(i * 16 + 1);
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(16, records[0].stateLen);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL((uint8_t)(i * 16 + 1), records[0].state[i]);
}

// CRITICAL DISCRIMINATOR (write-up §10): under the old capability scheme,
// b8 == 0x03 exactly forced a phantom 5th "constant" byte regardless of
// flags, so this 4-byte packet would have been rejected as truncated. Under
// the corrected framing, b8's low nibble is just `priority` — this is a
// complete, valid 4-byte record.
void test_06_b8_0x03_is_not_special(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x03, 0x80 };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(1, records[0].stateLen);
    TEST_ASSERT_EQUAL(3, records[0].priority);
    TEST_ASSERT_EQUAL(0x80, records[0].state[0]);
}

// Demonstrates the actual fix: b8=0x44 was rejected outright by the old
// "unknown capability" check; every b8 is now a valid state_len/priority pair.
void test_06_formerly_unknown_cap_now_valid(void) {
    const uint8_t pkt[] = { 2, 0x03, 0x44, 10, 20, 30, 40, 50 };  // state_len=5, priority=4
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(5, records[0].stateLen);
    TEST_ASSERT_EQUAL(4, records[0].priority);
}

// on/online decode independently from flags bits 0/1. Real hardware has
// only ever shown bit0==bit1 (see docs/captures/2026-08-04-0x06-framing/) —
// this is a decode-correctness test of the bit extraction itself, not a
// hardware claim that the mixed combinations occur in practice.
void test_06_on_online_independent_flag_matrix(void) {
    struct Case { uint8_t flags; bool on; bool online; };
    const Case cases[] = {
        { 0x00, false, false },
        { 0x01, true,  false },
        { 0x02, false, true  },
        { 0x03, true,  true  },
    };
    for (const auto& c : cases) {
        const uint8_t pkt[] = { 1, c.flags, 0x00, 42 };
        TEST_ASSERT_EQUAL(ParseStatus::Complete,
                          packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
        TEST_ASSERT_EQUAL(1, records.size());
        TEST_ASSERT_EQUAL(c.on, records[0].on);
        TEST_ASSERT_EQUAL(c.online, records[0].online);
    }
}

// state_len spread: 1 (tested elsewhere), 2, 3 (tested elsewhere), 16 (tested
// elsewhere) — this fills in the explicit 2-byte case.
void test_06_state_len_2(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x13, 200, 60 };  // b8=0x13 -> state_len 2
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(2, records[0].stateLen);
    TEST_ASSERT_EQUAL(200, records[0].state[0]);
    TEST_ASSERT_EQUAL(60, records[0].state[1]);
}

void test_06_multiple_records(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,          // state_len 1
        2, 0x03, 0x23, 200, 60, 30,  // state_len 3
    };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(2, records.size());
    TEST_ASSERT_EQUAL(1, records[0].unitId);
    TEST_ASSERT_EQUAL(2, records[1].unitId);
}

// TOLERANCE: a valid record followed by a truncated one keeps the valid
// prefix (Partial) — the tail is dropped, never guessed at.
void test_06_valid_then_truncated_partial(void) {
    const uint8_t pkt[] = {
        1, 0x03, 0x00, 100,     // valid record, state_len 1
        2, 0x03, 0x23, 200,     // truncated state_len-3 record
    };
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_EQUAL(1, records[0].unitId);
    TEST_ASSERT_EQUAL(100, records[0].state[0]);
}

// TOLERANCE: trailing bytes after valid records keep the records (Partial)
void test_06_trailing_bytes_partial(void) {
    const uint8_t pkt[] = { 1, 0x03, 0x00, 100, 0xAA };
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
}

void test_06_too_short_rejected(void) {
    const uint8_t pkt[] = { 1, 0x03, 0x00 };
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(ParseStatus::Malformed,
                      packetparse::parseStatusBroadcast(pkt, 0, records));
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

// The previous parser delegated into the 0x06 record format when byte 2
// "looked like a valid capability" — that heuristic no longer has a
// principled basis (every byte-2 value is now valid under the corrected
// 0x06 framing, see packet_parse.h), so 0x08 is always read as plain pairs
// now, even for a payload shaped like an old-style 0x06 record.
void test_08_always_pairs_no_delegation(void) {
    const uint8_t pkt[] = { 5, 0x03, 0x13, 200, 60 };  // 5 bytes = 2 pairs + 1 odd byte
    ParseDiag diag;
    TEST_ASSERT_EQUAL(ParseStatus::Partial,
                      packetparse::parseUnitStateUpdate(pkt, sizeof(pkt), states, &diag));
    TEST_ASSERT_EQUAL(2, states.size());
    TEST_ASSERT_EQUAL(5, states[0].unitId);
    TEST_ASSERT_EQUAL(0x03, states[0].level);
    TEST_ASSERT_EQUAL(0x13, states[1].unitId);
    TEST_ASSERT_EQUAL(200, states[1].level);
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

        ParseStatus st = packetparse::parseStatusBroadcast(buf, len, records);
        if (st == ParseStatus::Malformed) TEST_ASSERT_TRUE(records.empty());
        else                              TEST_ASSERT_FALSE(records.empty());

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
// boundaries parse Complete. Record 2 uses flags=0x17 (con + extra), the
// real-world shape seen on single-dimmer fixtures (type 1422) in
// docs/captures/2026-08-04-0x06-framing/.
void test_fuzz_truncated_and_mutated_valid_packets(void) {
    const uint8_t base[] = {
        1, 0x03, 0x00, 100,                // state_len 1, 4 bytes
        2, 0x17, 0x03, 0x80, 0x71, 128,    // con+extra, state_len 1, 6 bytes
        3, 0x03, 0x23, 200, 60, 30,        // state_len 3, 6 bytes
    };
    const size_t baseLen = sizeof(base);

    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(base, baseLen, records));
    TEST_ASSERT_EQUAL(3, records.size());

    // Record boundaries lie at offsets 4, 10 and 16. Cuts on a boundary are
    // Complete; cuts inside record 1 (0-3) leave nothing usable (Malformed);
    // cuts inside later records keep the intact leading records (Partial).
    for (size_t cut = 0; cut < baseLen; cut++) {
        ParseStatus st = packetparse::parseStatusBroadcast(base, cut, records);
        if (cut == 4 || cut == 10) {
            TEST_ASSERT_EQUAL(ParseStatus::Complete, st);
            TEST_ASSERT_EQUAL(cut == 4 ? 1 : 2, records.size());
        } else if (cut < 4) {
            TEST_ASSERT_EQUAL(ParseStatus::Malformed, st);
            TEST_ASSERT_TRUE(records.empty());
        } else {
            TEST_ASSERT_EQUAL(ParseStatus::Partial, st);
            TEST_ASSERT_EQUAL(cut < 10 ? 1 : 2, records.size());
        }
    }

    // Single-byte mutations: whatever the outcome, the contract must hold —
    // Malformed is empty, everything else carries only fully-parsed records.
    uint32_t seed = 0xDEADBEEF;
    uint8_t buf[sizeof(base)];
    for (int iter = 0; iter < 5000; iter++) {
        memcpy(buf, base, baseLen);
        buf[lcg(seed) % baseLen] = (uint8_t)(lcg(seed) >> 24);
        ParseStatus st = packetparse::parseStatusBroadcast(buf, baseLen, records);
        if (st == ParseStatus::Malformed) TEST_ASSERT_TRUE(records.empty());
        else                              TEST_ASSERT_FALSE(records.empty());
    }
}

// ---------------------------------------------------------------------------
// Golden vectors — real 0x06 payloads captured from a live network, verified
// against BOTH the old capability-heuristic framing and the corrected
// state_len/priority framing (they agree byte-for-byte on every real packet
// captured so far — see docs/captures/2026-08-04-0x06-framing/). `on`/
// `online` values below are the device's own flags bits, NOT re-derived from
// level — this is why unit 1 in the multi-unit snapshot is the one place
// these tests assert something the OLD parser got wrong (on: false when it
// should be true — see comment there).
// ---------------------------------------------------------------------------

static void expect_one(const uint8_t* pkt, size_t len, uint8_t id, uint8_t level,
                       uint8_t aux1, bool hasAux1, uint8_t aux2, bool hasAux2) {
    records.clear();
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, len, records));
    TEST_ASSERT_EQUAL(1, records.size());
    const UnitStateRecord& r = records[0];
    TEST_ASSERT_EQUAL(id, r.unitId);
    TEST_ASSERT_EQUAL(level, r.state[0]);
    TEST_ASSERT_EQUAL(hasAux1, r.stateLen >= 2);
    if (hasAux1) TEST_ASSERT_EQUAL(aux1, r.state[1]);
    TEST_ASSERT_EQUAL(hasAux2, r.stateLen >= 3);
    if (hasAux2) TEST_ASSERT_EQUAL(aux2, r.state[2]);
}

void test_06_golden_unit7_sweep(void) {
    // on: level 255, vertical 127, temp 0
    const uint8_t on[]     = { 0x07,0x03,0x23,0xff,0x7f,0x00 };
    expect_one(on, sizeof(on), 7, 255, 127, true, 0, true);
    // dimmed 46%: flags 0x13 => extra byte present, level 117
    const uint8_t dim[]    = { 0x07,0x13,0x23,0x75,0x75,0x7f,0x00 };
    expect_one(dim, sizeof(dim), 7, 117, 127, true, 0, true);
    // temperature cold: aux2 = 255
    const uint8_t cold[]   = { 0x07,0x13,0x23,0x75,0x75,0x7f,0xff };
    expect_one(cold, sizeof(cold), 7, 117, 127, true, 255, true);
    // vertical 100%: aux1 = 255
    const uint8_t vhi[]    = { 0x07,0x13,0x23,0x75,0x75,0xff,0x00 };
    expect_one(vhi, sizeof(vhi), 7, 117, 255, true, 0, true);
    // vertical 0%: aux1 = 0
    const uint8_t vlo[]    = { 0x07,0x13,0x23,0x75,0x75,0x00,0x00 };
    expect_one(vlo, sizeof(vlo), 7, 117, 0, true, 0, true);
}

void test_06_golden_multiunit_snapshot(void) {
    // 49-byte connect snapshot, 9 records, consumed exactly (Complete).
    const uint8_t pkt[] = {
        0x01,0x13,0x13,0xfe,0x00,0x80,           // unit 1: extra=0xfe, state[0]=0, state[1]=128
        0x02,0x10,0x00,0x71,0x00,                // unit 2: extra=0x71, state[0]=0
        0x03,0x00,0x00,0x00,                     // unit 3: state[0]=0
        0x05,0x10,0x10,0xf7,0x00,0x00,           // unit 5: extra=0xf7, state[0]=0, state[1]=0
        0x07,0x03,0x23,0xff,0x7f,0x00,           // unit 7: state=[255,127,0]
        0x08,0x10,0x10,0xf7,0x00,0x00,           // unit 8: extra=0xf7, state[0]=0, state[1]=0
        0x09,0x10,0x10,0xfe,0x00,0x00,           // unit 9: extra=0xfe, state[0]=0, state[1]=0
        0x0a,0x10,0x00,0xfe,0x00,                // unit 10: extra=0xfe, state[0]=0
        0x0b,0x07,0x03,0x80,0xff                 // unit 11: con=0x80, state[0]=255
    };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(9, records.size());

    // ids in order
    const uint8_t ids[9] = { 1,2,3,5,7,8,9,10,11 };
    for (int i = 0; i < 9; i++) TEST_ASSERT_EQUAL(ids[i], records[i].unitId);

    // unit 1: online true; on true (flags bit 0) — REGRESSION GUARD: the old
    // level-derived heuristic reported on=false here (state[0]==0), but the
    // device's own flags say on=true (remembered on, output currently 0).
    // This exact packet is why the parser was rewritten — see header comment.
    TEST_ASSERT_TRUE(records[0].online);
    TEST_ASSERT_TRUE(records[0].on);
    TEST_ASSERT_EQUAL(2, records[0].stateLen);
    TEST_ASSERT_EQUAL(0, records[0].state[0]);
    TEST_ASSERT_EQUAL(128, records[0].state[1]);

    // unit 2/3: offline (flags=0x10/0x00 -> online false)
    TEST_ASSERT_FALSE(records[1].online);
    TEST_ASSERT_FALSE(records[1].on);
    TEST_ASSERT_FALSE(records[2].online);
    TEST_ASSERT_FALSE(records[2].on);

    // unit 5: offline, state[1] = 0
    TEST_ASSERT_FALSE(records[3].online);
    TEST_ASSERT_EQUAL(2, records[3].stateLen);
    TEST_ASSERT_EQUAL(0, records[3].state[1]);

    // unit 7: online, on, state = [255, 127, 0]
    TEST_ASSERT_TRUE(records[4].online);
    TEST_ASSERT_TRUE(records[4].on);
    TEST_ASSERT_EQUAL(3, records[4].stateLen);
    TEST_ASSERT_EQUAL(255, records[4].state[0]);
    TEST_ASSERT_EQUAL(127, records[4].state[1]);
    TEST_ASSERT_EQUAL(0, records[4].state[2]);

    // unit 11: online, on, state[0] = 255, con byte present (0x80)
    TEST_ASSERT_TRUE(records[8].online);
    TEST_ASSERT_TRUE(records[8].on);
    TEST_ASSERT_EQUAL(255, records[8].state[0]);
    TEST_ASSERT_EQUAL(1, records[8].stateLen);
    TEST_ASSERT_TRUE(records[8].hasCon);
    TEST_ASSERT_EQUAL(0x80, records[8].con);
}

// Golden vector from docs/captures/2026-08-04-0x06-framing/02_offline-powercycle_serial.log:
// a genuine mains power-cycle capture (not a BLE command) — the one scenario
// no earlier capture had. Two packets for the SAME unit (3, type 1422),
// consecutive counters 0x80000043 -> 0x8000004b, con byte and state[0]
// IDENTICAL (0x80 / 0xff = 255) in both — only `flags` changed, when the
// unit was mains power-cycled off between the two captures.
//
// REGRESSION GUARD for the online-heuristic bug fix: the OLD "online" read
// (flags low nibble != 0) is WRONG on the second packet — flags=0x04 has a
// nonzero low nibble, so it would report online=true for a unit that had
// just gone dark. The flags-bit-1 interpretation correctly reports false.
void test_06_golden_online_to_offline_transition_unit3(void) {
    const uint8_t onlinePkt[]  = { 0x03, 0x07, 0x03, 0x80, 0xff };  // counter 0x80000043
    const uint8_t offlinePkt[] = { 0x03, 0x04, 0x03, 0x80, 0xff };  // counter 0x8000004b

    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(onlinePkt, sizeof(onlinePkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_TRUE(records[0].online);
    TEST_ASSERT_TRUE(records[0].on);
    TEST_ASSERT_TRUE(records[0].hasCon);
    TEST_ASSERT_EQUAL(0x80, records[0].con);
    TEST_ASSERT_EQUAL(255, records[0].state[0]);

    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(offlinePkt, sizeof(offlinePkt), records));
    TEST_ASSERT_EQUAL(1, records.size());
    TEST_ASSERT_FALSE(records[0].online);   // <-- old heuristic said true here
    TEST_ASSERT_FALSE(records[0].on);
    TEST_ASSERT_TRUE(records[0].hasCon);
    TEST_ASSERT_EQUAL(0x80, records[0].con);     // con byte unchanged
    TEST_ASSERT_EQUAL(255, records[0].state[0]); // stale state byte unchanged
}

// Golden vector: the first organically-captured multi-record 0x06 packet
// (units 5 and 8 changed together, bundled by the gateway) from the same
// live capture — con/extra framing does not desynchronize the second record.
void test_06_golden_organic_multirecord(void) {
    const uint8_t pkt[] = {
        0x05, 0x13, 0x13, 0x0c, 0x0c, 0xff,   // unit 5: extra=0x0c, state=[12,255]
        0x08, 0x13, 0x13, 0x0a, 0x0a, 0xff,   // unit 8: extra=0x0a, state=[10,255]
    };
    TEST_ASSERT_EQUAL(ParseStatus::Complete,
                      packetparse::parseStatusBroadcast(pkt, sizeof(pkt), records));
    TEST_ASSERT_EQUAL(2, records.size());

    TEST_ASSERT_EQUAL(5, records[0].unitId);
    TEST_ASSERT_TRUE(records[0].hasExtra);
    TEST_ASSERT_EQUAL(0x0c, records[0].extraByte);
    TEST_ASSERT_EQUAL(2, records[0].stateLen);
    TEST_ASSERT_EQUAL(12, records[0].state[0]);
    TEST_ASSERT_EQUAL(255, records[0].state[1]);

    TEST_ASSERT_EQUAL(8, records[1].unitId);
    TEST_ASSERT_TRUE(records[1].hasExtra);
    TEST_ASSERT_EQUAL(0x0a, records[1].extraByte);
    TEST_ASSERT_EQUAL(2, records[1].stateLen);
    TEST_ASSERT_EQUAL(10, records[1].state[0]);
    TEST_ASSERT_EQUAL(255, records[1].state[1]);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_06_single_simple_record);
    RUN_TEST(test_06_offline_flags);
    RUN_TEST(test_06_extra_byte_present);
    RUN_TEST(test_06_con_byte_present);
    RUN_TEST(test_06_sid_byte_present);
    RUN_TEST(test_06_all_optional_fields_and_padding);
    RUN_TEST(test_06_state_len_16);
    RUN_TEST(test_06_b8_0x03_is_not_special);
    RUN_TEST(test_06_formerly_unknown_cap_now_valid);
    RUN_TEST(test_06_on_online_independent_flag_matrix);
    RUN_TEST(test_06_state_len_2);
    RUN_TEST(test_06_multiple_records);
    RUN_TEST(test_06_valid_then_truncated_partial);
    RUN_TEST(test_06_trailing_bytes_partial);
    RUN_TEST(test_06_too_short_rejected);
    RUN_TEST(test_06_golden_unit7_sweep);
    RUN_TEST(test_06_golden_multiunit_snapshot);
    RUN_TEST(test_06_golden_online_to_offline_transition_unit3);
    RUN_TEST(test_06_golden_organic_multirecord);

    RUN_TEST(test_07_exact_length_ok);
    RUN_TEST(test_07_empty_payload_ok);
    RUN_TEST(test_07_declared_longer_rejected);
    RUN_TEST(test_07_trailing_bytes_partial);
    RUN_TEST(test_07_header_truncated_rejected);

    RUN_TEST(test_08_pairs_ok);
    RUN_TEST(test_08_odd_trailing_byte_partial);
    RUN_TEST(test_08_too_short_rejected);
    RUN_TEST(test_08_always_pairs_no_delegation);

    RUN_TEST(test_fuzz_random_buffers);
    RUN_TEST(test_fuzz_truncated_and_mutated_valid_packets);

    return UNITY_END();
}
