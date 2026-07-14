/**
 * Host-side unit tests for the pure config-validation helpers (S-01 / S-09).
 *
 * These run on the build host via `pio test -e native` — no ESP32 required —
 * so a refactor that weakens hex-key validation or the required-field checks
 * fails CI instead of only surfacing on the real gateway.
 */

#include <unity.h>
#include <ArduinoJson.h>
#include "storage/config_validation.h"

// Matches the firmware constants (src/config.h) without pulling in Arduino.
static const uint8_t MIN_PROTO = 10;
static const uint8_t MAX_PROTO = 10;
static const size_t  AES_BYTES = 16;   // 32 hex chars

// Build a minimal-but-valid config document with a single 32-hex-char key.
static void makeValidDoc(JsonDocument& doc) {
    doc["networkId"] = "net-123";
    doc["protocolVersion"] = 10;
    JsonArray keys = doc["keys"].to<JsonArray>();
    JsonObject k = keys.add<JsonObject>();
    k["key"] = "00112233445566778899aabbccddeeff";  // 32 hex chars
}

// ---- isValidHexKey ---------------------------------------------------------

void test_hexkey_exact_length_ok(void) {
    TEST_ASSERT_TRUE(configval::isValidHexKey("00112233445566778899aabbccddeeff", AES_BYTES));
    TEST_ASSERT_TRUE(configval::isValidHexKey("FFEEDDCCBBAA99887766554433221100", AES_BYTES));
}

void test_hexkey_too_short_rejected(void) {
    TEST_ASSERT_FALSE(configval::isValidHexKey("00112233", AES_BYTES));
    TEST_ASSERT_FALSE(configval::isValidHexKey("", AES_BYTES));
}

void test_hexkey_too_long_rejected(void) {
    TEST_ASSERT_FALSE(configval::isValidHexKey("00112233445566778899aabbccddeeff00", AES_BYTES));
}

void test_hexkey_nonhex_rejected(void) {
    // 32 chars but contains 'g' and 'z'.
    TEST_ASSERT_FALSE(configval::isValidHexKey("00112233445566778899aabbccddeegz", AES_BYTES));
}

void test_hexkey_null_rejected(void) {
    TEST_ASSERT_FALSE(configval::isValidHexKey(nullptr, AES_BYTES));
}

// ---- isValidConfigObject ---------------------------------------------------

void test_config_valid(void) {
    JsonDocument doc;
    makeValidDoc(doc);
    TEST_ASSERT_TRUE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

void test_config_missing_networkid(void) {
    JsonDocument doc;
    makeValidDoc(doc);
    doc.remove("networkId");
    TEST_ASSERT_FALSE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

void test_config_empty_networkid(void) {
    JsonDocument doc;
    makeValidDoc(doc);
    doc["networkId"] = "";
    TEST_ASSERT_FALSE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

void test_config_protocol_out_of_range_is_soft(void) {
    // A structurally valid config with an out-of-range (but present, positive)
    // protocol is NOT strictly valid, but IS committable — it must be persisted
    // and loaded with only a warning, never rejected. This is the exact case of
    // a v11 network against a firmware built for [10,10].
    JsonDocument doc;
    makeValidDoc(doc);
    doc["protocolVersion"] = 11;  // above MAX (10,10 here)
    TEST_ASSERT_EQUAL(configval::CFG_UNSUPPORTED_PROTOCOL,
                      configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
    TEST_ASSERT_TRUE(configval::isCommittable(
        configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES)));

    doc["protocolVersion"] = 9;   // below MIN — still committable (soft)
    TEST_ASSERT_TRUE(configval::isCommittable(
        configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES)));

    // In range -> fully OK.
    doc["protocolVersion"] = 10;
    TEST_ASSERT_TRUE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

void test_config_protocol_missing_or_zero_is_hard(void) {
    // Missing or zero protocol is corruption, not an unsupported version: hard
    // reject (not committable).
    JsonDocument doc;
    makeValidDoc(doc);
    doc["protocolVersion"] = 0;
    TEST_ASSERT_EQUAL(configval::CFG_BAD_PROTOCOL,
                      configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
    TEST_ASSERT_FALSE(configval::isCommittable(
        configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES)));

    doc.remove("protocolVersion");
    TEST_ASSERT_FALSE(configval::isCommittable(
        configval::validateConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES)));
}

void test_config_no_keys(void) {
    JsonDocument doc;
    makeValidDoc(doc);
    doc["keys"].to<JsonArray>();   // empty array
    TEST_ASSERT_FALSE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

void test_config_truncated_key_rejected(void) {
    JsonDocument doc;
    makeValidDoc(doc);
    doc["keys"][0]["key"] = "00112233";  // truncated hex key
    TEST_ASSERT_FALSE(configval::isValidConfigDoc(doc, MIN_PROTO, MAX_PROTO, AES_BYTES));
}

// ---- isValidWifiObject -----------------------------------------------------

void test_wifi_valid(void) {
    JsonDocument doc;
    doc["ssid"] = "MyNet";
    doc["password"] = "secret123";
    TEST_ASSERT_TRUE(configval::isValidWifiDoc(doc));
}

void test_wifi_missing_password(void) {
    JsonDocument doc;
    doc["ssid"] = "MyNet";
    TEST_ASSERT_FALSE(configval::isValidWifiDoc(doc));
}

void test_wifi_empty_ssid(void) {
    JsonDocument doc;
    doc["ssid"] = "";
    doc["password"] = "secret123";
    TEST_ASSERT_FALSE(configval::isValidWifiDoc(doc));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_hexkey_exact_length_ok);
    RUN_TEST(test_hexkey_too_short_rejected);
    RUN_TEST(test_hexkey_too_long_rejected);
    RUN_TEST(test_hexkey_nonhex_rejected);
    RUN_TEST(test_hexkey_null_rejected);
    RUN_TEST(test_config_valid);
    RUN_TEST(test_config_missing_networkid);
    RUN_TEST(test_config_empty_networkid);
    RUN_TEST(test_config_protocol_out_of_range_is_soft);
    RUN_TEST(test_config_protocol_missing_or_zero_is_hard);
    RUN_TEST(test_config_no_keys);
    RUN_TEST(test_config_truncated_key_rejected);
    RUN_TEST(test_wifi_valid);
    RUN_TEST(test_wifi_missing_password);
    RUN_TEST(test_wifi_empty_ssid);
    return UNITY_END();
}
