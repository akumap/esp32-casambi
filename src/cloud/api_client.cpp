/**
 * Casambi Cloud API Client Implementation
 */

#include "api_client.h"
#include <ArduinoJson.h>

CasambiAPIClient::CasambiAPIClient() {
}

CasambiAPIClient::~CasambiAPIClient() {
    disconnectWiFi();
}

bool CasambiAPIClient::connectWiFi(const String& ssid, const String& password) {
    Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());

    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        _lastError = "WiFi connection timeout";
        return false;
    }

    Serial.println("WiFi connected");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void CasambiAPIClient::disconnectWiFi() {
    WiFi.disconnect();
}

bool CasambiAPIClient::isWiFiConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool CasambiAPIClient::getNetworkId(const String& uuid, String& networkId) {
    if (!isWiFiConnected()) {
        _lastError = "WiFi not connected";
        return false;
    }

    String url = String(CASAMBI_API_BASE) + API_NETWORK_UUID_PATH + uuid;

    Serial.printf("API: GET %s\n", url.c_str());

    _http.begin(url);
    _http.setTimeout(API_REQUEST_TIMEOUT_MS);

    int httpCode = _http.GET();

    if (httpCode != 200) {
        _lastError = "HTTP " + String(httpCode);
        Serial.printf("API: Failed: %s\n", _lastError.c_str());
        _http.end();
        return false;
    }

    String response = _http.getString();
    _http.end();

    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        _lastError = "JSON parse error: " + String(error.c_str());
        Serial.printf("API: %s\n", _lastError.c_str());
        return false;
    }

    if (!doc["id"].is<String>()) {
        _lastError = "Response missing 'id' field";
        return false;
    }

    networkId = doc["id"].as<String>();
    Serial.printf("API: Network ID: %s\n", networkId.c_str());

    return true;
}

bool CasambiAPIClient::createSession(const String& networkId, const String& password, String& sessionToken) {
    if (!isWiFiConnected()) {
        _lastError = "WiFi not connected";
        return false;
    }

    String url = String(CASAMBI_API_BASE) + API_NETWORK_SESSION_PATH + networkId + "/session";

    Serial.printf("API: POST %s\n", url.c_str());

    // Build request body
    JsonDocument doc;
    doc["password"] = password;
    doc["deviceName"] = DEVICE_NAME;

    String requestBody;
    serializeJson(doc, requestBody);

    _http.begin(url);
    _http.setTimeout(API_REQUEST_TIMEOUT_MS);
    _http.addHeader("Content-Type", "application/json");

    int httpCode = _http.POST(requestBody);

    if (httpCode != 200) {
        _lastError = "HTTP " + String(httpCode);
        if (httpCode == 401 || httpCode == 403) {
            _lastError += " (Invalid password)";
        }
        Serial.printf("API: Failed: %s\n", _lastError.c_str());
        String errorBody = _http.getString();
        Serial.printf("API: Response: %s\n", errorBody.c_str());
        _http.end();
        return false;
    }

    String response = _http.getString();
    _http.end();

    // Parse JSON response
    JsonDocument responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);

    if (error) {
        _lastError = "JSON parse error: " + String(error.c_str());
        Serial.printf("API: %s\n", _lastError.c_str());
        return false;
    }

    if (!responseDoc["session"].is<String>()) {
        _lastError = "Response missing 'session' field";
        return false;
    }

    sessionToken = responseDoc["session"].as<String>();
    Serial.printf("API: Session created: %s\n", sessionToken.c_str());

    return true;
}

bool CasambiAPIClient::fetchNetworkConfig(const String& networkId, const String& sessionToken, NetworkConfig& config) {
    if (!isWiFiConnected()) {
        _lastError = "WiFi not connected";
        return false;
    }

    String url = String(CASAMBI_API_BASE) + API_NETWORK_CONFIG_PATH + networkId + "/";

    Serial.printf("API: PUT %s\n", url.c_str());

    // Build request body
    JsonDocument doc;
    doc["formatVersion"] = 1;
    doc["deviceName"] = DEVICE_NAME;
    doc["revision"] = 0;

    String requestBody;
    serializeJson(doc, requestBody);

    _http.begin(url);
    _http.setTimeout(API_REQUEST_TIMEOUT_MS);
    _http.addHeader("Content-Type", "application/json");
    _http.addHeader("X-Casambi-Session", sessionToken);

    int httpCode = _http.PUT(requestBody);

    if (httpCode != 200) {
        _lastError = "HTTP " + String(httpCode);
        Serial.printf("API: Failed: %s\n", _lastError.c_str());
        String errorBody = _http.getString();
        Serial.printf("API: Response: %s\n", errorBody.c_str());
        _http.end();
        return false;
    }

    String response = _http.getString();
    _http.end();

    Serial.printf("API: Received %d bytes\n", response.length());

    // Parse network configuration
    return _parseNetworkConfig(response, config);
}

