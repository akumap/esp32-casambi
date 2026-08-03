/**
 * Web Server - HTTP REST API + WebSocket for Casambi control
 *
 * Provides HTTP endpoints for controlling lights via home automation systems
 * like FHEM. WebSocket at /ws delivers real-time push events so clients
 * no longer need to poll.
 *
 * GET / serves the human-facing status & control dashboard (dashboard.h): a
 * static page that reads AND writes the same /api/* + /ws interface from the
 * browser — it is a CLIENT of the interface below, not part of it.
 *
 * WebSocket protocol (server → client, JSON):
 *   {"type":"hello","network":"My Home","ble_connected":true,
 *     "units":[{"id":1,"name":"...","online":true,
 *     "on":true,"level":128,"vertical":127,"colorTemp":58,"cctMin":2700,"cctMax":4000},...]}
 *     — carries at most WS_HELLO_MAX_UNITS units; beyond that
 *       "units_truncated":true is set and clients fetch GET /api/units
 *   {"type":"unit_state","id":1,"level":128,"online":true,"on":true,
 *     "vertical":127,"colorTemp":58,"cctMin":2700,"cctMax":4000}  – aux fields omitted if unsupported
 *   {"type":"connection_state","connected":true,"reason":0}
 *
 * This WebSocket protocol and the /api/* REST endpoints together form the
 * versioned ESP<->FHEM interface. Before changing message types, fields,
 * endpoints, or their semantics, read and follow the VERSIONING CONTRACT at
 * FHEM_API_VERSION_MAJOR in config.h.
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>

// NOTE: do NOT pre-define HTTP_GET/HTTP_POST/HTTP_DELETE here. The ESP32Async
// ESPAsyncWebServer declares them in the `AsyncWebRequestMethod` enum and
// exposes them globally itself (via `using namespace`, unless
// ASYNCWEBSERVER_NO_GLOBAL_HTTP_METHODS is set). Pre-defining them as macros
// corrupts that enum. The Arduino core's HTTPMethod enum (from <HTTPClient.h>,
// pulled in by cloud/api_client.h) also defines these names, but the two only
// clash at an actual point of use — and this header's translation units never
// reference a bare HTTP_* constant alongside HTTPClient, so no qualification is
// needed.
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>   // JsonDocument in the _parseBody/_requireUint8 helpers
#include <freertos/queue.h>
#include <atomic>
#include "../ble/casambi_client.h"
#include "../cloud/network_config.h"
#include "../cloud/state_codec.h"   // MAX_STATE_BYTES for the UnitState command payload

// A broadcast event posted from the BLE notification task. Plain value type
// by design: the BLE task enqueues a handful of bytes with NO heap
// allocation (the previous String*-per-event scheme allocated a
// JsonDocument, a String and its buffer on every state change — steady heap
// churn and an unchecked `new` on the BLE task). The loop task builds the
// JSON at send time, enriching unit events from NetworkConfig under
// g_configMutex there.
struct WsEvent {
    enum class Type : uint8_t { UnitState, ConnectionState };
    Type type;
    uint8_t unitId;    // UnitState
    uint8_t level;     // UnitState
    bool online;       // UnitState
    bool connected;    // ConnectionState
    int8_t reason;     // ConnectionState: DisconnectReason as int (0 = n/a)
};

// A queued light-control command from a REST handler. Plain value type by
// design: the async_tcp task validates the request and enqueues the command
// by value, the loop task dequeues and performs the (potentially seconds-
// long) BLE operation — no heap allocation, no shared ownership, and the
// async_tcp task never blocks on the BLE mutex or a GATT write.
struct BleCommand {
    enum class Type : uint8_t {
        SceneLevel, UnitLevel, GroupLevel,
        UnitVertical, GroupVertical,
        UnitTemperature, UnitColor,
        UnitSlider, GroupSlider,
        UnitState,
    };
    Type type;
    uint8_t id;         // scene/unit/group id (already validated against config)
    uint8_t a, b, c;    // level/value (a) or r,g,b depending on type
    uint16_t kelvin;    // UnitTemperature only
    // UnitState only: the fully encoded state blob (see state_codec.h). Fixed
    // array by design — the queue stays a plain value type with no ownership.
    uint8_t state[statecodec::MAX_STATE_BYTES];
    uint8_t stateLen;
};

class CasambiWebServer {
public:
    /**
     * Constructor
     * @param client BLE client instance
     * @param config Network configuration
     */
    CasambiWebServer(CasambiClient* client, NetworkConfig* config);

    /**
     * Destructor
     */
    ~CasambiWebServer();

    /**
     * Initialize and start the web server
     * @param port HTTP port (default: 80)
     * @return true on success
     */
    bool begin(uint16_t port = 80);

    /**
     * Stop the web server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool isRunning() const { return _running; }

    /**
     * Call from loop() to clean up disconnected WebSocket clients.
     * Should be called roughly every second.
     */
    void loop();

    /**
     * Check (and clear) whether a Casambi cloud-config refresh was requested
     * via POST /api/refresh. The actual refresh frees the BLE stack, talks to
     * the cloud over TLS and reboots, so it must run from the loop task — never
     * from inside the async request handler. Returns true exactly once per
     * request.
     */
    bool consumeRefreshRequest();

    /**
     * Check (and clear) whether a reboot was requested via POST /api/reboot.
     * The async handler must not call ESP.restart()/delay() itself (it would
     * block the TCP task before the response is flushed), so it only flags the
     * request and the loop task carries it out. Returns true exactly once.
     */
    bool consumeRebootRequest();

    /**
     * Check (and clear) whether POST /api/ntp submitted a new NTP server.
     * The async handler only validates and stores the value; applying it
     * (config mutation, LittleFS save, SNTP re-arm) runs in the loop task so
     * the flash write never blocks the async_tcp task and the config String
     * is only ever written from the loop task. Returns true exactly once and
     * copies the server into `serverOut`.
     */
    bool consumeNtpRequest(String& serverOut);

    /**
     * Check (and clear) whether DELETE /api/log requested an event-log wipe.
     * The async handler only sets a flag; the actual EventLog::clear() (which
     * takes a mutex with an unbounded wait and performs several LittleFS
     * deletions plus garbage collection) runs in the loop task so this flash
     * I/O never blocks the async_tcp task. Returns true exactly once per
     * request; coalesced rapid DELETEs collapse into a single clear.
     */
    bool consumeClearLogRequest();

    /**
     * Broadcast a unit state change to all connected WebSocket clients.
     * Enriches the message with vertical/colorTemp/cctMin/cctMax from NetworkConfig
     * (already updated before this callback fires).
     * Safe to call from the BLE notification task.
     * @param unitId  Casambi unit ID
     * @param level   Brightness 0-255
     * @param online  Whether the unit is reachable
     */
    void broadcastUnitState(uint8_t unitId, uint8_t level, bool online);

    /**
     * Broadcast a BLE connection state change to all WebSocket clients.
     * @param connected true = authenticated/connected, false = disconnected
     * @param reason    DisconnectReason cast to int (0 = n/a)
     */
    void broadcastConnectionState(bool connected, int reason = 0);

