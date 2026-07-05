/**
 * Web Server - HTTP REST API + WebSocket for Casambi control
 *
 * Provides HTTP endpoints for controlling lights via home automation systems
 * like FHEM. WebSocket at /ws delivers real-time push events so clients
 * no longer need to poll.
 *
 * WebSocket protocol (server → client, JSON):
 *   {"type":"hello","ble_connected":true,"units":[{"id":1,"name":"...","online":true,
 *     "on":true,"level":128,"vertical":127,"colorTemp":58,"cctMin":2700,"cctMax":4000},...]}
 *   {"type":"unit_state","id":1,"level":128,"online":true,"on":true,
 *     "vertical":127,"colorTemp":58,"cctMin":2700,"cctMax":4000}  – aux fields omitted if unsupported
 *   {"type":"connection_state","connected":true,"reason":0}
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
#include <freertos/queue.h>
#include "../ble/casambi_client.h"
#include "../cloud/network_config.h"

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

    // Set by POST /api/refreshCasambi, drained by the loop task via
    // consumeRefreshRequest(). volatile because it is written from the async
    // web-server context and read from the main loop.
    volatile bool _refreshRequested;

    // Set by POST /api/reboot, drained by the loop task via consumeRebootRequest().
    volatile bool _rebootRequested;

    // Set by POST /api/ntp, drained by the loop task via consumeNtpRequest().
    // _pendingNtpServer is written on the async_tcp task and read on the loop
    // task, so both sides access it under g_configMutex.
    volatile bool _ntpRequested;
    String _pendingNtpServer;

    // Derived API token (hex of SHA-256(prefix||casambiPassword)). Empty string
    // means authentication is disabled (no Casambi password stored). Computed
    // once in begin().
    String _apiToken;

    // Queue of String* broadcast messages posted from the BLE task and drained
    // by loop() so _ws->textAll() is always called from the loop task.
    QueueHandle_t _broadcastQueue;

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

    // Group control endpoints
    void _handleGroupLevel(AsyncWebServerRequest* request);
    void _handleGroupSlider(AsyncWebServerRequest* request);
    void _handleGroupVertical(AsyncWebServerRequest* request);

    // Utility methods
    void _sendJsonError(AsyncWebServerRequest* request, const String& error, int code = 400);
    void _sendJsonSuccess(AsyncWebServerRequest* request);
    String _getClientIP(AsyncWebServerRequest* request);

    // Resolve a gateway BLE MAC to the matching unit name (empty if unknown).
    String _gatewayName(const String& mac) const;

    // WebSocket helpers
    void _handleWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                               AwsEventType type, void* arg, uint8_t* data, size_t len);
    String _buildHelloMessage() const;
};

#endif // WEBSERVER_H
