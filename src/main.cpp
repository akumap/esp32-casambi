/**
 * ESP32 Casambi Controller - Main Application
 *
 * Hybrid WiFi/BLE controller for Casambi lighting systems
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include "config.h"
#include "cloud/network_config.h"
#include "cloud/api_client.h"
#include "storage/config_store.h"
#include "ble/casambi_client.h"
#include "web/webserver.h"

// Global state
NetworkConfig networkConfig;
CasambiClient* casambiClient = nullptr;
CasambiAPIClient* apiClient = nullptr;
CasambiWebServer* webServer = nullptr;
bool debugEnabled = false;  // Debug output toggle

// BLE scan state
struct ScannedDevice {
    String address;
    String name;
    int rssi;
};
std::vector<ScannedDevice> scannedDevices;

// Forward declarations
void runSetupWizard();
void scanForDevices();
void connectToDevice(int index);
void handleCommand(const String& cmd);

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n================================");
    Serial.println("  ESP32 Casambi Controller");
    Serial.println("================================\n");

    // Initialize filesystem
    if (!ConfigStore::init()) {
        Serial.println("ERROR: Failed to initialize storage");
        return;
    }

    // Check if we have configuration
    if (ConfigStore::hasValidConfig()) {
        Serial.println("Configuration found - entering operation mode");

        // Load config
        if (ConfigStore::loadNetworkConfig(networkConfig)) {
            Serial.printf("Network: %s\n", networkConfig.networkName.c_str());
            Serial.printf("Protocol: v%d\n", networkConfig.protocolVersion);
            Serial.printf("Units: %d\n", networkConfig.units.size());
            Serial.printf("Groups: %d\n", networkConfig.groups.size());
            Serial.printf("Scenes: %d\n", networkConfig.scenes.size());

            // Load debug setting
            debugEnabled = networkConfig.debugEnabled;

            // Initialize BLE first (before WiFi for proper coexistence)
            BLEDevice::init("ESP32-Casambi");

            // Initialize BLE client
            casambiClient = new CasambiClient(&networkConfig);

            // Auto-connect if enabled
            if (networkConfig.autoConnectEnabled && networkConfig.autoConnectAddress.length() > 0) {
                Serial.printf("Auto-connecting to %s...\n", networkConfig.autoConnectAddress.c_str());
                if (casambiClient->connect(networkConfig.autoConnectAddress)) {
                    Serial.println("Auto-connect successful!");
                } else {
                    Serial.println("Auto-connect failed. Use 'scan' and 'connect' manually.");
                }
            }

            // Connect to WiFi after BLE is initialized
            WiFiCredentials wifiCreds;
            if (ConfigStore::loadWiFiCredentials(wifiCreds)) {
                Serial.printf("\nConnecting to WiFi: %s...\n", wifiCreds.ssid.c_str());
                WiFi.mode(WIFI_STA);
                WiFi.begin(wifiCreds.ssid.c_str(), wifiCreds.password.c_str());

                unsigned long start = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                    delay(100);
                    Serial.print(".");
                }
                Serial.println();

                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
                } else {
                    Serial.println("WiFi connection failed - API will not be available");
                }
            } else {
                Serial.println("No WiFi credentials found - API will not be available");
            }

            // Start web server if WiFi connected
            if (WiFi.status() == WL_CONNECTED && casambiClient) {
                webServer = new CasambiWebServer(casambiClient, &networkConfig);
                if (webServer->begin()) {
                    Serial.printf("\nWeb API available at: http://%s/api\n", WiFi.localIP().toString().c_str());
                    Serial.println("Example: curl http://" + WiFi.localIP().toString() + "/api/status\n");
                }
            }

            Serial.println("\nReady. Type 'help' for commands.\n");
        } else {
            Serial.println("ERROR: Failed to load configuration");
        }
    } else {
        Serial.println("No configuration found - entering setup mode");
        Serial.println("Type 'help' for setup commands.\n");

        apiClient = new CasambiAPIClient();
    }
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
    // Handle serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.length() > 0) {
            handleCommand(cmd);
        }
    }

    delay(10);
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        // Check if this is a Casambi device (service UUID)
        if (advertisedDevice.haveServiceUUID()) {
            BLEUUID serviceUUID = advertisedDevice.getServiceUUID();
            if (serviceUUID.equals(BLEUUID(CASAMBI_SERVICE_UUID))) {
                ScannedDevice dev;
                dev.address = advertisedDevice.getAddress().toString().c_str();
                dev.name = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "Unknown";
                dev.rssi = advertisedDevice.getRSSI();

                // Check if already in list
                bool found = false;
                for (const auto& d : scannedDevices) {
                    if (d.address == dev.address) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    scannedDevices.push_back(dev);
                    Serial.printf("[%d] %s (%s) RSSI: %d\n",
                        scannedDevices.size() - 1,
                        dev.name.c_str(),
                        dev.address.c_str(),
                        dev.rssi);
                }
            }
        }
    }
};

// ============================================================================
// COMMAND HANDLER
// ============================================================================

void handleCommand(const String& cmd) {
        if (cmd == "help") {
            Serial.println("\n=== Commands ===");
            Serial.println("help          - Show this help");
            Serial.println("status        - Show status");
            Serial.println("refresh       - Refresh config from Casambi cloud");
            Serial.println("clearconfig   - Clear configuration (factory reset)");
            Serial.println();

            if (!ConfigStore::hasValidConfig()) {
                Serial.println("=== Setup Mode ===");
                Serial.println("setup         - Run setup wizard (scans for networks)");
                Serial.println();
            } else {
                Serial.println("=== BLE Commands ===");
                Serial.println("scan          - Scan for Casambi devices");
                Serial.println("connect <n>   - Connect to device n");
                Serial.println("disconnect    - Disconnect");
                Serial.println();
                Serial.println("autoconnect on/off - Enable/disable auto-connect");
                Serial.println("autoconnect status - Show auto-connect status");
                Serial.println("autoconnect set <mac> - Set auto-connect MAC address");
                Serial.println();
                Serial.println("wifi set <ssid> <password> - Update WiFi credentials");
                Serial.println("wifi status        - Show WiFi connection status");
                Serial.println();
                Serial.println("debug on/off       - Enable/disable debug output");
                Serial.println("debug status       - Show debug status");
                Serial.println();
                Serial.println("=== Control Commands ===");
                Serial.println("son <id>      - Turn scene ON");
                Serial.println("soff <id>     - Turn scene OFF");
                Serial.println("slevel <id> <0-255> - Set scene level");
                Serial.println();
                Serial.println("uon <id>      - Turn unit ON");
                Serial.println("uoff <id>     - Turn unit OFF");
                Serial.println("ulevel <id> <0-255> - Set unit level");
                Serial.println("ucolor <id> <r> <g> <b> - Set unit RGB color");
                Serial.println("utemp <id> <kelvin> - Set unit color temperature");
                Serial.println("uvertical <id> <0-255> - Set light balance (0=top only, 127=both, 255=bottom only)");
                Serial.println("uslider <id> <0-255> - Set motor position (0=up, 255=down)");
                Serial.println();
                Serial.println("glevel <id> <0-255> - Set group level");
                Serial.println("gvertical <id> <0-255> - Set light balance (0=top, 127=both, 255=bottom)");
                Serial.println("gslider <id> <0-255> - Set motor position (0=up, 255=down)");
                Serial.println();
                Serial.println("=== Info Commands ===");
                Serial.println("list units    - List all units");
                Serial.println("list groups   - List all groups");
                Serial.println("list scenes   - List all scenes");
                Serial.println("================");
                Serial.println();
                Serial.println("* Motor commands may not work on all units.");
                Serial.println("  Use scenes for reliable motor control.\n");
            }
        }
        else if (cmd == "status") {
            if (casambiClient) {
                Serial.printf("BLE Status: %s\n",
                    casambiClient->isAuthenticated() ? "Authenticated" : "Not connected");
            } else {
                Serial.println("Setup mode - no BLE client");
            }
        }
        else if (cmd == "refresh") {
            if (!ConfigStore::hasValidConfig()) {
                Serial.println("No configuration found. Run 'setup' first.");
                return;
            }

            Serial.println("\n=== Refresh Configuration ===");
            Serial.println("This will download fresh configuration from Casambi cloud.");
            Serial.println("Your local settings (auto-connect, debug) will be preserved.\n");

            // Check WiFi connection
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi not connected. Connecting...");
                WiFiCredentials wifiCreds;
                if (!ConfigStore::loadWiFiCredentials(wifiCreds)) {
                    Serial.println("ERROR: No WiFi credentials stored. Use 'wifi set' first.");
                    return;
                }

                WiFi.mode(WIFI_STA);
                WiFi.begin(wifiCreds.ssid.c_str(), wifiCreds.password.c_str());

                unsigned long start = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                    delay(100);
                    Serial.print(".");
                }
                Serial.println();

                if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("ERROR: WiFi connection failed");
                    return;
                }
                Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
            }

            // Get network password
            Serial.println("\nEnter your Casambi network password:");
            Serial.print("> ");
            while (!Serial.available()) delay(10);
            String password = Serial.readStringUntil('\n');
            password.trim();

            if (password.length() == 0) {
                Serial.println("Cancelled.");
                return;
            }

            // Preserve local settings
            bool savedAutoConnect = networkConfig.autoConnectEnabled;
            String savedAutoConnectAddr = networkConfig.autoConnectAddress;
            bool savedDebug = networkConfig.debugEnabled;

            // Get network ID from stored UUID
            Serial.println("--- Fetching network ID ---");
            String networkId;
            CasambiAPIClient tempClient;
            if (!tempClient.getNetworkId(networkConfig.networkUuid, networkId)) {
                Serial.printf("ERROR: Failed to get network ID: %s\n", tempClient.getLastError().c_str());
                return;
            }

            // Create session
            Serial.println("--- Creating session ---");
            String sessionToken;
            if (!tempClient.createSession(networkId, password, sessionToken)) {
                Serial.printf("ERROR: Authentication failed: %s\n", tempClient.getLastError().c_str());
                return;
            }

            // Fetch fresh configuration
            Serial.println("--- Downloading configuration ---");
            NetworkConfig freshConfig;
            if (!tempClient.fetchNetworkConfig(networkId, sessionToken, freshConfig)) {
                Serial.printf("ERROR: Failed to fetch config: %s\n", tempClient.getLastError().c_str());
                return;
            }

            // Restore network identifiers
            freshConfig.networkUuid = networkConfig.networkUuid;
            freshConfig.networkId = networkId;

            // Restore local settings
            freshConfig.autoConnectEnabled = savedAutoConnect;
            freshConfig.autoConnectAddress = savedAutoConnectAddr;
            freshConfig.debugEnabled = savedDebug;

            // Save updated configuration
            Serial.println("--- Saving to flash ---");
            if (!ConfigStore::saveNetworkConfig(freshConfig)) {
                Serial.println("ERROR: Failed to save configuration");
                return;
            }

            // Update in-memory config
            networkConfig = freshConfig;
            debugEnabled = freshConfig.debugEnabled;

            Serial.println("\n=== Refresh Complete! ===");
            Serial.printf("Network: %s\n", networkConfig.networkName.c_str());
            Serial.printf("Protocol: v%d (revision %d)\n", networkConfig.protocolVersion, networkConfig.revision);
            Serial.printf("Units: %d\n", networkConfig.units.size());
            Serial.printf("Groups: %d\n", networkConfig.groups.size());
            Serial.printf("Scenes: %d\n", networkConfig.scenes.size());
            Serial.println("\nConfiguration updated successfully!");
            Serial.println("Use 'list units/groups/scenes' to see changes.\n");
        }
        else if (cmd == "clearconfig") {
            ConfigStore::clearAll();
            Serial.println("Configuration cleared. Restarting...");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "setup") {
            if (apiClient) {
                runSetupWizard();
            } else {
                Serial.println("Already configured. Use 'clearconfig' to reset.");
            }
        }
        else if (cmd == "scan") {
            if (casambiClient) {
                scanForDevices();
            } else {
                Serial.println("Not in operation mode");
            }
        }
        else if (cmd.startsWith("connect ")) {
            if (casambiClient) {
                int index = cmd.substring(8).toInt();
                connectToDevice(index);
            } else {
                Serial.println("Not in operation mode");
            }
        }
        else if (cmd == "disconnect") {
            if (casambiClient) {
                casambiClient->disconnect();
                Serial.println("Disconnected");
            }
        }
        else if (cmd.startsWith("autoconnect ")) {
            String subcmd = cmd.substring(12);
            subcmd.trim();

            if (subcmd == "on") {
                networkConfig.autoConnectEnabled = true;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.println("Auto-connect enabled");
            }
            else if (subcmd == "off") {
                networkConfig.autoConnectEnabled = false;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.println("Auto-connect disabled");
            }
            else if (subcmd == "status") {
                Serial.printf("Auto-connect: %s\n",
                    networkConfig.autoConnectEnabled ? "enabled" : "disabled");
                if (networkConfig.autoConnectAddress.length() > 0) {
                    Serial.printf("MAC address: %s\n", networkConfig.autoConnectAddress.c_str());
                } else {
                    Serial.println("MAC address: (not set)");
                }
            }
            else if (subcmd.startsWith("set ")) {
                String mac = subcmd.substring(4);
                mac.trim();
                networkConfig.autoConnectAddress = mac;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Auto-connect MAC set to: %s\n", mac.c_str());
            }
            else {
                Serial.println("Usage: autoconnect on/off/status/set <mac>");
            }
        }
        else if (cmd.startsWith("wifi ")) {
            String subcmd = cmd.substring(5);
            subcmd.trim();

            if (subcmd.startsWith("set ")) {
                // Parse: wifi set <ssid> <password>
                String params = subcmd.substring(4);
                params.trim();

                int spaceIdx = params.indexOf(' ');
                if (spaceIdx == -1) {
                    Serial.println("Usage: wifi set <ssid> <password>");
                    Serial.println("Example: wifi set MyNetwork MyPassword123");
                } else {
                    String newSsid = params.substring(0, spaceIdx);
                    String newPassword = params.substring(spaceIdx + 1);
                    newPassword.trim();

                    // Save new credentials
                    WiFiCredentials newCreds;
                    newCreds.ssid = newSsid;
                    newCreds.password = newPassword;

                    if (ConfigStore::saveWiFiCredentials(newCreds)) {
                        Serial.printf("WiFi credentials updated (SSID: %s)\n", newSsid.c_str());
                        Serial.println("Reconnecting to WiFi...");

                        // Stop web server if running
                        if (webServer) {
                            webServer->stop();
                            delete webServer;
                            webServer = nullptr;
                        }

                        // Disconnect old WiFi
                        WiFi.disconnect();
                        delay(500);

                        // Connect to new WiFi
                        WiFi.mode(WIFI_STA);
                        WiFi.begin(newSsid.c_str(), newPassword.c_str());

                        unsigned long start = millis();
                        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                            delay(100);
                            Serial.print(".");
                        }
                        Serial.println();

                        if (WiFi.status() == WL_CONNECTED) {
                            Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

                            // Restart web server
                            if (casambiClient) {
                                webServer = new CasambiWebServer(casambiClient, &networkConfig);
                                if (webServer->begin()) {
                                    Serial.printf("Web API available at: http://%s/api\n", WiFi.localIP().toString().c_str());
                                }
                            }
                        } else {
                            Serial.println("WiFi connection failed!");
                            Serial.println("Credentials saved, but connection unsuccessful.");
                            Serial.println("Check SSID/password and restart ESP32.");
                        }
                    } else {
                        Serial.println("Failed to save WiFi credentials");
                    }
                }
            }
            else if (subcmd == "status") {
                Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
                    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
                    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
                }

                // Show stored credentials
                WiFiCredentials storedCreds;
                if (ConfigStore::loadWiFiCredentials(storedCreds)) {
                    Serial.printf("Stored SSID: %s\n", storedCreds.ssid.c_str());
                } else {
                    Serial.println("No stored WiFi credentials");
                }
            }
            else {
                Serial.println("Usage:");
                Serial.println("  wifi set <ssid> <password> - Update WiFi credentials");
                Serial.println("  wifi status                - Show WiFi status");
            }
        }
        else if (cmd.startsWith("debug ")) {
            String subcmd = cmd.substring(6);
            subcmd.trim();

            if (subcmd == "on") {
                debugEnabled = true;
                networkConfig.debugEnabled = true;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.println("Debug output enabled");
            }
            else if (subcmd == "off") {
                debugEnabled = false;
                networkConfig.debugEnabled = false;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.println("Debug output disabled");
            }
            else if (subcmd == "status") {
                Serial.printf("Debug output: %s\n", debugEnabled ? "enabled" : "disabled");
            }
            else {
                Serial.println("Usage: debug on/off/status");
            }
        }
        else if (cmd.startsWith("son ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(4).toInt();
                casambiClient->setSceneLevel(id, 0xFF);
                Serial.printf("Scene %d ON\n", id);
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("soff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(5).toInt();
                casambiClient->setSceneLevel(id, 0);
                Serial.printf("Scene %d OFF\n", id);
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("slevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 7);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(7, spacePos).toInt();
                    uint8_t level = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setSceneLevel(id, level);
                    Serial.printf("Scene %d level %d\n", id, level);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("uon ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(4).toInt();
                casambiClient->setUnitLevel(id, 255);
                Serial.printf("Unit %d ON\n", id);
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("uoff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(5).toInt();
                casambiClient->setUnitLevel(id, 0);
                Serial.printf("Unit %d OFF\n", id);
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("ulevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 7);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(7, spacePos).toInt();
                    uint8_t level = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setUnitLevel(id, level);
                    Serial.printf("Unit %d level %d\n", id, level);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("uvertical ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 10);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(10, spacePos).toInt();
                    uint8_t vertical = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setUnitVertical(id, vertical);
                    Serial.printf("Unit %d light balance %d (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("ucolor ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                // Parse: ucolor <id> <r> <g> <b>
                int space1 = cmd.indexOf(' ', 7);
                if (space1 > 0) {
                    int space2 = cmd.indexOf(' ', space1 + 1);
                    if (space2 > 0) {
                        int space3 = cmd.indexOf(' ', space2 + 1);
                        if (space3 > 0) {
                            uint8_t id = cmd.substring(7, space1).toInt();
                            uint8_t r = cmd.substring(space1 + 1, space2).toInt();
                            uint8_t g = cmd.substring(space2 + 1, space3).toInt();
                            uint8_t b = cmd.substring(space3 + 1).toInt();
                            casambiClient->setUnitColor(id, r, g, b);
                            Serial.printf("Unit %d color RGB(%d,%d,%d)\n", id, r, g, b);
                        }
                    }
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("utemp ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 6);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(6, spacePos).toInt();
                    uint16_t kelvin = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setUnitTemperature(id, kelvin);
                    Serial.printf("Unit %d temperature %dK\n", id, kelvin);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("uslider ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 8);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(8, spacePos).toInt();
                    uint8_t slider = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setUnitSlider(id, slider);
                    Serial.printf("Unit %d motor position %d (0=up, 255=down)\n", id, slider);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("glevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 7);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(7, spacePos).toInt();
                    uint8_t level = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setGroupLevel(id, level);
                    Serial.printf("Group %d level %d\n", id, level);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("gvertical ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 10);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(10, spacePos).toInt();
                    uint8_t vertical = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setGroupVertical(id, vertical);
                    Serial.printf("Group %d light balance %d (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("gslider ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 8);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(8, spacePos).toInt();
                    uint8_t slider = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setGroupSlider(id, slider);
                    Serial.printf("Group %d motor position %d (0=up, 255=down)\n", id, slider);
                }
            } else {
                Serial.println("Not authenticated");
            }
        }
        else if (cmd.startsWith("list ")) {
            String what = cmd.substring(5);
            if (what == "units") {
                Serial.printf("\n=== Units (%d) ===\n", networkConfig.units.size());
                for (const auto& unit : networkConfig.units) {
                    Serial.printf("[%d] %s\n", unit.deviceId, unit.name.c_str());
                }
                Serial.println();
            }
            else if (what == "groups") {
                Serial.printf("\n=== Groups (%d) ===\n", networkConfig.groups.size());
                for (const auto& group : networkConfig.groups) {
                    Serial.printf("[%d] %s\n", group.groupId, group.name.c_str());
                }
                Serial.println();
            }
            else if (what == "scenes") {
                Serial.printf("\n=== Scenes (%d) ===\n", networkConfig.scenes.size());
                for (const auto& scene : networkConfig.scenes) {
                    Serial.printf("[%d] %s\n", scene.sceneId, scene.name.c_str());
                }
                Serial.println();
            }
        }
        else {
            Serial.println("Unknown command. Type 'help'");
        }
}

// ============================================================================
// SETUP WIZARD
// ============================================================================

void runSetupWizard() {
    Serial.println("\n=== Casambi Setup Wizard ===\n");

    // Step 1: Initialize BLE and scan for devices
    Serial.println("Step 1: Scanning for Casambi networks...");
    Serial.println("(Make sure your Casambi lights are powered on)\n");

    BLEDevice::init("ESP32-Casambi");

    scannedDevices.clear();
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("Scanning for 10 seconds...\n");
    pBLEScan->start(10, false);

    if (scannedDevices.size() == 0) {
        Serial.println("\nNo Casambi networks found!");
        Serial.println("Make sure your lights are on and try again.");
        BLEDevice::deinit();
        return;
    }

    Serial.printf("\nFound %d Casambi network(s):\n\n", scannedDevices.size());
    for (size_t i = 0; i < scannedDevices.size(); i++) {
        Serial.printf("[%d] %s (%s) RSSI: %d\n",
            i,
            scannedDevices[i].name.c_str(),
            scannedDevices[i].address.c_str(),
            scannedDevices[i].rssi);
    }

    Serial.println("\nSelect network (enter number):");
    Serial.print("> ");
    while (!Serial.available()) delay(10);
    String indexStr = Serial.readStringUntil('\n');
    indexStr.trim();
    int selectedIndex = indexStr.toInt();

    if (selectedIndex < 0 || selectedIndex >= scannedDevices.size()) {
        Serial.println("Invalid selection. Cancelled.");
        BLEDevice::deinit();
        return;
    }

    // Extract network UUID from MAC address (remove colons, lowercase)
    String networkUuid = scannedDevices[selectedIndex].address;
    networkUuid.replace(":", "");
    networkUuid.toLowerCase();

    Serial.printf("\nSelected: %s\n", scannedDevices[selectedIndex].name.c_str());
    Serial.printf("Network UUID: %s\n", networkUuid.c_str());

    // Clean up BLE for now (will reinit WiFi)
    BLEDevice::deinit();
    delay(500);

    // Step 2: Get network password
    Serial.println("\nStep 2: Enter network password");
    Serial.println("Enter your Casambi network password:");
    Serial.print("> ");
    while (!Serial.available()) delay(10);
    String password = Serial.readStringUntil('\n');
    password.trim();

    if (password.length() == 0) {
        Serial.println("Cancelled.");
        return;
    }

    // Step 3: Get WiFi credentials
    Serial.println("\nStep 3: WiFi Configuration");
    Serial.println("Enter WiFi SSID:");
    Serial.print("> ");
    while (!Serial.available()) delay(10);
    String ssid = Serial.readStringUntil('\n');
    ssid.trim();

    if (ssid.length() == 0) {
        Serial.println("Cancelled.");
        return;
    }

    Serial.println("\nEnter WiFi password:");
    Serial.print("> ");
    while (!Serial.available()) delay(10);
    String wifiPassword = Serial.readStringUntil('\n');
    wifiPassword.trim();

    // Step 4: Connect to WiFi
    Serial.println("\nStep 4: Connecting to cloud");
    Serial.println("--- Connecting to WiFi ---");
    if (!apiClient->connectWiFi(ssid, wifiPassword)) {
        Serial.printf("ERROR: WiFi connection failed: %s\n", apiClient->getLastError().c_str());
        return;
    }

    // Get network ID from UUID
    Serial.println("--- Fetching network ID ---");
    String networkId;
    if (!apiClient->getNetworkId(networkUuid, networkId)) {
        Serial.printf("ERROR: Failed to get network ID: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Create session
    Serial.println("--- Creating session ---");
    String sessionToken;
    if (!apiClient->createSession(networkId, password, sessionToken)) {
        Serial.printf("ERROR: Failed to create session: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Fetch network configuration
    Serial.println("--- Downloading network configuration ---");
    if (!apiClient->fetchNetworkConfig(networkId, sessionToken, networkConfig)) {
        Serial.printf("ERROR: Failed to fetch config: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Store network UUID and ID
    networkConfig.networkUuid = networkUuid;
    networkConfig.networkId = networkId;

    // Step 5: Save configuration
    Serial.println("\nStep 5: Saving configuration");
    Serial.println("--- Saving to flash ---");
    if (!ConfigStore::saveNetworkConfig(networkConfig)) {
        Serial.println("ERROR: Failed to save configuration");
        apiClient->disconnectWiFi();
        return;
    }

    // Save WiFi credentials
    WiFiCredentials wifiCreds;
    wifiCreds.ssid = ssid;
    wifiCreds.password = wifiPassword;
    ConfigStore::saveWiFiCredentials(wifiCreds);

    // Disconnect WiFi
    apiClient->disconnectWiFi();

    Serial.println("\n=== Setup Complete! ===");
    Serial.printf("Network: %s\n", networkConfig.networkName.c_str());
    Serial.printf("Units: %d\n", networkConfig.units.size());
    Serial.printf("Groups: %d\n", networkConfig.groups.size());
    Serial.printf("Scenes: %d\n", networkConfig.scenes.size());
    Serial.println("\nRestarting to enter operation mode...");
    delay(2000);
    ESP.restart();
}

void scanForDevices() {
    Serial.println("\n=== Scanning for Casambi devices ===");
    Serial.println("Scanning for 10 seconds...\n");

    scannedDevices.clear();

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    BLEScanResults foundDevices = pBLEScan->start(10, false);

    Serial.printf("\nFound %d Casambi device(s)\n", scannedDevices.size());
    Serial.println("Use 'connect <n>' to connect to device n\n");

    pBLEScan->clearResults();
}

void connectToDevice(int index) {
    if (index < 0 || index >= scannedDevices.size()) {
        Serial.printf("Invalid device index. Use 0-%d\n", scannedDevices.size() - 1);
        return;
    }

    ScannedDevice& dev = scannedDevices[index];
    Serial.printf("Connecting to %s (%s)...\n", dev.name.c_str(), dev.address.c_str());

    if (casambiClient->connect(dev.address)) {
        Serial.println("Connected and authenticated successfully!");

        // Auto-save MAC address for auto-connect
        if (networkConfig.autoConnectAddress != dev.address) {
            networkConfig.autoConnectAddress = dev.address;
            ConfigStore::saveNetworkConfig(networkConfig);
            Serial.printf("Saved MAC address for auto-connect: %s\n", dev.address.c_str());
        }
    } else {
        Serial.println("Connection failed");
    }
}
