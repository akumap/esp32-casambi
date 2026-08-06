/**
 * Host-side unit tests for TelnetLineParser (net/telnet_line.h) — the
 * Telnet IAC filter and line splitter used by the telnet console
 * (docs/konzept-tcp-konsole.md, section 4.3/E10).
 */

#include <unity.h>
#include <cstring>

#include "net/telnet_line.h"

using Result = TelnetLineParser::Result;

void setUp(void) {}
void tearDown(void) {}

static void feedAll(TelnetLineParser& p, const uint8_t* bytes, size_t n, Result* lastOut = nullptr) {
    Result last = Result::Ignored;
    for (size_t i = 0; i < n; i++) last = p.feed(bytes[i]);
    if (lastOut) *lastOut = last;
}

static void test_bare_lf_ends_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t in[] = "hello\n";
    Result last;
    feedAll(p, in, sizeof(in) - 1, &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("hello", p.line());
}

static void test_crlf_collapses_to_one_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t in[] = "hello\r\n";
    Result results[8];
    for (size_t i = 0; i < sizeof(in) - 1; i++) results[i] = p.feed(in[i]);
    // '\r' (index 5) ends the line; '\n' (index 6) must be swallowed, not a
    // second LineReady.
    TEST_ASSERT_EQUAL(Result::LineReady, results[5]);
    TEST_ASSERT_EQUAL(Result::Ignored, results[6]);
    TEST_ASSERT_EQUAL_STRING("hello", p.line());
}

static void test_cr_nul_collapses_to_one_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t in[] = {'h', 'i', '\r', '\0'};
    Result last;
    feedAll(p, in, sizeof(in), &last);
    TEST_ASSERT_EQUAL(Result::Ignored, last);   // the NUL half is swallowed
    TEST_ASSERT_EQUAL_STRING("hi", p.line());
}

static void test_bare_cr_then_new_content_starts_next_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t first[] = "ab\r";
    Result last;
    feedAll(p, first, sizeof(first) - 1, &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("ab", p.line());

    // 'c' is NOT the second half of a CRLF/CR-NUL pair; it starts a fresh line.
    Result r = p.feed('c');
    TEST_ASSERT_EQUAL(Result::Consumed, r);
    TEST_ASSERT_EQUAL_STRING("c", p.line());
}

static void test_new_line_auto_clears_previous_content() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t first[] = "first\n";
    feedAll(p, first, sizeof(first) - 1);
    TEST_ASSERT_EQUAL_STRING("first", p.line());

    const uint8_t second[] = "ab\n";
    Result last;
    feedAll(p, second, sizeof(second) - 1, &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    // Must be exactly "ab", not "abrst" (leftover tail from "first").
    TEST_ASSERT_EQUAL_STRING("ab", p.line());
}

static void test_backspace_edits_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(Result::Consumed, p.feed('a'));
    TEST_ASSERT_EQUAL(Result::Consumed, p.feed('b'));
    TEST_ASSERT_EQUAL(Result::Backspaced, p.feed(0x08));   // BS removes 'b'
    TEST_ASSERT_EQUAL(Result::Consumed, p.feed('c'));
    TEST_ASSERT_EQUAL(Result::LineReady, p.feed('\n'));
    TEST_ASSERT_EQUAL_STRING("ac", p.line());
}

static void test_del_behaves_like_backspace() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    p.feed('x');
    Result r = p.feed(0x7f);
    TEST_ASSERT_EQUAL(Result::Backspaced, r);
    TEST_ASSERT_EQUAL_size_t(0, p.length());
}

static void test_backspace_on_empty_line_is_safe_noop() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    Result r = p.feed(0x08);
    TEST_ASSERT_EQUAL(Result::Ignored, r);
    TEST_ASSERT_EQUAL_size_t(0, p.length());
}

static void test_iac_will_negotiation_stripped() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    // IAC WILL ECHO, then "hi\n"
    const uint8_t in[] = {255, 251, 1, 'h', 'i', '\n'};
    Result last;
    feedAll(p, in, sizeof(in), &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("hi", p.line());
}

static void test_iac_subnegotiation_stripped() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    // IAC SB <3 arbitrary bytes> IAC SE, then "x\n"
    const uint8_t in[] = {255, 250, 0x01, 0x02, 0x03, 255, 240, 'x', '\n'};
    Result last;
    feedAll(p, in, sizeof(in), &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("x", p.line());
}

