/**
 * ESP32 Casambi Controller - Main Application
 *
 * Hybrid WiFi/BLE controller for Casambi lighting systems.
 * Includes auto-reconnect, watchdog, and connection monitoring.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <atomic>
#include "config.h"
#include "cloud/network_config.h"
#include "cloud/api_client.h"
#include "storage/config_store.h"
#include "ble/casambi_client.h"
#include "ble/casambi_scan.h"
#include "ble/packet.h"
#include "crypto/encryption.h"
#include "web/webserver.h"
#include "web/setup_portal.h"
#include "log/event_log.h"
#include "serial_args.h"
#include "app_state.h"
#include "net/time_sync.h"
#include "net/wifi_manager.h"
#include "ble/reconnect_supervisor.h"
#include "diagnostics.h"
#include "cloud_refresh.h"
#include "serial_console.h"

// Global state
NetworkConfig networkConfig;
CasambiClient* casambiClient = nullptr;
CasambiAPIClient* apiClient = nullptr;
CasambiWebServer* webServer = nullptr;
SetupPortal* setupPortal = nullptr;

// Guards the runtime-mutable NetworkConfig String fields (see config.h). All
// writers run on the loop task; the async_tcp task copies under this mutex.
SemaphoreHandle_t g_configMutex = nullptr;

// Take/give g_configMutex around a String mutation on the loop task.
void configLock()   { if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY); }
void configUnlock() { if (g_configMutex) xSemaphoreGive(g_configMutex); }
bool bleDebugEnabled     = false;
bool casambiDebugEnabled = true;
bool webDebugEnabled     = true;
bool parseDebugEnabled   = false;
bool heapDebugEnabled    = false;
bool cloudDebugEnabled   = false;

// BLE scan state (ScannedDevice is declared in app_state.h)
std::vector<ScannedDevice> scannedDevices;

// ============================================================================
// RECONNECT & MONITORING STATE
// ============================================================================

// WiFi monitoring state
static unsigned long lastWiFiCheck = 0;

// Connection health check state
static unsigned long lastConnectionCheck = 0;

// Forward declarations

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    // Matches platformio.ini's monitor_speed — the two must be changed
    // together, upload_speed (flashing) is independent of this.
    Serial.begin(1500000);
    delay(1000);

    g_configMutex = xSemaphoreCreateMutex();
    if (!g_configMutex) {
        // Heap exhaustion this early in boot is unrecoverable, and the lock
        // helpers would deliberately run unlocked on a null mutex (see
        // configLock) — restart instead of running with undefined races.
        // (EventLog is not initialized yet, so Serial is all we have here.)
        Serial.println("FATAL: config mutex creation failed - restarting");
        delay(1000);
        ESP.restart();
    }

    Serial.println("\n================================");
    Serial.println("  ESP32 Casambi Controller");
    Serial.println("================================\n");

    // Initialize hardware watchdog timer
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);  // true = panic on timeout
    esp_task_wdt_add(NULL);  // Add current task (loopTask) to WDT
    Serial.printf("Watchdog timer: %d seconds\n", WDT_TIMEOUT_SECONDS);

    // Log initial heap
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    initHeapMonitor(ESP.getFreeHeap());

    // Initialize filesystem
    if (!ConfigStore::init()) {
        Serial.println("ERROR: Failed to initialize storage");
        return;
    }

    // Initialize the non-volatile event log. This flushes any RTC "last words"
    // from a previous crash into LittleFS and records this boot's reset reason.
    EventLog::begin();

    // Register before any connect path (boot AND the later 'wifi set' path) so
    // every STA disconnect is captured with its IDF reason code.
    wifiInstallEventHandler();

    // Validate the AES-CMAC implementation against the RFC 4493 test vectors
    // once at boot. A failure here means the BLE crypto cannot be trusted.
    if (CasambiEncryption::selfTestRFC4493()) {
        Serial.println("CMAC self-test: PASS (RFC 4493 vectors)");
    } else {
        Serial.println("CMAC self-test: FAIL — BLE crypto is broken!");
        EventLog::log(LOG_ERROR, "CMAC self-test failed (RFC 4493)");
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

            checkCasambiVersions(networkConfig);

            // Load debug settings. The values parsed from the main config are
            // the fallback for installations predating the split; once the
            // device has written /debug_flags.json that file wins.
            ConfigStore::loadDebugFlags(networkConfig);
            bleDebugEnabled     = networkConfig.bleDebugEnabled;
            casambiDebugEnabled = networkConfig.casambiDebugEnabled;
            webDebugEnabled     = networkConfig.webDebugEnabled;
            parseDebugEnabled   = networkConfig.parseDebugEnabled;
            heapDebugEnabled    = networkConfig.heapDebugEnabled;
            cloudDebugEnabled   = networkConfig.cloudDebugEnabled;

            // A refresh scheduled via the serial command or POST /api/refreshCasambi
            // runs here, before BLE/web are up, on a clean heap with no concurrent
            // tasks. Clear the marker first so a failed download cannot loop. On
            // success runScheduledCloudRefresh() reboots; on failure it returns and
            // we continue normally with the existing config.
            if (ConfigStore::isRefreshPending()) {
                ConfigStore::clearRefreshPending();
                runScheduledCloudRefresh();
            }

            // Initialize BLE first (before WiFi for proper coexistence)
            NimBLEDevice::init(DEVICE_NAME);
            // Confirm the stack actually came up, and with which identity — a
            // controller that failed to start is otherwise only noticeable
            // through connect attempts that all fail for no stated reason.
            Serial.printf("BLE: stack initialized, own address %s, target gateway %s\n",
                          NimBLEDevice::getAddress().toString().c_str(),
                          networkConfig.autoConnectAddress.length()
                              ? networkConfig.autoConnectAddress.c_str() : "(none configured)");

            // Initialize BLE client
            casambiClient = new CasambiClient(&networkConfig);

            // Set up connection state callback for auto-reconnect and WebSocket push
            casambiClient->setConnectionStateCallback(
                [](ConnectionState newState, DisconnectReason reason) {
                    if (newState == ConnectionState::None &&
                        reason != DisconnectReason::UserRequested) {
                        Serial.printf("*** BLE connection lost (reason=%d/%s, source=%s, phase=%s) "
                                      "- will auto-reconnect ***\n",
                                      static_cast<int>(reason), disconnectReasonName(reason),
                                      casambiClient->getLastDisconnectSource(),
                                      casambiClient->getLastConnectPhase());
                        // Auth/key-exchange failures are errors; a plain link
                        // loss is a warning. Include which detector fired and
                        // the last known link RSSI so drops can be attributed
                        // (weak signal vs. sudden loss) from the log alone.
                        bool authIssue = (reason == DisconnectReason::AuthFailed ||
                                          reason == DisconnectReason::KeyExchangeFailed);
                        EventLog::log(authIssue ? LOG_ERROR : LOG_WARN,
                                      "BLE connection lost (reason=%s, src=%s, phase=%s, rssi=%d)",
                                      disconnectReasonName(reason),
                                      casambiClient->getLastDisconnectSource(),
                                      casambiClient->getLastConnectPhase(),
                                      casambiClient->getLastRssi());
                        bleNoteLinkLost();
                    }
                    if (webServer) {
                        bool connected = (newState == ConnectionState::Authenticated);
                        webServer->broadcastConnectionState(connected,
                                                            static_cast<int>(reason));
                    }
                }
            );

            // Set up unit state callback – log and push via WebSocket
            casambiClient->setUnitStateCallback(
                [](uint8_t unitId, uint8_t level, bool online, bool on) {
                    if (casambiDebugEnabled) {
                        Serial.printf("CALLBACK: Unit %d -> level=%d online=%d on=%d\n",
                                      unitId, level, online, on);
                    }
                    if (webServer) {
                        webServer->broadcastUnitState(unitId, level, online, on);
                    }
                }
            );

            // Set up 0x07 INVOCATION event callbacks — DEBUG LOGGING ONLY, no
            // WebSocket/REST exposure. 0x07 has never been observed on this
            // network (see packet_parse.h's parseInvocationStream doc
            // comment); this is diagnostic visibility only, pending a real
            // capture, same spirit as the previous "0x07 diagnostic" logging.
            casambiClient->setInputEventCallback(
                [](const invocation_events::CasambiInputEvent& ev) {
                    if (!casambiDebugEnabled) return;
                    Serial.printf("CALLBACK: 0x07 input event unit=%d index=%d label=%d type=%d",
                                  ev.unitId, ev.index, ev.label, static_cast<int>(ev.type));
                    if (ev.isButtonStream) {
                        Serial.printf(" pressed=%d p=%d s=%d", ev.pressed, ev.p, ev.s);
                    } else {
                        Serial.printf(" code=0x%02x channel=%d", ev.inputCode, ev.channel);
                        if (ev.hasValue16) Serial.printf(" value16=%d", ev.value16);
                    }
                    Serial.println();
                }
            );
            casambiClient->setRawInvocationCallback(
                [](const InvocationFrame& frame) {
                    if (!casambiDebugEnabled) return;
                    Serial.printf("CALLBACK: 0x07 raw frame op=%d target=0x%04x origin=%d age=%d payload=%d bytes\n",
                                  frame.opcode, frame.target, frame.origin, frame.age, frame.payloadLen);
                }
            );

            // Auto-connect if enabled
            if (networkConfig.autoConnectEnabled && networkConfig.autoConnectAddress.length() > 0) {
                Serial.printf("Auto-connecting to %s...\n", networkConfig.autoConnectAddress.c_str());
                if (casambiClient->connect(networkConfig.autoConnectAddress)) {
                    Serial.println("Auto-connect successful!");
                    bleNoteConnected();
                } else {
                    Serial.println("Auto-connect failed. Will retry automatically.");
                    bleNoteConnectAttempt();
                }
            }

            // Connect to WiFi after BLE is initialized
            if (wifiLoadCachedCredentials()) {
                const WiFiCredentials& wifiCreds = wifiCachedCredentials();
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
                    wifiNoteConnected();
                    syncTime();  // start NTP (UTC)
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
                startMDNS();
            }

            Serial.println("\nReady. Type 'help' for commands.\n");
        } else {
            // hasValidConfig() said a config existed, but loading it failed
            // (corrupt beyond both live and backup copies). Do NOT dead-end in a
            // half-initialised operation mode — fall through to the setup portal
            // so the device stays recoverable, and record why.
            Serial.println("ERROR: Failed to load configuration - entering setup mode");
            EventLog::log(LOG_ERROR, "Config load failed; falling back to setup portal");
            setupPortal = new SetupPortal();
            setupPortal->begin();
            apiClient = new CasambiAPIClient();
            Serial.println("(Serial fallback: type 'setup' to use the wizard instead.)\n");
        }
    } else {
        Serial.println("No configuration found - entering setup mode");

        // Primary path: open SoftAP + captive portal for browser-based setup.
        setupPortal = new SetupPortal();
        setupPortal->begin();

        // Serial wizard remains available as a fallback ('setup' command).
        apiClient = new CasambiAPIClient();
        Serial.println("(Serial fallback: type 'setup' to use the wizard instead.)\n");
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

    // === Setup mode: drive the provisioning portal ===
    if (setupPortal) {
        setupPortal->loop();
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

    // Detect first successful NTP sync and log it with the real wall-clock time.
    if (!timeSynced() && WiFi.status() == WL_CONNECTED) {
        time_t nowSec = time(nullptr);
        if (nowSec >= 1577836800) {  // >= 2020-01-01 → NTP has set the clock
            setTimeSynced(true);
            char ts[32];
            struct tm tmUtc;
            gmtime_r(&nowSec, &tmUtc);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmUtc);
            EventLog::log(LOG_INFO, "Time synced via NTP: %s UTC", ts);
        }
    }

    // Monitor heap usage
    monitorHeap();

    // Persist event-log entries that other tasks (BLE host, async_tcp) queued
    // in RTC RAM — those tasks must not block on LittleFS writes themselves.
    EventLog::flush();

    // WebSocket housekeeping (clean up disconnected clients)
    if (webServer) {
        webServer->loop();

        // NTP server change submitted via POST /api/ntp. Applied here so the
        // config String is only ever mutated on the loop task and the LittleFS
        // save never blocks the async_tcp task.
        String newNtpServer;
        if (webServer->consumeNtpRequest(newNtpServer)) {
            configLock();
            networkConfig.ntpServer = newNtpServer;
            configUnlock();
            ConfigStore::saveNetworkConfig(networkConfig);
            setTimeSynced(false);
            syncTime();  // re-arm SNTP with the new server
            EventLog::log(LOG_INFO, "NTP server changed to %s", newNtpServer.c_str());
        }

        // FHEM-triggered cloud-config refresh (POST /api/refreshCasambi). Handle
        // it here in the loop task — never from the async web-server context. It
        // only schedules the refresh and reboots; the actual download runs early
        // at the next boot (race-free). Uses the stored password.
        if (webServer->consumeRefreshRequest()) {
            Serial.println("\n*** Cloud refresh requested via API ***");
            requestCloudRefresh(networkConfig.casambiPassword);  // never returns
        }

        // Event-log wipe requested via DELETE /api/log. Run the blocking
        // EventLog::clear() (mutex wait + multiple LittleFS deletions + GC) here
        // on the loop task, never in the async_tcp callback.
        if (webServer->consumeClearLogRequest()) {
            EventLog::clear();
            EventLog::log(LOG_INFO, "Event log cleared via API");
        }

        // Reboot requested via POST /api/reboot. Carried out here (not in the
        // async handler) so the HTTP response is flushed first. The short delay
        // gives the TCP task time to send the queued 200 before the reset.
        if (webServer->consumeRebootRequest()) {
            Serial.println("\n*** Reboot requested via API ***");
            delay(250);
            ESP.restart();
        }
    }

    delay(10);
}

