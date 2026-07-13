/**
 * Configuration Storage Implementation
 */

#include "config_store.h"
#include "config_validation.h"
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

// ----------------------------------------------------------------------------
// Atomic write / recovery helpers
// ----------------------------------------------------------------------------

// Swap a validated temp file into place, keeping the previous live file as a
// backup until the new one is confirmed. On a power loss at any point either the
// old or the new file remains fully intact and recoverable:
//   - crash while writing tmp        → live untouched, tmp discarded on load
//   - crash after live→bak rename    → live missing, bak holds old good copy
//   - crash after tmp→live rename    → live holds new good copy, bak lingers
//                                       (cleaned up on the next successful save)
static bool commitAtomic(const char* livePath, const char* tmpPath,
                         const char* bakPath) {
    if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);

    if (LittleFS.exists(livePath)) {
        if (!LittleFS.rename(livePath, bakPath)) {
            Serial.println("Storage: failed to back up live file; aborting save");
            LittleFS.remove(tmpPath);
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath, livePath)) {
        Serial.println("Storage: failed to move temp into place; restoring backup");
        if (LittleFS.exists(bakPath)) LittleFS.rename(bakPath, livePath);
        LittleFS.remove(tmpPath);
        return false;
    }

    // New file is confirmed in place — the backup is no longer needed.
    if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);
    return true;
}

bool ConfigStore::hasValidConfig() {
    if (!_initialized && !init()) return false;

    // Lightweight presence check: is there a config to try — live OR a backup
    // left by an interrupted save? This is deliberately cheap because it also
    // runs inside the async web handler (POST /api/refreshCasambi); the real
    // parse + semantic validation (and recovery) happens in loadNetworkConfig(),
    // which is the actual gate the boot path uses to decide operation vs. setup
    // mode. Existence alone never causes the firmware to trust a file: a corrupt
    // config makes loadNetworkConfig() fail, and setup() then falls back to the
    // setup portal instead of booting half-initialised.
    return LittleFS.exists(CONFIG_FILE_PATH) || LittleFS.exists(CONFIG_BAK_PATH);
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
    doc["gatewayName"] = config.gatewayName;

    // NTP server
    doc["ntpServer"] = config.ntpServer;

    // Casambi network password (reused by `refresh`)
    doc["casambiPassword"] = config.casambiPassword;

    // Debug settings (per category)
    doc["bleDebugEnabled"]     = config.bleDebugEnabled;
    doc["casambiDebugEnabled"] = config.casambiDebugEnabled;
    doc["webDebugEnabled"]     = config.webDebugEnabled;
    doc["parseDebugEnabled"]   = config.parseDebugEnabled;
    doc["heapDebugEnabled"]    = config.heapDebugEnabled;

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

    // Write to a temp file first, never straight onto the live file: opening the
    // live file with "w" would truncate the only good copy before the new data
    // is safely on flash.
    File file = LittleFS.open(CONFIG_TMP_PATH, "w");
    if (!file) {
        Serial.println("Storage: Failed to open temp config file for writing");
        return false;
    }
    if (serializeJson(doc, file) == 0) {
        Serial.println("Storage: Failed to write JSON");
        file.close();
        LittleFS.remove(CONFIG_TMP_PATH);
        return false;
    }
    file.close();

    // Re-read and semantically validate the temp file before committing it, so a
    // serialize/flash glitch cannot promote a corrupt file over the good one.
    {
        File verify = LittleFS.open(CONFIG_TMP_PATH, "r");
        if (!verify) {
            Serial.println("Storage: Failed to reopen temp config for validation");
            LittleFS.remove(CONFIG_TMP_PATH);
            return false;
        }
        JsonDocument vdoc;
        DeserializationError verr = deserializeJson(vdoc, verify);
        verify.close();
        if (verr || !configval::isValidConfigObject(vdoc.as<JsonObjectConst>(),
                                                     MIN_PROTOCOL_VERSION,
                                                     MAX_PROTOCOL_VERSION,
                                                     AES_KEY_SIZE)) {
            Serial.printf("Storage: temp config failed validation (%s); not committing\n",
                          verr ? verr.c_str() : "semantic");
            LittleFS.remove(CONFIG_TMP_PATH);
            return false;
        }
    }

    if (!commitAtomic(CONFIG_FILE_PATH, CONFIG_TMP_PATH, CONFIG_BAK_PATH)) {
        return false;
    }

    Serial.printf("Storage: Saved config (%d keys, %d units, %d groups, %d scenes)\n",
                  config.keys.size(), config.units.size(),
                  config.groups.size(), config.scenes.size());

    return true;
}

