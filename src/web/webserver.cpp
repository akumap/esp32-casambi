/**
 * Web Server Implementation
 */

#include "webserver.h"
#include "../config.h"
#include "../log/event_log.h"
#include "../storage/config_store.h"
#include "../ble/packet.h"   // packetParseStats() for /api/status diagnostics
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <memory>
#include <mbedtls/sha256.h>

#define WEB_LOG(...) do { if (webDebugEnabled) Serial.printf(__VA_ARGS__); } while(0)

// Copy a mutable NetworkConfig string field under g_configMutex so a
// concurrent writer on the loop task cannot reallocate the String buffer
// while the async_tcp task is reading it.
static String lockedCopy(const String& s) {
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    String out = s;
    if (g_configMutex) xSemaphoreGive(g_configMutex);
    return out;
}

// Emit a unit's channels as a generic `controls` array so consumers (FHEM)
// derive reading names from the cloud control types instead of hardcoding
// vertical/colorTemp. Each entry: { type, value }, and temperature adds the
// resolved kelvin plus its min/max bounds. Caller must already hold
// g_configMutex (control values are written by the BLE task). No-op for units
// without a fixture definition (older configs) — the legacy fields still ship.
static void addUnitControls(JsonObject u, const CasambiUnit& unit) {
    if (unit.controls.empty()) return;
    JsonArray arr = u["controls"].to<JsonArray>();
    for (const auto& c : unit.controls) {
        JsonObject co = arr.add<JsonObject>();
        co["type"]  = c.typeName;
        co["value"] = c.value;
        if (c.typeName == "temperature" && c.max > c.min) {
            co["kelvin"] = (uint16_t)(c.min + (uint32_t)c.value * (c.max - c.min) / 255);
            co["min"]    = c.min;
            co["max"]    = c.max;
        }
    }
}

CasambiWebServer::CasambiWebServer(CasambiClient* client, NetworkConfig* config)
    : _server(nullptr), _ws(nullptr), _client(client), _config(config), _running(false),
      _refreshRequested(false), _rebootRequested(false), _clearLogRequested(false),
      _ntpRequested(false),
      _broadcastQueue(nullptr), _bleCmdQueue(nullptr), _resyncNeeded(false),
      _wsDropCount(0) {
    _broadcastQueue = xQueueCreate(WS_BROADCAST_QUEUE_DEPTH, sizeof(WsEvent));
    _bleCmdQueue    = xQueueCreate(BLE_CMD_QUEUE_DEPTH, sizeof(BleCommand));
}

CasambiWebServer::~CasambiWebServer() {
    stop();
    // Both queues carry plain values — nothing to free per entry, pending
    // items are simply dropped with the queue. Callers must ensure no task
    // can still hold a pointer to this instance (see main.cpp teardown).
    if (_broadcastQueue) {
        vQueueDelete(_broadcastQueue);
        _broadcastQueue = nullptr;
    }
    if (_bleCmdQueue) {
        vQueueDelete(_bleCmdQueue);
        _bleCmdQueue = nullptr;
    }
}

bool CasambiWebServer::begin(uint16_t port) {
    if (_running) {
        Serial.println("Web: Server already running");
        return true;
    }

    _server = new AsyncWebServer(port);

    // Derive the API token from the stored Casambi password. Empty when no
    // password is stored → authentication stays disabled (backward compatible).
    _deriveApiToken();
    if (_apiToken.isEmpty()) {
        Serial.println("Web: API auth DISABLED (no Casambi password stored)");
    } else {
        WEB_LOG("Web: API auth enabled (X-API-Key required)\n");
    }

    // Create WebSocket endpoint
    _ws = new AsyncWebSocket("/ws");
    _ws->onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len) {
        _handleWebSocketEvent(server, client, type, arg, data, len);
    });
    // Gate the WebSocket upgrade on the API token. A rejected handshake is not
    // handled here and falls through to onNotFound, which answers 401 for /ws
    // so clients (FHEM) can tell an auth problem from a wrong path; the FHEM
    // module then backs off and retries via /api/info.
    _ws->setFilter([this](AsyncWebServerRequest* request) {
        return _wsAuthOk(request);
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

        // Drop any pending broadcast events (plain values, nothing to free).
        if (_broadcastQueue) {
            xQueueReset(_broadcastQueue);
        }

        Serial.println("Web: Server stopped");
    }
}

