/**
 * Host-side unit tests for ConsoleRingBuffer (console_ring_buffer.h) — the
 * ring buffer console_out.cpp mirrors output into for the telnet console
 * (docs/konzept-tcp-konsole.md, section 4.2).
 */

#include <unity.h>
#include <cstring>

#include "console_ring_buffer.h"

void setUp(void) {}
void tearDown(void) {}

static void test_basic_round_trip() {
    uint8_t storage[16];
    ConsoleRingBuffer rb(storage, sizeof(storage));

    rb.write((const uint8_t*)"hello", 5);
    TEST_ASSERT_EQUAL_UINT64(5, rb.written());
    TEST_ASSERT_EQUAL_UINT64(0, rb.oldestAvailable());

    uint64_t cursor = 0;
    uint8_t out[16] = {0};
    size_t n = rb.read(&cursor, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_UINT64(5, cursor);
    TEST_ASSERT_EQUAL_MEMORY("hello", out, 5);

    // Nothing new: a repeat read at the same cursor returns 0.
    n = rb.read(&cursor, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

static void test_wraparound_preserves_data() {
    uint8_t storage[8];
    ConsoleRingBuffer rb(storage, sizeof(storage));

    // Write more than capacity across multiple calls so the internal
    // position wraps mid-buffer at least once.
    rb.write((const uint8_t*)"ABCDE", 5);
    rb.write((const uint8_t*)"FGHIJ", 5);   // total 10 written, capacity 8

    TEST_ASSERT_EQUAL_UINT64(10, rb.written());
    TEST_ASSERT_EQUAL_UINT64(2, rb.oldestAvailable());   // 10 - 8

    uint64_t cursor = rb.oldestAvailable();
    uint8_t out[8] = {0};
    size_t n = rb.read(&cursor, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(8, n);
    // Bytes 2..9 of "ABCDEFGHIJ" = "CDEFGHIJ"
    TEST_ASSERT_EQUAL_MEMORY("CDEFGHIJ", out, 8);
    TEST_ASSERT_EQUAL_UINT64(10, cursor);
}

static void test_overflow_drops_oldest_and_reports() {
    uint8_t storage[4];
    ConsoleRingBuffer rb(storage, sizeof(storage));

    uint64_t cursor = 0;   // reader starts at the very beginning
    rb.write((const uint8_t*)"AB", 2);
    rb.write((const uint8_t*)"CDEF", 4);   // total 6 written, capacity 4 -> "AB" overwritten

    TEST_ASSERT_EQUAL_UINT64(2, rb.oldestAvailable());

    uint64_t dropped = 0;
    uint8_t out[4] = {0};
    size_t n = rb.read(&cursor, out, sizeof(out), &dropped);
    TEST_ASSERT_EQUAL_UINT64(2, dropped);      // "AB" was lost
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_MEMORY("CDEF", out, 4);
    TEST_ASSERT_EQUAL_UINT64(6, cursor);
}

static void test_single_write_larger_than_capacity_keeps_tail() {
    uint8_t storage[4];
    ConsoleRingBuffer rb(storage, sizeof(storage));

    rb.write((const uint8_t*)"ABCDEFGH", 8);   // only "EFGH" can survive
    TEST_ASSERT_EQUAL_UINT64(8, rb.written());
    TEST_ASSERT_EQUAL_UINT64(4, rb.oldestAvailable());

    uint64_t cursor = 0;
    uint8_t out[4] = {0};
    uint64_t dropped = 0;
    size_t n = rb.read(&cursor, out, sizeof(out), &dropped);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_MEMORY("EFGH", out, 4);
    TEST_ASSERT_EQUAL_UINT64(4, dropped);
}

static void test_independent_cursors_see_consistent_data() {
    uint8_t storage[8];
    ConsoleRingBuffer rb(storage, sizeof(storage));

    rb.write((const uint8_t*)"1234", 4);
    uint64_t early = 0;
    uint64_t late = rb.written();   // "just connected" reader: starts at the end

    rb.write((const uint8_t*)"5678", 4);

    uint8_t outEarly[8] = {0};
    size_t nEarly = rb.read(&early, outEarly, sizeof(outEarly));
    TEST_ASSERT_EQUAL_size_t(8, nEarly);
    TEST_ASSERT_EQUAL_MEMORY("12345678", outEarly, 8);

    uint8_t outLate[8] = {0};
    size_t nLate = rb.read(&late, outLate, sizeof(outLate));
    TEST_ASSERT_EQUAL_size_t(4, nLate);
    TEST_ASSERT_EQUAL_MEMORY("5678", outLate, 4);
}

static void test_maxlen_caps_a_single_read() {
    uint8_t storage[16];
    ConsoleRingBuffer rb(storage, sizeof(storage));
    rb.write((const uint8_t*)"0123456789", 10);

    uint64_t cursor = 0;
    uint8_t out[3] = {0};
    size_t n = rb.read(&cursor, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_MEMORY("012", out, 3);
    TEST_ASSERT_EQUAL_UINT64(3, cursor);

    n = rb.read(&cursor, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_MEMORY("345", out, 3);
}

static void test_zero_length_write_is_noop() {
    uint8_t storage[4];
    ConsoleRingBuffer rb(storage, sizeof(storage));
    rb.write(nullptr, 0);
    TEST_ASSERT_EQUAL_UINT64(0, rb.written());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_basic_round_trip);
    RUN_TEST(test_wraparound_preserves_data);
    RUN_TEST(test_overflow_drops_oldest_and_reports);
    RUN_TEST(test_single_write_larger_than_capacity_keeps_tail);
    RUN_TEST(test_independent_cursors_see_consistent_data);
    RUN_TEST(test_maxlen_caps_a_single_read);
    RUN_TEST(test_zero_length_write_is_noop);
    return UNITY_END();
}