bool ConfigStore::loadNetworkConfig(NetworkConfig& config) {
    if (!_initialized && !init()) return false;

    // Prefer the live file; fall back to the backup left behind by an
    // interrupted save. A recovered backup is promoted to the live name so
    // subsequent boots use it directly.
    if (_loadNetworkConfigFrom(CONFIG_FILE_PATH, config)) return true;

    if (LittleFS.exists(CONFIG_BAK_PATH) &&
        _loadNetworkConfigFrom(CONFIG_BAK_PATH, config)) {
        Serial.println("Storage: recovered network config from backup");
        if (LittleFS.exists(CONFIG_FILE_PATH)) LittleFS.remove(CONFIG_FILE_PATH);
        LittleFS.rename(CONFIG_BAK_PATH, CONFIG_FILE_PATH);  // best-effort promote
        return true;
    }

    Serial.println("Storage: no valid network config (live or backup)");
    return false;
}

bool ConfigStore::_loadNetworkConfigFrom(const char* path, NetworkConfig& config) {
    if (!LittleFS.exists(path)) {
        return false;
    }

    Serial.printf("Storage: Loading network config from %s...\n", path);

    File file = LittleFS.open(path, "r");
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

    // Reject a syntactically valid but semantically incomplete config (missing
    // networkId, out-of-range protocol, or a truncated/non-hex AES key) instead
    // of loading a half-valid state. The reason is logged so a rejection is
    // diagnosable from the serial log alone.
    configval::ConfigValidationReason reason =
        configval::validateConfigObject(doc.as<JsonObjectConst>(),
                                        MIN_PROTOCOL_VERSION,
                                        MAX_PROTOCOL_VERSION,
                                        AES_KEY_SIZE);
    if (reason != configval::CFG_OK) {
        Serial.printf("Storage: config failed semantic validation (reason=%d, "
                      "networkId='%s', protocolVersion=%d, keys=%d)\n",
                      (int)reason,
                      doc["networkId"].as<const char*>() ? doc["networkId"].as<const char*>() : "(none)",
                      doc["protocolVersion"].as<int>(),
                      (int)doc["keys"].as<JsonArrayConst>().size());
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
    config.gatewayName = doc["gatewayName"] | "";

    // Load NTP server (with default for backward compatibility)
    config.ntpServer = doc["ntpServer"] | NTP_SERVER_DEFAULT;

    // Load saved Casambi network password (empty for configs from before
    // this field existed, in which case `refresh` will prompt for it)
    config.casambiPassword = doc["casambiPassword"] | "";

    // Load debug settings (with defaults for backward compatibility)
    config.bleDebugEnabled     = doc["bleDebugEnabled"]     | false;
    config.casambiDebugEnabled = doc["casambiDebugEnabled"] | true;
    config.webDebugEnabled     = doc["webDebugEnabled"]     | true;
    config.parseDebugEnabled   = doc["parseDebugEnabled"]   | false;
    config.heapDebugEnabled    = doc["heapDebugEnabled"]    | false;

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

    // Same atomic temp→validate→commit dance as the network config: never
    // truncate the live credentials before the new ones are safely on flash.
    File file = LittleFS.open(WIFI_TMP_PATH, "w");
    if (!file) {
        Serial.println("Storage: Failed to open temp WiFi file for writing");
        return false;
    }
    if (serializeJson(doc, file) == 0) {
        Serial.println("Storage: Failed to write WiFi JSON");
        file.close();
        LittleFS.remove(WIFI_TMP_PATH);
        return false;
    }
    file.close();

    {
        File verify = LittleFS.open(WIFI_TMP_PATH, "r");
        if (!verify) {
            LittleFS.remove(WIFI_TMP_PATH);
            return false;
        }
        JsonDocument vdoc;
        DeserializationError verr = deserializeJson(vdoc, verify);
        verify.close();
        if (verr || !configval::isValidWifiObject(vdoc.as<JsonObjectConst>())) {
            Serial.println("Storage: temp WiFi creds failed validation; not committing");
            LittleFS.remove(WIFI_TMP_PATH);
            return false;
        }
    }

    if (!commitAtomic(WIFI_FILE_PATH, WIFI_TMP_PATH, WIFI_BAK_PATH)) {
        return false;
    }

    Serial.printf("Storage: Saved WiFi credentials (SSID: %s)\n", creds.ssid.c_str());
    return true;
}

bool ConfigStore::loadWiFiCredentials(WiFiCredentials& creds) {
    if (!_initialized && !init()) return false;

    if (_loadWiFiCredentialsFrom(WIFI_FILE_PATH, creds)) return true;

    if (LittleFS.exists(WIFI_BAK_PATH) &&
        _loadWiFiCredentialsFrom(WIFI_BAK_PATH, creds)) {
        Serial.println("Storage: recovered WiFi credentials from backup");
        if (LittleFS.exists(WIFI_FILE_PATH)) LittleFS.remove(WIFI_FILE_PATH);
        LittleFS.rename(WIFI_BAK_PATH, WIFI_FILE_PATH);  // best-effort promote
        return true;
    }

    Serial.println("Storage: no valid WiFi credentials (live or backup)");
    return false;
}

bool ConfigStore::_loadWiFiCredentialsFrom(const char* path, WiFiCredentials& creds) {
    if (!LittleFS.exists(path)) {
        return false;
    }

    File file = LittleFS.open(path, "r");
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

    if (!configval::isValidWifiObject(doc.as<JsonObjectConst>())) {
        Serial.println("Storage: WiFi credentials failed validation");
        return false;
    }

    creds.ssid = doc["ssid"].as<String>();
    creds.password = doc["password"].as<String>();

    Serial.printf("Storage: Loaded WiFi credentials (SSID: %s)\n", creds.ssid.c_str());
    return true;
}

void ConfigStore::clearAll() {
    if (!_initialized && !init()) return;

    // Remove live files plus any temp/backup companions so a factory reset
    // cannot leave a stale recoverable copy behind.
    const char* paths[] = {
        CONFIG_FILE_PATH, CONFIG_TMP_PATH, CONFIG_BAK_PATH,
        WIFI_FILE_PATH,   WIFI_TMP_PATH,   WIFI_BAK_PATH,
    };
    for (const char* p : paths) {
        if (LittleFS.exists(p)) LittleFS.remove(p);
    }

    Serial.println("Configuration cleared");
}

void ConfigStore::setRefreshPending() {
    if (!_initialized && !init()) return;

    File file = LittleFS.open(REFRESH_FLAG_PATH, "w");
    if (file) {
        file.print("1");
        file.close();
    }
}

bool ConfigStore::isRefreshPending() {
    if (!_initialized && !init()) return false;

    return LittleFS.exists(REFRESH_FLAG_PATH);
}

void ConfigStore::clearRefreshPending() {
    if (!_initialized && !init()) return;

    if (LittleFS.exists(REFRESH_FLAG_PATH)) {
        LittleFS.remove(REFRESH_FLAG_PATH);
    }
}
