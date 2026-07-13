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

// Semantic validation of a parsed network-config object. Returns true only for a
// document the firmware can safely run on:
//   - networkId present and non-empty,
//   - protocolVersion an integer within [minProto, maxProto],
//   - at least one key, each with a full-length hex AES key.
// Existence of the file alone is NOT sufficient (the previous behaviour).
inline bool isValidConfigObject(JsonObjectConst doc, uint8_t minProto,
                                uint8_t maxProto, size_t aesKeySize) {
    const char* nid = doc["networkId"].is<const char*>()
                          ? doc["networkId"].as<const char*>()
                          : nullptr;
    if (nid == nullptr || nid[0] == '\0') return false;

    if (!doc["protocolVersion"].is<int>()) return false;
    const int proto = doc["protocolVersion"].as<int>();
    if (proto < minProto || proto > maxProto) return false;

    JsonArrayConst keys = doc["keys"];
    if (keys.isNull() || keys.size() == 0) return false;
    for (JsonObjectConst keyObj : keys) {
        const char* hex = keyObj["key"].is<const char*>()
                              ? keyObj["key"].as<const char*>()
                              : nullptr;
        if (!isValidHexKey(hex, aesKeySize)) return false;
    }
    return true;
}

// Semantic validation of WiFi credentials: both SSID and password must be
// present and non-empty (mirrors WiFiCredentials::isValid()).
inline bool isValidWifiObject(JsonObjectConst doc) {
    const char* ssid = doc["ssid"].is<const char*>()
                           ? doc["ssid"].as<const char*>()
                           : nullptr;
    const char* pass = doc["password"].is<const char*>()
                           ? doc["password"].as<const char*>()
                           : nullptr;
    return ssid && ssid[0] != '\0' && pass && pass[0] != '\0';
}

}  // namespace configval

#endif  // CONFIG_VALIDATION_H
