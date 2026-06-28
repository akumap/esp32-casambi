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
};

#endif // CONFIG_STORE_H
