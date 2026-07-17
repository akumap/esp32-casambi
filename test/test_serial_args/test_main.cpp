/**
 * Host-side unit tests for the pure serial argument parsing (serial_args.h).
 *
 * These run via `pio test -e native` and pin down the strict validation the
 * serial control commands share with the REST API: String::toInt() silently
 * mapped "300"→44, "-1"→255 and "foo"→0 after the uint8 cast — commands then
 * hit the WRONG target. A minimal std::string-backed shim stands in for
 * Arduino String (trim / length / operator[] / substring).
 */

#include <unity.h>
#include <string>
#include <cctype>

#include "serial_args.h"

// ---------------------------------------------------------------------------
// Arduino-String shim: exactly the interface serial_args.h requires.
// trim() matches Arduino's behavior (strip isspace() from both ends).
// ---------------------------------------------------------------------------

class ShimString {
public:
    ShimString() {}
    ShimString(const char* c) : _s(c) {}

    void trim() {
        size_t b = 0, e = _s.size();
        while (b < e && std::isspace((unsigned char)_s[b])) b++;
        while (e > b && std::isspace((unsigned char)_s[e - 1])) e--;
        _s = _s.substr(b, e - b);
    }
    unsigned length() const { return (unsigned)_s.size(); }
    char operator[](unsigned i) const { return _s[i]; }
    ShimString substring(unsigned from, unsigned to) const {
        ShimString out;
        out._s = _s.substr(from, to - from);
        return out;
    }
    const char* c_str() const { return _s.c_str(); }

private:
    std::string _s;
};

static bool parse(const char* s, long minV, long maxV, long& out) {
    return serialargs::parseInt(ShimString(s), minV, maxV, out);
}

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// parseInt
// ---------------------------------------------------------------------------

void test_parse_valid_values(void) {
    long v = -1;
    TEST_ASSERT_TRUE(parse("0", 0, 255, v));    TEST_ASSERT_EQUAL(0, v);
    TEST_ASSERT_TRUE(parse("255", 0, 255, v));  TEST_ASSERT_EQUAL(255, v);
    TEST_ASSERT_TRUE(parse("42", 0, 255, v));   TEST_ASSERT_EQUAL(42, v);
    TEST_ASSERT_TRUE(parse("007", 0, 255, v));  TEST_ASSERT_EQUAL(7, v);
}

void test_parse_whitespace_trimmed(void) {
    long v = -1;
    TEST_ASSERT_TRUE(parse("  128  ", 0, 255, v));
    TEST_ASSERT_EQUAL(128, v);
}

// The regressions that motivated the strict parser:
void test_parse_out_of_range_rejected(void) {
    long v = -1;
    TEST_ASSERT_FALSE(parse("300", 0, 255, v));   // toInt(): 300 → uint8 44
    TEST_ASSERT_FALSE(parse("256", 0, 255, v));
    TEST_ASSERT_FALSE(parse("-1", 0, 255, v));    // toInt(): -1 → uint8 255
}

void test_parse_nonnumeric_rejected(void) {
    long v = -1;
    TEST_ASSERT_FALSE(parse("foo", 0, 255, v));   // toInt(): "foo" → 0
    TEST_ASSERT_FALSE(parse("12a", 0, 255, v));
    TEST_ASSERT_FALSE(parse("a12", 0, 255, v));
    TEST_ASSERT_FALSE(parse("1 2", 0, 255, v));   // inner space: not one token
    TEST_ASSERT_FALSE(parse("1.5", 0, 255, v));
    TEST_ASSERT_FALSE(parse("+5", 0, 255, v));    // '+' not accepted
    TEST_ASSERT_FALSE(parse("", 0, 255, v));
    TEST_ASSERT_FALSE(parse("   ", 0, 255, v));
    TEST_ASSERT_FALSE(parse("-", 0, 255, v));
}

void test_parse_negative_range(void) {
    long v = 0;
    TEST_ASSERT_TRUE(parse("-40", -100, 100, v));
    TEST_ASSERT_EQUAL(-40, v);
    TEST_ASSERT_FALSE(parse("-101", -100, 100, v));
    TEST_ASSERT_FALSE(parse("--5", -100, 100, v));
}