bool CasambiAPIClient::_parseNetworkConfig(const String& json, NetworkConfig& config) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    if (error) {
        _lastError = "JSON parse error: " + String(error.c_str());
        Serial.printf("Parse: %s\n", _lastError.c_str());
        return false;
    }

    // Check for network object
    if (!doc["network"].is<JsonObjectConst>()) {
        _lastError = "Missing 'network' object";
        return false;
    }

    JsonObjectConst network = doc["network"].as<JsonObjectConst>();

    // Parse basic network info
    config.networkName = network["name"].as<String>();
    config.protocolVersion = network["protocolVersion"].as<uint8_t>();
    config.revision = network["revision"].as<int>();

    Serial.printf("Parse: Network '%s', protocol v%d, revision %d\n",
                  config.networkName.c_str(), config.protocolVersion, config.revision);

    // Parse keyStore (Evolution networks)
    if (network["keyStore"].is<JsonObjectConst>()) {
        JsonObjectConst keyStore = network["keyStore"];
        if (keyStore["keys"].is<JsonArrayConst>()) {
            JsonArrayConst keys = keyStore["keys"];
            if (!_parseKeys(keys, config)) {
                Serial.println("Parse: Warning - Failed to parse keys");
            }
        }
    } else {
        Serial.println("Parse: No keyStore (Classic network?)");
    }

    // Parse units
    if (network["units"].is<JsonArrayConst>()) {
        JsonArrayConst units = network["units"];
        if (!_parseUnits(units, config)) {
            Serial.println("Parse: Warning - Failed to parse units");
        }
    }

    // Parse scenes
    if (network["scenes"].is<JsonArrayConst>()) {
        JsonArrayConst scenes = network["scenes"];
        if (!_parseScenes(scenes, config)) {
            Serial.println("Parse: Warning - Failed to parse scenes");
        }
    }

    // Parse groups (from grid structure)
    if (network["grid"].is<JsonObjectConst>()) {
        JsonObjectConst grid = network["grid"];
        if (!_parseGroups(grid, config)) {
            Serial.println("Parse: Warning - Failed to parse groups");
        }
    }

    Serial.printf("Parse: Complete - %d keys, %d units, %d groups, %d scenes\n",
                  config.keys.size(), config.units.size(),
                  config.groups.size(), config.scenes.size());

    return true;
}

bool CasambiAPIClient::_parseKeys(const JsonArrayConst& keysArray, NetworkConfig& config) {
    config.keys.clear();

    for (JsonObjectConst keyObj : keysArray) {
        CasambiKey key;

        key.id = keyObj["id"].as<uint8_t>();
        key.type = keyObj["type"].as<uint8_t>();
        key.role = keyObj["role"].as<uint8_t>();
        key.name = keyObj["name"].as<String>();

        // Parse hex key string
        String keyHex = keyObj["key"].as<String>();
        if (!_hexToBytes(keyHex, key.key, AES_KEY_SIZE)) {
            Serial.printf("Parse: Invalid key hex for '%s'\n", key.name.c_str());
            continue;
        }

        config.keys.push_back(key);
        Serial.printf("Parse: Key '%s' (id=%d, role=%d)\n", key.name.c_str(), key.id, key.role);
    }

    return config.keys.size() > 0;
}

bool CasambiAPIClient::_parseUnits(const JsonArrayConst& unitsArray, NetworkConfig& config) {
    config.units.clear();

    for (JsonObjectConst unitObj : unitsArray) {
        CasambiUnit unit;

        unit.deviceId = unitObj["deviceID"].as<uint8_t>();
        unit.type = unitObj["type"].as<uint16_t>();
        unit.uuid = unitObj["uuid"].as<String>();
        unit.address = unitObj["address"].as<String>();
        unit.name = unitObj["name"].as<String>();
        unit.firmware = unitObj["firmware"].as<String>();
        unit.online = false;
        unit.on = false;

        config.units.push_back(unit);
        Serial.printf("Parse: Unit [%d] '%s'\n", unit.deviceId, unit.name.c_str());
    }

    return config.units.size() > 0;
}

bool CasambiAPIClient::_parseGroups(const JsonObjectConst& gridObj, NetworkConfig& config) {
    config.groups.clear();

    if (!gridObj["cells"].is<JsonArrayConst>()) {
        return false;
    }

    JsonArrayConst cells = gridObj["cells"];

    uint8_t groupId = 0;
    for (JsonObjectConst cellObj : cells) {
        // Type 2 = group
        if (cellObj["type"].as<int>() != 2) continue;

        CasambiGroup group;
        group.groupId = groupId++;
        group.name = cellObj["name"].as<String>();

        // Parse group members
        if (cellObj["cells"].is<JsonArrayConst>()) {
            JsonArrayConst subCells = cellObj["cells"];
            for (JsonObjectConst subCell : subCells) {
                // Type 1 = unit
                if (subCell["type"].as<int>() == 1) {
                    uint8_t unitId = subCell["deviceID"].as<uint8_t>();
                    group.unitIds.push_back(unitId);
                }
            }
        }

        config.groups.push_back(group);
        Serial.printf("Parse: Group [%d] '%s' (%d units)\n",
                      group.groupId, group.name.c_str(), group.unitIds.size());
    }

    return config.groups.size() > 0;
}

bool CasambiAPIClient::_parseScenes(const JsonArrayConst& scenesArray, NetworkConfig& config) {
    config.scenes.clear();

    for (JsonObjectConst sceneObj : scenesArray) {
        CasambiScene scene;

        // Casambi API uses "sceneID" (capital ID), not "id"
        scene.sceneId = sceneObj["sceneID"].as<uint8_t>();
        scene.name = sceneObj["name"].as<String>();

        config.scenes.push_back(scene);
        Serial.printf("Parse: Scene [%d] '%s'\n", scene.sceneId, scene.name.c_str());
    }

    return config.scenes.size() > 0;
}

bool CasambiAPIClient::_hexToBytes(const String& hex, uint8_t* bytes, size_t len) {
    if (hex.length() != len * 2) return false;

    for (size_t i = 0; i < len; i++) {
        String byteStr = hex.substring(i * 2, i * 2 + 2);
        bytes[i] = strtol(byteStr.c_str(), nullptr, 16);
    }

    return true;
}
