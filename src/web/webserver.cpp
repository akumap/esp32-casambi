/**
 * Web Server Implementation
 */

#include "webserver.h"
#include "../config.h"
#include <ArduinoJson.h>
#include <WiFi.h>

#define WEB_LOG(...) do { if (webDebugEnabled) Serial.printf(__VA_ARGS__); } while(0)

CasambiWebServer::CasambiWebServer(CasambiClient* client, NetworkConfig* config)
    : _server(nullptr), _ws(nullptr), _client(client), _config(config), _running(false) {
}

CasambiWebServer::~CasambiWebServer() {
    stop();
}

bool CasambiWebServer::begin(uint16_t port) {
    if (_running) {
        Serial.println("Web: Server already running");
        return true;
    }

    _server = new AsyncWebServer(port);

    // Create WebSocket endpoint
    _ws = new AsyncWebSocket("/ws");
    _ws->onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len) {
        _handleWebSocketEvent(server, client, type, arg, data, len);
    });
    _server->addHandler(_ws);

    _setupRoutes();
    _server->begin();
    _running = true;

    WEB_LOG("Web: Server started on port %d (WebSocket: ws://[ip]:%d/ws)\n", port, port);
    return true;
}

void CasambiWebServer::stop() {
    if (_server && _running) {
        if (_ws) {
            _ws->closeAll();
        }
        _server->end();
        delete _server;
        _server = nullptr;
        // _ws is owned by _server (added via addHandler), do not delete separately
        _ws = nullptr;
        _running = false;
        Serial.println("Web: Server stopped");
    }
}

void CasambiWebServer::loop() {
    if (_ws) {
        _ws->cleanupClients();
    }
}

// ============================================================================
// WebSocket Support
// ============================================================================

void CasambiWebServer::_handleWebSocketEvent(AsyncWebSocket* server,
                                              AsyncWebSocketClient* client,
                                              AwsEventType type, void* arg,
                                              uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            WEB_LOG("WS: client #%u connected from %s\n",
                    client->id(), client->remoteIP().toString().c_str());
            // Send full state to the newly connected client
            client->text(_buildHelloMessage());
            break;

        case WS_EVT_DISCONNECT:
            WEB_LOG("WS: client #%u disconnected\n", client->id());
            break;

        case WS_EVT_ERROR:
            WEB_LOG("WS: client #%u error(%u): %s\n",
                    client->id(), *((uint16_t*)arg), (char*)data);
            break;

        case WS_EVT_DATA:
            // Incoming messages from clients are ignored for now.
            // Future: accept control commands via WebSocket.
            break;

        default:
            break;
    }
}

String CasambiWebServer::_buildHelloMessage() const {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["build"] = FIRMWARE_BUILD;
    doc["ble_connected"] = _client->isAuthenticated();

    JsonArray units = doc["units"].to<JsonArray>();
    for (const auto& unit : _config->units) {
        JsonObject u = units.add<JsonObject>();
        u["id"]          = unit.deviceId;
        u["name"]        = unit.name;
        u["address"]     = unit.address;     // BLE MAC address — stable identifier
        u["uuid"]        = unit.uuid;
        u["online"]      = unit.online;
        u["on"]          = unit.on;
        u["level"]       = unit.level;
        u["hasCCT"]      = unit.hasCCT;
        u["hasVertical"] = unit.hasVertical;
        u["numChannels"] = unit.numChannels;
        if (unit.hasVertical) u["vertical"]  = unit.vertical;
        if (unit.hasCCT) {
            u["colorTemp"] = unit.colorTemp;
            u["cctMin"]    = unit.cctMinKelvin;
            u["cctMax"]    = unit.cctMaxKelvin;
        }
    }

    String msg;
    serializeJson(doc, msg);
    return msg;
}

