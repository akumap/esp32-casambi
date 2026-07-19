/**
 * Casambi Cloud API Client Implementation
 */

#include "api_client.h"
#include "config_invariants.h"
#include <ArduinoJson.h>
#include <utility>   // std::move for the transactional config commit

#ifndef CASAMBI_TLS_INSECURE
// Mozilla root-CA bundle embedded by the arduino-esp32 core. Validating against
// the whole bundle (instead of pinning one certificate) authenticates
// api.casambi.com without breaking when Casambi rotates its CA. If a particular
// core build does not export this symbol, build with -DCASAMBI_TLS_INSECURE to
// fall back to the previous (unauthenticated) behaviour.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
#endif

CasambiAPIClient::CasambiAPIClient() {
}

void CasambiAPIClient::_beginRequest(const String& url) {
#ifdef CASAMBI_TLS_INSECURE
    // Explicit opt-out: no server-certificate validation. The cloud channel
    // carries the Casambi password and is then exposed to MITM — only for
    // builds where the CA bundle is unavailable.
    static bool warned = false;
    if (!warned) {
        Serial.println("API: WARNING - TLS certificate validation DISABLED "
                       "(CASAMBI_TLS_INSECURE)");
        warned = true;
    }
    _tls.setInsecure();
#else
    // Server-authenticated TLS: reject any certificate that does not chain to a
    // trusted root in the embedded bundle.
    _tls.setCACertBundle(rootca_crt_bundle_start);
#endif
    _http.begin(_tls, url);
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

    _beginRequest(url);
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

    _beginRequest(url);
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
    // Never log the session token itself: serial logs are frequently persisted,
    // pasted into support tickets or forwarded to network loggers, and the token
    // is a reusable credential for the duration of its validity. Log only that a
    // session was created, plus its length for coarse correlation.
    Serial.printf("API: Session created (token length %u)\n",
                  (unsigned)sessionToken.length());

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

    _beginRequest(url);
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

    // Optional raw dump for protocol analysis (fixture modes/settings, capability
    // signals). AES keys are redacted — see _dumpRedactedConfig. Opt-in via
    // `debug cloud on`; off by default so key material is never printed unasked.
    if (cloudDebugEnabled) {
        _dumpRedactedConfig(response);
    }

    // Transactional parse: build a scratch config first, and on full success
    // commit only the cloud-owned fields. A structurally broken response can
    // therefore never leave `config` half-overwritten — important for callers
    // that pass the LIVE networkConfig (serial setup) — and local settings
    // (auto-connect, NTP, debug flags, password) are never touched here.
    NetworkConfig parsed;
    if (!_parseNetworkConfig(response, parsed)) {
        return false;
    }
    config.networkName     = parsed.networkName;
    config.protocolVersion = parsed.protocolVersion;
    config.revision        = parsed.revision;
    config.keys   = std::move(parsed.keys);
    config.units  = std::move(parsed.units);
    config.groups = std::move(parsed.groups);
    config.scenes = std::move(parsed.scenes);
    return true;
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

    if (config.protocolVersion == 0) {
        _lastError = "Missing/zero protocolVersion";
        return false;
    }

    Serial.printf("Parse: Network '%s', protocol v%d, revision %d\n",
                  config.networkName.c_str(), config.protocolVersion, config.revision);

    // Sub-parser contract: a MISSING optional section is fine (Classic
    // networks have no keyStore; an empty network has no units/scenes/
    // groups), but a section that is present and structurally broken —
    // invalid key hex, duplicate ids, oversized lists — fails the WHOLE
    // parse. A partial configuration must never be committed or persisted:
    // it would silently drop units/keys and drift from the real network.
    //
    // This stays tolerant toward cloud API EVOLUTION: unknown JSON fields
    // and sections are ignored by construction (only known keys are read),
    // and stale references are dropped, not fatal. The hard failures above
    // are internal inconsistencies no API revision produces legitimately —
    // and failing keeps the previously working config in place, which is
    // the operationally tolerant outcome.

    // Parse keyStore (Evolution networks)
    if (network["keyStore"].is<JsonObjectConst>()) {
        JsonObjectConst keyStore = network["keyStore"];
        if (!keyStore["keys"].is<JsonArrayConst>()) {
            _lastError = "keyStore present but has no keys array";
            Serial.printf("Parse: FAILED - %s\n", _lastError.c_str());
            return false;
        }
        if (!_parseKeys(keyStore["keys"], config)) {
            Serial.printf("Parse: FAILED keys - %s\n", _lastError.c_str());
            return false;
        }
    } else {
        Serial.println("Parse: No keyStore (Classic network?)");
    }

    // Parse units (before groups — group members are validated against them)
    if (network["units"].is<JsonArrayConst>()) {
        if (!_parseUnits(network["units"], config)) {
            Serial.printf("Parse: FAILED units - %s\n", _lastError.c_str());
            return false;
        }
    } else {
        Serial.println("Parse: No units array (empty network?)");
    }

    // Parse scenes
    if (network["scenes"].is<JsonArrayConst>()) {
        if (!_parseScenes(network["scenes"], config)) {
            Serial.printf("Parse: FAILED scenes - %s\n", _lastError.c_str());
            return false;
        }
    }

    // Parse groups (from grid structure)
    if (network["grid"].is<JsonObjectConst>()) {
        if (!_parseGroups(network["grid"], config)) {
            Serial.printf("Parse: FAILED groups - %s\n", _lastError.c_str());
            return false;
        }
    }

    // Global structural invariants over the fully parsed result (pure,
    // host-tested — see config_invariants.h): duplicate ids would make the
    // getXById() lookups ambiguous, limits bound the heap, and the group-
    // member check re-verifies the stale-reference filter above.
    uint8_t badId = 0;
    cloudval::CloudLimits limits = { CLOUD_MAX_KEYS, CLOUD_MAX_UNITS,
                                     CLOUD_MAX_GROUPS, CLOUD_MAX_SCENES,
                                     CLOUD_MAX_GROUP_MEMBERS };
    cloudval::CloudInvariantResult inv =
        cloudval::validateStructure(config, limits, &badId);
    if (inv != cloudval::CLOUD_OK) {
        _lastError = String(cloudval::cloudInvariantName(inv)) +
                     " (id " + String(badId) + ")";
        Serial.printf("Parse: FAILED invariants - %s\n", _lastError.c_str());
        return false;
    }

    Serial.printf("Parse: Complete - %d keys, %d units, %d groups, %d scenes\n",
                  config.keys.size(), config.units.size(),
                  config.groups.size(), config.scenes.size());

    return true;
}

