/**
 * ESP32 Casambi Controller - Main Application
 *
 * Hybrid WiFi/BLE controller for Casambi lighting systems.
 * Includes auto-reconnect, watchdog, and connection monitoring.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
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
bool bleDebugEnabled   = false;
bool webDebugEnabled   = true;
bool parseDebugEnabled = false;
bool heapDebugEnabled  = false;

// BLE scan state
struct ScannedDevice {
    String address;
    String name;
    int rssi;
};
std::vector<ScannedDevice> scannedDevices;

// ============================================================================
// RECONNECT & MONITORING STATE
// ============================================================================

// BLE reconnect state
static unsigned long lastBLEReconnectAttempt = 0;
static unsigned long bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
static uint8_t consecutiveReconnectFailures = 0;
static bool bleReconnectEnabled = true;  // Can be disabled via command

// WiFi monitoring state
static unsigned long lastWiFiCheck = 0;

// Heap monitoring state
static unsigned long lastHeapCheck = 0;
static size_t minFreeHeap = UINT32_MAX;

// Connection health check state
static unsigned long lastConnectionCheck = 0;

// Forward declarations
void runSetupWizard();
void scanForDevices();
void connectToDevice(int index);
void handleCommand(const String& cmd);
void checkAndReconnectBLE();
void checkAndReconnectWiFi();
void monitorHeap();
void printStatus();

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n================================");
    Serial.println("  ESP32 Casambi Controller");
    Serial.println("================================\n");

    // Initialize hardware watchdog timer
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);  // true = panic on timeout
    esp_task_wdt_add(NULL);  // Add current task (loopTask) to WDT
    Serial.printf("Watchdog timer: %d seconds\n", WDT_TIMEOUT_SECONDS);

    // Log initial heap
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    minFreeHeap = ESP.getFreeHeap();

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

            // Load debug settings
            bleDebugEnabled   = networkConfig.bleDebugEnabled;
            webDebugEnabled   = networkConfig.webDebugEnabled;
            parseDebugEnabled = networkConfig.parseDebugEnabled;
            heapDebugEnabled  = networkConfig.heapDebugEnabled;

            // Initialize BLE first (before WiFi for proper coexistence)
            BLEDevice::init("ESP32-Casambi");

            // Initialize BLE client
            casambiClient = new CasambiClient(&networkConfig);

            // Set up connection state callback for auto-reconnect
            casambiClient->setConnectionStateCallback(
                [](ConnectionState newState, DisconnectReason reason) {
                    if (newState == ConnectionState::None &&
                        reason != DisconnectReason::UserRequested) {
                        Serial.printf("*** BLE connection lost (reason: %d) - will auto-reconnect ***\n",
                                      static_cast<int>(reason));
                        // Reset backoff on new disconnect
                        bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
                        lastBLEReconnectAttempt = millis();
                    }
                }
            );

            // Set up unit state callback for logging
            casambiClient->setUnitStateCallback(
                [](uint8_t unitId, uint8_t level, bool online) {
                    // This is called from the notification handler
                    // Could be used to push state to home automation, MQTT, etc.
                    if (bleDebugEnabled) {
                        Serial.printf("CALLBACK: Unit %d -> level=%d online=%d\n",
                                      unitId, level, online);
                    }
                }
            );

            // Auto-connect if enabled
            if (networkConfig.autoConnectEnabled && networkConfig.autoConnectAddress.length() > 0) {
                Serial.printf("Auto-connecting to %s...\n", networkConfig.autoConnectAddress.c_str());
                if (casambiClient->connect(networkConfig.autoConnectAddress)) {
                    Serial.println("Auto-connect successful!");
                    consecutiveReconnectFailures = 0;
                } else {
                    Serial.println("Auto-connect failed. Will retry automatically.");
                    lastBLEReconnectAttempt = millis();
                }
            }

            // Connect to WiFi after BLE is initialized
            WiFiCredentials wifiCreds;
            if (ConfigStore::loadWiFiCredentials(wifiCreds)) {
                Serial.printf("\nConnecting to WiFi: %s...\n", wifiCreds.ssid.c_str());
                WiFi.mode(WIFI_STA);
                WiFi.setAutoReconnect(true);  // Enable WiFi auto-reconnect
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
                    Serial.println("WiFi connection failed - will retry in background");
                }
            } else {
                Serial.println("No WiFi credentials found - API will not be available");
            }

            // Start web server if WiFi connected
            if (WiFi.status() == WL_CONNECTED && casambiClient) {
                webServer = new CasambiWebServer(casambiClient, &networkConfig);
                if (webServer->begin()) {
                    Serial.printf("\nWeb API available at: http://%s/api\n", WiFi.localIP().toString().c_str());
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
// LOOP - with watchdog feeding, reconnect, and monitoring
// ============================================================================

void loop() {
    // Feed the watchdog - MUST happen every WDT_TIMEOUT_SECONDS
    esp_task_wdt_reset();

    // Handle serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.length() > 0) {
            handleCommand(cmd);
        }
    }

    // === Periodic tasks (only in operation mode) ===
    if (casambiClient) {
        // Check BLE connection health
        unsigned long now = millis();
        if (now - lastConnectionCheck >= CONNECTION_CHECK_INTERVAL_MS) {
            lastConnectionCheck = now;
            casambiClient->checkConnectionHealth();
        }

        // Auto-reconnect BLE if disconnected
        checkAndReconnectBLE();
    }


    static unsigned long lastKeepalive = 0;
    if (casambiClient && casambiClient->isAuthenticated()) {
        if (millis() - lastKeepalive >= 30000) {
            lastKeepalive = millis();
            if (!casambiClient->sendKeepalive()) {
                Serial.println("*** BLE keepalive failed, auto-reconnect will handle it ***");
            }
        }
    }

    // Check WiFi connection
    checkAndReconnectWiFi();

    // Monitor heap usage
    monitorHeap();

    delay(10);
}

// ============================================================================
// BLE AUTO-RECONNECT
// ============================================================================

void checkAndReconnectBLE() {
    if (!casambiClient || !bleReconnectEnabled) return;
    if (casambiClient->isAuthenticated()) return;  // Already connected

    // Only reconnect if we have an auto-connect address
    if (!networkConfig.autoConnectEnabled || networkConfig.autoConnectAddress.length() == 0) return;

    unsigned long now = millis();
    if (now - lastBLEReconnectAttempt < bleReconnectInterval) return;

    lastBLEReconnectAttempt = now;

    Serial.printf("BLE: Auto-reconnect attempt #%d to %s (backoff: %lu ms)...\n",
                  consecutiveReconnectFailures + 1,
                  networkConfig.autoConnectAddress.c_str(),
                  bleReconnectInterval);

    if (casambiClient->connect(networkConfig.autoConnectAddress)) {
        Serial.println("BLE: Reconnect successful!");
        consecutiveReconnectFailures = 0;
        bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;  // Reset backoff

        // Restart web server if needed
        if (WiFi.status() == WL_CONNECTED && !webServer) {
            webServer = new CasambiWebServer(casambiClient, &networkConfig);
            if (webServer->begin()) {
                Serial.printf("Web API restarted at: http://%s/api\n",
                              WiFi.localIP().toString().c_str());
            }
        }
    } else {
        consecutiveReconnectFailures++;

        // Exponential backoff (double interval, up to max)
        bleReconnectInterval = min(bleReconnectInterval * 2, (unsigned long)BLE_RECONNECT_MAX_BACKOFF_MS);

        Serial.printf("BLE: Reconnect failed (%d/%d). Next attempt in %lu ms\n",
                      consecutiveReconnectFailures, MAX_RECONNECT_FAILURES,
                      bleReconnectInterval);

        // If too many failures, restart the ESP32
        if (consecutiveReconnectFailures >= MAX_RECONNECT_FAILURES) {
            Serial.println("*** Too many BLE reconnect failures! Restarting ESP32 ***");
            delay(1000);
            ESP.restart();
        }
    }
}

// ============================================================================
// WIFI MONITORING & RECONNECT
// ============================================================================

void checkAndReconnectWiFi() {
    unsigned long now = millis();
    if (now - lastWiFiCheck < WIFI_RECONNECT_INTERVAL_MS) return;
    lastWiFiCheck = now;

    if (WiFi.status() == WL_CONNECTED) return;

    // WiFi is disconnected
    WiFiCredentials wifiCreds;
    if (!ConfigStore::loadWiFiCredentials(wifiCreds)) return;

    Serial.println("WiFi: Connection lost, attempting reconnect...");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(wifiCreds.ssid.c_str(), wifiCreds.password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
        delay(100);
        esp_task_wdt_reset();  // Feed watchdog during WiFi connect
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());

        // Restart web server
        if (casambiClient && !webServer) {
            webServer = new CasambiWebServer(casambiClient, &networkConfig);
            if (webServer->begin()) {
                Serial.printf("Web API restarted at: http://%s/api\n",
                              WiFi.localIP().toString().c_str());
            }
        }
    } else {
        if (bleDebugEnabled) {
            Serial.println("WiFi: Reconnect failed, will retry later");
        }
    }
}

// ============================================================================
// HEAP MONITORING
// ============================================================================

void monitorHeap() {
    unsigned long now = millis();
    if (now - lastHeapCheck < HEAP_MONITOR_INTERVAL_MS) return;
    lastHeapCheck = now;

    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = ESP.getMaxAllocHeap();

    if (freeHeap < minFreeHeap) {
        minFreeHeap = freeHeap;
    }

    if (heapDebugEnabled) {
        Serial.printf("HEAP: free=%d, min=%d, largest_block=%d\n",
                      freeHeap, minFreeHeap, largestBlock);
    }

    // Critical heap warning
    if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
        Serial.printf("*** CRITICAL: Free heap %d bytes < %d threshold! ***\n",
                      freeHeap, HEAP_CRITICAL_THRESHOLD);
        Serial.println("*** Restarting ESP32 to prevent crash ***");
        delay(1000);
        ESP.restart();
    }
}

// ============================================================================
// STATUS DISPLAY
// ============================================================================

void printStatus() {
    Serial.println("\n=== System Status ===");

    // BLE status
    if (casambiClient) {
        Serial.printf("BLE: %s\n",
            casambiClient->isAuthenticated() ? "Authenticated" :
            (casambiClient->getState() == ConnectionState::None ? "Disconnected" : "Connecting..."));

        if (casambiClient->isAuthenticated()) {
            unsigned long uptime = casambiClient->getConnectionUptime();
            Serial.printf("  Uptime: %lu:%02lu:%02lu\n",
                          uptime / 3600000, (uptime / 60000) % 60, (uptime / 1000) % 60);
            Serial.printf("  Packets received: %u\n", casambiClient->getReceivedPacketCount());
            Serial.printf("  Connected to: %s\n", casambiClient->getConnectedAddress().c_str());
        }

        if (casambiClient->getLastDisconnectReason() != DisconnectReason::None) {
            Serial.printf("  Last disconnect reason: %d\n",
                          static_cast<int>(casambiClient->getLastDisconnectReason()));
        }
    } else {
        Serial.println("BLE: Setup mode - no client");
    }

    // WiFi status
    Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  IP: %s, RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    // Web server
    Serial.printf("Web Server: %s\n",
                  (webServer && webServer->isRunning()) ? "Running" : "Stopped");

    // System info
    Serial.printf("Heap: free=%d, min=%d, largest=%d\n",
                  ESP.getFreeHeap(), minFreeHeap, ESP.getMaxAllocHeap());
    Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
    Serial.printf("Reconnect failures: %d/%d\n", consecutiveReconnectFailures, MAX_RECONNECT_FAILURES);
    Serial.printf("Auto-reconnect: %s\n", bleReconnectEnabled ? "enabled" : "disabled");
    Serial.println();
}

// ============================================================================
// BLE SCANNING (with memory leak fix)
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

static ScanCallbacks* scanCallbackInstance = nullptr;

// ============================================================================
// COMMAND HANDLER
// ============================================================================

void handleCommand(const String& cmd) {
	Serial.printf(">>> CMD: %s\n", cmd.c_str());
        if (cmd == "help") {
            Serial.println("\n=== Commands ===");
            Serial.println("help          - Show this help");
            Serial.println("status        - Show detailed status");
            Serial.println("refresh       - Refresh config from Casambi cloud");
            Serial.println("clearconfig   - Clear configuration (factory reset)");
            Serial.println("restart       - Restart ESP32");
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
                Serial.println("reconnect on/off   - Enable/disable auto-reconnect");
                Serial.println();
                Serial.println("wifi set <ssid> <password> - Update WiFi credentials");
                Serial.println("wifi status        - Show WiFi connection status");
                Serial.println();
                Serial.println("debug on/off       - Restore/suppress all debug (settings preserved)");
                Serial.println("debug ble on/off   - BLE/crypto verbose output");
                Serial.println("debug web on/off   - Web API request logging");
                Serial.println("debug parse on/off - Protocol compact output (P06/P07...)");
                Serial.println("debug heap on/off  - Heap monitoring");
                Serial.println("debug status       - Show debug status per category");
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
            printStatus();
        }
        else if (cmd == "restart") {
            Serial.println("Restarting...");
            delay(500);
            ESP.restart();
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
                    esp_task_wdt_reset();
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
            while (!Serial.available()) {
                delay(10);
                esp_task_wdt_reset();
            }
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
        else if (cmd.startsWith("reconnect ")) {
            String subcmd = cmd.substring(10);
            subcmd.trim();
            if (subcmd == "on") {
                bleReconnectEnabled = true;
                consecutiveReconnectFailures = 0;
                bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
                Serial.println("Auto-reconnect enabled");
            } else if (subcmd == "off") {
                bleReconnectEnabled = false;
                Serial.println("Auto-reconnect disabled");
            } else {
                Serial.printf("Auto-reconnect: %s\n", bleReconnectEnabled ? "enabled" : "disabled");
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
                        WiFi.setAutoReconnect(true);
                        WiFi.begin(newSsid.c_str(), newPassword.c_str());

                        unsigned long start = millis();
                        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                            delay(100);
                            esp_task_wdt_reset();
                            Serial.print(".");
                        }
                        Serial.println();

                        if (WiFi.status() == WL_CONNECTED) {
                            Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

                            // Restart web server
                            if (casambiClient) {
                                webServer = new CasambiWebServer(casambiClient, &networkConfig);
                                if (webServer->begin()) {
                                    Serial.printf("Web API available at: http://%s/api\n",
                                                  WiFi.localIP().toString().c_str());
                                }
                            }
                        } else {
                            Serial.println("WiFi connection failed!");
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
                }
            }
            else {
                Serial.println("Usage: wifi set <ssid> <password> | wifi status");
            }
        }
        else if (cmd.startsWith("debug ")) {
            String subcmd = cmd.substring(6);
            subcmd.trim();

            if (subcmd == "on") {
                // Restore all saved per-category settings
                bleDebugEnabled   = networkConfig.bleDebugEnabled;
                webDebugEnabled   = networkConfig.webDebugEnabled;
                parseDebugEnabled = networkConfig.parseDebugEnabled;
                heapDebugEnabled  = networkConfig.heapDebugEnabled;
                Serial.printf("Debug on: ble=%s web=%s parse=%s heap=%s\n",
                              bleDebugEnabled ? "on" : "off",
                              webDebugEnabled ? "on" : "off",
                              parseDebugEnabled ? "on" : "off",
                              heapDebugEnabled ? "on" : "off");
            }
            else if (subcmd == "off") {
                // Suppress all output without changing saved settings
                bleDebugEnabled   = false;
                webDebugEnabled   = false;
                parseDebugEnabled = false;
                heapDebugEnabled  = false;
                Serial.println("Debug off (settings preserved, use 'debug on' to restore)");
            }
            else if (subcmd.startsWith("ble ")) {
                bool val = subcmd.endsWith(" on");
                bleDebugEnabled = val;
                networkConfig.bleDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("BLE debug: %s\n", val ? "on" : "off");
            }
            else if (subcmd.startsWith("web ")) {
                bool val = subcmd.endsWith(" on");
                webDebugEnabled = val;
                networkConfig.webDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Web debug: %s\n", val ? "on" : "off");
            }
            else if (subcmd.startsWith("parse ")) {
                bool val = subcmd.endsWith(" on");
                parseDebugEnabled = val;
                networkConfig.parseDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Parse debug: %s\n", val ? "on" : "off");
            }
            else if (subcmd.startsWith("heap ")) {
                bool val = subcmd.endsWith(" on");
                heapDebugEnabled = val;
                networkConfig.heapDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Heap debug: %s\n", val ? "on" : "off");
            }
            else if (subcmd == "status") {
                Serial.printf("ble=%s  web=%s  parse=%s  heap=%s\n",
                              bleDebugEnabled   ? "on" : "off",
                              webDebugEnabled   ? "on" : "off",
                              parseDebugEnabled ? "on" : "off",
                              heapDebugEnabled  ? "on" : "off");
            }
            else {
                Serial.println("Usage: debug on/off/status");
                Serial.println("       debug ble on/off    - BLE/crypto verbose output");
                Serial.println("       debug web on/off    - Web API request logging");
                Serial.println("       debug parse on/off  - Protocol compact output (P06/P07...)");
                Serial.println("       debug heap on/off   - Heap monitoring");
            }
        }
        // Scene commands
        else if (cmd.startsWith("son ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(4).toInt();
                casambiClient->setSceneLevel(id, 0xFF);
                Serial.printf("Scene %d ON\n", id);
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("soff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(5).toInt();
                casambiClient->setSceneLevel(id, 0);
                Serial.printf("Scene %d OFF\n", id);
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
        }
        // Unit commands
        else if (cmd.startsWith("uon ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(4).toInt();
                casambiClient->setUnitLevel(id, 255);
                Serial.printf("Unit %d ON\n", id);
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("uoff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id = cmd.substring(5).toInt();
                casambiClient->setUnitLevel(id, 0);
                Serial.printf("Unit %d OFF\n", id);
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("ucolor ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
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
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
        }
        // Group commands
        else if (cmd.startsWith("glevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                int spacePos = cmd.indexOf(' ', 7);
                if (spacePos > 0) {
                    uint8_t id = cmd.substring(7, spacePos).toInt();
                    uint8_t level = cmd.substring(spacePos + 1).toInt();
                    casambiClient->setGroupLevel(id, level);
                    Serial.printf("Group %d level %d\n", id, level);
                }
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
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
            } else { Serial.println("Not authenticated"); }
        }
        // List commands
        else if (cmd.startsWith("list ")) {
            String what = cmd.substring(5);
            if (what == "units") {
                Serial.printf("\n=== Units (%d) ===\n", networkConfig.units.size());
                for (const auto& unit : networkConfig.units) {
                    Serial.printf("[%d] %s %s\n", unit.deviceId, unit.name.c_str(),
                                  unit.on ? "(ON)" : "(OFF)");
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

    Serial.println("Step 1: Scanning for Casambi networks...");
    Serial.println("(Make sure your Casambi lights are powered on)\n");

    BLEDevice::init("ESP32-Casambi");

    scannedDevices.clear();
    BLEScan* pBLEScan = BLEDevice::getScan();

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setAdvertisedDeviceCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("Scanning for 10 seconds...\n");
    pBLEScan->start(10, false);
    pBLEScan->clearResults();  // Free scan result memory

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
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String indexStr = Serial.readStringUntil('\n');
    indexStr.trim();
    int selectedIndex = indexStr.toInt();

    if (selectedIndex < 0 || selectedIndex >= (int)scannedDevices.size()) {
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
    Serial.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String password = Serial.readStringUntil('\n');
    password.trim();
    if (password.length() == 0) { Serial.println("Cancelled."); return; }

    // Step 3: Get WiFi credentials
    Serial.println("\nStep 3: WiFi Configuration");
    Serial.println("Enter WiFi SSID:");
    Serial.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String ssid = Serial.readStringUntil('\n');
    ssid.trim();
    if (ssid.length() == 0) { Serial.println("Cancelled."); return; }

    Serial.println("\nEnter WiFi password:");
    Serial.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
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

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setAdvertisedDeviceCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    BLEScanResults foundDevices = pBLEScan->start(10, false);

    Serial.printf("\nFound %d Casambi device(s)\n", scannedDevices.size());
    Serial.println("Use 'connect <n>' to connect to device n\n");

    pBLEScan->clearResults();
}

void connectToDevice(int index) {
    if (index < 0 || index >= (int)scannedDevices.size()) {
        Serial.printf("Invalid device index. Use 0-%d\n", scannedDevices.size() - 1);
        return;
    }

    ScannedDevice& dev = scannedDevices[index];
    Serial.printf("Connecting to %s (%s)...\n", dev.name.c_str(), dev.address.c_str());

    if (casambiClient->connect(dev.address)) {
        Serial.println("Connected and authenticated successfully!");
        consecutiveReconnectFailures = 0;

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