void CasambiWebServer::broadcastUnitState(uint8_t unitId, uint8_t level, bool online) {
    if (!_ws || _ws->count() == 0) return;

    JsonDocument doc;
    doc["type"]   = "unit_state";
    doc["id"]     = unitId;
    doc["level"]  = level;
    doc["online"] = online;
    doc["on"]     = (level > 0);

    // Enrich with current aux state from NetworkConfig (already updated before callback fires)
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (unit) {
        if (unit->hasVertical) doc["vertical"]  = unit->vertical;
        if (unit->hasCCT) {
            doc["colorTemp"] = unit->colorTemp;
            doc["cctMin"]    = unit->cctMinKelvin;
            doc["cctMax"]    = unit->cctMaxKelvin;
        }
    }

    String msg;
    serializeJson(doc, msg);
    _ws->textAll(msg);

    WEB_LOG("WS: broadcast unit_state id=%d level=%d online=%d (%u client(s))\n",
            unitId, level, online, _ws->count());
}

void CasambiWebServer::broadcastConnectionState(bool connected, int reason) {
    if (!_ws || _ws->count() == 0) return;

    JsonDocument doc;
    doc["type"]      = "connection_state";
    doc["connected"] = connected;
    if (reason != 0) doc["reason"] = reason;

    String msg;
    serializeJson(doc, msg);
    _ws->textAll(msg);

    WEB_LOG("WS: broadcast connection_state connected=%d reason=%d (%u client(s))\n",
            connected, reason, _ws->count());
}

void CasambiWebServer::_setupRoutes() {
    // Enable CORS for all endpoints
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // Status & discovery endpoints
    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetStatus(request);
    });

    _server->on("/api/units", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetUnits(request);
    });

    _server->on("/api/groups", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetGroups(request);
    });

    _server->on("/api/scenes", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetScenes(request);
    });

    // Reboot endpoint
    _server->on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(1000);
        ESP.restart();
    });

    // NotFound handler for POST requests to match dynamic routes
    _server->onNotFound([this](AsyncWebServerRequest *request) {
        if (request->method() != HTTP_POST) {
            _sendJsonError(request, "Endpoint not found", 404);
            return;
        }

        String path = request->url();

        // Scene control endpoints (no body needed)
        if (path.indexOf("/api/scenes/") == 0) {
            if (path.endsWith("/on")) {
                _handleSceneOn(request);
                return;
            } else if (path.endsWith("/off")) {
                _handleSceneOff(request);
                return;
            }
        }
        // Unit control endpoints (no body needed)
        else if (path.indexOf("/api/units/") == 0) {
            if (path.endsWith("/on")) {
                _handleUnitOn(request);
                return;
            } else if (path.endsWith("/off")) {
                _handleUnitOff(request);
                return;
            }
        }

        _sendJsonError(request, "Endpoint not found", 404);
    });

    // Generic POST handler for dynamic routes with body (level, color, temperature)
    // This works around ESPAsyncWebServer's poor regex support
    _server->onRequestBody([this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        // Only handle POST requests to /api/*
        if (request->method() != HTTP_POST || !request->url().startsWith("/api/")) {
            return;
        }

        String path = request->url();

        // Accumulate body for first chunk
        if (index == 0) {
            String* body = new String();
            body->reserve(total);
            request->_tempObject = body;
        }

        // Append data to body
        String* body = (String*)request->_tempObject;
        for (size_t i = 0; i < len; i++) {
            body->concat((char)data[i]);
        }

        // Process request on last chunk
        if (index + len == total) {
            // Route to appropriate handler
            // NOTE: Handlers are responsible for deleting the body after use
            if (path.indexOf("/api/scenes/") == 0 && path.endsWith("/level")) {
                _handleSceneLevel(request);
            } else if (path.indexOf("/api/units/") == 0) {
                if (path.endsWith("/level")) _handleUnitLevel(request);
                else if (path.endsWith("/color")) _handleUnitColor(request);
                else if (path.endsWith("/temperature")) _handleUnitTemperature(request);
                else if (path.endsWith("/slider")) _handleUnitSlider(request);
                else if (path.endsWith("/vertical")) _handleUnitVertical(request);
            } else if (path.indexOf("/api/groups/") == 0) {
                if (path.endsWith("/level")) _handleGroupLevel(request);
                else if (path.endsWith("/slider")) _handleGroupSlider(request);
                else if (path.endsWith("/vertical")) _handleGroupVertical(request);
            } else {
                // Unhandled endpoint - cleanup body
                delete body;
                request->_tempObject = nullptr;
            }
        }
    });

    // Root endpoint
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        String html = "<!DOCTYPE html><html><head><title>ESP32 Casambi</title></head><body>";
        html += "<h1>ESP32 Casambi Controller</h1>";
        html += "<p>API Endpoints:</p><ul>";
        html += "<li>GET /api/status - Connection status</li>";
        html += "<li>GET /api/units - List units</li>";
        html += "<li>GET /api/groups - List groups</li>";
        html += "<li>GET /api/scenes - List scenes</li>";
        html += "<li>POST /api/scenes/:id/on - Activate scene</li>";
        html += "<li>POST /api/scenes/:id/off - Deactivate scene</li>";
        html += "<li>POST /api/scenes/:id/level - Set scene level</li>";
        html += "<li>POST /api/units/:id/on - Turn unit on</li>";
        html += "<li>POST /api/units/:id/off - Turn unit off</li>";
        html += "<li>POST /api/units/:id/level - Set unit level</li>";
        html += "<li>POST /api/units/:id/color - Set unit color</li>";
        html += "<li>POST /api/units/:id/temperature - Set unit temperature</li>";
        html += "<li>POST /api/groups/:id/level - Set group level</li>";
        html += "</ul></body></html>";
        request->send(200, "text/html", html);
    });
}

