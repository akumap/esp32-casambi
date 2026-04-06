/**
 * Web Server - HTTP REST API + WebSocket for Casambi control
 *
 * Provides HTTP endpoints for controlling lights via home automation systems
 * like FHEM. WebSocket at /ws delivers real-time push events so clients
 * no longer need to poll.
 *
 * WebSocket protocol (server → client, JSON):
 *   {"type":"hello","ble_connected":true,"units":[...]}  – sent on connect
 *   {"type":"unit_state","id":1,"level":128,"online":true}
 *   {"type":"connection_state","connected":true,"reason":0}
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>

// HTTP method constants (must be defined before ESPAsyncWebServer.h)
#ifndef HTTP_ANY
#define HTTP_ANY 0
#endif
#ifndef HTTP_GET
#define HTTP_GET 1
#endif
#ifndef HTTP_POST
#define HTTP_POST 2
#endif

#include <ESPAsyncWebServer.h>
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
     * Broadcast a unit state change to all connected WebSocket clients.
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

    // Setup route handlers
    void _setupRoutes();

    // Status endpoints
    void _handleGetStatus(AsyncWebServerRequest* request);
    void _handleGetUnits(AsyncWebServerRequest* request);
    void _handleGetGroups(AsyncWebServerRequest* request);
    void _handleGetScenes(AsyncWebServerRequest* request);

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

    // WebSocket helpers
    void _handleWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                               AwsEventType type, void* arg, uint8_t* data, size_t len);
    String _buildHelloMessage() const;
};

#endif // WEBSERVER_H