private:
    // Server and WebSocket instances
    AsyncWebServer*  _server;
    AsyncWebSocket*  _ws;

    // References to BLE client and config
    CasambiClient* _client;
    NetworkConfig* _config;

    // Server state
    bool _running;

    // Request flags: set on the async_tcp task, drained by the loop task via
    // the consume*() methods (atomic exchange, so a concurrent set cannot be
    // lost between the check and the clear — volatile only prevented compiler
    // caching, not the read-modify-write race).
    std::atomic<bool> _refreshRequested;   // POST /api/refreshCasambi
    std::atomic<bool> _rebootRequested;    // POST /api/reboot
    std::atomic<bool> _clearLogRequested;  // DELETE /api/log

    // Set by POST /api/ntp, drained by the loop task via consumeNtpRequest().
    // The handoff is transactional: writer and consumer update/read the flag
    // AND _pendingNtpServer under the same g_configMutex hold, so the flag can
    // never be observed without its matching server string (or vice versa).
    std::atomic<bool> _ntpRequested;
    String _pendingNtpServer;

    // Derived API token (hex of SHA-256(prefix||casambiPassword)). Empty string
    // means authentication is disabled (no Casambi password stored). Computed
    // once in begin().
    String _apiToken;

    // Queue of WsEvent values posted from the BLE task and drained by loop()
    // so _ws->textAll() is always called from the loop task, and no JSON/
    // String allocation ever happens on the BLE task.
    QueueHandle_t _broadcastQueue;

    // Queue of BleCommand values posted by the REST control handlers
    // (async_tcp task) and executed one per loop() call on the loop task, so
    // the up-to-1 s BLE mutex wait and the GATT write never stall the
    // async_tcp task (head-of-line blocking of all HTTP/WS traffic).
    QueueHandle_t _bleCmdQueue;

    // Set (BLE task) when a broadcast had to be dropped because the queue was
    // full; loop() then pushes a fresh hello snapshot to all clients so nobody
    // stays stale on a missed unit_state. _wsDropCount tracks the total for
    // diagnostics (/api/status ws_drops).
    std::atomic<bool> _resyncNeeded;
    std::atomic<uint32_t> _wsDropCount;

    // WebSocket sends refused or aborted because the heap could not serve the
    // payload (/api/status ws_send_fails). Distinct from ws_drops: that counts
    // a full broadcast QUEUE, this counts a failed ALLOCATION. A rising value
    // means clients are missing updates under memory pressure — before this
    // counter existed the same condition rebooted the device instead.
    std::atomic<uint32_t> _wsSendFailCount;

    // Build and send the JSON for one dequeued broadcast event (loop task).
    void _sendWsEvent(const WsEvent& ev);

    // Send `msg` to one client, or to all when `client` is nullptr, without
    // letting an allocation failure abort the device. Returns false if the
    // payload was refused (insufficient contiguous heap) or the send threw.
    bool _wsSendGuarded(AsyncWebSocketClient* client, const String& msg);

    // Send the hello snapshot, degrading to a unit-less hello and finally to
    // closing the client if the heap cannot serve it. `client` nullptr
    // broadcasts to all.
    void _sendHello(AsyncWebSocketClient* client);

    // Rough contiguous heap a hello with `units` units needs to BUILD (document
    // plus serialized String). Checked before building because a default
    // arduino-esp32 build cannot catch a failed allocation.
    size_t _helloHeapEstimate(size_t units) const;

    // Setup route handlers
    void _setupRoutes();

    // Authentication ---------------------------------------------------------
    // Derive _apiToken from the stored Casambi password (no-op → empty token,
    // i.e. auth disabled, when no password is stored).
    void _deriveApiToken();
    // Reject the request with 401 unless the X-API-Key header matches the token
    // (or auth is disabled). Returns true when the request may proceed; on
    // failure it has already sent the 401 response. Frees a buffered POST body
    // (_tempObject) so an unauthenticated POST does not leak it.
    bool _authOk(AsyncWebServerRequest* request);
    // Same check for the WebSocket upgrade, used as the handler filter. Accepts
    // the token via X-API-Key header or "?k=" query param (browsers cannot set
    // custom headers on a WebSocket handshake). Does not send a response.
    bool _wsAuthOk(AsyncWebServerRequest* request) const;
    // Constant-time string compare (no early exit) to avoid a timing oracle.
    static bool _constantTimeEquals(const String& a, const String& b);

    // Status endpoints
    void _handleGetStatus(AsyncWebServerRequest* request);
    void _handleGetUnits(AsyncWebServerRequest* request);
    void _handleGetGroups(AsyncWebServerRequest* request);
    void _handleGetScenes(AsyncWebServerRequest* request);

    // Event-log endpoints
    void _handleGetLog(AsyncWebServerRequest* request);
    void _handleDeleteLog(AsyncWebServerRequest* request);

    // NTP / time configuration
    void _handleGetNtp(AsyncWebServerRequest* request);
    void _handleSetNtp(AsyncWebServerRequest* request);

    // Scene control endpoints
    void _handleSceneOn(AsyncWebServerRequest* request);
    void _handleSceneOff(AsyncWebServerRequest* request);
    void _handleSceneLevel(AsyncWebServerRequest* request);

    // Unit control endpoints
    void _handleUnitOn(AsyncWebServerRequest* request);
    void _handleUnitOff(AsyncWebServerRequest* request);
    void _handleUnitLevel(AsyncWebServerRequest* request);
    void _handleUnitColor(AsyncWebServerRequest* request);
    void _handleUnitTemperature(AsyncWebServerRequest* request);
    void _handleUnitSlider(AsyncWebServerRequest* request);
    void _handleUnitVertical(AsyncWebServerRequest* request);
    // POST /api/units/:id/state — generic full-state write: body maps control
    // NAMES (see controlName() in network_config.h) to raw values; controls
    // not named keep their current value. Encoded here (async_tcp task) from a
    // config snapshot, sent as one atomic SetState via the command queue.
    void _handleUnitState(AsyncWebServerRequest* request);

    // Group control endpoints
    void _handleGroupLevel(AsyncWebServerRequest* request);
    void _handleGroupSlider(AsyncWebServerRequest* request);
    void _handleGroupVertical(AsyncWebServerRequest* request);

    // Request helpers (shared by all control handlers) ------------------------
    // 503 unless the BLE link is authenticated; true when the request may proceed.
    bool _checkBle(AsyncWebServerRequest* request);
    // Resolve the 0-255 id segment of the URL and look up the entity; on any
    // failure the 400/404 response has already been sent and nullptr returns.
    CasambiScene* _sceneFromPath(AsyncWebServerRequest* request, const char* suffix, uint8_t& idOut);
    CasambiUnit*  _unitFromPath(AsyncWebServerRequest* request, const char* suffix, uint8_t& idOut);
    CasambiGroup* _groupFromPath(AsyncWebServerRequest* request, const char* suffix, uint8_t& idOut);
    // Take the buffered JSON body out of _tempObject and parse it into doc.
    // Frees the buffer in all cases; false after sending the 400 response.
    bool _parseBody(AsyncWebServerRequest* request, JsonDocument& doc);
    // Require an integer field 0..255; sends the 400 response on failure.
    bool _requireUint8(AsyncWebServerRequest* request, JsonDocument& doc,
                       const char* key, uint8_t& out);

    // Post a validated control command to _bleCmdQueue and answer the request:
    // 202 {"success":true,"queued":true} on accept, 503 when the queue is full
    // (the caller should retry). The BLE operation itself runs later on the
    // loop task (_executeBleCommand); its outcome is reported via the
    // WebSocket unit_state events and the event log, not this HTTP response.
    void _enqueueBleCommand(AsyncWebServerRequest* request, const BleCommand& cmd);
    // Perform one dequeued command on the loop task; failures go to EventLog.
    void _executeBleCommand(const BleCommand& cmd);

    // Utility methods
    // _sendJsonError also frees a still-buffered POST body (_tempObject), so
    // every rejection path (401/404/413/503/...) cleans up uniformly.
    void _sendJsonError(AsyncWebServerRequest* request, const String& error, int code = 400);
    String _getClientIP(AsyncWebServerRequest* request);

    // Resolve a gateway BLE MAC to the matching unit name (empty if unknown).
    String _gatewayName(const String& mac) const;

    // WebSocket helpers
    void _handleWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                               AwsEventType type, void* arg, uint8_t* data, size_t len);
    // Full state snapshot for a WebSocket client. `maxUnits` bounds the unit
    // list (WS_HELLO_MAX_UNITS by default; _sendHello retries with 0 under
    // memory pressure). Returns an EMPTY string if the heap could not serve
    // the build — callers must treat that as "no message", not as valid JSON.
    String _buildHelloMessage(size_t maxUnits = WS_HELLO_MAX_UNITS) const;
};

#endif // WEBSERVER_H
