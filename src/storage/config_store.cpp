/**
 * Configuration Storage Implementation
 */

#include "config_store.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

bool ConfigStore::_initialized = false;

bool ConfigStore::init() {
    if (_initialized) return true;

    if (!LittleFS.begin(true)) {
        Serial.println("Failed to mount LittleFS");
        return false;
    }

    _initialized = true;
    Serial.println("LittleFS mounted");
    return true;
}

bool ConfigStore::hasValidConfig() {
    if (!_initialized && !init()) return false;

    return LittleFS.exists(CONFIG_FILE_PATH);
}

bool ConfigStore::saveNetworkConfig(const NetworkConfig& config) {
    if (!_initialized && !init()) return false;

    Serial.println("Storage: Saving network config...");

    // Create JSON document
    JsonDocument doc;

    // Basic network info
    doc["networkId"] = config.networkId;
    doc["networkUuid"] = config.networkUuid;
    doc["networkName"] = config.networkName;
    doc["protocolVersion"] = config.protocolVersion;
    doc["revision"] = config.revision;

    // Auto-connect settings
    doc["autoConnectEnabled"] = config.autoConnectEnabled;
    doc["autoConnectAddress"] = config.autoConnectAddress;

    // Debug settings
    doc["debugEnabled"] = config.debugEnabled;

    // Save keys
    JsonArray keysArray = doc["keys"].to<JsonArray>();
    for (const auto& key : config.keys) {
        JsonObject keyObj = keysArray.add<JsonObject>();
        keyObj["id"] = key.id;
        keyObj["type"] = key.type;
        keyObj["role"] = key.role;
        keyObj["name"] = key.name;

        // Convert binary key to hex string
        char hexKey[AES_KEY_SIZE * 2 + 1];
        for (size_t i = 0; i < AES_KEY_SIZE; i++) {
            sprintf(hexKey + (i * 2), "%02x", key.key[i]);
        }
        hexKey[AES_KEY_SIZE * 2] = '\0';
        keyObj["key"] = String(hexKey);
    }

    // Save units (with capabilities)
    JsonArray unitsArray = doc["units"].to<JsonArray>();
    for (const auto& unit : config.units) {
        JsonObject unitObj = unitsArray.add<JsonObject>();
        unitObj["deviceId"] = unit.deviceId;
        unitObj["type"] = unit.type;
        unitObj["uuid"] = unit.uuid;
        unitObj["address"] = unit.address;
        unitObj["name"] = unit.name;
        unitObj["firmware"] = unit.firmware;

        // Capabilities
        unitObj["numChannels"] = unit.numChannels;
        unitObj["hasCCT"] = unit.hasCCT;
        unitObj["hasVertical"] = unit.hasVertical;
        if (unit.hasCCT) {
            unitObj["cctMin"] = unit.cctMinKelvin;
            unitObj["cctMax"] = unit.cctMaxKelvin;
        }
    }

    // Save groups
    JsonArray groupsArray = doc["groups"].to<JsonArray>();
    for (const auto& group : config.groups) {
        JsonObject groupObj = groupsArray.add<JsonObject>();
        groupObj["groupId"] = group.groupId;
        groupObj["name"] = group.name;

        JsonArray unitIdsArray = groupObj["unitIds"].to<JsonArray>();
        for (uint8_t unitId : group.unitIds) {
            unitIdsArray.add(unitId);
        }
    }

    // Save scenes
    JsonArray scenesArray = doc["scenes"].to<JsonArray>();
    for (const auto& scene : config.scenes) {
        JsonObject sceneObj = scenesArray.add<JsonObject>();
        sceneObj["sceneId"] = scene.sceneId;
        sceneObj["name"] = scene.name;
    }

    // Open file for writing
    File file = LittleFS.open(CONFIG_FILE_PATH, "w");
    if (!file) {
        Serial.println("Storage: Failed to open config file for writing");
        return false;
    }

    // Serialize to file
    if (serializeJson(doc, file) == 0) {
        Serial.println("Storage: Failed to write JSON");
        file.close();
        return false;
    }

    file.close();
    Serial.printf("Storage: Saved config (%d keys, %d units, %d groups, %d scenes)\n",
                  config.keys.size(), config.units.size(),
                  config.groups.size(), config.scenes.size());

    return true;
}