bool CasambiAPIClient::_parseKeys(const JsonArrayConst& keysArray, NetworkConfig& config) {
    config.keys.clear();

    if (keysArray.size() > CLOUD_MAX_KEYS) {
        _lastError = "keyStore exceeds " + String(CLOUD_MAX_KEYS) + " keys";
        return false;
    }

    for (JsonObjectConst keyObj : keysArray) {
        CasambiKey key;

        key.id = keyObj["id"].as<uint8_t>();
        key.type = keyObj["type"].as<uint8_t>();
        key.role = keyObj["role"].as<uint8_t>();
        key.name = keyObj["name"].as<String>();

        // Invalid key material is structural corruption, not something to
        // skip: a silently dropped key would make BLE auth fail later with
        // no hint at the cause. Fail the whole parse instead. (Duplicate ids
        // are caught by the post-parse invariant check in _parseNetworkConfig.)
        String keyHex = keyObj["key"].as<String>();
        if (!_hexToBytes(keyHex, key.key, AES_KEY_SIZE)) {
            _lastError = "invalid AES key hex for '" + key.name + "'";
            return false;
        }

        config.keys.push_back(key);
        Serial.printf("Parse: Key '%s' (id=%d, role=%d)\n", key.name.c_str(), key.id, key.role);
    }

    if (config.keys.empty()) {
        _lastError = "keyStore present but contains no keys";
        return false;
    }
    return true;
}

