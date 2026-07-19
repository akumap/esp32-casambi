/**
 * Casambi Cloud API Client
 *
 * HTTPS client for Casambi Cloud API
 */

#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
     * Fetch network configuration.
     * Transactional: parses into a scratch config and commits only the
     * cloud-owned fields (name, protocol, revision, keys/units/groups/
     * scenes) to `config` on FULL success. On any failure — HTTP, JSON, or
     * a structurally broken section (invalid key hex, duplicate ids,
     * oversized lists, see CLOUD_MAX_*) — `config` is left untouched.
     * Local settings in `config` are never modified.
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
    WiFiClientSecure _tls;   // server-authenticated transport for the cloud API
    HTTPClient _http;

    /**
     * Begin an HTTPS request with the TLS transport configured for server-
     * certificate validation (or setInsecure() when built with
     * -DCASAMBI_TLS_INSECURE). Must be called instead of _http.begin(url) so the
     * Casambi password / session token never travel over an unauthenticated
     * channel. Always paired with _http.end() like the previous begin() calls.
     */
    void _beginRequest(const String& url);

    /**
     * Parse network configuration JSON. All-or-nothing: a missing optional
     * section (keyStore on Classic networks, units/scenes/grid on an empty
     * network) is fine, but any present-but-broken section fails the whole
     * parse with _lastError set. Never returns true for a partial result.
     */
    bool _parseNetworkConfig(const String& json, NetworkConfig& config);

    // Section parsers. Contract: return false ONLY for a structural error
    // (invalid key material, list over its CLOUD_MAX_* cap) with _lastError
    // set; a well-formed empty section returns true. Cross-section
    // invariants (duplicate ids, group references) run afterwards via
    // cloudval::validateStructure (config_invariants.h, host-tested).
    bool _parseKeys(const JsonArrayConst& keysArray, NetworkConfig& config);
    bool _parseUnits(const JsonArrayConst& unitsArray, NetworkConfig& config);
    /** Groups: members referencing unknown units are dropped with a warning
     *  (stale cloud data), duplicates/oversize fail. Requires units parsed. */
    bool _parseGroups(const JsonObjectConst& gridObj, NetworkConfig& config);
    bool _parseScenes(const JsonArrayConst& scenesArray, NetworkConfig& config);

    /**
     * Convert hex string to bytes
     */
    bool _hexToBytes(const String& hex, uint8_t* bytes, size_t len);

    /**
     * Enhance the parsed units with capabilities from their fixture (unit-type)
     * definition. For each DISTINCT unit type, GETs /fixture/{type}, dumps the
     * raw response when cloudDebugEnabled, parses its `controls`, and derives
     * hasVertical/hasCCT/cctMin/Max from the actual control list — the
     * authoritative signal, replacing the mode-string heuristic. Non-fatal: a
     * type whose fixture cannot be fetched or parsed keeps its heuristic
     * capabilities (fallback), so an unreachable endpoint never breaks a unit.
     * The pre-existing heuristic values are logged alongside for verification.
     */
    void _fetchFixtures(NetworkConfig& config);

    /**
     * Fetch and parse a single fixture (unit type). Fills the capability
     * out-params from the fixture `controls`. Returns false (out-params
     * untouched) on HTTP/JSON failure or a missing `controls` array — the
     * caller then keeps the heuristic values for that type.
     */
    bool _fetchFixtureControls(uint16_t type, bool& hasVertical, bool& hasCCT,
                               uint16_t& cctMinKelvin, uint16_t& cctMaxKelvin,
                               String& model, String& mode);

    /**
     * Dump the raw cloud config response to Serial for analysis, with every
     * AES key value (keyStore.keys[].key — exactly AES_KEY_SIZE*2 hex chars)
     * replaced by "***" so no BLE key material is leaked. Gated by the caller
     * on cloudDebugEnabled. Streams verbatim segments in chunks (no full-copy
     * of the — potentially multi-kByte — response).
     */
    void _dumpRedactedConfig(const String& json);
};

#endif // API_CLIENT_H