bool ConfigStore::loadNetworkConfig(NetworkConfig& config) {
    if (!_initialized && !init()) return false;

    if (!LittleFS.exists(CONFIG_FILE_PATH)) {
        Serial.println("Storage: Config file does not exist");
        return false;
    }

    Serial.println("Storage: Loading network config...");

    File file = LittleFS.open(CONFIG_FILE_PATH, "r");
    if (!file) {
        Serial.println("Storage: Failed to open config file");
        return false;
    }

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("Storage: JSON parse error: %s\n", error.c_str());
        return false;
    }

    // Load basic network info
    config.networkId = doc["networkId"].as<String>();
    config.networkUuid = doc["networkUuid"].as<String>();
    config.networkName = doc["networkName"].as<String>();
    config.protocolVersion = doc["protocolVersion"].as<uint8_t>();
    config.revision = doc["revision"].as<int>();

    // Load auto-connect settings (with defaults for backward compatibility)
    config.autoConnectEnabled = doc["autoConnectEnabled"] | false;
    config.autoConnectAddress = doc["autoConnectAddress"] | "";

    // Load debug settings (with defaults for backward compatibility)
    config.debugEnabled = doc["debugEnabled"] | false;

    // Load keys
    config.keys.clear();
    if (doc["keys"].is<JsonArrayConst>()) {
        JsonArrayConst keysArray = doc["keys"];
        for (JsonObjectConst keyObj : keysArray) {
            CasambiKey key;
            key.id = keyObj["id"].as<uint8_t>();
            key.type = keyObj["type"].as<uint8_t>();
            key.role = keyObj["role"].as<uint8_t>();
            key.name = keyObj["name"].as<String>();

            // Convert hex string to binary
            String hexKey = keyObj["key"].as<String>();
            for (size_t i = 0; i < AES_KEY_SIZE && i * 2 < hexKey.length(); i++) {
                String byteStr = hexKey.substring(i * 2, i * 2 + 2);
                key.key[i] = strtol(byteStr.c_str(), nullptr, 16);
            }

            config.keys.push_back(key);
        }
    }

    // Load units (with capabilities)
    config.units.clear();
    if (doc["units"].is<JsonArrayConst>()) {
        JsonArrayConst unitsArray = doc["units"];
        for (JsonObjectConst unitObj : unitsArray) {
            CasambiUnit unit;
            unit.deviceId = unitObj["deviceId"].as<uint8_t>();
            unit.type = unitObj["type"].as<uint16_t>();
            unit.uuid = unitObj["uuid"].as<String>();
            unit.address = unitObj["address"].as<String>();
            unit.name = unitObj["name"].as<String>();
            unit.firmware = unitObj["firmware"].as<String>();
            unit.online = false;
            unit.on = false;

            // Capabilities (with defaults for backward compatibility)
            unit.numChannels = unitObj["numChannels"] | 1;
            unit.hasCCT = unitObj["hasCCT"] | false;
            unit.hasVertical = unitObj["hasVertical"] | false;
            unit.cctMinKelvin = unitObj["cctMin"] | 0;
            unit.cctMaxKelvin = unitObj["cctMax"] | 0;

            config.units.push_back(unit);
        }
    }

    // Load groups
    config.groups.clear();
    if (doc["groups"].is<JsonArrayConst>()) {
        JsonArrayConst groupsArray = doc["groups"];
        for (JsonObjectConst groupObj : groupsArray) {
            CasambiGroup group;
            group.groupId = groupObj["groupId"].as<uint8_t>();
            group.name = groupObj["name"].as<String>();

            if (groupObj["unitIds"].is<JsonArrayConst>()) {
                JsonArrayConst unitIdsArray = groupObj["unitIds"];
                for (uint8_t unitId : unitIdsArray) {
                    group.unitIds.push_back(unitId);
                }
            }

            config.groups.push_back(group);
        }
    }

    // Load scenes
    config.scenes.clear();
    if (doc["scenes"].is<JsonArrayConst>()) {
        JsonArrayConst scenesArray = doc["scenes"];
        for (JsonObjectConst sceneObj : scenesArray) {
            CasambiScene scene;
            scene.sceneId = sceneObj["sceneId"].as<uint8_t>();
            scene.name = sceneObj["name"].as<String>();

            config.scenes.push_back(scene);
        }
    }

    Serial.printf("Storage: Loaded config (%d keys, %d units, %d groups, %d scenes)\n",
                  config.keys.size(), config.units.size(),
                  config.groups.size(), config.scenes.size());

    return true;
}

bool ConfigStore::saveWiFiCredentials(const WiFiCredentials& creds) {
    if (!_initialized && !init()) return false;

    Serial.println("Storage: Saving WiFi credentials...");

    JsonDocument doc;
    doc["ssid"] = creds.ssid;
    doc["password"] = creds.password;

    File file = LittleFS.open(WIFI_FILE_PATH, "w");
    if (!file) {
        Serial.println("Storage: Failed to open WiFi file for writing");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Storage: Failed to write WiFi JSON");
        file.close();
        return false;
    }

    file.close();
    Serial.printf("Storage: Saved WiFi credentials (SSID: %s)\n", creds.ssid.c_str());

    return true;
}

bool ConfigStore::loadWiFiCredentials(WiFiCredentials& creds) {
    if (!_initialized && !init()) return false;

    if (!LittleFS.exists(WIFI_FILE_PATH)) {
        Serial.println("Storage: WiFi file does not exist");
        return false;
    }

    Serial.println("Storage: Loading WiFi credentials...");

    File file = LittleFS.open(WIFI_FILE_PATH, "r");
    if (!file) {
        Serial.println("Storage: Failed to open WiFi file");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("Storage: WiFi JSON parse error: %s\n", error.c_str());
        return false;
    }

    creds.ssid = doc["ssid"].as<String>();
    creds.password = doc["password"].as<String>();

    Serial.printf("Storage: Loaded WiFi credentials (SSID: %s)\n", creds.ssid.c_str());

    return true;
}

void ConfigStore::clearAll() {
    if (!_initialized && !init()) return;

    if (LittleFS.exists(CONFIG_FILE_PATH)) {
        LittleFS.remove(CONFIG_FILE_PATH);
    }
    if (LittleFS.exists(WIFI_FILE_PATH)) {
        LittleFS.remove(WIFI_FILE_PATH);
    }

    Serial.println("Configuration cleared");
}