void test_parse_kelvin_range(void) {
    // Same bounds the REST API and utemp enforce.
    long v = 0;
    TEST_ASSERT_TRUE(parse("3000", 1000, 10000, v));  TEST_ASSERT_EQUAL(3000, v);
    TEST_ASSERT_TRUE(parse("1000", 1000, 10000, v));
    TEST_ASSERT_TRUE(parse("10000", 1000, 10000, v));
    TEST_ASSERT_FALSE(parse("999", 1000, 10000, v));
    TEST_ASSERT_FALSE(parse("10001", 1000, 10000, v));
    TEST_ASSERT_FALSE(parse("500", 1000, 10000, v));
}

void test_parse_overflow_guard(void) {
    // Magnitudes above 100000 are rejected before any overflow can occur —
    // long ranges beyond that are deliberately unsupported.
    long v = 0;
    TEST_ASSERT_TRUE(parse("100000", 0, 100000, v));
    TEST_ASSERT_EQUAL(100000, v);
    TEST_ASSERT_FALSE(parse("100001", 0, 2000000, v));
    TEST_ASSERT_FALSE(parse("99999999999999999999", 0, 255, v));
}

void test_parse_out_untouched_on_failure(void) {
    long v = 77;
    TEST_ASSERT_FALSE(parse("foo", 0, 255, v));
    TEST_ASSERT_EQUAL(77, v);   // out must stay unmodified on failure
}

// ---------------------------------------------------------------------------
// splitArgs
// ---------------------------------------------------------------------------

void test_split_basic(void) {
    ShimString tok[4];
    TEST_ASSERT_EQUAL(2, serialargs::splitArgs(ShimString("5 128"), tok, 4));
    TEST_ASSERT_EQUAL_STRING("5", tok[0].c_str());
    TEST_ASSERT_EQUAL_STRING("128", tok[1].c_str());
}

void test_split_multiple_spaces_and_padding(void) {
    ShimString tok[4];
    TEST_ASSERT_EQUAL(3, serialargs::splitArgs(ShimString("  1   2  3  "), tok, 4));
    TEST_ASSERT_EQUAL_STRING("1", tok[0].c_str());
    TEST_ASSERT_EQUAL_STRING("2", tok[1].c_str());
    TEST_ASSERT_EQUAL_STRING("3", tok[2].c_str());
}

void test_split_empty(void) {
    ShimString tok[2];
    TEST_ASSERT_EQUAL(0, serialargs::splitArgs(ShimString(""), tok, 2));
    TEST_ASSERT_EQUAL(0, serialargs::splitArgs(ShimString("   "), tok, 2));
}

void test_split_too_many_tokens(void) {
    // More tokens than maxTok is an error (-1), not a silent truncation —
    // "slevel 1 2 3" must not be treated as "slevel 1 2".
    ShimString tok[2];
    TEST_ASSERT_EQUAL(-1, serialargs::splitArgs(ShimString("1 2 3"), tok, 2));
}

void test_split_exact_max(void) {
    ShimString tok[2];
    TEST_ASSERT_EQUAL(2, serialargs::splitArgs(ShimString("1 2"), tok, 2));
}

void test_split_single(void) {
    ShimString tok[2];
    TEST_ASSERT_EQUAL(1, serialargs::splitArgs(ShimString("42"), tok, 2));
    TEST_ASSERT_EQUAL_STRING("42", tok[0].c_str());
}

// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid_values);
    RUN_TEST(test_parse_whitespace_trimmed);
    RUN_TEST(test_parse_out_of_range_rejected);
    RUN_TEST(test_parse_nonnumeric_rejected);
    RUN_TEST(test_parse_negative_range);
    RUN_TEST(test_parse_kelvin_range);
    RUN_TEST(test_parse_overflow_guard);
    RUN_TEST(test_parse_out_untouched_on_failure);
    RUN_TEST(test_split_basic);
    RUN_TEST(test_split_multiple_spaces_and_padding);
    RUN_TEST(test_split_empty);
    RUN_TEST(test_split_too_many_tokens);
    RUN_TEST(test_split_single);
    RUN_TEST(test_split_exact_max);
    return UNITY_END();
}