void CasambiWebServer::loop() {
    // Execute at most ONE queued control command per call. The REST handlers
    // (async_tcp task) only validate and enqueue; the BLE mutex wait (up to
    // 1 s) and the GATT write happen here on the loop task, so HTTP/WebSocket
    // traffic is never stalled behind a BLE operation. One per call bounds
    // each loop iteration to a single blocking operation.
    if (_bleCmdQueue) {
        BleCommand cmd;
        if (xQueueReceive(_bleCmdQueue, &cmd, 0) == pdTRUE) {
            _executeBleCommand(cmd);
        }
    }

    // Drain the inter-task broadcast queue.  Events are posted here from the
    // BLE task (broadcastUnitState / broadcastConnectionState) as plain
    // values; the JSON is built HERE at send time — so the BLE task never
    // allocates, _ws->textAll() is never called from a BLE callback, and with
    // no client connected the serialization is skipped entirely (the queue is
    // still drained so stale events don't linger).
    if (_ws && _broadcastQueue) {
        const bool haveClients = (_ws->count() > 0);
        WsEvent ev;
        while (xQueueReceive(_broadcastQueue, &ev, 0) == pdTRUE) {
            if (haveClients) {
                _sendWsEvent(ev);
            }
        }

        // A broadcast was dropped (queue full): push a fresh full snapshot so
        // clients cannot keep a stale unit state — a missed unit_state would
        // otherwise only be corrected by the unit's NEXT change or a reconnect.
        // exchange() so a set racing this clear is never lost.
        if (_resyncNeeded.exchange(false)) {
            if (_ws->count() > 0) {
                _ws->textAll(_buildHelloMessage());
                WEB_LOG("WS: broadcast drop -> resync hello pushed\n");
            }
        }

        // Enforce client limit: evict oldest clients beyond WS_MAX_CLIENTS.
        _ws->cleanupClients(WS_MAX_CLIENTS);
    }
}

bool CasambiWebServer::consumeRefreshRequest() {
    return _refreshRequested.exchange(false);
}

bool CasambiWebServer::consumeRebootRequest() {
    return _rebootRequested.exchange(false);
}

bool CasambiWebServer::consumeClearLogRequest() {
    return _clearLogRequested.exchange(false);
}

bool CasambiWebServer::consumeNtpRequest(String& serverOut) {
    // Cheap unlocked pre-check for the loop hot path; a flag set right after
    // it is picked up on the next iteration.
    if (!_ntpRequested.load()) return false;
    // Transactional consume: flag and string are read/cleared under the same
    // mutex hold the writer (_handleSetNtp) uses to set them, so the pair can
    // never be observed half-updated.
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    const bool had = _ntpRequested.exchange(false);
    if (had) {
        serverOut = _pendingNtpServer;
        _pendingNtpServer = "";
    }
    if (g_configMutex) xSemaphoreGive(g_configMutex);
    return had;
}

// ============================================================================
// Authentication
// ============================================================================

void CasambiWebServer::_deriveApiToken() {
    _apiToken = "";
    if (!_config || _config->casambiPassword.isEmpty()) return;

    // apiToken = hex( SHA-256( API_TOKEN_PREFIX || casambiPassword ) )
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const uint8_t*)API_TOKEN_PREFIX, strlen(API_TOKEN_PREFIX));
    mbedtls_sha256_update(&ctx, (const uint8_t*)_config->casambiPassword.c_str(),
                          _config->casambiPassword.length());
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[sizeof(hash) * 2 + 1];
    for (size_t i = 0; i < sizeof(hash); i++) sprintf(hex + i * 2, "%02x", hash[i]);
    hex[sizeof(hash) * 2] = '\0';
    _apiToken = String(hex);
}

