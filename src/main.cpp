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
#include <ESPmDNS.h>

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
static void configLock()   { if (g_configMutex) xSemaphoreTake(g_configMutex, portMAX_DELAY); }
static void configUnlock() { if (g_configMutex) xSemaphoreGive(g_configMutex); }
bool bleDebugEnabled     = false;
bool casambiDebugEnabled = true;
bool webDebugEnabled     = true;
bool parseDebugEnabled   = false;
bool heapDebugEnabled    = false;
bool cloudDebugEnabled   = false;

// Cached WiFi credentials — loaded once at boot and updated by 'wifi set'.
// Avoids repeated LittleFS reads in the 30 s reconnect loop.
static WiFiCredentials g_wifiCreds;
static bool g_wifiCredsLoaded = false;

// BLE scan state
struct ScannedDevice {
    String address;
    String name;
    int rssi;
    // Advertised address type. The reconnect path always connects as "public",
    // so a device listed here as "random" can be discovered but never reached.
    uint8_t addrType;

    ScannedDevice() : rssi(0), addrType(0) {}
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

// millis() when the BLE link was lost (0 = not currently lost). Set by the
// connection-state callback, cleared by the reconnect success log.
static unsigned long bleLostAt = 0;

// Throttle for the "why is nobody reconnecting" trace below. A gateway that is
// disconnected because auto-connect is off (or has no stored MAC) used to
// produce NO output whatsoever — the most confusing failure mode of all, since
// it looks exactly like a firmware that is trying and silently failing.
static unsigned long lastBLEIdleNotice = 0;
static const unsigned long BLE_IDLE_NOTICE_INTERVAL_MS = 60000;
// Ensures the reason for a permanently idle BLE stack reaches the persistent
// event log once per boot, not just the serial console nobody was attached to.
static bool bleIdleReasonLogged = false;

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
void requestCloudRefresh(const String& password);
void runScheduledCloudRefresh();
void checkAndReconnectBLE();
void checkAndReconnectWiFi();
void monitorHeap();
void printStatus();
void printBLEDiagnostics();
void checkCasambiVersions(const NetworkConfig& cfg);
void syncTime();
void startMDNS();

// Short, stable per-device suffix (the LAST two MAC octets) used for the mDNS
// hostname and the setup-AP SSID so several gateways stay distinguishable.
// ESP.getEfuseMac() stores MAC octet 0 (the vendor OUI) in the LOWEST byte, so
// `mac & 0xFFFF` would yield the OUI — identical across boards of a batch. The
// device-specific tail lives in bits 32..47. (Keep in sync with the copy in
// setup_portal.cpp.)
static String deviceSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[5];
    sprintf(buf, "%02x%02x",
            (unsigned)((mac >> 32) & 0xFF),    // mac[4]
            (unsigned)((mac >> 40) & 0xFF));   // mac[5], last octet
    return String(buf);
}

// True once MDNS.begin() succeeded, so the recovery paths below can call
// startMDNS() unconditionally without re-registering the responder.
static bool g_mdnsStarted = false;

// Advertise the configured gateway as casambi-XXXX.local so FHEM can find it.
// Idempotent: called from the boot path and from every WiFi/webserver recovery
// path, because a device that boots before the router is up would otherwise
// never become discoverable. ESPmDNS re-announces on later IP changes itself.
void startMDNS() {
    if (g_mdnsStarted) return;
    String host = "casambi-" + deviceSuffix();
    if (MDNS.begin(host.c_str())) {
        g_mdnsStarted = true;
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "configured", "1");
        MDNS.addServiceTxt("http", "tcp", "build", String(FIRMWARE_BUILD));
        MDNS.addServiceTxt("http", "tcp", "network", networkConfig.networkName);
        Serial.printf("mDNS: http://%s.local/\n", host.c_str());
    } else {
        Serial.println("mDNS: failed to start (will retry on next WiFi recovery)");
    }
}

// Tracks whether NTP has reported a valid wall-clock time yet.
static bool g_timeSynced = false;
// True while WiFi is known-up, so we only log the WiFi-loss event once per drop.
static bool g_wifiWasConnected = false;

// ============================================================================
// WIFI DISCONNECT DIAGNOSTICS
// ============================================================================

// WiFi.status() only says THAT the link is down; WHY is delivered exclusively
// in the STA_DISCONNECTED event (IDF wifi_err_reason_t) and would otherwise be
// lost. The reason separates the three broad causes that need different fixes:
// AP kicked us (ASSOC_LEAVE/AUTH_EXPIRE — steering, reboot, WLAN schedule),
// radio problems (BEACON_TIMEOUT/NO_AP_FOUND — signal, interference, channel
// change) and credential trouble (AUTH_FAIL/HANDSHAKE_TIMEOUT).

// Last RSSI sampled while the link was up (0 = never sampled). Written on
// loopTask / event task, read on the WiFi event task at disconnect time.
static std::atomic<int8_t> g_lastWifiRssi{0};

// Reason of the current disconnect episode, 0 = link is up. The IDF retries
// every few seconds during an outage and fires STA_DISCONNECTED on every
// failed attempt; this deduplicates the flash log to one entry per episode
// (plus one per reason change).
static std::atomic<uint8_t> g_lastWifiDiscReason{0};

// millis() at the first disconnect event of the current episode. The 30 s poll
// in checkAndReconnectWiFi() quantizes its log entries to the check grid; the
// event pair DISCONNECTED→GOT_IP measures the true outage duration.
static std::atomic<uint32_t> g_wifiLostAtMs{0};

static const char* wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED:            return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE:            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:             return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE:           return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY:          return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED:             return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED:            return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE:            return "ASSOC_LEAVE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:         return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:            return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:              return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:             return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:        return "CONNECTION_FAIL";
        default:                                 return "unknown";
    }
}

// Runs on the WiFi event task, not loopTask. EventLog::log() is task-safe and
// writes only RTC RAM from foreign tasks (flash persistence happens later via
// flush() on loopTask), so no flash I/O blocks the event task here.
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            if (g_lastWifiDiscReason == 0) g_wifiLostAtMs = millis();
            if (reason != g_lastWifiDiscReason) {
                g_lastWifiDiscReason = reason;
                if (g_lastWifiRssi != 0) {
                    EventLog::log(LOG_WARN, "WiFi disconnect: reason=%u (%s), last rssi=%d dBm",
                                  (unsigned)reason, wifiDisconnectReasonName(reason),
                                  (int)g_lastWifiRssi);
                } else {
                    EventLog::log(LOG_WARN, "WiFi disconnect: reason=%u (%s)",
                                  (unsigned)reason, wifiDisconnectReasonName(reason));
                }
            }
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            // Only after a disconnect episode — GOT_IP also fires on the boot
            // connect and on DHCP lease renewals.
            if (g_lastWifiDiscReason != 0) {
                uint32_t outageMs = millis() - g_wifiLostAtMs;
                EventLog::log(LOG_INFO, "WiFi got IP after %lu.%lus offline",
                              (unsigned long)(outageMs / 1000),
                              (unsigned long)((outageMs % 1000) / 100));
            }
            g_lastWifiDiscReason = 0;
            g_lastWifiRssi = WiFi.RSSI();
            break;
        default:
            break;
    }
}

// ============================================================================
// TIME SYNCHRONISATION (NTP, UTC)
// ============================================================================