static void test_overflow_stays_bounded_and_is_reported() {
    char buf[8];   // 7 usable chars + NUL
    TelnetLineParser p(buf, sizeof(buf));
    const char* longLine = "0123456789ABCDEF\n";
    Result last;
    feedAll(p, (const uint8_t*)longLine, strlen(longLine), &last);
    // The line is bounded to the buffer, but it must NOT be handed over as a
    // ready command: a truncated line is itself executable ("ulevel 5 200…"
    // cut short switches a real light), so the caller has to reject it.
    TEST_ASSERT_EQUAL(Result::LineTooLong, last);
    TEST_ASSERT_EQUAL_STRING("0123456", p.line());   // bounded to capacity-1
    TEST_ASSERT_EQUAL_size_t(7, p.length());
}

static void test_overflow_flag_clears_on_the_next_line() {
    char buf[8];
    TelnetLineParser p(buf, sizeof(buf));
    Result last;
    feedAll(p, (const uint8_t*)"0123456789\n", 11, &last);
    TEST_ASSERT_EQUAL(Result::LineTooLong, last);
    TEST_ASSERT_FALSE(p.overflowed());   // cleared with the line that carried it

    // The connection resynchronises: the following short line is normal again.
    feedAll(p, (const uint8_t*)"help\n", 5, &last);
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("help", p.line());
}

static void test_overflow_does_not_leak_into_the_following_command() {
    // Everything past the capacity is dropped, not glued onto the next line.
    char buf[6];   // 5 usable chars
    TelnetLineParser p(buf, sizeof(buf));
    Result last;
    feedAll(p, (const uint8_t*)"uon 199999\r", 11, &last);
    TEST_ASSERT_EQUAL(Result::LineTooLong, last);

    feedAll(p, (const uint8_t*)"\nhelp\r", 6, &last);   // LF of the CR LF, then a short line
    TEST_ASSERT_EQUAL(Result::LineReady, last);
    TEST_ASSERT_EQUAL_STRING("help", p.line());
}

static void test_explicit_reset_abandons_partial_line() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    p.feed('a');
    p.feed('b');
    TEST_ASSERT_EQUAL_size_t(2, p.length());
    p.reset();
    TEST_ASSERT_EQUAL_size_t(0, p.length());
    TEST_ASSERT_EQUAL_STRING("", p.line());

    // Parser is fully usable afterwards.
    Result r = p.feed('z');
    TEST_ASSERT_EQUAL(Result::Consumed, r);
    TEST_ASSERT_EQUAL_STRING("z", p.line());
}

static void test_multiple_lines_in_sequence() {
    char buf[32];
    TelnetLineParser p(buf, sizeof(buf));
    const uint8_t in[] = "help\r\nstatus\r\n";
    size_t n = sizeof(in) - 1;
    int lineReadyCount = 0;
    const char* lastLine = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (p.feed(in[i]) == Result::LineReady) {
            lineReadyCount++;
            lastLine = p.line();
            if (lineReadyCount == 1) TEST_ASSERT_EQUAL_STRING("help", lastLine);
        }
    }
    TEST_ASSERT_EQUAL_INT(2, lineReadyCount);
    TEST_ASSERT_EQUAL_STRING("status", lastLine);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_bare_lf_ends_line);
    RUN_TEST(test_crlf_collapses_to_one_line);
    RUN_TEST(test_cr_nul_collapses_to_one_line);
    RUN_TEST(test_bare_cr_then_new_content_starts_next_line);
    RUN_TEST(test_new_line_auto_clears_previous_content);
    RUN_TEST(test_backspace_edits_line);
    RUN_TEST(test_del_behaves_like_backspace);
    RUN_TEST(test_backspace_on_empty_line_is_safe_noop);
    RUN_TEST(test_iac_will_negotiation_stripped);
    RUN_TEST(test_iac_subnegotiation_stripped);
    RUN_TEST(test_overflow_stays_bounded_and_is_reported);
    RUN_TEST(test_overflow_flag_clears_on_the_next_line);
    RUN_TEST(test_overflow_does_not_leak_into_the_following_command);
    RUN_TEST(test_explicit_reset_abandons_partial_line);
    RUN_TEST(test_multiple_lines_in_sequence);
    return UNITY_END();
}