bool CasambiWebServer::_constantTimeEquals(const String& a, const String& b) {
    // Compare without an early exit so the time taken does not reveal how many
    // leading characters matched. Length mismatch still short-circuits (lengths
    // are not secret), but equal-length comparisons run in constant time.
    if (a.length() != b.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return diff == 0;
}

bool CasambiWebServer::_authOk(AsyncWebServerRequest* request) {
    if (_apiToken.isEmpty()) return true;  // auth disabled

    if (request->hasHeader(API_KEY_HEADER) &&
        _constantTimeEquals(request->header(API_KEY_HEADER), _apiToken)) {
        return true;
    }

    // An unauthenticated POST may already have a buffered body in _tempObject
    // (onRequestBody runs before this dispatch); _sendJsonError frees it.
    WEB_LOG("Web: 401 unauthenticated request to %s\n", request->url().c_str());
    _sendJsonError(request, "Unauthorized", 401);
    return false;
}

bool CasambiWebServer::_wsAuthOk(AsyncWebServerRequest* request) const {
    if (_apiToken.isEmpty()) return true;  // auth disabled
    if (request->hasHeader(API_KEY_HEADER) &&
        _constantTimeEquals(request->header(API_KEY_HEADER), _apiToken)) {
        return true;
    }
    // Browsers cannot set custom headers on a WebSocket handshake; allow the
    // token as a query parameter too (?k=<token>).
    if (request->hasParam("k") &&
        _constantTimeEquals(request->getParam("k")->value(), _apiToken)) {
        return true;
    }
    return false;
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
    // its hardware MAC rather than a random static address). Unit strings are
    // only written before the web server exists (boot/refresh), so reading
    // them here without the config mutex is safe.
    for (const auto& u : _config->units) {
        String a = u.address; a.replace(":", ""); a.toLowerCase();
        if (a == want) return u.name;
    }

    // Fallback: the advertised name captured at provisioning for this gateway.
    // autoConnectAddress can be rewritten at runtime ('autoconnect set',
    // connect command) → copy it under the config mutex.
    String ac = lockedCopy(_config->autoConnectAddress);
    ac.replace(":", ""); ac.toLowerCase();
    String gwName = lockedCopy(_config->gatewayName);
    if (!ac.isEmpty() && ac == want && gwName.length())
        return gwName;

    // No reliable name: the connection endpoint is typically a random network
    // gateway address that maps to no named unit. Leave empty rather than
    // mislabeling it (gatewayMac is the meaningful identifier).
    return String("");
}

String CasambiWebServer::_buildHelloMessage() const {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["build"] = FIRMWARE_BUILD;  // from generated firmware_build.h (see config.h)
    // ESP<->FHEM interface version. If you add/remove/rename fields in ANY
    // WebSocket message or /api/* endpoint, follow the VERSIONING CONTRACT at
    // FHEM_API_VERSION_MAJOR in config.h (bump version + mirror in FHEM).
    doc["api_version_major"] = FHEM_API_VERSION_MAJOR;
    doc["api_version_minor"] = FHEM_API_VERSION_MINOR;
    // Casambi network protocol version vs. the range this firmware is tested
    // with (FHEM computes the mismatch warning from these three numbers).
    // Authenticated hello only — deliberately NOT in the open /api/info.
    doc["casambi_protocol_version"] = _config->protocolVersion;
    doc["casambi_protocol_min"]     = MIN_PROTOCOL_VERSION;
    doc["casambi_protocol_max"]     = MAX_PROTOCOL_VERSION;
    // Network name for the FHEM "network" reading. Deliberately NOT part of
    // the unauthenticated /api/info — hello only reaches authenticated
    // WebSocket clients. Safe unlocked read: written only at boot/refresh,
    // before the web server exists (same as the unit strings below).
    doc["network"] = _config->networkName;
    bool bleConn = _client->isAuthenticated();
    doc["ble_connected"] = bleConn;

    // Currently connected gateway (BLE MAC + resolved unit name) for transparency.
    String gwMac = bleConn ? _client->getConnectedAddress() : String("");
    JsonObject gw = doc["gateway"].to<JsonObject>();
    gw["connected"] = bleConn;
    gw["mac"]       = gwMac;
    gw["name"]      = _gatewayName(gwMac);

    // Copy the unit state under g_configMutex: the BLE task applies incoming
    // state updates under the same mutex (_applyUnitStates), so each unit's
    // on/level/vertical/colorTemp here is one consistent snapshot. The unit
    // list itself and the identity strings are only written at boot/refresh.
    //
    // Bound the snapshot size: hello is pushed proactively (connect, resync)
    // and must never grow into an allocation a fragmented heap cannot serve
    // (see WS_HELLO_MAX_UNITS). Clients see "units_truncated" and can fetch
    // the full list via GET /api/units themselves.
    if (_config->units.size() > WS_HELLO_MAX_UNITS) {
        doc["units_truncated"] = true;
    }
    size_t helloUnits = 0;
    JsonArray units = doc["units"].to<JsonArray>();
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    for (const auto& unit : _config->units) {
        if (++helloUnits > WS_HELLO_MAX_UNITS) break;
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
        addUnitControls(u, unit);   // generic, cloud-derived channel list
    }
    if (g_configMutex) xSemaphoreGive(g_configMutex);

    String msg;
    serializeJson(doc, msg);
    return msg;
}

void CasambiWebServer::broadcastUnitState(uint8_t unitId, uint8_t level, bool online) {
    if (!_ws || !_broadcastQueue) return;

    // Runs on the BLE notification task: post the raw event only — no JSON,
    // no String, no config reads. loop() builds the message at send time.
    // Non-blocking: if the queue is full the event is dropped, but the
    // resync flag makes loop() push a fresh hello snapshot — a dropped
    // unit_state would otherwise stay stale until that unit's NEXT change.
    WsEvent ev{};
    ev.type   = WsEvent::Type::UnitState;
    ev.unitId = unitId;
    ev.level  = level;
    ev.online = online;
    if (xQueueSend(_broadcastQueue, &ev, 0) != pdTRUE) {
        _resyncNeeded = true;
        _wsDropCount++;
        WEB_LOG("WS: broadcast queue full, dropped unit_state id=%d (resync scheduled)\n", unitId);
    } else {
        WEB_LOG("WS: queued unit_state id=%d level=%d online=%d\n",
                unitId, level, online);
    }
}

void CasambiWebServer::broadcastConnectionState(bool connected, int reason) {
    if (!_ws || !_broadcastQueue) return;

    WsEvent ev{};
    ev.type      = WsEvent::Type::ConnectionState;
    ev.connected = connected;
    ev.reason    = (int8_t)reason;
    if (xQueueSend(_broadcastQueue, &ev, 0) != pdTRUE) {
        _resyncNeeded = true;   // hello also carries ble_connected + gateway
        _wsDropCount++;
        WEB_LOG("WS: broadcast queue full, dropped connection_state (resync scheduled)\n");
    } else {
        WEB_LOG("WS: queued connection_state connected=%d reason=%d\n",
                connected, reason);
    }
}

void CasambiWebServer::_sendWsEvent(const WsEvent& ev) {
    JsonDocument doc;

    if (ev.type == WsEvent::Type::UnitState) {
        doc["type"]   = "unit_state";
        doc["id"]     = ev.unitId;
        doc["level"]  = ev.level;
        doc["online"] = ev.online;
        doc["on"]     = (ev.level > 0);

        // Enrich with the unit's aux state. Read under g_configMutex so the
        // fields form one consistent snapshot with the BLE task's writes
        // (_applyUnitStates). This may reflect an update newer than this
        // event's level — events are drained within one loop tick, and the
        // hello resync covers any drop.
        if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
        CasambiUnit* unit = _config->getUnitById(ev.unitId);
        if (unit) {
            if (unit->hasVertical) doc["vertical"]  = unit->vertical;
            if (unit->hasCCT) {
                doc["colorTemp"] = unit->colorTemp;
                doc["cctMin"]    = unit->cctMinKelvin;
                doc["cctMax"]    = unit->cctMaxKelvin;
            }
            addUnitControls(doc.as<JsonObject>(), *unit);   // generic channel list
        }
        if (g_configMutex) xSemaphoreGive(g_configMutex);
    } else {
        doc["type"]      = "connection_state";
        doc["connected"] = ev.connected;
        if (ev.reason != 0) doc["reason"] = (int)ev.reason;

        // Gateway info resolved here on the loop task — _gatewayName and
        // getConnectedAddress take g_configMutex themselves, so this must
        // stay outside any g_configMutex hold.
        String gwMac = ev.connected ? _client->getConnectedAddress() : String("");
        JsonObject gw = doc["gateway"].to<JsonObject>();
        gw["connected"] = ev.connected;
        gw["mac"]       = gwMac;
        gw["name"]      = _gatewayName(gwMac);
    }

    String msg;
    serializeJson(doc, msg);
    _ws->textAll(msg);
}

// The dynamic POST routes that carry a JSON body. Shared by onRequestBody
// (buffer only for these — bodies sent to any other endpoint are ignored, so
// nothing is allocated that no handler would ever free) and by the onNotFound
// dispatcher.
static bool isBodyPostEndpoint(const String& path) {
    return (path.indexOf("/api/scenes/") == 0 && path.endsWith("/level")) ||
           (path.indexOf("/api/units/")  == 0 && (path.endsWith("/level") ||
               path.endsWith("/color") || path.endsWith("/temperature") ||
               path.endsWith("/slider") || path.endsWith("/vertical"))) ||
           (path.indexOf("/api/groups/") == 0 && (path.endsWith("/level") ||
               path.endsWith("/slider") || path.endsWith("/vertical"))) ||
           (path == "/api/ntp");
}

void CasambiWebServer::_setupRoutes() {
    // No CORS headers: the control API is not meant to be called from arbitrary
    // browser origins. Omitting Access-Control-Allow-Origin makes browsers block
    // cross-origin JavaScript (closing the wildcard-CORS access path); FHEM and
    // other same-origin/non-browser clients are unaffected.

    // Discovery: lets FHEM tell a configured gateway apart from one still in
    // setup mode (the portal serves the same path with configured:false).
    // Intentionally LEFT UNAUTHENTICATED so discovery works before the client
    // knows the token — but reduced to the two fields FHEM actually needs, so it
    // no longer leaks network name / MAC / IP without auth.
    _server->on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        JsonDocument d;
        d["configured"] = true;
        d["build"]      = FIRMWARE_BUILD;
        // Interface version, exposed unauthenticated on purpose: it reveals
        // nothing about the network and lets FHEM detect an incompatible
        // firmware BEFORE opening the WebSocket. Any change to this endpoint's
        // fields falls under the VERSIONING CONTRACT in config.h — keep the
        // setup-portal /api/info (setup_portal.cpp) shape-identical.
        d["api_version_major"] = FHEM_API_VERSION_MAJOR;
        d["api_version_minor"] = FHEM_API_VERSION_MINOR;
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

    // Reboot endpoint. Only flag the request and let the loop task restart;
    // calling delay()/ESP.restart() here would block the TCP task before the
    // response is flushed (same pattern as refreshCasambi below).
    _server->on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authOk(request)) return;
        _rebootRequested = true;
        request->send(200, "application/json", "{\"status\":\"rebooting\"}");
    });

    // Re-read the Casambi cloud configuration using the stored network password.
    // This reboots the device (the download then runs early at the next boot),
    // so it cannot run inside this async handler — we only flag the request and
    // let the loop task carry it out (see consumeRefreshRequest).
    _server->on("/api/refreshCasambi", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authOk(request)) return;
        if (!ConfigStore::hasValidConfig()) {
            _sendJsonError(request, "No configuration found; run setup first", 409);
            return;
        }
        if (lockedCopy(_config->casambiPassword).length() == 0) {
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
            // A WebSocket upgrade rejected by the auth filter falls through to
            // here. Answer 401 (not 404) so clients — the FHEM module logs the
            // handshake status line — can tell an auth problem from a bad path.
            if (request->method() == HTTP_GET && request->url() == "/ws") {
                _sendJsonError(request, "Unauthorized", 401);
                return;
            }
            _sendJsonError(request, "Endpoint not found", 404);
            return;
        }

        // Single auth gate for every dynamic POST control route (scenes/units/
        // groups/ntp). The 401 path frees any buffered body (_sendJsonError).
        if (!_authOk(request)) return;

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
        if (isBodyPostEndpoint(path)) {
            // Oversized payloads are not buffered by onRequestBody; reject here
            // (this is still the single response point, so no double-send;
            // _sendJsonError frees a partially buffered body).
            if (request->contentLength() > 512) {
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
        // Buffer ONLY for the known body endpoints. Buffering for every
        // /api/* POST allocated a String that the no-body handlers (e.g.
        // /api/reboot, /on, /off) never freed — held until connection close
        // and, with keep-alive request-object reuse, overwritten (leaked) by
        // the next bodied request on the same connection.
        if (request->method() != HTTP_POST || !isBodyPostEndpoint(request->url())) {
            return;
        }

        // Don't buffer oversized bodies; onNotFound rejects them via
        // request->contentLength(). Our JSON payloads are tiny (< 64 bytes).
        // Moderately oversized requests still get a clean 413; clearly abusive
        // sizes are cut off mid-stream instead of streaming megabytes through.
        if (total > 512) {
            if (total > 4096) request->client()->close();
            return;
        }

        // Allocate the buffer on the first chunk and arm a disconnect handler so
        // an aborted (incomplete) body is freed — onNotFound never runs for a
        // request that never completes, so this is the only cleanup path then.
        if (index == 0) {
            // Defensive: free a stale buffer a previous request on the same
            // (kept-alive) connection may have left behind.
            if (request->_tempObject) {
                delete static_cast<String*>(request->_tempObject);
                request->_tempObject = nullptr;
            }
            String* body = new String();
            // reserve() returns false when the heap cannot provide the
            // capacity. Leave _tempObject null then: every later chunk falls
            // through the `if (!body)` guard and the handler answers 400
            // (missing body) — the single-response contract of onNotFound
            // forbids sending a 503 from this body collector.
            if (total > 0 && !body->reserve(total)) {
                delete body;
                return;
            }
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
    if (!_authOk(request)) return;
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
    // Broadcast events dropped on a full queue (each triggers a hello resync).
    doc["ws_drops"] = _wsDropCount.load();
    // Tolerant-parser diagnostics, summed over the packet types: "partial" =
    // packets whose understood prefix was applied while an undecoded tail was
    // dropped (a protocol element we do not know yet — worth investigating
    // when it grows), "malformed" = packets that yielded nothing usable.
    // Per-type detail is available via the serial `status` command.
    const PacketParseStats& pps = packetParseStats();
    doc["parse_partial"]   = pps.partial06.load() + pps.partial07.load()
                           + pps.partial08.load();
    doc["parse_malformed"] = pps.malformed06.load() + pps.malformed07.load()
                           + pps.malformed08.load();
    doc["ntp_server"] = lockedCopy(_config->ntpServer);

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
        // Cached value (refreshed by the loop task's health check) — never a
        // live BLE stack call from the async_tcp task.
        doc["gateway_rssi"] = _client->getLastRssi();
    }

    if (_client->getLastDisconnectReason() != DisconnectReason::None) {
        doc["last_disconnect_reason"] = static_cast<int>(_client->getLastDisconnectReason());
        doc["last_disconnect_source"] = _client->getLastDisconnectSource();
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleGetUnits(AsyncWebServerRequest* request) {
    if (!_authOk(request)) return;
    WEB_LOG("Web: /api/units from %s\n", _getClientIP(request).c_str());
    JsonDocument doc;
    JsonArray units = doc["units"].to<JsonArray>();
    // Same consistent-snapshot rule as _buildHelloMessage: state fields are
    // written by the BLE task under g_configMutex, so read them under it too.
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
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
        addUnitControls(u, unit);   // generic, cloud-derived channel list
    }
    if (g_configMutex) xSemaphoreGive(g_configMutex);
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void CasambiWebServer::_handleGetGroups(AsyncWebServerRequest* request) {
    if (!_authOk(request)) return;
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
    if (!_authOk(request)) return;
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
    uint32_t generation = 0;        // EventLog::generation() at snapshot time
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
                    // The snapshot indices are void if the ping-pong files
                    // switched (or the log was cleared) since the response
                    // started — close the array cleanly instead of streaming
                    // shifted/cleared records.
                    if (EventLog::generation() != generation) {
                        phase = 2;
                        continue;
                    }
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
    if (!_authOk(request)) return;
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
    src->generation = EventLog::generation();

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json",
        [src](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            return src->fill(buffer, maxLen);
        });
    request->send(response);
}

void CasambiWebServer::_handleDeleteLog(AsyncWebServerRequest* request) {
    if (!_authOk(request)) return;
    WEB_LOG("Web: DELETE /api/log from %s\n", _getClientIP(request).c_str());
    // Do NOT clear here: EventLog::clear() waits on a mutex with an unbounded
    // timeout and performs several LittleFS deletions (plus GC that can exceed
    // 100 ms), which would stall the async_tcp task and every other HTTP/WS
    // connection. Only raise a flag; the loop task performs the actual clear.
    // Rapid repeated DELETEs coalesce into a single clear.
    _clearLogRequested = true;
    JsonDocument doc;
    doc["success"] = true;
    doc["status"] = "clear scheduled";
    String response;
    serializeJson(doc, response);
    request->send(202, "application/json", response);
}

// ============================================================================
// NTP / Time Configuration Endpoints
// ============================================================================

void CasambiWebServer::_handleGetNtp(AsyncWebServerRequest* request) {
    if (!_authOk(request)) return;
    JsonDocument doc;
    doc["ntp_server"] = lockedCopy(_config->ntpServer);

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
    JsonDocument doc;
    if (!_parseBody(request, doc)) return;

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

    // Only hand the value to the loop task (consumeNtpRequest). Mutating
    // _config and writing LittleFS here would (a) race the loop task on the
    // ntpServer String and (b) block the async_tcp task on a flash write.
    // String AND flag are set under the same mutex hold so the consumer can
    // never see the flag without its matching value (transactional handoff).
    if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY);
    _pendingNtpServer = server;
    _ntpRequested.store(true);
    if (g_configMutex) xSemaphoreGive(g_configMutex);

    WEB_LOG("Web: NTP server change to %s requested from %s\n",
            server.c_str(), _getClientIP(request).c_str());

    JsonDocument resp;
    resp["success"] = true;
    resp["ntp_server"] = server;
    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
}

// ============================================================================
// Request helpers (shared by the control endpoints)
// ============================================================================

// Parse the decimal id segment between `prefix` and `suffix` of the URL.
// Returns -1 unless the segment is a plain 0-255 integer — so a path like
// /api/units/300/level cannot alias unit 44 via uint8 truncation.
static int parseIdSegment(const String& path, const char* prefix, const char* suffix) {
    int start = path.indexOf(prefix);
    if (start < 0) return -1;
    start += strlen(prefix);
    int end = path.indexOf(suffix, start);
    if (end < 0 || end == start) return -1;
    long v = 0;
    for (int i = start; i < end; i++) {
        char c = path[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
        if (v > 255) return -1;
    }
    return (int)v;
}

bool CasambiWebServer::_checkBle(AsyncWebServerRequest* request) {
    if (!_client->isAuthenticated()) {
        _sendJsonError(request, "Not connected to BLE gateway", 503);
        return false;
    }
    return true;
}

CasambiScene* CasambiWebServer::_sceneFromPath(AsyncWebServerRequest* request,
                                               const char* suffix, uint8_t& idOut) {
    int id = parseIdSegment(request->url(), "/scenes/", suffix);
    if (id < 0) { _sendJsonError(request, "Invalid scene id", 400); return nullptr; }
    CasambiScene* scene = _config->getSceneById((uint8_t)id);
    if (!scene) { _sendJsonError(request, "Scene not found", 404); return nullptr; }
    idOut = (uint8_t)id;
    return scene;
}

CasambiUnit* CasambiWebServer::_unitFromPath(AsyncWebServerRequest* request,
                                             const char* suffix, uint8_t& idOut) {
    int id = parseIdSegment(request->url(), "/units/", suffix);
    if (id < 0) { _sendJsonError(request, "Invalid unit id", 400); return nullptr; }
    CasambiUnit* unit = _config->getUnitById((uint8_t)id);
    if (!unit) { _sendJsonError(request, "Unit not found", 404); return nullptr; }
    idOut = (uint8_t)id;
    return unit;
}

CasambiGroup* CasambiWebServer::_groupFromPath(AsyncWebServerRequest* request,
                                               const char* suffix, uint8_t& idOut) {
    int id = parseIdSegment(request->url(), "/groups/", suffix);
    if (id < 0) { _sendJsonError(request, "Invalid group id", 400); return nullptr; }
    CasambiGroup* group = _config->getGroupById((uint8_t)id);
    if (!group) { _sendJsonError(request, "Group not found", 404); return nullptr; }
    idOut = (uint8_t)id;
    return group;
}

bool CasambiWebServer::_parseBody(AsyncWebServerRequest* request, JsonDocument& doc) {
    if (!request->_tempObject) {
        _sendJsonError(request, "Missing request body", 400);
        return false;
    }
    String* bodyStr = static_cast<String*>(request->_tempObject);
    DeserializationError error = deserializeJson(doc, *bodyStr);
    delete bodyStr;
    request->_tempObject = nullptr;
    if (error) {
        _sendJsonError(request, "Invalid JSON", 400);
        return false;
    }
    return true;
}

bool CasambiWebServer::_requireUint8(AsyncWebServerRequest* request, JsonDocument& doc,
                                     const char* key, uint8_t& out) {
    if (!doc[key].is<uint8_t>()) {
        _sendJsonError(request, String("Missing '") + key + "' parameter (0-255)", 400);
        return false;
    }
    out = doc[key];
    return true;
}

// ============================================================================
// Scene Control Endpoints
// ============================================================================

void CasambiWebServer::_handleSceneOn(AsyncWebServerRequest* request) {
    uint8_t sceneId;
    if (!_checkBle(request)) return;
    CasambiScene* scene = _sceneFromPath(request, "/on", sceneId);
    if (!scene) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::SceneLevel;
    cmd.id   = sceneId;
    cmd.a    = 0xFF;

    WEB_LOG("Web: Scene %d (%s) ON queued from %s\n", sceneId, scene->name.c_str(), _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleSceneOff(AsyncWebServerRequest* request) {
    uint8_t sceneId;
    if (!_checkBle(request)) return;
    CasambiScene* scene = _sceneFromPath(request, "/off", sceneId);
    if (!scene) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::SceneLevel;
    cmd.id   = sceneId;
    cmd.a    = 0;

    WEB_LOG("Web: Scene %d (%s) OFF queued from %s\n", sceneId, scene->name.c_str(), _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleSceneLevel(AsyncWebServerRequest* request) {
    uint8_t sceneId;
    if (!_checkBle(request)) return;
    CasambiScene* scene = _sceneFromPath(request, "/level", sceneId);
    if (!scene) return;

    JsonDocument doc;
    uint8_t level;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "level", level)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::SceneLevel;
    cmd.id   = sceneId;
    cmd.a    = level;

    WEB_LOG("Web: Scene %d (%s) level=%d queued from %s\n", sceneId, scene->name.c_str(), level, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

// ============================================================================
// Unit Control Endpoints
// ============================================================================

void CasambiWebServer::_handleUnitOn(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/on", unitId);
    if (!unit) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitLevel;
    cmd.id   = unitId;
    cmd.a    = 255;

    WEB_LOG("Web: Unit %d (%s) ON queued from %s\n", unitId, unit->name.c_str(), _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitOff(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/off", unitId);
    if (!unit) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitLevel;
    cmd.id   = unitId;
    cmd.a    = 0;

    WEB_LOG("Web: Unit %d (%s) OFF queued from %s\n", unitId, unit->name.c_str(), _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitLevel(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/level", unitId);
    if (!unit) return;

    JsonDocument doc;
    uint8_t level;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "level", level)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitLevel;
    cmd.id   = unitId;
    cmd.a    = level;

    WEB_LOG("Web: Unit %d (%s) level=%d queued from %s\n", unitId, unit->name.c_str(), level, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitColor(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/color", unitId);
    if (!unit) return;

    JsonDocument doc;
    uint8_t r, g, b;
    if (!_parseBody(request, doc)) return;
    if (!_requireUint8(request, doc, "r", r) ||
        !_requireUint8(request, doc, "g", g) ||
        !_requireUint8(request, doc, "b", b)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitColor;
    cmd.id   = unitId;
    cmd.a    = r;
    cmd.b    = g;
    cmd.c    = b;

    WEB_LOG("Web: Unit %d (%s) color=(%d,%d,%d) queued from %s\n", unitId, unit->name.c_str(), r, g, b, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitTemperature(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/temperature", unitId);
    if (!unit) return;

    JsonDocument doc;
    if (!_parseBody(request, doc)) return;

    if (!doc["kelvin"].is<uint16_t>()) {
        _sendJsonError(request, "Missing 'kelvin' parameter", 400);
        return;
    }
    uint16_t kelvin = doc["kelvin"];
    if (kelvin < 1000 || kelvin > 10000) {
        _sendJsonError(request, "Kelvin must be 1000-10000", 400);
        return;
    }

    BleCommand cmd{};
    cmd.type   = BleCommand::Type::UnitTemperature;
    cmd.id     = unitId;
    cmd.kelvin = kelvin;

    WEB_LOG("Web: Unit %d (%s) temperature=%dK queued from %s\n", unitId, unit->name.c_str(), kelvin, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitSlider(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/slider", unitId);
    if (!unit) return;

    JsonDocument doc;
    uint8_t value;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "value", value)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitSlider;
    cmd.id   = unitId;
    cmd.a    = value;

    WEB_LOG("Web: Unit %d (%s) slider=%d queued from %s\n", unitId, unit->name.c_str(), value, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleUnitVertical(AsyncWebServerRequest* request) {
    uint8_t unitId;
    if (!_checkBle(request)) return;
    CasambiUnit* unit = _unitFromPath(request, "/vertical", unitId);
    if (!unit) return;

    JsonDocument doc;
    uint8_t value;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "value", value)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::UnitVertical;
    cmd.id   = unitId;
    cmd.a    = value;

    WEB_LOG("Web: Unit %d (%s) vertical=%d queued from %s\n", unitId, unit->name.c_str(), value, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

// ============================================================================
// Group Control Endpoints
// ============================================================================

void CasambiWebServer::_handleGroupLevel(AsyncWebServerRequest* request) {
    uint8_t groupId;
    if (!_checkBle(request)) return;
    CasambiGroup* group = _groupFromPath(request, "/level", groupId);
    if (!group) return;

    JsonDocument doc;
    uint8_t level;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "level", level)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::GroupLevel;
    cmd.id   = groupId;
    cmd.a    = level;

    WEB_LOG("Web: Group %d (%s) level=%d queued from %s\n", groupId, group->name.c_str(), level, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleGroupSlider(AsyncWebServerRequest* request) {
    uint8_t groupId;
    if (!_checkBle(request)) return;
    CasambiGroup* group = _groupFromPath(request, "/slider", groupId);
    if (!group) return;

    JsonDocument doc;
    uint8_t value;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "value", value)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::GroupSlider;
    cmd.id   = groupId;
    cmd.a    = value;

    WEB_LOG("Web: Group %d (%s) slider=%d queued from %s\n", groupId, group->name.c_str(), value, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

void CasambiWebServer::_handleGroupVertical(AsyncWebServerRequest* request) {
    uint8_t groupId;
    if (!_checkBle(request)) return;
    CasambiGroup* group = _groupFromPath(request, "/vertical", groupId);
    if (!group) return;

    JsonDocument doc;
    uint8_t value;
    if (!_parseBody(request, doc) || !_requireUint8(request, doc, "value", value)) return;

    BleCommand cmd{};
    cmd.type = BleCommand::Type::GroupVertical;
    cmd.id   = groupId;
    cmd.a    = value;

    WEB_LOG("Web: Group %d (%s) vertical=%d queued from %s\n", groupId, group->name.c_str(), value, _getClientIP(request).c_str());
    _enqueueBleCommand(request, cmd);
}

// ============================================================================
// Utility Methods
// ============================================================================

void CasambiWebServer::_sendJsonError(AsyncWebServerRequest* request, const String& error, int code) {
    // Uniform cleanup: rejection paths (401/404/413/503/...) may run before a
    // handler consumed the buffered POST body — free it here so it never
    // lingers until connection close. Handlers that already consumed it have
    // nulled _tempObject, so this is a no-op then.
    if (request->_tempObject) {
        delete static_cast<String*>(request->_tempObject);
        request->_tempObject = nullptr;
    }
    JsonDocument doc;
    doc["success"] = false;
    doc["error"] = error;
    String response;
    serializeJson(doc, response);
    request->send(code, "application/json", response);
}

void CasambiWebServer::_enqueueBleCommand(AsyncWebServerRequest* request, const BleCommand& cmd) {
    // Non-blocking send: a full queue means the loop task is backed up behind
    // slow BLE operations — push back on the client instead of piling on.
    if (!_bleCmdQueue || xQueueSend(_bleCmdQueue, &cmd, 0) != pdTRUE) {
        _sendJsonError(request, "Command queue full; retry shortly", 503);
        return;
    }
    JsonDocument doc;
    doc["success"] = true;
    doc["queued"]  = true;
    String response;
    serializeJson(doc, response);
    request->send(202, "application/json", response);
}

void CasambiWebServer::_executeBleCommand(const BleCommand& cmd) {
    bool ok = false;
    switch (cmd.type) {
        case BleCommand::Type::SceneLevel:      ok = _client->setSceneLevel(cmd.id, cmd.a); break;
        case BleCommand::Type::UnitLevel:       ok = _client->setUnitLevel(cmd.id, cmd.a); break;
        case BleCommand::Type::GroupLevel:      ok = _client->setGroupLevel(cmd.id, cmd.a); break;
        case BleCommand::Type::UnitVertical:    ok = _client->setUnitVertical(cmd.id, cmd.a); break;
        case BleCommand::Type::GroupVertical:   ok = _client->setGroupVertical(cmd.id, cmd.a); break;
        case BleCommand::Type::UnitTemperature: ok = _client->setUnitTemperature(cmd.id, cmd.kelvin); break;
        case BleCommand::Type::UnitColor:       ok = _client->setUnitColor(cmd.id, cmd.a, cmd.b, cmd.c); break;
        case BleCommand::Type::UnitSlider:      ok = _client->setUnitSlider(cmd.id, cmd.a); break;
        case BleCommand::Type::GroupSlider:     ok = _client->setGroupSlider(cmd.id, cmd.a); break;
    }
    if (!ok) {
        // The HTTP 202 already went out; the failure surfaces here and via the
        // absent unit_state event (clients keep their previous state).
        EventLog::log(LOG_WARN, "BLE command failed (type=%u id=%u)",
                      (unsigned)static_cast<uint8_t>(cmd.type), (unsigned)cmd.id);
    }
}

String CasambiWebServer::_getClientIP(AsyncWebServerRequest* request) {
    // Use the real peer address only. X-Forwarded-For is attacker-controlled
    // (any client can set it) and there is no trusted reverse proxy in front of
    // this device, so honoring it would just let clients spoof the IP in logs.
    return request->client()->remoteIP().toString();
}
