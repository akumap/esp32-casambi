/**
 * Configuration Storage
 *
 * Save/load network configuration and WiFi credentials to/from LittleFS
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <Arduino.h>
#include "../cloud/network_config.h"

class ConfigStore {
public:
    /**
     * Initialize LittleFS
     * @return true on success
     */
    static bool init();

    /**
     * Check if valid network configuration exists
     */
    static bool hasValidConfig();

    /**
     * Save network configuration to flash
     * @param config Network configuration
     * @return true on success
     */
    static bool saveNetworkConfig(const NetworkConfig& config);

    /**
     * Load network configuration from flash
     * @param config Output network configuration
     * @return true on success
     */
    static bool loadNetworkConfig(NetworkConfig& config);

    /**
     * Persist only the per-category debug flags (six bools, own file).
     *
     * Kept apart from saveNetworkConfig() on purpose: that path serializes the
     * whole network configuration and re-parses it for validation, which is a
     * large transient heap cost to record a debug toggle. Use this for any
     * change that only touches the debug flags.
     *
     * @return true on success
     */
    static bool saveDebugFlags(const NetworkConfig& config);

    /**
     * Overlay the debug flags from their own file onto `config`, if that file
     * exists. Call right after loadNetworkConfig(): the values parsed from the
     * main config act as the fallback for installations that predate the split,
     * and this overrides them once the device has written the file at least
     * once.
     *
     * @return true if the flags file existed and was applied
     */
    static bool loadDebugFlags(NetworkConfig& config);

    /**
     * Save WiFi credentials to flash
     * @param creds WiFi credentials
     * @return true on success
     */
    static bool saveWiFiCredentials(const WiFiCredentials& creds);

    /**
     * Load WiFi credentials from flash
     * @param creds Output WiFi credentials
     * @return true on success
     */
    static bool loadWiFiCredentials(WiFiCredentials& creds);

    /**
     * Clear all configuration (factory reset)
     */
    static void clearAll();

    /**
     * Schedule a Casambi cloud-config refresh for the next boot. Setting this
     * marker and rebooting lets the refresh run early in setup() — before BLE
     * and the async web server are started — which avoids the task races of
     * tearing those down at runtime.
     */
    static void setRefreshPending();

    /**
     * @return true if a cloud-config refresh was scheduled for this boot.
     */
    static bool isRefreshPending();

    /**
     * Clear the refresh marker (call before attempting the refresh so a failed
     * download cannot cause a reboot loop).
     */
    static void clearRefreshPending();

private:
    static bool _initialized;

    // Parse, validate and populate from a specific file (live or backup). Return
    // false if the file is missing, unparseable or semantically invalid.
    static bool _loadNetworkConfigFrom(const char* path, NetworkConfig& config);
    static bool _loadWiFiCredentialsFrom(const char* path, WiFiCredentials& creds);
};

#endif // CONFIG_STORE_H
