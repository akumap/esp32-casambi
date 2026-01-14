/**
 * Web Server - HTTP REST API for Casambi control
 *
 * Provides HTTP endpoints for controlling lights via home automation systems
 * like Loxone. Runs concurrently with BLE connection.
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

private:
    // Server instance
    AsyncWebServer* _server;

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
};

#endif // WEBSERVER_H
