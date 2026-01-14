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

private:
    static bool _initialized;
};

#endif // CONFIG_STORE_H
