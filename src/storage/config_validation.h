/**
 * Pure configuration-validation helpers.
 *
 * Deliberately free of Arduino / LittleFS dependencies (only ArduinoJson and the
 * C++ standard library) so the same logic can be exercised by host-side unit
 * tests. ConfigStore uses these to decide whether a parsed config is safe to
 * operate on, both when saving (validate before committing) and when loading
 * (reject/recover corrupt files).
 */

#ifndef CONFIG_VALIDATION_H
#define CONFIG_VALIDATION_H

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

namespace configval {

// True iff `hex` is exactly `expectedBytes * 2` characters long and every
// character is a hexadecimal digit. Used to reject truncated or non-hex AES
// keys, which the old loader would have partially converted and silently
// accepted as a (wrong) key.
inline bool isValidHexKey(const char* hex, size_t expectedBytes) {
    if (!hex) return false;
    size_t n = 0;
    while (hex[n] != '\0') {
        const char c = hex[n];
        const bool isHex = (c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F');
        if (!isHex) return false;
        n++;
    }
    return n == expectedBytes * 2;
}

// Result of config validation: `ok` plus a coarse reason for logging when it
// fails. Deliberately not an enum class so it prints cleanly and stays trivial.
enum ConfigValidationReason {
    CFG_OK = 0,
    CFG_BAD_NETWORK_ID,     // networkId missing or empty
    CFG_BAD_PROTOCOL,       // protocolVersion missing or out of range
    CFG_NO_KEYS,            // keys array missing or empty
    CFG_BAD_KEY,            // a key is missing / not full-length hex
};

// Semantic validation of a parsed network-config document. Returns CFG_OK only
// for a document the firmware can safely run on:
//   - networkId present and non-empty,
//   - protocolVersion an integer within [minProto, maxProto],
//   - at least one key, each with a full-length hex AES key.
// Existence of the file alone is NOT sufficient (the previous behaviour).
//
// Templated on the document type so the caller can pass the JsonDocument
// directly and every access goes through the SAME operator[] the working loader
// uses. Two hard-won compatibility rules for ArduinoJson 7.0.0 (the pinned
// firmware version):
//   1. Access fields straight off the JsonDocument — do NOT convert the root to
//      JsonObjectConst first (`doc.as<JsonObjectConst>()`), which is broken there.
//   2. Use only `.as<T>()`, never `.is<T>()`: `.is<const char*>()` / `.is<int>()`
//      mis-handle parsed values. `.as<const char*>()` is nullptr for a
//      non-string and `.as<long>()` is 0 for a non-number, so explicit
//      null/range checks are enough and portable.
template <typename TDoc>
inline ConfigValidationReason validateConfigDoc(const TDoc& doc, uint8_t minProto,
                                                uint8_t maxProto, size_t aesKeySize) {
    const char* nid = doc["networkId"].template as<const char*>();
    if (nid == nullptr || nid[0] == '\0') return CFG_BAD_NETWORK_ID;

    if (doc["protocolVersion"].isNull()) return CFG_BAD_PROTOCOL;
    const long proto = doc["protocolVersion"].template as<long>();
    if (proto < (long)minProto || proto > (long)maxProto) return CFG_BAD_PROTOCOL;

    JsonArrayConst keys = doc["keys"].template as<JsonArrayConst>();
    if (keys.isNull() || keys.size() == 0) return CFG_NO_KEYS;
    for (JsonObjectConst keyObj : keys) {
        const char* hex = keyObj["key"].template as<const char*>();
        if (!isValidHexKey(hex, aesKeySize)) return CFG_BAD_KEY;
    }
    return CFG_OK;
}

// Convenience boolean wrapper.
template <typename TDoc>
inline bool isValidConfigDoc(const TDoc& doc, uint8_t minProto,
                             uint8_t maxProto, size_t aesKeySize) {
    return validateConfigDoc(doc, minProto, maxProto, aesKeySize) == CFG_OK;
}

// Semantic validation of WiFi credentials: both SSID and password must be
// present and non-empty (mirrors WiFiCredentials::isValid()). Same access rules
// as validateConfigDoc above.
template <typename TDoc>
inline bool isValidWifiDoc(const TDoc& doc) {
    const char* ssid = doc["ssid"].template as<const char*>();
    const char* pass = doc["password"].template as<const char*>();
    return ssid && ssid[0] != '\0' && pass && pass[0] != '\0';
}

}  // namespace configval

#endif  // CONFIG_VALIDATION_H
