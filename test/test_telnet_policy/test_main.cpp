/**
 * Host-side unit tests for the telnet command policy (net/telnet_policy.h) —
 * the guard that keeps `setup` and `wifi set` on the serial console
 * (docs/konzept-tcp-konsole.md, decision E3).
 *
 * The point of these tests is the mismatch that made the original guard
 * bypassable: it matched the RAW line with startsWith("wifi set"), while the
 * dispatcher in serial_console.cpp trims its sub-command — so `wifi  set` (two
 * spaces) executed anyway, with the WiFi password echoed in clear text.
 */

#include <unity.h>
#include <cstring>

#include "net/telnet_policy.h"

void setUp(void) {}
void tearDown(void) {}

static bool blocked(const char* raw) {
    char norm[160];
    telnetpolicy::normalize(raw, norm, sizeof(norm));
    return telnetpolicy::isSerialOnly(norm);
}

static void expectNormalized(const char* raw, const char* expected) {
    char norm[160];
    telnetpolicy::normalize(raw, norm, sizeof(norm));
    TEST_ASSERT_EQUAL_STRING(expected, norm);
}

static void test_normalize_trims_and_collapses() {
    expectNormalized("  status  ", "status");
    expectNormalized("wifi  set  a  b", "wifi set a b");
    expectNormalized("wifi\tset a b", "wifi set a b");
    expectNormalized("", "");
    expectNormalized("   ", "");
    expectNormalized("uon 5", "uon 5");
}

static void test_normalize_respects_capacity() {
    char small[5];
    telnetpolicy::normalize("abcdefgh", small, sizeof(small));
    TEST_ASSERT_EQUAL_STRING("abcd", small);   // NUL-terminated, never overflowing

    char zero[1];
    telnetpolicy::normalize("abc", zero, sizeof(zero));
    TEST_ASSERT_EQUAL_STRING("", zero);
}

static void test_plain_forms_are_blocked() {
    TEST_ASSERT_TRUE(blocked("setup"));
    TEST_ASSERT_TRUE(blocked("wifi set MyNet secret"));
}

static void test_whitespace_variants_are_blocked() {
    // Each of these reaches the same handler in serial_console.cpp.
    TEST_ASSERT_TRUE(blocked("wifi  set MyNet secret"));
    TEST_ASSERT_TRUE(blocked("wifi   set   MyNet   secret"));
    TEST_ASSERT_TRUE(blocked("wifi\tset MyNet secret"));
    TEST_ASSERT_TRUE(blocked("  setup  "));
    TEST_ASSERT_TRUE(blocked("setup\r"));
    TEST_ASSERT_TRUE(blocked("wifi set"));          // bare usage line
    TEST_ASSERT_TRUE(blocked("wifi set   "));
}

static void test_allowed_commands_stay_allowed() {
    TEST_ASSERT_FALSE(blocked("wifi status"));
    TEST_ASSERT_FALSE(blocked("status"));
    TEST_ASSERT_FALSE(blocked("help"));
    TEST_ASSERT_FALSE(blocked("telnet timeout 60"));
    TEST_ASSERT_FALSE(blocked(""));
    // Not the blocked commands, just similar-looking ones.
    TEST_ASSERT_FALSE(blocked("setupfoo"));
    TEST_ASSERT_FALSE(blocked("wifi settings"));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_normalize_trims_and_collapses);
    RUN_TEST(test_normalize_respects_capacity);
    RUN_TEST(test_plain_forms_are_blocked);
    RUN_TEST(test_whitespace_variants_are_blocked);
    RUN_TEST(test_allowed_commands_stay_allowed);
    return UNITY_END();
}