bool CasambiAPIClient::_parseUnits(const JsonArrayConst& unitsArray, NetworkConfig& config) {
    config.units.clear();

    if (unitsArray.size() > CLOUD_MAX_UNITS) {
        _lastError = "units list exceeds " + String(CLOUD_MAX_UNITS);
        return false;
    }

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

        // --- Parse capabilities from modes[0].state ---
        // State string length / 2 = number of control channels
        // 1 byte  ("ff")     = brightness only
        // 2 bytes ("ff00")   = brightness + 1 aux
        // 3 bytes ("ff3300") = brightness + 2 aux (vertical + temp)
        if (unitObj["modes"].is<JsonArrayConst>()) {
            JsonArrayConst modes = unitObj["modes"];
            if (modes.size() > 0) {
                JsonObjectConst mode0 = modes[0];
                if (mode0["state"].is<const char*>()) {
                    String stateStr = mode0["state"].as<String>();
                    unit.numChannels = stateStr.length() / 2;
                    if (unit.numChannels < 1) unit.numChannels = 1;
                    if (unit.numChannels > 3) unit.numChannels = 3;
                }
            }
        }

        // --- Parse CCT capability from settings ---
        if (unitObj["settings"].is<JsonObjectConst>()) {
            JsonObjectConst settings = unitObj["settings"];
            if (settings["cct.minKelvins"].is<float>()) {
                unit.hasCCT = true;
                unit.cctMinKelvin = static_cast<uint16_t>(settings["cct.minKelvins"].as<float>());
                unit.cctMaxKelvin = static_cast<uint16_t>(settings["cct.maxKelvins"].as<float>());
            }
        }

        // --- Derive hasVertical ---
        // If 3 channels: has both vertical and CCT
        // If 2 channels and hasCCT: aux is CCT, no vertical
        // If 2 channels and !hasCCT: aux is vertical
        if (unit.numChannels >= 3) {
            unit.hasVertical = true;
            // hasCCT should already be set from settings
        } else if (unit.numChannels == 2) {
            if (!unit.hasCCT) {
                unit.hasVertical = true;
            }
            // else: aux is CCT, hasVertical stays false
        }

        config.units.push_back(unit);

        // Build capability string for logging
        String caps = "dim";
        if (unit.hasVertical) caps += "+vertical";
        if (unit.hasCCT) caps += "+cct(" + String(unit.cctMinKelvin) + "-" + String(unit.cctMaxKelvin) + "K)";

        Serial.printf("Parse: Unit [%d] '%s' (type=%d, ch=%d, %s)\n",
                      unit.deviceId, unit.name.c_str(), unit.type,
                      unit.numChannels, caps.c_str());
    }

    // An empty (but well-formed) units array is a valid empty network.
    return true;
}

bool CasambiAPIClient::_parseGroups(const JsonObjectConst& gridObj, NetworkConfig& config) {
    config.groups.clear();

    // A grid without cells simply has no groups — not a structural error.
    if (!gridObj["cells"].is<JsonArrayConst>()) {
        return true;
    }

    JsonArrayConst cells = gridObj["cells"];

    for (JsonObjectConst cellObj : cells) {
        // Type 2 = group
        if (cellObj["type"].as<int>() != 2) continue;

        if (config.groups.size() >= CLOUD_MAX_GROUPS) {
            _lastError = "groups exceed " + String(CLOUD_MAX_GROUPS);
            return false;
        }

        CasambiGroup group;

        // Use actual groupID from the grid, not sequential
        group.groupId = cellObj["groupID"].as<uint8_t>();
        group.name = cellObj["name"].as<String>();

        // Parse group members. A member referencing a unit that is not in
        // the units list is stale cloud data — drop the member with a
        // warning rather than failing the refresh (controlling the rest of
        // the group still works), but a group so large it breaches the cap
        // is structural.
        if (cellObj["cells"].is<JsonArrayConst>()) {
            JsonArrayConst subCells = cellObj["cells"];
            for (JsonObjectConst subCell : subCells) {
                // Type 1 = unit
                if (subCell["type"].as<int>() != 1) continue;

                if (group.unitIds.size() >= CLOUD_MAX_GROUP_MEMBERS) {
                    _lastError = "group " + String(group.groupId) +
                                 " exceeds " + String(CLOUD_MAX_GROUP_MEMBERS) + " members";
                    return false;
                }
                uint8_t unitId = subCell["unit"].as<uint8_t>();
                if (!config.getUnitById(unitId)) {
                    Serial.printf("Parse: Group [%d] drops unknown unit %d (stale reference)\n",
                                  group.groupId, unitId);
                    continue;
                }
                group.unitIds.push_back(unitId);
            }
        }

        config.groups.push_back(group);
        Serial.printf("Parse: Group [%d] '%s' (%d units)\n",
                      group.groupId, group.name.c_str(), group.unitIds.size());
    }

    return true;
}

