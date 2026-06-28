/**
 * Web Server Implementation
 */

#include "webserver.h"
#include "../config.h"
#include "../log/event_log.h"
#include "../storage/config_store.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <memory>

#define WEB_LOG(...) do { if (webDebugEnabled) Serial.printf(__VA_ARGS__); } while(0)

CasambiWebServer::CasambiWebServer(CasambiClient* client, NetworkConfig* config)
    : _server(nullptr), _ws(nullptr), _client(client), _config(config), _running(false),
      _refreshRequested(false), _broadcastQueue(nullptr) {
    _broadcastQueue = xQueueCreate(WS_BROADCAST_QUEUE_DEPTH, sizeof(String*));
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

        // Drain and free any pending broadcast messages so nothing leaks when
        // the server is torn down while the BLE task is still posting.
        if (_broadcastQueue) {
            String* msg = nullptr;
            while (xQueueReceive(_broadcastQueue, &msg, 0) == pdTRUE) {
                delete msg;
            }
        }

        Serial.println("Web: Server stopped");
    }
}

void CasambiWebServer::loop() {
    // Drain the inter-task broadcast queue.  Messages are posted here from the
    // BLE task (broadcastUnitState / broadcastConnectionState) and sent out
    // here in the loop task so _ws->textAll() is never called from a BLE
    // callback — avoiding races with the async_tcp task's _clients management.
    if (_ws && _broadcastQueue) {
        String* msg = nullptr;
        while (xQueueReceive(_broadcastQueue, &msg, 0) == pdTRUE) {
            if (msg) {
                _ws->textAll(*msg);
                delete msg;
            }
        }
        // Enforce client limit: evict oldest clients beyond WS_MAX_CLIENTS.
        _ws->cleanupClients(WS_MAX_CLIENTS);
    }
}

bool CasambiWebServer::consumeRefreshRequest() {
    if (!_refreshRequested) return false;
    _refreshRequested = false;
    return true;
}

// ============================================================================
// WebSocket Support
// ============================================================================