// True for RFC 1918 private IPv4 addresses (typical home-LAN router/DNS).
static bool isPrivateIPv4(const IPAddress& ip) {
    uint8_t a = ip[0], b = ip[1];
    return (a == 10) ||
           (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168);
}

// Candidate order of the last syncTime() call, for 'ntp status'.
static String g_ntpCandidates;

// Kick off SNTP. Non-blocking: the first call from setup() starts the query;
// the loop re-checks and logs once time is valid.
//
// Local-first: while the configured server is still the untouched default
// (pool.ntp.org), the DHCP-provided DNS server and the gateway — in home
// networks usually the router, which typically serves NTP — are tried first.
// lwIP-SNTP is built with SNTP_MAX_SERVERS=3 and cycles to the next candidate
// on timeout, so a router without NTP only delays the first sync by a few
// seconds, while a network without internet still gets valid time from the
// router. An explicitly configured server ('ntp set' / POST /api/ntp) always
// takes precedence alone — no auto-detection in that case.
void syncTime() {
    // lwIP's sntp_setservername() stores the passed POINTER, not a copy, so
    // every string handed to configTime() must stay valid for the lifetime of
    // the SNTP session. Static buffers make that unconditional (the heap
    // buffer of networkConfig.ntpServer can move when the setting changes).
    static char cfgServer[65];
    static char dnsServer[16];
    static char gwServer[16];

    String cfg = networkConfig.ntpServer.length() > 0
                     ? networkConfig.ntpServer
                     : String(NTP_SERVER_DEFAULT);
    strlcpy(cfgServer, cfg.c_str(), sizeof(cfgServer));

    const char* localDns = nullptr;
    const char* localGw  = nullptr;
    if (cfg == NTP_SERVER_DEFAULT) {
        IPAddress dns = WiFi.dnsIP();
        IPAddress gw  = WiFi.gatewayIP();
        if (isPrivateIPv4(dns)) {
            strlcpy(dnsServer, dns.toString().c_str(), sizeof(dnsServer));
            localDns = dnsServer;
        }
        if (isPrivateIPv4(gw) && !(gw == dns)) {
            strlcpy(gwServer, gw.toString().c_str(), sizeof(gwServer));
            localGw = gwServer;
        }
    }

    if (localDns && localGw) {
        configTime(0, 0, localDns, localGw, cfgServer);  // UTC (no offset, no DST)
        g_ntpCandidates = String(localDns) + " (DNS), " + localGw + " (gateway), " + cfgServer;
    } else if (localDns) {
        configTime(0, 0, localDns, cfgServer);
        g_ntpCandidates = String(localDns) + " (DNS), " + cfgServer;
    } else if (localGw) {
        configTime(0, 0, localGw, cfgServer);
        g_ntpCandidates = String(localGw) + " (gateway), " + cfgServer;
    } else {
        configTime(0, 0, cfgServer);
        g_ntpCandidates = cfgServer;
    }
    Serial.printf("NTP: time sync requested, server order: %s (UTC)\n",
                  g_ntpCandidates.c_str());
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
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
    minFreeHeap = ESP.getFreeHeap();

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
    WiFi.onEvent(onWiFiEvent);

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

            // Load debug settings
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
                        if (bleLostAt == 0) bleLostAt = millis();
                        bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
                        lastBLEReconnectAttempt = millis();
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
                [](uint8_t unitId, uint8_t level, bool online) {
                    if (casambiDebugEnabled) {
                        Serial.printf("CALLBACK: Unit %d -> level=%d online=%d\n",
                                      unitId, level, online);
                    }
                    if (webServer) {
                        webServer->broadcastUnitState(unitId, level, online);
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
                g_wifiCreds = wifiCreds;
                g_wifiCredsLoaded = true;
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
                    g_wifiWasConnected = true;
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
    if (!g_timeSynced && WiFi.status() == WL_CONNECTED) {
        time_t nowSec = time(nullptr);
        if (nowSec >= 1577836800) {  // >= 2020-01-01 → NTP has set the clock
            g_timeSynced = true;
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
            g_timeSynced = false;
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

// ============================================================================
// BLE AUTO-RECONNECT
// ============================================================================

// Explain, at most once a minute, why the link is down and nothing is being
// done about it. `reason` is a static string.
static void noteBLEIdle(const char* reason, const char* remedy) {
    unsigned long now = millis();
    if (lastBLEIdleNotice != 0 && now - lastBLEIdleNotice < BLE_IDLE_NOTICE_INTERVAL_MS) return;
    lastBLEIdleNotice = now;

    Serial.printf("BLE: not connected and NOT attempting to reconnect - %s (%s)\n", reason, remedy);
    if (!bleIdleReasonLogged) {
        bleIdleReasonLogged = true;
        EventLog::log(LOG_WARN, "BLE idle, no reconnect: %s", reason);
    }
}

void checkAndReconnectBLE() {
    if (!casambiClient) return;

    if (casambiClient->isAuthenticated()) {
        // Link is up: re-arm the notice so a later outage explains itself again.
        lastBLEIdleNotice = 0;
        return;
    }

    if (!bleReconnectEnabled) {
        noteBLEIdle("auto-reconnect is disabled", "enable with 'reconnect on'");
        return;
    }

    // Only reconnect if we have an auto-connect address
    if (!networkConfig.autoConnectEnabled) {
        noteBLEIdle("auto-connect is disabled", "enable with 'autoconnect on'");
        return;
    }
    if (networkConfig.autoConnectAddress.length() == 0) {
        noteBLEIdle("no gateway MAC stored",
                    "run 'scan' then 'connect <n>', or 'autoconnect set <mac>'");
        return;
    }

    unsigned long now = millis();
    if (now - lastBLEReconnectAttempt < bleReconnectInterval) return;

    lastBLEReconnectAttempt = now;

    Serial.printf("BLE: Auto-reconnect attempt #%d to %s (backoff: %lu ms, offline %lus, heap=%u)...\n",
                  consecutiveReconnectFailures + 1,
                  networkConfig.autoConnectAddress.c_str(),
                  bleReconnectInterval,
                  bleLostAt ? (millis() - bleLostAt) / 1000 : 0,
                  ESP.getFreeHeap());

    if (casambiClient->connect(networkConfig.autoConnectAddress)) {
        Serial.println("BLE: Reconnect successful!");
        // Record the recovery: attempts needed and how long the link was down.
        // Together with the loss entry this shows outage windows in the log.
        unsigned long offlineSecs = bleLostAt ? (millis() - bleLostAt) / 1000 : 0;
        EventLog::log(LOG_INFO, "BLE reconnected (attempt %u, offline %lus, rssi=%d)",
                      (unsigned)(consecutiveReconnectFailures + 1), offlineSecs,
                      casambiClient->getLastRssi());
        bleLostAt = 0;
        consecutiveReconnectFailures = 0;
        bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;  // Reset backoff

        // Restart web server if needed
        if (WiFi.status() == WL_CONNECTED && !webServer) {
            webServer = new CasambiWebServer(casambiClient, &networkConfig);
            if (webServer->begin()) {
                Serial.printf("Web API restarted at: http://%s/api\n",
                              WiFi.localIP().toString().c_str());
            }
            startMDNS();
        }
    } else {
        if (consecutiveReconnectFailures < 0xFF) consecutiveReconnectFailures++;

        // Exponential backoff (double interval, up to max)
        bleReconnectInterval = min(bleReconnectInterval * 2, (unsigned long)BLE_RECONNECT_MAX_BACKOFF_MS);

        // Classify the failure: a plain link loss / connect timeout means the
        // peer is absent (e.g. lights cut by a wall switch) — rebooting cannot
        // fix that, so we keep retrying at max backoff indefinitely. Only
        // repeated INTERNAL failures (auth, key exchange, GATT structure),
        // where a fresh BLE stack can actually help, restart the ESP32.
        DisconnectReason lastReason = casambiClient->getLastDisconnectReason();
        bool internalFailure = (lastReason == DisconnectReason::AuthFailed ||
                                lastReason == DisconnectReason::KeyExchangeFailed ||
                                lastReason == DisconnectReason::InternalError);
        static uint8_t internalFailureStreak = 0;
        internalFailureStreak = internalFailure ? internalFailureStreak + 1 : 0;

        // The phase says where it broke, the rc says why the radio said no.
        // "peer unreachable" alone sent people hunting for range problems when
        // the real fault was one phase further in (see issue #42).
        Serial.printf("BLE: Reconnect failed (#%d, %s, phase=%s, reason=%d/%s, rc=%d). "
                      "Next attempt in %lu ms\n",
                      consecutiveReconnectFailures,
                      internalFailure ? "internal error" : "peer unreachable",
                      casambiClient->getLastConnectPhase(),
                      static_cast<int>(lastReason), disconnectReasonName(lastReason),
                      casambiClient->getLastConnectError(),
                      bleReconnectInterval);

        // First failure of an outage goes to the persistent log as well: the
        // serial console is usually not attached when a link dies at night.
        if (consecutiveReconnectFailures == 1) {
            EventLog::log(LOG_WARN, "BLE connect failed: phase=%s reason=%s rc=%d",
                          casambiClient->getLastConnectPhase(),
                          disconnectReasonName(lastReason),
                          casambiClient->getLastConnectError());
        }

        if (internalFailureStreak >= MAX_RECONNECT_FAILURES) {
            Serial.println("*** Too many internal BLE failures! Restarting ESP32 ***");
            EventLog::log(LOG_CRITICAL, "Restart: %d internal BLE failures (phase=%s, reason=%s)",
                          internalFailureStreak, casambiClient->getLastConnectPhase(),
                          disconnectReasonName(lastReason));
            delay(1000);
            ESP.restart();
        }

        // Record the onset of a longer outage once, then stay quiet in the log.
        if (consecutiveReconnectFailures == MAX_RECONNECT_FAILURES && !internalFailure) {
            EventLog::log(LOG_WARN,
                          "BLE peer unreachable after %d attempts; retrying at %lus backoff (no restart)",
                          consecutiveReconnectFailures, bleReconnectInterval / 1000);
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

    if (WiFi.status() == WL_CONNECTED) {
        // Keep a fresh signal reading around so the disconnect event handler
        // can report the last strength BEFORE the drop (afterwards RSSI is
        // unreadable). Granularity is one reading per check interval.
        g_lastWifiRssi = WiFi.RSSI();

        // Handle the disconnected → connected transition exactly once, so NTP
        // re-arm and the (defensive) web-server restart run only on a real
        // recovery, not on every check while connected.
        if (!g_wifiWasConnected) {
            g_wifiWasConnected = true;
            Serial.printf("WiFi: Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
            if (heapDebugEnabled) Serial.println("WiFiRC: <- reconnected");
            EventLog::log(LOG_INFO, "WiFi reconnected (SSID %s)", WiFi.SSID().c_str());
            syncTime();  // re-arm NTP after reconnect

            // Restart web server only if it was torn down (defensive — it is
            // normally kept alive across a WiFi drop).
            if (casambiClient && !webServer) {
                webServer = new CasambiWebServer(casambiClient, &networkConfig);
                if (webServer->begin()) {
                    Serial.printf("Web API restarted at: http://%s/api\n",
                                  WiFi.localIP().toString().c_str());
                }
            }

            // Covers the boot-before-router case: without this, a device whose
            // WiFi only came up after setup() would never be discoverable.
            if (casambiClient) startMDNS();
        }
        return;
    }

    // WiFi is disconnected — use cached credentials (avoids repeated LittleFS reads)
    if (!g_wifiCredsLoaded) return;

    // Log the drop once per disconnect episode.
    if (g_wifiWasConnected) {
        EventLog::log(LOG_WARN, "WiFi connection lost (SSID %s)", g_wifiCreds.ssid.c_str());
        g_wifiWasConnected = false;
    }

    // NON-BLOCKING reconnect (issue #18, part B).
    // The old path blocked loopTask in WiFi.disconnect()+begin()+a 5 s wait
    // loop. Under heap distress WiFi.begin() itself stalled for the full 30 s
    // without returning, so loopTask stopped feeding the task watchdog → WDT
    // reboot. We now never block here:
    //   * WiFi.setAutoReconnect(true) (set at first connect) already makes the
    //     IDF WiFi task retry on its own — that is the primary recovery path.
    //   * We only give it a periodic nudge via WiFi.reconnect(), which reuses
    //     the stored config (lighter than begin()) and returns immediately;
    //     the new status is observed on a later tick, not awaited here.
    // The WiFiRC: checkpoints are kept (gated behind 'debug heap on', like WSDBG)
    // so a future stall (if any) is still localizable from the last line printed
    // before a reboot. The esp_task_wdt_reset() calls stay UNCONDITIONAL — they
    // feed the watchdog and are functional, not diagnostics.
    esp_task_wdt_reset();
    if (heapDebugEnabled) Serial.println("WiFiRC: -> reconnect() [non-blocking]");
    WiFi.reconnect();
    esp_task_wdt_reset();
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

    // Critical heap handling with debounce: a single low reading is usually a
    // transient spike (web/WS TCP buffers, a BLE reconnect) that recovers on its
    // own. Only restart after HEAP_CRITICAL_CONSECUTIVE sustained low readings.
    static uint8_t lowHeapStreak = 0;
    if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
        lowHeapStreak++;
        Serial.printf("*** Low heap %d < %d (%u/%u) ***\n",
                      freeHeap, HEAP_CRITICAL_THRESHOLD,
                      lowHeapStreak, HEAP_CRITICAL_CONSECUTIVE);
        // Record the onset of a low-heap episode once, for post-mortem analysis.
        if (lowHeapStreak == 1) {
            EventLog::log(LOG_WARN, "Low heap %u < %u (transient?)",
                          (unsigned)freeHeap, (unsigned)HEAP_CRITICAL_THRESHOLD);
        }
        if (lowHeapStreak >= HEAP_CRITICAL_CONSECUTIVE) {
            Serial.println("*** Sustained low heap - restarting ESP32 ***");
            EventLog::log(LOG_CRITICAL, "Restart: low heap %u < %u bytes (%u readings)",
                          (unsigned)freeHeap, (unsigned)HEAP_CRITICAL_THRESHOLD,
                          lowHeapStreak);
            delay(1000);
            ESP.restart();
        }
    } else {
        lowHeapStreak = 0;   // recovered → reset the episode
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
            Serial.printf("  RSSI: %d dBm\n", casambiClient->getLastRssi());
        }

        // Parser counters. "partial" = the understood prefix was applied and
        // an undecoded tail dropped (likely a protocol element the reverse-
        // engineering does not cover yet — worth a look when it grows);
        // "malformed" = the packet yielded nothing usable.
        const PacketParseStats& ps = packetParseStats();
        if (ps.partial06.load() || ps.partial07.load() || ps.partial08.load()) {
            Serial.printf("  Partially decoded packets: 0x06=%u 0x07=%u 0x08=%u\n",
                          ps.partial06.load(), ps.partial07.load(), ps.partial08.load());
        }
        if (ps.malformed06.load() || ps.malformed07.load() || ps.malformed08.load()) {
            Serial.printf("  Malformed packets dropped: 0x06=%u 0x07=%u 0x08=%u\n",
                          ps.malformed06.load(), ps.malformed07.load(), ps.malformed08.load());
        }

        if (casambiClient->getLastDisconnectReason() != DisconnectReason::None) {
            Serial.printf("  Last disconnect: reason=%d/%s, source=%s\n",
                          static_cast<int>(casambiClient->getLastDisconnectReason()),
                          disconnectReasonName(casambiClient->getLastDisconnectReason()),
                          casambiClient->getLastDisconnectSource());
        }
        if (!casambiClient->isAuthenticated()) {
            // Where the last attempt broke is the single most useful number
            // when the link never comes up ("link" = never reached the peer,
            // anything else = we talked to it and the handshake failed).
            Serial.printf("  Last connect phase: %s (rc=%d)\n",
                          casambiClient->getLastConnectPhase(),
                          casambiClient->getLastConnectError());
            Serial.printf("  Auto-connect: %s, MAC: %s\n",
                          networkConfig.autoConnectEnabled ? "enabled" : "disabled",
                          networkConfig.autoConnectAddress.length()
                              ? networkConfig.autoConnectAddress.c_str() : "(none)");
            Serial.println("  Run 'blediag' for a full BLE diagnostic report");
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
// BLE DIAGNOSTIC REPORT
// ============================================================================

// One command that answers "why is BLE not connecting?" in a form that can be
// pasted into a bug report: config, client state, the phase the last attempt
// died in, and a live picture of what is actually advertising nearby. Written
// for issue #42, where a device was discovered by setup but never connected
// and the existing traces said nothing at all.
void printBLEDiagnostics() {
    Serial.println("\n=== BLE Diagnostics ===");
    Serial.printf("Firmware build: %d, uptime: %lus, heap: free=%u min=%u largest=%u\n",
                  FIRMWARE_BUILD, millis() / 1000,
                  ESP.getFreeHeap(), minFreeHeap, ESP.getMaxAllocHeap());
    Serial.printf("Chip: %s rev %d, %d core(s)\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());

    // --- Configuration -----------------------------------------------------
    Serial.println("\n-- Configuration --");
    Serial.printf("Network: '%s' (uuid=%s)\n",
                  networkConfig.networkName.c_str(), networkConfig.networkUuid.c_str());
    Serial.printf("Protocol version: %d (minimum %d)\n",
                  networkConfig.protocolVersion, MIN_PROTOCOL_VERSION);
    Serial.printf("Units: %d, groups: %d, scenes: %d\n",
                  (int)networkConfig.units.size(), (int)networkConfig.groups.size(),
                  (int)networkConfig.scenes.size());
    Serial.printf("Keys: %d\n", (int)networkConfig.keys.size());
    for (const auto& k : networkConfig.keys) {
        // Metadata only — never the key material itself.
        Serial.printf("  key id=%d type=%d role=%d name='%s'\n",
                      k.id, k.type, k.role, k.name.c_str());
    }
    if (networkConfig.keys.empty()) {
        Serial.println("  *** No keys: the gateway will connect but never authenticate "
                       "(re-run 'setup' or 'refresh') ***");
    }
    Serial.printf("Auto-connect: %s, MAC: %s\n",
                  networkConfig.autoConnectEnabled ? "enabled" : "disabled",
                  networkConfig.autoConnectAddress.length()
                      ? networkConfig.autoConnectAddress.c_str() : "(none)");
    Serial.printf("Auto-reconnect: %s, failures: %d, next backoff: %lu ms\n",
                  bleReconnectEnabled ? "enabled" : "disabled",
                  consecutiveReconnectFailures, bleReconnectInterval);
    Serial.printf("Debug flags: ble=%s casambi=%s\n",
                  bleDebugEnabled ? "on" : "off", casambiDebugEnabled ? "on" : "off");

    // --- Client state ------------------------------------------------------
    Serial.println("\n-- Link --");
    if (!casambiClient) {
        Serial.println("No BLE client (setup mode)");
        Serial.println();
        return;
    }

    Serial.printf("State: %d (%s)\n", static_cast<int>(casambiClient->getState()),
                  casambiClient->isAuthenticated() ? "authenticated"
                      : (casambiClient->getState() == ConnectionState::None ? "disconnected"
                                                                            : "connecting"));
    Serial.printf("Last connect phase: %s (NimBLE rc=%d)\n",
                  casambiClient->getLastConnectPhase(), casambiClient->getLastConnectError());
    Serial.printf("Last disconnect: reason=%d/%s, source=%s\n",
                  static_cast<int>(casambiClient->getLastDisconnectReason()),
                  disconnectReasonName(casambiClient->getLastDisconnectReason()),
                  casambiClient->getLastDisconnectSource());
    Serial.printf("Gateway: %s, rssi=%d dBm (accept threshold %d dBm)\n",
                  casambiClient->getConnectedAddress().length()
                      ? casambiClient->getConnectedAddress().c_str() : "(never connected)",
                  casambiClient->getLastRssi(), BLE_MIN_CONNECT_RSSI);
    Serial.printf("Packets received: %u, link uptime: %lus, offline for: %lus\n",
                  casambiClient->getReceivedPacketCount(),
                  casambiClient->getConnectionUptime() / 1000,
                  bleLostAt ? (millis() - bleLostAt) / 1000 : 0);

    // --- What is actually out there ----------------------------------------
    Serial.println("\n-- Advertisement probe --");
    if (casambiClient->isAuthenticated()) {
        // Scanning while connected can disturb a healthy link for no benefit.
        Serial.println("Link is up - skipping the scan.");
        Serial.println();
        return;
    }

    Serial.println("Scanning 5 s for Casambi advertisers...");
    std::vector<CasambiScanResult> seen;
    CasambiScan::run(5, seen);
    esp_task_wdt_reset();

    if (seen.empty()) {
        Serial.println("NO Casambi device is advertising.");
        Serial.println("  - are the lights powered on and in range?");
        Serial.println("  - is a phone with the Casambi app (or another gateway) already connected?");
        Serial.println("    a Casambi unit accepts only ONE central at a time");
        Serial.println();
        return;
    }

    bool targetSeen = false;
    Serial.printf("Found %d advertiser(s):\n", (int)seen.size());
    for (const auto& r : seen) {
        bool isTarget = networkConfig.autoConnectAddress.length() &&
                        r.mac.equalsIgnoreCase(networkConfig.autoConnectAddress);
        targetSeen = targetSeen || isTarget;
        Serial.printf("  %s type=%s rssi=%d name='%s'%s\n",
                      r.mac.c_str(), CasambiScan::addrTypeName(r.addrType), r.rssi,
                      r.name.c_str(), isTarget ? "  <-- configured gateway" : "");
        if (r.mfgData.length()) Serial.printf("      mfg: %s\n", r.mfgData.c_str());
        if (r.svcData.length()) Serial.printf("      svc: %s\n", r.svcData.c_str());
    }

    if (networkConfig.autoConnectAddress.length() && !targetSeen) {
        Serial.printf("\n*** Configured MAC %s is NOT among them ***\n",
                      networkConfig.autoConnectAddress.c_str());
        Serial.println("Every unit of a network advertises its own address, so a stored MAC");
        Serial.println("disappears when that particular unit is switched off. Fix with:");
        Serial.println("  scan, then connect <n>   (stores the MAC of a unit that is present)");
    }
    Serial.println();
}

// ============================================================================
// VERSION CHECKS
// ============================================================================

// Warn on boot/refresh if protocol version or any unit firmware is below minimum.
// Unit firmware format: "Evolution/48.2" — we parse the numeric part after '/'.
void checkCasambiVersions(const NetworkConfig& cfg) {
    // Check Casambi BLE protocol version
    if (cfg.protocolVersion < MIN_PROTOCOL_VERSION) {
        Serial.printf("*** WARNING: Casambi protocol v%d is below minimum v%d! ***\n",
                      cfg.protocolVersion, MIN_PROTOCOL_VERSION);
        Serial.println("*** Update Casambi firmware or check network configuration. ***");
    } else if (cfg.protocolVersion > MAX_PROTOCOL_VERSION) {
        Serial.printf("*** WARNING: Casambi protocol v%d exceeds maximum v%d — may be incompatible! ***\n",
                      cfg.protocolVersion, MAX_PROTOCOL_VERSION);
    }

    // Check unit firmware versions
    for (const auto& unit : cfg.units) {
        if (unit.firmware.isEmpty()) continue;

        // Parse "Evolution/48.2" — find '/' and take the part after it
        int slashPos = unit.firmware.indexOf('/');
        if (slashPos < 0) continue;

        float fwVersion = unit.firmware.substring(slashPos + 1).toFloat();
        if (fwVersion > 0.0f && fwVersion < MIN_UNIT_FIRMWARE_VERSION) {
            Serial.printf("*** WARNING: Unit '%s' (id=%d) firmware %.1f < minimum %.1f ***\n",
                          unit.name.c_str(), unit.deviceId,
                          fwVersion, MIN_UNIT_FIRMWARE_VERSION);
        }
    }
}

// ============================================================================
// BLE SCANNING (with memory leak fix)
// ============================================================================

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        // Check if this is a Casambi device (service UUID)
        if (advertisedDevice->haveServiceUUID()) {
            NimBLEUUID serviceUUID = advertisedDevice->getServiceUUID();
            if (serviceUUID.equals(NimBLEUUID(CASAMBI_SERVICE_UUID))) {
                ScannedDevice dev;
                dev.address = advertisedDevice->getAddress().toString().c_str();
                dev.name = advertisedDevice->haveName() ? advertisedDevice->getName().c_str() : "Unknown";
                dev.rssi = advertisedDevice->getRSSI();
                dev.addrType = advertisedDevice->getAddress().getType();

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
                    Serial.printf("[%d] %s (%s) RSSI: %d, addr type: %s\n",
                        scannedDevices.size() - 1,
                        dev.name.c_str(),
                        dev.address.c_str(),
                        dev.rssi,
                        CasambiScan::addrTypeName(dev.addrType));
                }
            }
        }
    }
};

static ScanCallbacks* scanCallbackInstance = nullptr;

// ============================================================================
// CLOUD CONFIG REFRESH
// ============================================================================

// Schedule a Casambi cloud-config refresh and reboot. The refresh itself runs
// at the next boot via runScheduledCloudRefresh(), BEFORE BLE and the async web
// server are started.
//
// Doing it that way avoids a use-after-free crash (issue #21): tearing the BLE
// stack and the AsyncWebServer down at runtime races with the async_tcp task,
// which may still be processing the disconnect of the very connection that
// triggered the refresh (e.g. the FHEM WebSocket). At boot, neither subsystem
// is up yet, so the TLS download runs on a clean, large heap with no
// concurrent tasks.
//
// Shared by the serial `refresh` command and the FHEM-triggered
// POST /api/refreshCasambi. On success it does not return — it reboots.
void requestCloudRefresh(const String& password) {
    if (password.length() == 0) {
        Serial.println("ERROR: No network password available for refresh.");
        return;
    }

    // Persist the password to use after the reboot (saved one, or one newly
    // typed at the serial prompt).
    if (password != networkConfig.casambiPassword) {
        configLock();
        networkConfig.casambiPassword = password;
        configUnlock();
        if (!ConfigStore::saveNetworkConfig(networkConfig)) {
            Serial.println("ERROR: Failed to store password for refresh");
            return;
        }
    }

    ConfigStore::setRefreshPending();
    Serial.println("Cloud refresh scheduled. Restarting to apply...");
    delay(500);
    ESP.restart();
}

// Perform a scheduled cloud-config refresh. Called from setup() when the
// refresh marker is present, before BLE/WiFi/web are initialised. `networkConfig`
// must already be loaded (for the stored password and local settings).
//
// The marker is cleared by the caller BEFORE this runs, so a failed download
// cannot cause a reboot loop — on any failure we simply return and setup()
// continues into normal operation with the existing (unchanged) config. On
// success the device reboots and comes up with the fresh config.
void runScheduledCloudRefresh() {
    Serial.println("\n=== Scheduled Cloud Refresh ===");

    const String password = networkConfig.casambiPassword;
    if (password.length() == 0) {
        Serial.println("ERROR: No stored network password; skipping refresh.");
        return;
    }

    // Bring up WiFi (nothing else is connected yet at this point in boot).
    WiFiCredentials wifiCreds;
    if (!ConfigStore::loadWiFiCredentials(wifiCreds)) {
        Serial.println("ERROR: No WiFi credentials stored; skipping refresh.");
        return;
    }
    g_wifiCreds = wifiCreds;
    g_wifiCredsLoaded = true;

    Serial.printf("Connecting to WiFi: %s...\n", wifiCreds.ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiCreds.ssid.c_str(), wifiCreds.password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(100);
        esp_task_wdt_reset();
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR: WiFi connection failed; skipping refresh.");
        WiFi.disconnect(true);
        return;
    }
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // On any failure below, drop WiFi so setup() resumes from its normal
    // "BLE first, then WiFi" ordering with the existing config untouched.

    // Get network ID from stored UUID
    Serial.println("--- Fetching network ID ---");
    String networkId;
    CasambiAPIClient tempClient;
    if (!tempClient.getNetworkId(networkConfig.networkUuid, networkId)) {
        Serial.printf("ERROR: Failed to get network ID: %s\n", tempClient.getLastError().c_str());
        WiFi.disconnect(true);
        return;
    }

    // Create session
    Serial.println("--- Creating session ---");
    String sessionToken;
    if (!tempClient.createSession(networkId, password, sessionToken)) {
        Serial.printf("ERROR: Authentication failed: %s\n", tempClient.getLastError().c_str());
        WiFi.disconnect(true);
        return;
    }

    // Fetch fresh configuration
    Serial.println("--- Downloading configuration ---");
    NetworkConfig freshConfig;
    if (!tempClient.fetchNetworkConfig(networkId, sessionToken, freshConfig)) {
        Serial.printf("ERROR: Failed to fetch config: %s\n", tempClient.getLastError().c_str());
        WiFi.disconnect(true);
        return;
    }

    // Restore network identifiers
    freshConfig.networkUuid = networkConfig.networkUuid;
    freshConfig.networkId = networkId;

    // Persist the password used for this refresh
    freshConfig.casambiPassword = password;

    // Preserve local settings that are not part of the cloud config. Kept in one
    // helper so a newly added local field cannot be forgotten here again.
    preserveLocalSettings(networkConfig, freshConfig);

    // Save updated configuration
    Serial.println("--- Saving to flash ---");
    if (!ConfigStore::saveNetworkConfig(freshConfig)) {
        Serial.println("ERROR: Failed to save configuration; keeping existing config.");
        WiFi.disconnect(true);
        return;
    }

    Serial.println("\n=== Refresh Complete! ===");
    Serial.printf("Network: %s\n", freshConfig.networkName.c_str());
    Serial.printf("Protocol: v%d (revision %d)\n", freshConfig.protocolVersion, freshConfig.revision);
    Serial.printf("Units: %d\n", freshConfig.units.size());
    Serial.printf("Groups: %d\n", freshConfig.groups.size());
    Serial.printf("Scenes: %d\n", freshConfig.scenes.size());

    checkCasambiVersions(freshConfig);

    Serial.println("\nConfiguration updated. Restarting to apply...");
    delay(2000);
    ESP.restart();
}

// ============================================================================
// STRICT SERIAL ARGUMENT PARSING
// ============================================================================
// The control commands share the same validation the REST API performs:
// strict decimal parsing (String::toInt() silently maps "300"→44, "-1"→255
// and "foo"→0 after the uint8 cast — commands then hit the WRONG target),
// explicit value ranges, and an entity lookup before anything is sent.

// Entity kinds the control commands address.
enum class SerialEntity : uint8_t { Unit, Group, Scene };

// The parsing itself lives in serial_args.h — pure and host-tested
// (test/test_serial_args); these wrappers only bind it to Arduino String.

// Parse a decimal integer strictly: the whole trimmed string must be digits
// (one optional leading '-') and the value must lie within [minV, maxV].
static bool parseSerialInt(const String& raw, long minV, long maxV, long& out) {
    return serialargs::parseInt(raw, minV, maxV, out);
}

// Split a command's argument tail into whitespace-separated tokens.
// Returns the token count, or -1 when there are more than maxTok tokens.
static int splitSerialArgs(const String& tail, String* tok, int maxTok) {
    return serialargs::splitArgs(tail, tok, maxTok);
}

// Verify the addressed entity exists in the loaded config (same rule as the
// REST API's 404) and complain on the console when it does not.
static bool serialEntityExists(SerialEntity kind, uint8_t id) {
    switch (kind) {
        case SerialEntity::Unit:
            if (networkConfig.getUnitById(id)) return true;
            Serial.printf("Unit %u not found (see 'list units')\n", id);
            return false;
        case SerialEntity::Group:
            if (networkConfig.getGroupById(id)) return true;
            Serial.printf("Group %u not found (see 'list groups')\n", id);
            return false;
        case SerialEntity::Scene:
            if (networkConfig.getSceneById(id)) return true;
            Serial.printf("Scene %u not found (see 'list scenes')\n", id);
            return false;
    }
    return false;
}

// Parse "<id>" (wantValue=false) or "<id> <value>" (wantValue=true) strictly
// and verify the entity exists. On any failure a usage/error line has been
// printed and false returns; the command must then send nothing.
static bool parseControlArgs(SerialEntity kind, const String& tail,
                             uint8_t& id, bool wantValue,
                             long valMin, long valMax, long& value,
                             const char* usage) {
    String tok[2];
    const int want = wantValue ? 2 : 1;
    if (splitSerialArgs(tail, tok, 2) != want) {
        Serial.printf("Usage: %s\n", usage);
        return false;
    }
    long idL;
    if (!parseSerialInt(tok[0], 0, 255, idL)) {
        Serial.println("Invalid id (must be 0-255)");
        return false;
    }
    if (wantValue && !parseSerialInt(tok[1], valMin, valMax, value)) {
        Serial.printf("Invalid value (must be %ld-%ld)\n", valMin, valMax);
        return false;
    }
    id = (uint8_t)idL;
    return serialEntityExists(kind, id);
}

// Report the real outcome of a BLE setter — the setters return false when
// the command was NOT transmitted (link lost, mutex timeout, GATT failure),
// and the console must not claim success then.
static void reportSerialSend(bool ok) {
    if (!ok) Serial.println("FAILED: command not transmitted (BLE send failed)");
}

// ============================================================================
// COMMAND HANDLER
// ============================================================================

void handleCommand(const String& cmd) {
	// Echo the command, but never log credentials. `wifi set <ssid> <password>`
	// carries the WiFi password as a plain argument, so mask everything after
	// the subcommand for password-bearing commands.
	if (cmd.startsWith("wifi set")) {
		Serial.println(">>> CMD: wifi set <redacted>");
	} else {
		Serial.printf(">>> CMD: %s\n", cmd.c_str());
	}
        if (cmd == "help") {
            Serial.println("\n=== Commands ===");
            Serial.println("help          - Show this help");
            Serial.println("status        - Show detailed status");
            Serial.println("blediag       - BLE troubleshooting report (config, last connect phase, scan)");
            Serial.println("refresh       - Refresh config from Casambi cloud");
            Serial.println("clearconfig   - Clear configuration (factory reset)");
            Serial.println("restart       - Restart ESP32");
            Serial.println("log [n]       - Show newest n event-log entries (default 30)");
            Serial.println("log clear     - Erase the event log");
            Serial.println("ntp status    - Show NTP server and sync state");
            Serial.println("ntp set <host|ip> - Set NTP server hostname or IP (UTC)");
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
                Serial.println("debug on/off          - Restore/suppress all debug (settings preserved)");
                Serial.println("debug ble on/off      - BLE/crypto verbose output");
                Serial.println("debug casambi on/off  - Casambi network events (units, echo)");
                Serial.println("debug web on/off      - Web API request logging");
                Serial.println("debug parse on/off    - Protocol compact output (P06/P07...)");
                Serial.println("debug heap on/off     - Heap monitoring");
                Serial.println("debug cloud on/off    - Raw cloud config dump on refresh (keys redacted)");
                Serial.println("debug status          - Show debug status per category");
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
        else if (cmd == "blediag") {
            printBLEDiagnostics();
        }
        else if (cmd == "restart") {
            Serial.println("Restarting...");
            EventLog::log(LOG_INFO, "Restart: requested via serial command");
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

            // Get network password: reuse the saved one if present (just press
            // Enter), or type a new one (e.g. after the password was changed).
            String password = networkConfig.casambiPassword;
            if (password.length() > 0) {
                Serial.println("Using saved network password.");
                Serial.println("Press Enter to keep it, or type a new password:");
            } else {
                Serial.println("Enter your Casambi network password:");
            }
            Serial.print("> ");
            while (!Serial.available()) {
                delay(10);
                esp_task_wdt_reset();
            }
            String enteredPassword = Serial.readStringUntil('\n');
            enteredPassword.trim();

            if (enteredPassword.length() > 0) {
                password = enteredPassword;
            } else if (password.length() == 0) {
                Serial.println("Cancelled (no password available).");
                return;
            }

            // Schedule the refresh and reboot; the download runs early at the
            // next boot (race-free, see requestCloudRefresh). Never returns.
            requestCloudRefresh(password);
        }
        else if (cmd == "clearconfig") {
            ConfigStore::clearAll();
            Serial.println("Configuration cleared. Restarting...");
            EventLog::log(LOG_INFO, "Restart: configuration cleared (factory reset)");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "log" || cmd.startsWith("log ")) {
            String sub = cmd.length() > 3 ? cmd.substring(4) : "";
            sub.trim();
            if (sub == "clear") {
                EventLog::clear();
                Serial.println("Event log cleared.");
            } else {
                int n = sub.length() > 0 ? sub.toInt() : 30;
                if (n <= 0) n = 30;
                Serial.printf("\n=== Event Log (newest %d) ===\n", n);
                EventLog::writeText(Serial, n);
                Serial.println();
            }
        }
        else if (cmd.startsWith("ntp")) {
            String sub = cmd.length() > 3 ? cmd.substring(4) : "";
            sub.trim();
            if (sub.startsWith("set ")) {
                String server = sub.substring(4);
                server.trim();
                if (server.length() == 0) {
                    Serial.println("Usage: ntp set <hostname|ip>  (e.g. pool.ntp.org or 192.168.1.1)");
                } else {
                    configLock();
                    networkConfig.ntpServer = server;
                    configUnlock();
                    ConfigStore::saveNetworkConfig(networkConfig);
                    Serial.printf("NTP server set to: %s\n", server.c_str());
                    if (WiFi.status() == WL_CONNECTED) {
                        g_timeSynced = false;
                        syncTime();
                    }
                }
            } else {
                bool isDefault = (networkConfig.ntpServer == NTP_SERVER_DEFAULT ||
                                  networkConfig.ntpServer.length() == 0);
                Serial.printf("NTP server (configured): %s%s\n",
                              networkConfig.ntpServer.c_str(),
                              isDefault ? " (default; local router tried first)" : "");
                if (g_ntpCandidates.length() > 0) {
                    Serial.printf("Server order: %s\n", g_ntpCandidates.c_str());
                }
                Serial.printf("Time synced: %s\n", g_timeSynced ? "yes" : "no");
                if (g_timeSynced) {
                    time_t nowSec = time(nullptr);
                    char ts[32];
                    struct tm tmUtc;
                    gmtime_r(&nowSec, &tmUtc);
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmUtc);
                    Serial.printf("Current UTC: %s\n", ts);
                }
            }
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
                // Strict parse: toInt() turned "connect foo" into index 0 and
                // silently connected to the first scanned device.
                long index;
                if (!parseSerialInt(cmd.substring(8), 0, 255, index)) {
                    Serial.println("Usage: connect <index>  (from 'scan' results)");
                } else {
                    connectToDevice((int)index);
                }
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
                configLock();
                networkConfig.autoConnectAddress = mac;
                configUnlock();
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

                        // Do NOT tear down the running AsyncWebServer and
                        // re-join WiFi at runtime: deleting the server races
                        // the async_tcp task (a handler may still be inside a
                        // request on the old instance — no grace period is
                        // provably long enough, a control setter alone can
                        // hold its BLE mutex for 1000 ms) and the BLE task's
                        // broadcast path. Same rationale as the cloud refresh
                        // (issue #21): reboot and come up cleanly with the new
                        // credentials — the boot path loads and connects them.
                        EventLog::log(LOG_INFO, "WiFi credentials changed via serial; restarting");
                        Serial.println("Restarting to apply the new WiFi credentials...");
                        delay(500);
                        ESP.restart();
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
                } else if (g_lastWifiDiscReason != 0) {
                    Serial.printf("Last disconnect: reason=%u (%s)\n",
                                  (unsigned)g_lastWifiDiscReason,
                                  wifiDisconnectReasonName(g_lastWifiDiscReason));
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
                bleDebugEnabled     = networkConfig.bleDebugEnabled;
                casambiDebugEnabled = networkConfig.casambiDebugEnabled;
                webDebugEnabled     = networkConfig.webDebugEnabled;
                parseDebugEnabled   = networkConfig.parseDebugEnabled;
                heapDebugEnabled    = networkConfig.heapDebugEnabled;
                cloudDebugEnabled   = networkConfig.cloudDebugEnabled;
                Serial.printf("Debug on: ble=%s casambi=%s web=%s parse=%s heap=%s cloud=%s\n",
                              bleDebugEnabled     ? "on" : "off",
                              casambiDebugEnabled ? "on" : "off",
                              webDebugEnabled     ? "on" : "off",
                              parseDebugEnabled   ? "on" : "off",
                              heapDebugEnabled    ? "on" : "off",
                              cloudDebugEnabled   ? "on" : "off");
            }
            else if (subcmd == "off") {
                // Suppress all output without changing saved settings
                bleDebugEnabled     = false;
                casambiDebugEnabled = false;
                webDebugEnabled     = false;
                parseDebugEnabled   = false;
                heapDebugEnabled    = false;
                cloudDebugEnabled   = false;
                Serial.println("Debug off (settings preserved, use 'debug on' to restore)");
            }
            else if (subcmd.startsWith("ble ")) {
                bool val = subcmd.endsWith(" on");
                bleDebugEnabled = val;
                networkConfig.bleDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("BLE debug: %s\n", val ? "on" : "off");
            }
            else if (subcmd.startsWith("casambi ")) {
                bool val = subcmd.endsWith(" on");
                casambiDebugEnabled = val;
                networkConfig.casambiDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Casambi debug: %s\n", val ? "on" : "off");
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
            else if (subcmd.startsWith("cloud ")) {
                bool val = subcmd.endsWith(" on");
                cloudDebugEnabled = val;
                networkConfig.cloudDebugEnabled = val;
                ConfigStore::saveNetworkConfig(networkConfig);
                Serial.printf("Cloud debug: %s (raw config dumped on next refresh, AES keys redacted)\n",
                              val ? "on" : "off");
            }
            else if (subcmd == "status") {
                Serial.printf("ble=%s  casambi=%s  web=%s  parse=%s  heap=%s  cloud=%s\n",
                              bleDebugEnabled     ? "on" : "off",
                              casambiDebugEnabled ? "on" : "off",
                              webDebugEnabled     ? "on" : "off",
                              parseDebugEnabled   ? "on" : "off",
                              heapDebugEnabled    ? "on" : "off",
                              cloudDebugEnabled   ? "on" : "off");
            }
            else {
                Serial.println("Usage: debug on/off/status");
                Serial.println("       debug ble on/off     - BLE/crypto verbose output");
                Serial.println("       debug casambi on/off - Casambi network events (units, echo)");
                Serial.println("       debug web on/off     - Web API request logging");
                Serial.println("       debug parse on/off   - Protocol compact output (P06/P07...)");
                Serial.println("       debug heap on/off    - Heap monitoring");
                Serial.println("       debug cloud on/off   - Raw cloud config dump (keys redacted)");
            }
        }
        // Scene commands
        else if (cmd.startsWith("son ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long v;
                if (parseControlArgs(SerialEntity::Scene, cmd.substring(4), id,
                                     false, 0, 0, v, "son <sceneId>")) {
                    bool ok = casambiClient->setSceneLevel(id, 0xFF);
                    if (ok) Serial.printf("Scene %u ON\n", id);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("soff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long v;
                if (parseControlArgs(SerialEntity::Scene, cmd.substring(5), id,
                                     false, 0, 0, v, "soff <sceneId>")) {
                    bool ok = casambiClient->setSceneLevel(id, 0);
                    if (ok) Serial.printf("Scene %u OFF\n", id);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("slevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long level;
                if (parseControlArgs(SerialEntity::Scene, cmd.substring(7), id,
                                     true, 0, 255, level, "slevel <sceneId> <0-255>")) {
                    bool ok = casambiClient->setSceneLevel(id, (uint8_t)level);
                    if (ok) Serial.printf("Scene %u level %ld\n", id, level);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        // Unit commands
        else if (cmd.startsWith("uon ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long v;
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(4), id,
                                     false, 0, 0, v, "uon <unitId>")) {
                    bool ok = casambiClient->setUnitLevel(id, 255);
                    if (ok) Serial.printf("Unit %u ON\n", id);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("uoff ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long v;
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(5), id,
                                     false, 0, 0, v, "uoff <unitId>")) {
                    bool ok = casambiClient->setUnitLevel(id, 0);
                    if (ok) Serial.printf("Unit %u OFF\n", id);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("ulevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long level;
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(7), id,
                                     true, 0, 255, level, "ulevel <unitId> <0-255>")) {
                    bool ok = casambiClient->setUnitLevel(id, (uint8_t)level);
                    if (ok) Serial.printf("Unit %u level %ld\n", id, level);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("uvertical ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long vertical;
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(10), id,
                                     true, 0, 255, vertical, "uvertical <unitId> <0-255>")) {
                    bool ok = casambiClient->setUnitVertical(id, (uint8_t)vertical);
                    if (ok) Serial.printf("Unit %u light balance %ld (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("ucolor ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                String tok[4];
                long id, r, g, b;
                if (splitSerialArgs(cmd.substring(7), tok, 4) != 4 ||
                    !parseSerialInt(tok[0], 0, 255, id) ||
                    !parseSerialInt(tok[1], 0, 255, r)  ||
                    !parseSerialInt(tok[2], 0, 255, g)  ||
                    !parseSerialInt(tok[3], 0, 255, b)) {
                    Serial.println("Usage: ucolor <unitId> <r> <g> <b>  (each 0-255)");
                } else if (serialEntityExists(SerialEntity::Unit, (uint8_t)id)) {
                    bool ok = casambiClient->setUnitColor((uint8_t)id, (uint8_t)r,
                                                          (uint8_t)g, (uint8_t)b);
                    if (ok) Serial.printf("Unit %ld color RGB(%ld,%ld,%ld)\n", id, r, g, b);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("utemp ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long kelvin;
                // Same range the REST API enforces for POST /api/units/:id/temperature.
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(6), id,
                                     true, 1000, 10000, kelvin, "utemp <unitId> <1000-10000>")) {
                    bool ok = casambiClient->setUnitTemperature(id, (uint16_t)kelvin);
                    if (ok) Serial.printf("Unit %u temperature %ldK\n", id, kelvin);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("uslider ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long slider;
                if (parseControlArgs(SerialEntity::Unit, cmd.substring(8), id,
                                     true, 0, 255, slider, "uslider <unitId> <0-255>")) {
                    bool ok = casambiClient->setUnitSlider(id, (uint8_t)slider);
                    if (ok) Serial.printf("Unit %u motor position %ld (0=up, 255=down)\n", id, slider);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        // Group commands
        else if (cmd.startsWith("glevel ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long level;
                if (parseControlArgs(SerialEntity::Group, cmd.substring(7), id,
                                     true, 0, 255, level, "glevel <groupId> <0-255>")) {
                    bool ok = casambiClient->setGroupLevel(id, (uint8_t)level);
                    if (ok) Serial.printf("Group %u level %ld\n", id, level);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("gvertical ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long vertical;
                if (parseControlArgs(SerialEntity::Group, cmd.substring(10), id,
                                     true, 0, 255, vertical, "gvertical <groupId> <0-255>")) {
                    bool ok = casambiClient->setGroupVertical(id, (uint8_t)vertical);
                    if (ok) Serial.printf("Group %u light balance %ld (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                    reportSerialSend(ok);
                }
            } else { Serial.println("Not authenticated"); }
        }
        else if (cmd.startsWith("gslider ")) {
            if (casambiClient && casambiClient->isAuthenticated()) {
                uint8_t id; long slider;
                if (parseControlArgs(SerialEntity::Group, cmd.substring(8), id,
                                     true, 0, 255, slider, "gslider <groupId> <0-255>")) {
                    bool ok = casambiClient->setGroupSlider(id, (uint8_t)slider);
                    if (ok) Serial.printf("Group %u motor position %ld (0=up, 255=down)\n", id, slider);
                    reportSerialSend(ok);
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

    NimBLEDevice::init(DEVICE_NAME);

    scannedDevices.clear();
    NimBLEScan* pBLEScan = NimBLEDevice::getScan();

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setScanCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("Scanning for 10 seconds...\n");
    pBLEScan->getResults(10000, false);  // blocking, duration in ms
    pBLEScan->clearResults();  // Free scan result memory

    if (scannedDevices.size() == 0) {
        Serial.println("\nNo Casambi networks found!");
        Serial.println("Make sure your lights are on and try again.");
        NimBLEDevice::deinit();
        return;
    }

    Serial.printf("\nFound %d Casambi network(s):\n\n", scannedDevices.size());
    for (size_t i = 0; i < scannedDevices.size(); i++) {
        Serial.printf("[%d] %s (%s) RSSI: %d, addr type: %s\n",
            i,
            scannedDevices[i].name.c_str(),
            scannedDevices[i].address.c_str(),
            scannedDevices[i].rssi,
            CasambiScan::addrTypeName(scannedDevices[i].addrType));
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
        NimBLEDevice::deinit();
        return;
    }

    // Extract network UUID from MAC address (remove colons, lowercase)
    String networkUuid = scannedDevices[selectedIndex].address;
    networkUuid.replace(":", "");
    networkUuid.toLowerCase();

    Serial.printf("\nSelected: %s\n", scannedDevices[selectedIndex].name.c_str());
    Serial.printf("Network UUID: %s\n", networkUuid.c_str());

    // Clean up BLE for now (will reinit WiFi)
    NimBLEDevice::deinit();
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

    // Persist the network password so `refresh` can reuse it later
    networkConfig.casambiPassword = password;

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

    NimBLEScan* pBLEScan = NimBLEDevice::getScan();

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setScanCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    pBLEScan->getResults(10000, false);  // blocking, duration in ms

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
        bleLostAt = 0;  // manual recovery — don't attribute the next loss to the old outage

        // Auto-save MAC address for auto-connect
        if (networkConfig.autoConnectAddress != dev.address) {
            configLock();
            networkConfig.autoConnectAddress = dev.address;
            configUnlock();
            ConfigStore::saveNetworkConfig(networkConfig);
            Serial.printf("Saved MAC address for auto-connect: %s\n", dev.address.c_str());
        }
    } else {
        Serial.printf("Connection failed (phase=%s, reason=%s, rc=%d) - "
                      "run 'blediag' for details\n",
                      casambiClient->getLastConnectPhase(),
                      disconnectReasonName(casambiClient->getLastDisconnectReason()),
                      casambiClient->getLastConnectError());
    }
}