bool CasambiAPIClient::_parseScenes(const JsonArrayConst& scenesArray, NetworkConfig& config) {
    config.scenes.clear();

    if (scenesArray.size() > CLOUD_MAX_SCENES) {
        _lastError = "scenes list exceeds " + String(CLOUD_MAX_SCENES);
        return false;
    }

    for (JsonObjectConst sceneObj : scenesArray) {
        CasambiScene scene;

        scene.sceneId = sceneObj["sceneID"].as<uint8_t>();
        scene.name = sceneObj["name"].as<String>();

        config.scenes.push_back(scene);
        Serial.printf("Parse: Scene [%d] '%s'\n", scene.sceneId, scene.name.c_str());
    }

    return true;
}

bool CasambiAPIClient::_hexToBytes(const String& hex, uint8_t* bytes, size_t len) {
    if (hex.length() != len * 2) return false;

    for (size_t i = 0; i < len; i++) {
        String byteStr = hex.substring(i * 2, i * 2 + 2);
        bytes[i] = strtol(byteStr.c_str(), nullptr, 16);
    }

    return true;
}

// True only for a run of exactly AES_KEY_SIZE*2 hex digits — the shape of a
// keyStore AES key. The length guard makes a false positive on some other
// "key"-named field essentially impossible; even if one matched, redaction only
// removes data from a debug view, so it is harmless either way.
static bool _looksLikeAesKeyHex(const char* s, size_t len) {
    if (len != AES_KEY_SIZE * 2) return false;
    for (size_t k = 0; k < len; k++) {
        const char c = s[k];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

void CasambiAPIClient::_dumpRedactedConfig(const String& json) {
    const char* p = json.c_str();
    const size_t n = json.length();
    static const char* NEEDLE = "\"key\"";
    static const size_t NLEN  = 5;

    Serial.println("API: ---- raw cloud config (AES keys redacted) ----");

    // Emit verbatim in chunks; only the 32-hex value after a "key" field is
    // swapped for "***". `seg` marks the start of the not-yet-flushed run.
    size_t seg = 0;
    size_t i = 0;
    while (i < n) {
        if (i + NLEN <= n && memcmp(p + i, NEEDLE, NLEN) == 0) {
            size_t j = i + NLEN;
            while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ':')) j++;
            if (j < n && p[j] == '"') {
                const size_t valStart = j + 1;
                size_t valEnd = valStart;
                while (valEnd < n && p[valEnd] != '"') valEnd++;
                if (valEnd < n && _looksLikeAesKeyHex(p + valStart, valEnd - valStart)) {
                    // Flush up to and including the opening quote, then redact.
                    Serial.write(reinterpret_cast<const uint8_t*>(p + seg), valStart - seg);
                    Serial.print("***");
                    seg = valEnd;      // resume at the closing quote
                    i = valEnd;
                    continue;
                }
            }
        }
        i++;
    }
    if (seg < n) {
        Serial.write(reinterpret_cast<const uint8_t*>(p + seg), n - seg);
    }
    Serial.println();
    Serial.println("API: ---- end raw cloud config ----");
}