void CasambiWebServer::_handleWebSocketEvent(AsyncWebSocket* server,
                                              AsyncWebSocketClient* client,
                                              AwsEventType type, void* arg,
                                              uint8_t* data, size_t len) {
    // Investigation trace (enable with 'debug heap on'): one line per WS
    // lifecycle event with live client count and heap, to locate a churn-time
    // leak (heap drops per cycle and never recovers) or crash (last line before
    // a reboot identifies where). Low volume: one line per connect/disconnect.
    if (heapDebugEnabled &&
        (type == WS_EVT_CONNECT || type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR)) {
        const char* ev = (type == WS_EVT_CONNECT) ? "CONNECT"
                       : (type == WS_EVT_DISCONNECT) ? "DISCONN" : "ERROR";
        Serial.printf("WSDBG %s id=%u count=%u free=%u largest=%u\n",
                      ev, client->id(), (unsigned)server->count(),
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }
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

String CasambiWebServer::_gatewayName(const String& mac) const {
    if (mac.isEmpty()) return String("");
    String want = mac; want.replace(":", ""); want.toLowerCase();

    // Try to resolve via the unit list (works only if the gateway advertises
    // its hardware MAC rather than a random static address).
    for (const auto& u : _config->units) {
        String a = u.address; a.replace(":", ""); a.toLowerCase();
        if (a == want) return u.name;
    }

    // Fallback: the advertised name captured at provisioning for this gateway.
    String ac = _config->autoConnectAddress; ac.replace(":", ""); ac.toLowerCase();
    if (!ac.isEmpty() && ac == want && _config->gatewayName.length())
        return _config->gatewayName;

    // No reliable name: the connection endpoint is typically a random network
    // gateway address that maps to no named unit. Leave empty rather than
    // mislabeling it (gatewayMac is the meaningful identifier).
    return String("");
}

String CasambiWebServer::_buildHelloMessage() const {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["build"] = FIRMWARE_BUILD;  // injected by scripts/build_number.py
    bool bleConn = _client->isAuthenticated();
    doc["ble_connected"] = bleConn;

    // Currently connected gateway (BLE MAC + resolved unit name) for transparency.
    String gwMac = bleConn ? _client->getConnectedAddress() : String("");
    JsonObject gw = doc["gateway"].to<JsonObject>();
    gw["connected"] = bleConn;
    gw["mac"]       = gwMac;
    gw["name"]      = _gatewayName(gwMac);

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
    if (!_ws || !_broadcastQueue) return;

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

    // Post to the inter-task queue; loop() drains it in the loop task.
    // Non-blocking: if the queue is full the message is silently dropped
    // (the next BLE notification will carry the latest state anyway).
    String* msg = new String();
    serializeJson(doc, *msg);
    if (xQueueSend(_broadcastQueue, &msg, 0) != pdTRUE) {
        delete msg;
        WEB_LOG("WS: broadcast queue full, dropped unit_state id=%d\n", unitId);
    } else {
        WEB_LOG("WS: queued unit_state id=%d level=%d online=%d\n",
                unitId, level, online);
    }
}

void CasambiWebServer::broadcastConnectionState(bool connected, int reason) {
    if (!_ws || !_broadcastQueue) return;

    JsonDocument doc;
    doc["type"]      = "connection_state";
    doc["connected"] = connected;
    if (reason != 0) doc["reason"] = reason;

    // Report which gateway we are on (empty when disconnected) for transparency.
    String gwMac = connected ? _client->getConnectedAddress() : String("");
    JsonObject gw = doc["gateway"].to<JsonObject>();
    gw["connected"] = connected;
    gw["mac"]       = gwMac;
    gw["name"]      = _gatewayName(gwMac);

    String* msg = new String();
    serializeJson(doc, *msg);
    if (xQueueSend(_broadcastQueue, &msg, 0) != pdTRUE) {
        delete msg;
        WEB_LOG("WS: broadcast queue full, dropped connection_state\n");
    } else {
        WEB_LOG("WS: queued connection_state connected=%d reason=%d\n",
                connected, reason);
    }
}

void CasambiWebServer::_setupRoutes() {
    // Enable CORS for all endpoints
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // Discovery: lets FHEM tell a configured gateway apart from one still in
    // setup mode (the portal serves the same path with configured:false).
    _server->on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        JsonDocument d;
        d["configured"] = true;
        d["build"]      = FIRMWARE_BUILD;
        d["network"]    = _config->networkName;
        d["mac"]        = WiFi.macAddress();
        d["ip"]         = WiFi.localIP().toString();
        String out; serializeJson(d, out);
        request->send(200, "application/json", out);
    });

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

    // Event-log endpoints
    _server->on("/api/log", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetLog(request);
    });
    _server->on("/api/log", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        _handleDeleteLog(request);
    });

    // NTP / time configuration
    _server->on("/api/ntp", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGetNtp(request);
    });

    // Reboot endpoint
    _server->on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(1000);
        ESP.restart();
    });

    // Re-read the Casambi cloud configuration using the stored network password.
    // The download frees the BLE stack, performs a TLS request and reboots, so
    // it cannot run inside this async handler — we only flag the request and let
    // the loop task carry it out (see consumeRefreshRequest / performCloudRefresh).
    _server->on("/api/refreshCasambi", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!ConfigStore::hasValidConfig()) {
            _sendJsonError(request, "No configuration found; run setup first", 409);
            return;
        }
        if (_config->casambiPassword.length() == 0) {
            _sendJsonError(request, "No stored Casambi password; refresh once via serial first", 409);
            return;
        }
        _refreshRequested = true;
        request->send(200, "application/json", "{\"status\":\"refreshing\"}");
    });

    // Catch-all request handler. ESPAsyncWebServer routes any request without a
    // matching server.on() handler here, and — crucially — it invokes this
    // request handler exactly ONCE per request (after the body, if any, has
    // arrived). It is therefore the single place that sends a response for the
    // dynamic POST routes. The companion onRequestBody below only BUFFERS the
    // body; it must never send, otherwise the request gets two responses and
    // the first response object leaks (~216 B per POST — the bug behind the
    // heap exhaustion under load).
    _server->onNotFound([this](AsyncWebServerRequest *request) {
        if (request->method() != HTTP_POST) {
            _sendJsonError(request, "Endpoint not found", 404);
            return;
        }

        String path = request->url();

        // --- No-body control endpoints (respond immediately) ---
        if (path.indexOf("/api/scenes/") == 0) {
            if (path.endsWith("/on"))  { _handleSceneOn(request);  return; }
            if (path.endsWith("/off")) { _handleSceneOff(request); return; }
        } else if (path.indexOf("/api/units/") == 0) {
            if (path.endsWith("/on"))  { _handleUnitOn(request);  return; }
            if (path.endsWith("/off")) { _handleUnitOff(request); return; }
        }

        // --- Body endpoints (body buffered into _tempObject by onRequestBody) ---
        bool bodyEndpoint =
            (path.indexOf("/api/scenes/") == 0 && path.endsWith("/level")) ||
            (path.indexOf("/api/units/")  == 0 && (path.endsWith("/level") ||
                path.endsWith("/color") || path.endsWith("/temperature") ||
                path.endsWith("/slider") || path.endsWith("/vertical"))) ||
            (path.indexOf("/api/groups/") == 0 && (path.endsWith("/level") ||
                path.endsWith("/slider") || path.endsWith("/vertical"))) ||
            (path == "/api/ntp");

        if (bodyEndpoint) {
            // Oversized payloads are not buffered by onRequestBody; reject here
            // (this is still the single response point, so no double-send).
            if (request->contentLength() > 512) {
                if (request->_tempObject) {
                    delete static_cast<String*>(request->_tempObject);
                    request->_tempObject = nullptr;
                }
                _sendJsonError(request, "Request body too large", 413);
                return;
            }
            // Each handler reads the buffered body, sends exactly one response,
            // and frees _tempObject. A missing body is handled inside them (400).
            if (path.indexOf("/api/scenes/") == 0) {
                if (path.endsWith("/level")) _handleSceneLevel(request);
            } else if (path.indexOf("/api/units/") == 0) {
                if      (path.endsWith("/level"))       _handleUnitLevel(request);
                else if (path.endsWith("/color"))       _handleUnitColor(request);
                else if (path.endsWith("/temperature")) _handleUnitTemperature(request);
                else if (path.endsWith("/slider"))      _handleUnitSlider(request);
                else if (path.endsWith("/vertical"))    _handleUnitVertical(request);
            } else if (path.indexOf("/api/groups/") == 0) {
                if      (path.endsWith("/level"))    _handleGroupLevel(request);
                else if (path.endsWith("/slider"))   _handleGroupSlider(request);
                else if (path.endsWith("/vertical")) _handleGroupVertical(request);
            } else if (path == "/api/ntp") {
                _handleSetNtp(request);
            }
            return;
        }

        _sendJsonError(request, "Endpoint not found", 404);
    });

    // Body collector for the dynamic POST routes. This ONLY accumulates the
    // request body into _tempObject; dispatch and the single response happen in
    // onNotFound above. (Sending from here as well would double-respond.)
    _server->onRequestBody([this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (request->method() != HTTP_POST || !request->url().startsWith("/api/")) {
            return;
        }

        // Don't buffer oversized bodies; onNotFound rejects them via
        // request->contentLength(). Our JSON payloads are tiny (< 64 bytes).
        if (total > 512) {
            return;
        }

        // Allocate the buffer on the first chunk and arm a disconnect handler so
        // an aborted (incomplete) body is freed — onNotFound never runs for a
        // request that never completes, so this is the only cleanup path then.
        if (index == 0) {
            String* body = new String();
            body->reserve(total);
            request->_tempObject = body;
            request->onDisconnect([request]() {
                if (request->_tempObject) {
                    delete static_cast<String*>(request->_tempObject);
                    request->_tempObject = nullptr;
                }
            });
        }

        String* body = (String*)request->_tempObject;
        if (!body) return;  // safety: should not happen after index==0 above
        body->concat((const char*)data, len);
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
        html += "<li>GET /api/log[?n=50] - Event log (newest first)</li>";
        html += "<li>DELETE /api/log - Clear event log</li>";
        html += "<li>GET /api/ntp - NTP server &amp; time status</li>";
        html += "<li>POST /api/ntp - Set NTP server ({\"server\":\"...\"})</li>";
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
    // Diagnostics: largest allocatable block exposes heap fragmentation (which
    // free_heap alone hides), and the all-time minimum free heap shows the
    // worst dip since boot. Both feed the stress-test's leak/fragmentation check.
    doc["largest_block"] = ESP.getMaxAllocHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["boot_count"] = EventLog::bootCount();
    doc["ntp_server"] = _config->ntpServer;

    // Wall-clock time (UTC). Only meaningful once NTP has synced.
    time_t nowSec = time(nullptr);
    bool timeSynced = nowSec >= 1577836800;  // >= 2020-01-01
    doc["time_synced"] = timeSynced;
    if (timeSynced) {
        doc["time_utc_ms"] = (int64_t)nowSec * 1000;
        char ts[32];
        struct tm tmUtc;
        gmtime_r(&nowSec, &tmUtc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
        doc["time_utc"] = ts;
    }

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
// Event-Log Endpoints
// ============================================================================

namespace {

// Print sink that writes into a fixed stack/struct buffer with no allocation.
// Used to serialize one log entry at a time for the chunked /api/log response.
class BufPrint : public Print {
public:
    BufPrint(char* buf, size_t cap) : _buf(buf), _cap(cap), _len(0) {}
    size_t write(uint8_t c) override {
        if (_len < _cap) _buf[_len++] = (char)c;
        return 1;
    }
    size_t write(const uint8_t* data, size_t len) override {
        size_t room = _cap - _len;
        size_t n = (len < room) ? len : room;
        memcpy(_buf + _len, data, n);
        _len += n;
        return len;
    }
    size_t length() const { return _len; }
private:
    char*  _buf;
    size_t _cap;
    size_t _len;
};

// Worst-case one entry as JSON: ~100 B of fixed fields plus a 120-char message
// that may expand 6× when JSON-escaped (\uXXXX) → ~820 B. 1 KB is a safe bound.
static const size_t LOG_ENTRY_STAGE = 1024;

// Resumable generator for the /api/log JSON array. Streams entries newest-first
// in HTTP chunks, holding only a single entry in RAM at a time — so a large log
// never needs a big contiguous buffer (which would fail on a fragmented heap).
struct LogJsonSource {
    static const size_t READ_BATCH = 4;   // records fetched per LittleFS open

    size_t cOlder = 0, cNewer = 0, total = 0, start = 0;
    size_t nextGlobal = 0;          // next global index to emit (descending)
    int    phase = 0;               // 0='[', 1=entries, 2=']', 3=done
    bool   first = true;
    char   stage[LOG_ENTRY_STAGE];
    size_t stageLen = 0, stagePos = 0;
    LogEntry batch[READ_BATCH];     // newest-first run, refilled on demand
    size_t batchCount = 0, batchPos = 0;

    size_t fill(uint8_t* out, size_t maxLen) {
        size_t produced = 0;
        while (produced < maxLen) {
            // 1) Drain any bytes already staged.
            if (stagePos < stageLen) {
                size_t avail = stageLen - stagePos;
                size_t room  = maxLen - produced;
                size_t cp    = (avail < room) ? avail : room;
                memcpy(out + produced, stage + stagePos, cp);
                stagePos += cp;
                produced += cp;
                continue;
            }
            // 2) Staging empty → produce the next piece.
            stageLen = stagePos = 0;
            if (phase == 0) {
                stage[stageLen++] = '[';
                phase = 1;
                continue;
            }
            if (phase == 1) {
                // Refill the batch (one flash open per READ_BATCH records).
                if (batchPos >= batchCount) {
                    if (nextGlobal > start) {
                        size_t from = nextGlobal - 1;
                        batchCount = EventLog::readDescRun(from, start, READ_BATCH,
                                                           cOlder, cNewer, batch);
                        batchPos = 0;
                        nextGlobal = from - batchCount + 1;
                        if (batchCount == 0) phase = 2;   // read failure → stop
                        continue;
                    }
                    phase = 2;
                    continue;
                }
                // Serialize one entry from the batch into the stage buffer.
                BufPrint bp(stage, sizeof(stage));
                if (!first) bp.write(',');
                first = false;
                EventLog::writeEntryJson(bp, batch[batchPos++]);
                stageLen = bp.length();
                continue;
            }
            if (phase == 2) {
                stage[stageLen++] = ']';
                phase = 3;
                continue;
            }
            break;              // phase 3: done
        }
        return produced;        // 0 once fully drained → ends the response
    }
};

}  // namespace

void CasambiWebServer::_handleGetLog(AsyncWebServerRequest* request) {
    WEB_LOG("Web: /api/log from %s\n", _getClientIP(request).c_str());

    // Default to the newest 25 entries (~3.5 KB JSON) so a plain GET fits in a
    // single TCP window and the framework's per-chunk malloc(space) (~5.5 KB)
    // stays within the largest free block. Larger responses span multiple chunks
    // and, on this device's tight/fragmented heap (largest block ~13 KB), can
    // fail to send (client sees HTTP/0.9). Use ?n=<count> for more (?n=0 = all)
    // at your own risk, or fetch the full log over serial ('log N').
    int n = 25;
    if (request->hasParam("n")) {
        n = request->getParam("n")->value().toInt();
        if (n <= 0) n = -1;   // ?n=0 → all entries
    }

    // Stream the array in HTTP chunks: the source reads one record at a time, so
    // only ~1 KB is ever held regardless of log size. Avoids the big contiguous
    // allocation an AsyncResponseStream would grow to hold the whole array.
    auto src = std::make_shared<LogJsonSource>();
    EventLog::snapshotNewest(n, src->cOlder, src->cNewer, src->total, src->start);
    src->nextGlobal = src->total;   // descend to src->start

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json",
        [src](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            return src->fill(buffer, maxLen);
        });
    request->send(response);
}

void CasambiWebServer::_handleDeleteLog(AsyncWebServerRequest* request) {
    WEB_LOG("Web: DELETE /api/log from %s\n", _getClientIP(request).c_str());
    EventLog::clear();
    _sendJsonSuccess(request);
}

// ============================================================================
// NTP / Time Configuration Endpoints
// ============================================================================

void CasambiWebServer::_handleGetNtp(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["ntp_server"] = _config->ntpServer;

    time_t nowSec = time(nullptr);
    bool timeSynced = nowSec >= 1577836800;
    doc["time_synced"] = timeSynced;
    if (timeSynced) {
        char ts[32];
        struct tm tmUtc;
        gmtime_r(&nowSec, &tmUtc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
        doc["time_utc"] = ts;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleSetNtp(AsyncWebServerRequest* request) {
    // Parse JSON body (stored as String* in _tempObject by body handler)
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return;
    }

    String* bodyStr = (String*)request->_tempObject;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *bodyStr);

    delete bodyStr;
    request->_tempObject = nullptr;

    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return;
    }

    if (!doc["server"].is<const char*>()) {
        _sendJsonError(request, "Missing 'server' parameter", 400);
        return;
    }

    String server = doc["server"].as<String>();
    server.trim();
    if (server.length() == 0 || server.length() > 64) {
        _sendJsonError(request, "Invalid 'server' value", 400);
        return;
    }

    _config->ntpServer = server;
    ConfigStore::saveNetworkConfig(*_config);

    // Re-arm NTP immediately (WiFi is up since the web server is running).
    configTime(0, 0, server.c_str());

    WEB_LOG("Web: NTP server set to %s from %s\n", server.c_str(), _getClientIP(request).c_str());
    EventLog::log(LOG_INFO, "NTP server changed to %s", server.c_str());

    JsonDocument resp;
    resp["success"] = true;
    resp["ntp_server"] = server;
    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
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
        _sendJsonError(request, "Missing 'level' parameter (0-255)", 400);
        return;
    }

    uint8_t level = doc["level"];

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
        _sendJsonError(request, "Missing 'level' parameter (0-255)", 400);
        return;
    }

    uint8_t level = doc["level"];

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
        _sendJsonError(request, "Missing or out-of-range 'r', 'g', or 'b' parameter (0-255)", 400);
        return;
    }

    uint8_t r = doc["r"];
    uint8_t g = doc["g"];
    uint8_t b = doc["b"];

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
        _sendJsonError(request, "Missing 'level' parameter (0-255)", 400);
        return;
    }

    uint8_t level = doc["level"];

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
        _sendJsonError(request, "Missing 'value' parameter (0-255)", 400);
        return;
    }

    uint8_t value = doc["value"];

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
        _sendJsonError(request, "Missing 'value' parameter (0-255)", 400);
        return;
    }

    uint8_t value = doc["value"];

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
        _sendJsonError(request, "Missing 'value' parameter (0-255)", 400);
        return;
    }

    uint8_t value = doc["value"];

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
        _sendJsonError(request, "Missing 'value' parameter (0-255)", 400);
        return;
    }

    uint8_t value = doc["value"];

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
