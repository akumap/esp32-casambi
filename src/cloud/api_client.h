/**
 * Casambi Cloud API Client
 *
 * HTTPS client for Casambi Cloud API
 */

#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "network_config.h"

class CasambiAPIClient {
public:
    CasambiAPIClient();
    ~CasambiAPIClient();

    /**
     * Connect to WiFi
     * @param ssid WiFi SSID
     * @param password WiFi password
     * @return true on success
     */
    bool connectWiFi(const String& ssid, const String& password);

    /**
     * Disconnect from WiFi
     */
    void disconnectWiFi();

    /**
     * Check if WiFi is connected
     */
    bool isWiFiConnected() const;

    /**
     * Get network ID from UUID (MAC address)
     * @param uuid Network UUID (12 hex chars, no colons)
     * @param networkId Output network ID
     * @return true on success
     */
    bool getNetworkId(const String& uuid, String& networkId);

    /**
     * Create session (authenticate with password)
     * @param networkId Network ID
     * @param password Network password
     * @param sessionToken Output session token
     * @return true on success
     */
    bool createSession(const String& networkId, const String& password, String& sessionToken);

    /**
     * Fetch network configuration
     * @param networkId Network ID
     * @param sessionToken Session token
     * @param config Output network configuration
     * @return true on success
     */
    bool fetchNetworkConfig(const String& networkId, const String& sessionToken, NetworkConfig& config);

    /**
     * Get last error message
     */
    String getLastError() const { return _lastError; }

private:
    String _lastError;
    HTTPClient _http;

    /**
     * Parse network configuration JSON
     */
    bool _parseNetworkConfig(const String& json, NetworkConfig& config);

    /**
     * Parse keys from JSON
     */
    bool _parseKeys(const JsonArrayConst& keysArray, NetworkConfig& config);

    /**
     * Parse units from JSON
     */
    bool _parseUnits(const JsonArrayConst& unitsArray, NetworkConfig& config);

    /**
     * Parse groups from JSON
     */
    bool _parseGroups(const JsonObjectConst& gridObj, NetworkConfig& config);

    /**
     * Parse scenes from JSON
     */
    bool _parseScenes(const JsonArrayConst& scenesArray, NetworkConfig& config);

    /**
     * Convert hex string to bytes
     */
    bool _hexToBytes(const String& hex, uint8_t* bytes, size_t len);
};

#endif // API_CLIENT_H