// ============================================================================
// Status Endpoints
// ============================================================================

void CasambiWebServer::_handleGetStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;

    doc["ble_connected"] = _client->isAuthenticated();
    doc["ble_state"] = static_cast<int>(_client->getState());
    doc["network_name"] = _config->networkName;
    doc["wifi_ssid"] = WiFi.SSID();
    doc["wifi_ip"] = WiFi.localIP().toString();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["uptime_ms"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();

WEB_LOG("Web: /api/status from %s\n", _getClientIP(request).c_str());

    if (_client->isAuthenticated()) {
        doc["gateway_mac"] = _client->getConnectedAddress();
        doc["connection_uptime_ms"] = _client->getConnectionUptime();
        doc["packets_received"] = _client->getReceivedPacketCount();
    }

    if (_client->getLastDisconnectReason() != DisconnectReason::None) {
        doc["last_disconnect_reason"] = static_cast<int>(_client->getLastDisconnectReason());
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleGetUnits(AsyncWebServerRequest* request) {
    WEB_LOG("Web: /api/units from %s\n", _getClientIP(request).c_str());
    JsonDocument doc;
    JsonArray units = doc["units"].to<JsonArray>();
    for (const auto& unit : _config->units) {
        JsonObject u = units.add<JsonObject>();
        u["id"] = unit.deviceId;
        u["name"] = unit.name;
        u["type"] = unit.type;
        u["address"] = unit.address;
        u["online"] = unit.online;
        u["on"] = unit.on;
        u["level"] = unit.level;
        u["numChannels"] = unit.numChannels;
        if (unit.hasVertical) u["vertical"] = unit.vertical;
        if (unit.hasCCT) {
            u["colorTemp"] = unit.colorTemp;
            u["cctMin"] = unit.cctMinKelvin;
            u["cctMax"] = unit.cctMaxKelvin;
        }
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleGetGroups(AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray groups = doc["groups"].to<JsonArray>();

    for (const auto& group : _config->groups) {
        JsonObject g = groups.add<JsonObject>();
        g["id"] = group.groupId;
        g["name"] = group.name;

        JsonArray unit_ids = g["units"].to<JsonArray>();
        for (const auto& unit_id : group.unitIds) {
            unit_ids.add(unit_id);
        }
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleGetScenes(AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray scenes = doc["scenes"].to<JsonArray>();

    for (const auto& scene : _config->scenes) {
        JsonObject s = scenes.add<JsonObject>();
        s["id"] = scene.sceneId;
        s["name"] = scene.name;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// ============================================================================
// Scene Control Endpoints
// ============================================================================

void CasambiWebServer::_handleSceneOn(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract scene ID from path
    String path = request->url();
    int startIdx = path.indexOf("/scenes/") + 8;
    int endIdx = path.indexOf("/on");
    uint8_t sceneId = path.substring(startIdx, endIdx).toInt();

    // Find scene
    CasambiScene* scene = _config->getSceneById(sceneId);
    if (!scene) {
        _sendJsonError(request, "Scene not found", 404);
        return;
    }

    // Execute command
    _client->setSceneLevel(sceneId, 0xFF);

    WEB_LOG("Web: Scene %d (%s) ON from %s\n", sceneId, scene->name.c_str(), _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleSceneOff(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract scene ID from path
    String path = request->url();
    int startIdx = path.indexOf("/scenes/") + 8;
    int endIdx = path.indexOf("/off");
    uint8_t sceneId = path.substring(startIdx, endIdx).toInt();

    // Find scene
    CasambiScene* scene = _config->getSceneById(sceneId);
    if (!scene) {
        _sendJsonError(request, "Scene not found", 404);
        return;
    }

    // Execute command
    _client->setSceneLevel(sceneId, 0);

    WEB_LOG("Web: Scene %d (%s) OFF from %s\n", sceneId, scene->name.c_str(), _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleSceneLevel(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract scene ID from path
    String path = request->url();
    int startIdx = path.indexOf("/scenes/") + 8;
    int endIdx = path.indexOf("/level");
    uint8_t sceneId = path.substring(startIdx, endIdx).toInt();

    // Find scene
    CasambiScene* scene = _config->getSceneById(sceneId);
    if (!scene) {
        _sendJsonError(request, "Scene not found", 404);
        return;
    }

    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["level"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'level' parameter", 400);
        return;
    }

    uint8_t level = doc["level"];
    if (level > 255) {
        _sendJsonError(request, "Level must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setSceneLevel(sceneId, level);

    WEB_LOG("Web: Scene %d (%s) level=%d from %s\n", sceneId, scene->name.c_str(), level, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

// ============================================================================
// Unit Control Endpoints
// ============================================================================

void CasambiWebServer::_handleUnitOn(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/on");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Execute command
    _client->setUnitLevel(unitId, 255);

    WEB_LOG("Web: Unit %d (%s) ON from %s\n", unitId, unit->name.c_str(), _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitOff(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/off");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Execute command
    _client->setUnitLevel(unitId, 0);

    WEB_LOG("Web: Unit %d (%s) OFF from %s\n", unitId, unit->name.c_str(), _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitLevel(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/level");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["level"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'level' parameter", 400);
        return;
    }

    uint8_t level = doc["level"];
    if (level > 255) {
        _sendJsonError(request, "Level must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setUnitLevel(unitId, level);

    WEB_LOG("Web: Unit %d (%s) level=%d from %s\n", unitId, unit->name.c_str(), level, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitColor(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/color");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["r"].is<uint8_t>() || !doc["g"].is<uint8_t>() || !doc["b"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'r', 'g', or 'b' parameter", 400);
        return;
    }

    uint8_t r = doc["r"];
    uint8_t g = doc["g"];
    uint8_t b = doc["b"];

    if (r > 255 || g > 255 || b > 255) {
        _sendJsonError(request, "RGB values must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setUnitColor(unitId, r, g, b);

    WEB_LOG("Web: Unit %d (%s) color=(%d,%d,%d) from %s\n", unitId, unit->name.c_str(), r, g, b, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitTemperature(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/temperature");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["kelvin"].is<uint16_t>()) {
        _sendJsonError(request, "Missing 'kelvin' parameter", 400);
        return;
    }

    uint16_t kelvin = doc["kelvin"];
    if (kelvin < 1000 || kelvin > 10000) {
        _sendJsonError(request, "Kelvin must be 1000-10000", 400);
        return;
    }

    // Execute command
    _client->setUnitTemperature(unitId, kelvin);

    WEB_LOG("Web: Unit %d (%s) temperature=%dK from %s\n", unitId, unit->name.c_str(), kelvin, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

// ============================================================================
// Group Control Endpoints
// ============================================================================

void CasambiWebServer::_handleGroupLevel(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract group ID from path
    String path = request->url();
    int startIdx = path.indexOf("/groups/") + 8;
    int endIdx = path.indexOf("/level");
    uint8_t groupId = path.substring(startIdx, endIdx).toInt();

    // Find group
    CasambiGroup* group = _config->getGroupById(groupId);
    if (!group) {
        _sendJsonError(request, "Group not found", 404);
        return;
    }

    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["level"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'level' parameter", 400);
        return;
    }

    uint8_t level = doc["level"];
    if (level > 255) {
        _sendJsonError(request, "Level must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setGroupLevel(groupId, level);

    WEB_LOG("Web: Group %d (%s) level=%d from %s\n", groupId, group->name.c_str(), level, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitSlider(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/slider");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Parse JSON body
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["value"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'value' parameter", 400);
        return;
    }

    uint8_t value = doc["value"];
    if (value > 255) {
        _sendJsonError(request, "Value must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setUnitSlider(unitId, value);

    WEB_LOG("Web: Unit %d (%s) slider=%d from %s\n", unitId, unit->name.c_str(), value, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleUnitVertical(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract unit ID from path
    String path = request->url();
    int startIdx = path.indexOf("/units/") + 7;
    int endIdx = path.indexOf("/vertical");
    uint8_t unitId = path.substring(startIdx, endIdx).toInt();

    // Find unit
    CasambiUnit* unit = _config->getUnitById(unitId);
    if (!unit) {
        _sendJsonError(request, "Unit not found", 404);
        return;
    }

    // Parse JSON body
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["value"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'value' parameter", 400);
        return;
    }

    uint8_t value = doc["value"];
    if (value > 255) {
        _sendJsonError(request, "Value must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setUnitVertical(unitId, value);

    WEB_LOG("Web: Unit %d (%s) vertical=%d from %s\n", unitId, unit->name.c_str(), value, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleGroupSlider(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract group ID from path
    String path = request->url();
    int startIdx = path.indexOf("/groups/") + 8;
    int endIdx = path.indexOf("/slider");
    uint8_t groupId = path.substring(startIdx, endIdx).toInt();

    // Find group
    CasambiGroup* group = _config->getGroupById(groupId);
    if (!group) {
        _sendJsonError(request, "Group not found", 404);
        return;
    }

    // Parse JSON body
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["value"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'value' parameter", 400);
        return;
    }

    uint8_t value = doc["value"];
    if (value > 255) {
        _sendJsonError(request, "Value must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setGroupSlider(groupId, value);

    WEB_LOG("Web: Group %d (%s) slider=%d from %s\n", groupId, group->name.c_str(), value, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

void CasambiWebServer::_handleGroupVertical(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return;
    }

    // Extract group ID from path
    String path = request->url();
    int startIdx = path.indexOf("/groups/") + 8;
    int endIdx = path.indexOf("/vertical");
    uint8_t groupId = path.substring(startIdx, endIdx).toInt();

    // Find group
    CasambiGroup* group = _config->getGroupById(groupId);
    if (!group) {
        _sendJsonError(request, "Group not found", 404);
        return;
    }

    // Parse JSON body
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    // Cleanup body immediately after parsing
    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["value"].is<uint8_t>()) {
        _sendJsonError(request, "Missing 'value' parameter", 400);
        return;
    }

    uint8_t value = doc["value"];
    if (value > 255) {
        _sendJsonError(request, "Value must be 0-255", 400);
        return;
    }

    // Execute command
    _client->setGroupVertical(groupId, value);

    WEB_LOG("Web: Group %d (%s) vertical=%d from %s\n", groupId, group->name.c_str(), value, _getClientIP(request).c_str());
    _sendJsonSuccess(request);
}

// ============================================================================
// Utility Methods
// ============================================================================

void CasambiWebServer::_sendJsonError(AsyncWebServerRequest* request, const String& error, int code) {
    JsonDocument doc;
    doc["success"] = false;
    doc["error"] = error;
    String response;
    serializeJson(doc, response);
    request->send(code, "application/json", response);
}

void CasambiWebServer::_sendJsonSuccess(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["success"] = true;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

String CasambiWebServer::_getClientIP(AsyncWebServerRequest* request) {
    if (request->hasHeader("X-Forwarded-For")) {
        return request->header("X-Forwarded-For");
    }
    return request->client()->remoteIP().toString();
}
