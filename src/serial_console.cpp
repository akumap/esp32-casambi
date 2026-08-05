/**
 * Serial console — see serial_console.h.
 */

#include "serial_console.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "config.h"
#include "app_state.h"
#include "console_out.h"
#include "serial_args.h"
#include "cloud/api_client.h"
#include "storage/config_store.h"
#include "log/event_log.h"
#include "ble/casambi_client.h"
#include "ble/casambi_scan.h"
#include "ble/reconnect_supervisor.h"
#include "net/time_sync.h"
#include "net/wifi_manager.h"
#include "net/telnet_console.h"
#include "diagnostics.h"
#include "cloud_refresh.h"

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
                    Console.printf("[%d] %s (%s) RSSI: %d, addr type: %s\n",
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
            Console.printf("Unit %u not found (see 'list units')\n", id);
            return false;
        case SerialEntity::Group:
            if (networkConfig.getGroupById(id)) return true;
            Console.printf("Group %u not found (see 'list groups')\n", id);
            return false;
        case SerialEntity::Scene:
            if (networkConfig.getSceneById(id)) return true;
            Console.printf("Scene %u not found (see 'list scenes')\n", id);
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
        Console.printf("Usage: %s\n", usage);
        return false;
    }
    long idL;
    if (!parseSerialInt(tok[0], 0, 255, idL)) {
        Console.println("Invalid id (must be 0-255)");
        return false;
    }
    if (wantValue && !parseSerialInt(tok[1], valMin, valMax, value)) {
        Console.printf("Invalid value (must be %ld-%ld)\n", valMin, valMax);
        return false;
    }
    id = (uint8_t)idL;
    return serialEntityExists(kind, id);
}

// Report the real outcome of a BLE setter — the setters return false when
// the command was NOT transmitted (link lost, mutex timeout, GATT failure),
// and the console must not claim success then.
static void reportSerialSend(bool ok) {
    if (!ok) Console.println("FAILED: command not transmitted (BLE send failed)");
}

// ============================================================================
// COMMAND HANDLER
// ============================================================================

// Serial-console 'help' text.
static void cmdHelp() {
    Console.println("\n=== Commands ===");
    Console.println("help          - Show this help");
    Console.println("status        - Show detailed status");
    Console.println("blediag       - BLE troubleshooting report (config, last connect phase, scan)");
    Console.println("refresh [password] - Refresh config from Casambi cloud");
    Console.println("clearconfig   - Clear configuration (factory reset)");
    Console.println("restart       - Restart ESP32");
    Console.println("log [n]       - Show newest n event-log entries (default 30)");
    Console.println("log clear     - Erase the event log");
    Console.println("ntp status    - Show NTP server and sync state");
    Console.println("ntp set <host|ip> - Set NTP server hostname or IP (UTC)");
    Console.println("telnet status - Show telnet console status");
    Console.println("telnet timeout <seconds> - Idle timeout (0 = disabled)");
    Console.println();

    if (!ConfigStore::hasValidConfig()) {
        Console.println("=== Setup Mode ===");
        Console.println("setup         - Run setup wizard (scans for networks)");
        Console.println();
    } else {
        Console.println("=== BLE Commands ===");
        Console.println("scan          - Scan for Casambi devices");
        Console.println("connect <n>   - Connect to device n");
        Console.println("disconnect    - Disconnect");
        Console.println();
        Console.println("autoconnect on/off - Enable/disable auto-connect");
        Console.println("autoconnect status - Show auto-connect status");
        Console.println("autoconnect set <mac> - Set auto-connect MAC address");
        Console.println();
        Console.println("reconnect on/off   - Enable/disable auto-reconnect");
        Console.println();
        Console.println("wifi set <ssid> <password> - Update WiFi credentials");
        Console.println("wifi status        - Show WiFi connection status");
        Console.println();
        Console.println("debug on/off          - Restore/suppress all debug (settings preserved)");
        Console.println("debug ble on/off      - BLE/crypto verbose output");
        Console.println("debug casambi on/off  - Casambi network events (units, echo)");
        Console.println("debug web on/off      - Web API request logging");
        Console.println("debug parse on/off    - Protocol compact output (P06/P07...)");
        Console.println("debug heap on/off     - Heap monitoring");
        Console.println("debug cloud on/off    - Raw cloud config dump on refresh (keys redacted)");
        Console.println("debug status          - Show debug status per category");
        Console.println();
        Console.println("=== Control Commands ===");
        Console.println("son <id>      - Turn scene ON");
        Console.println("soff <id>     - Turn scene OFF");
        Console.println("slevel <id> <0-255> - Set scene level");
        Console.println();
        Console.println("uon <id>      - Turn unit ON");
        Console.println("uoff <id>     - Turn unit OFF");
        Console.println("ulevel <id> <0-255> - Set unit level");
        Console.println("ucolor <id> <r> <g> <b> - Set unit RGB color");
        Console.println("utemp <id> <kelvin> - Set unit color temperature");
        Console.println("uvertical <id> <0-255> - Set light balance (0=top only, 127=both, 255=bottom only)");
        Console.println("uslider <id> <0-255> - Set motor position (0=up, 255=down)");
        Console.println();
        Console.println("glevel <id> <0-255> - Set group level");
        Console.println("gvertical <id> <0-255> - Set light balance (0=top, 127=both, 255=bottom)");
        Console.println("gslider <id> <0-255> - Set motor position (0=up, 255=down)");
        Console.println();
        Console.println("=== Info Commands ===");
        Console.println("list units    - List all units");
        Console.println("list groups   - List all groups");
        Console.println("list scenes   - List all scenes");
        Console.println("================");
        Console.println();
        Console.println("* Motor commands may not work on all units.");
        Console.println("  Use scenes for reliable motor control.\n");
    }
}

// 'refresh' / 'refresh <password>' — re-download the Casambi cloud
// configuration. This command runs from both the serial and the telnet
// console (net/telnet_console.cpp), and only the serial console can safely
// block waiting for typed input — so bare 'refresh' must never block: it
// uses the saved password whenever one is stored. A password argument
// overrides the saved one without an interactive follow-up prompt (e.g.
// after the Casambi cloud password changed).
static void cmdRefresh(const String& args) {
    if (!ConfigStore::hasValidConfig()) {
        Console.println("No configuration found. Run 'setup' first.");
        return;
    }

    Console.println("\n=== Refresh Configuration ===");
    Console.println("This will download fresh configuration from Casambi cloud.");
    Console.println("Your local settings (auto-connect, debug) will be preserved.\n");

    String password = args;
    password.trim();
    if (password.length() > 0) {
        Console.println("Using the given network password.");
    } else if (networkConfig.casambiPassword.length() > 0) {
        password = networkConfig.casambiPassword;
        Console.println("Using saved network password.");
    } else {
        // No password stored yet (a config predating the persisted-password
        // field) and none given as an argument — this can only happen on the
        // serial console; the telnet console never starts without a stored
        // password (net/telnet_console.h, decision E8).
        Console.println("Enter your Casambi network password:");
        Console.print("> ");
        while (!Serial.available()) {
            delay(10);
            esp_task_wdt_reset();
        }
        password = Serial.readStringUntil('\n');
        password.trim();
        if (password.length() == 0) {
            Console.println("Cancelled (no password available).");
            return;
        }
    }

    // Schedule the refresh and reboot; the download runs early at the
    // next boot (race-free, see requestCloudRefresh). Never returns.
    requestCloudRefresh(password);
}

// 'log [n]' / 'log clear' — event-log console access.
static void cmdLog(const String& cmd) {
    String sub = cmd.length() > 3 ? cmd.substring(4) : "";
    sub.trim();
    if (sub == "clear") {
        EventLog::clear();
        Console.println("Event log cleared.");
    } else {
        int n = sub.length() > 0 ? sub.toInt() : 30;
        if (n <= 0) n = 30;
        Console.printf("\n=== Event Log (newest %d) ===\n", n);
        EventLog::writeText(Console, n);
        Console.println();
    }
}

// 'ntp status' / 'ntp set <host>' — NTP server configuration.
static void cmdNtp(const String& cmd) {
    String sub = cmd.length() > 3 ? cmd.substring(4) : "";
    sub.trim();
    if (sub.startsWith("set ")) {
        String server = sub.substring(4);
        server.trim();
        if (server.length() == 0) {
            Console.println("Usage: ntp set <hostname|ip>  (e.g. pool.ntp.org or 192.168.1.1)");
        } else {
            configLock();
            networkConfig.ntpServer = server;
            configUnlock();
            ConfigStore::saveNetworkConfig(networkConfig);
            Console.printf("NTP server set to: %s\n", server.c_str());
            if (WiFi.status() == WL_CONNECTED) {
                setTimeSynced(false);
                syncTime();
            }
        }
    } else {
        bool isDefault = (networkConfig.ntpServer == NTP_SERVER_DEFAULT ||
                          networkConfig.ntpServer.length() == 0);
        Console.printf("NTP server (configured): %s%s\n",
                      networkConfig.ntpServer.c_str(),
                      isDefault ? " (default; local router tried first)" : "");
        if (ntpCandidates().length() > 0) {
            Console.printf("Server order: %s\n", ntpCandidates().c_str());
        }
        Console.printf("Time synced: %s\n", timeSynced() ? "yes" : "no");
        if (timeSynced()) {
            time_t nowSec = time(nullptr);
            char ts[32];
            struct tm tmUtc;
            gmtime_r(&nowSec, &tmUtc);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmUtc);
            Console.printf("Current UTC: %s\n", ts);
        }
    }
}

// 'telnet status' / 'telnet timeout <seconds>' — network console config
// (docs/konzept-tcp-konsole.md, decision E6a).
static void cmdTelnet(const String& cmd) {
    String sub = cmd.length() > 6 ? cmd.substring(7) : "";
    sub.trim();
    if (sub.startsWith("timeout")) {
        String arg = sub.substring(7);
        arg.trim();
        long secs;
        if (!parseSerialInt(arg, 0, TELNET_TIMEOUT_MAX_SECONDS, secs)) {
            Console.printf("Usage: telnet timeout <seconds>  (0 = disabled, max %d)\n",
                          TELNET_TIMEOUT_MAX_SECONDS);
            return;
        }
        networkConfig.telnetTimeoutSeconds = (uint32_t)secs;
        ConfigStore::saveNetworkConfig(networkConfig);
        if (secs == 0) {
            Console.println("Telnet idle timeout disabled.");
        } else {
            Console.printf("Telnet idle timeout set to %ld s.\n", secs);
        }
    } else {
        if (telnetConsole) {
            Console.printf("Telnet console: listening on port %d\n", TELNET_PORT);
            Console.printf("Session: %s\n", telnetConsole->sessionActive() ? "active" : "none");
            Console.printf("Dropped bytes (slow client): %lu\n",
                          (unsigned long)telnetConsole->droppedBytes());
        } else {
            Console.println("Telnet console: not started (no network password set, or WiFi not connected)");
        }
        if (networkConfig.telnetTimeoutSeconds == 0) {
            Console.println("Idle timeout: disabled");
        } else {
            Console.printf("Idle timeout: %lu s\n", (unsigned long)networkConfig.telnetTimeoutSeconds);
        }
    }
}

// 'connect <index>' — connect to a previously scanned device.
static void cmdConnect(const String& cmd) {
    if (casambiClient) {
        // Strict parse: toInt() turned "connect foo" into index 0 and
        // silently connected to the first scanned device.
        long index;
        if (!parseSerialInt(cmd.substring(8), 0, 255, index)) {
            Console.println("Usage: connect <index>  (from 'scan' results)");
        } else {
            connectToDevice((int)index);
        }
    } else {
        Console.println("Not in operation mode");
    }
}

// 'reconnect on/off' — enable/disable BLE auto-reconnect.
static void cmdReconnect(const String& cmd) {
    String subcmd = cmd.substring(10);
    subcmd.trim();
    if (subcmd == "on") {
        setBleReconnectEnabled(true);
        Console.println("Auto-reconnect enabled");
    } else if (subcmd == "off") {
        setBleReconnectEnabled(false);
        Console.println("Auto-reconnect disabled");
    } else {
        Console.printf("Auto-reconnect: %s\n", bleReconnectEnabled() ? "enabled" : "disabled");
    }
}

// 'autoconnect on/off/status/set <mac>'.
static void cmdAutoconnect(const String& cmd) {
    String subcmd = cmd.substring(12);
    subcmd.trim();

    if (subcmd == "on") {
        networkConfig.autoConnectEnabled = true;
        ConfigStore::saveNetworkConfig(networkConfig);
        Console.println("Auto-connect enabled");
    }
    else if (subcmd == "off") {
        networkConfig.autoConnectEnabled = false;
        ConfigStore::saveNetworkConfig(networkConfig);
        Console.println("Auto-connect disabled");
    }
    else if (subcmd == "status") {
        Console.printf("Auto-connect: %s\n",
            networkConfig.autoConnectEnabled ? "enabled" : "disabled");
        if (networkConfig.autoConnectAddress.length() > 0) {
            Console.printf("MAC address: %s\n", networkConfig.autoConnectAddress.c_str());
        } else {
            Console.println("MAC address: (not set)");
        }
    }
    else if (subcmd.startsWith("set ")) {
        String mac = subcmd.substring(4);
        mac.trim();
        configLock();
        networkConfig.autoConnectAddress = mac;
        configUnlock();
        ConfigStore::saveNetworkConfig(networkConfig);
        Console.printf("Auto-connect MAC set to: %s\n", mac.c_str());
    }
    else {
        Console.println("Usage: autoconnect on/off/status/set <mac>");
    }
}

// 'wifi set <ssid> <password>' / 'wifi status'.
static void cmdWifi(const String& cmd) {
    String subcmd = cmd.substring(5);
    subcmd.trim();

    if (subcmd.startsWith("set ")) {
        // Parse: wifi set <ssid> <password>
        String params = subcmd.substring(4);
        params.trim();

        int spaceIdx = params.indexOf(' ');
        if (spaceIdx == -1) {
            Console.println("Usage: wifi set <ssid> <password>");
            Console.println("Example: wifi set MyNetwork MyPassword123");
        } else {
            String newSsid = params.substring(0, spaceIdx);
            String newPassword = params.substring(spaceIdx + 1);
            newPassword.trim();

            // Save new credentials
            WiFiCredentials newCreds;
            newCreds.ssid = newSsid;
            newCreds.password = newPassword;

            if (ConfigStore::saveWiFiCredentials(newCreds)) {
                Console.printf("WiFi credentials updated (SSID: %s)\n", newSsid.c_str());

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
                Console.println("Restarting to apply the new WiFi credentials...");
                delay(500);
                ESP.restart();
            } else {
                Console.println("Failed to save WiFi credentials");
            }
        }
    }
    else if (subcmd == "status") {
        Console.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        if (WiFi.status() == WL_CONNECTED) {
            Console.printf("SSID: %s\n", WiFi.SSID().c_str());
            Console.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            Console.printf("RSSI: %d dBm\n", WiFi.RSSI());
        } else if (wifiLastDisconnectReason() != 0) {
            Console.printf("Last disconnect: reason=%u (%s)\n",
                          (unsigned)wifiLastDisconnectReason(),
                          wifiDisconnectReasonName(wifiLastDisconnectReason()));
        }

        // Show stored credentials
        WiFiCredentials storedCreds;
        if (ConfigStore::loadWiFiCredentials(storedCreds)) {
            Console.printf("Stored SSID: %s\n", storedCreds.ssid.c_str());
        }
    }
    else {
        Console.println("Usage: wifi set <ssid> <password> | wifi status");
    }
}

// 'debug ...' — per-category debug flag control.
static void cmdDebug(const String& cmd) {
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
        Console.printf("Debug on: ble=%s casambi=%s web=%s parse=%s heap=%s cloud=%s\n",
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
        Console.println("Debug off (settings preserved, use 'debug on' to restore)");
    }
    else if (subcmd.startsWith("ble ")) {
        bool val = subcmd.endsWith(" on");
        bleDebugEnabled = val;
        networkConfig.bleDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("BLE debug: %s\n", val ? "on" : "off");
    }
    else if (subcmd.startsWith("casambi ")) {
        bool val = subcmd.endsWith(" on");
        casambiDebugEnabled = val;
        networkConfig.casambiDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("Casambi debug: %s\n", val ? "on" : "off");
    }
    else if (subcmd.startsWith("web ")) {
        bool val = subcmd.endsWith(" on");
        webDebugEnabled = val;
        networkConfig.webDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("Web debug: %s\n", val ? "on" : "off");
    }
    else if (subcmd.startsWith("parse ")) {
        bool val = subcmd.endsWith(" on");
        parseDebugEnabled = val;
        networkConfig.parseDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("Parse debug: %s\n", val ? "on" : "off");
    }
    else if (subcmd.startsWith("heap ")) {
        bool val = subcmd.endsWith(" on");
        heapDebugEnabled = val;
        networkConfig.heapDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("Heap debug: %s\n", val ? "on" : "off");
    }
    else if (subcmd.startsWith("cloud ")) {
        bool val = subcmd.endsWith(" on");
        cloudDebugEnabled = val;
        networkConfig.cloudDebugEnabled = val;
        ConfigStore::saveDebugFlags(networkConfig);
        Console.printf("Cloud debug: %s (raw config dumped on next refresh, AES keys redacted)\n",
                      val ? "on" : "off");
    }
    else if (subcmd == "status") {
        Console.printf("ble=%s  casambi=%s  web=%s  parse=%s  heap=%s  cloud=%s\n",
                      bleDebugEnabled     ? "on" : "off",
                      casambiDebugEnabled ? "on" : "off",
                      webDebugEnabled     ? "on" : "off",
                      parseDebugEnabled   ? "on" : "off",
                      heapDebugEnabled    ? "on" : "off",
                      cloudDebugEnabled   ? "on" : "off");
    }
    else {
        Console.println("Usage: debug on/off/status");
        Console.println("       debug ble on/off     - BLE/crypto verbose output");
        Console.println("       debug casambi on/off - Casambi network events (units, echo)");
        Console.println("       debug web on/off     - Web API request logging");
        Console.println("       debug parse on/off   - Protocol compact output (P06/P07...)");
        Console.println("       debug heap on/off    - Heap monitoring");
        Console.println("       debug cloud on/off   - Raw cloud config dump (keys redacted)");
    }
}

// Scene control: 'son <id>', 'soff <id>', 'slevel <id> <0-255>'.
static void cmdSceneCommand(const String& cmd) {
    if (cmd.startsWith("son ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long v;
            if (parseControlArgs(SerialEntity::Scene, cmd.substring(4), id,
                                 false, 0, 0, v, "son <sceneId>")) {
                bool ok = casambiClient->setSceneLevel(id, 0xFF);
                if (ok) Console.printf("Scene %u ON\n", id);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("soff ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long v;
            if (parseControlArgs(SerialEntity::Scene, cmd.substring(5), id,
                                 false, 0, 0, v, "soff <sceneId>")) {
                bool ok = casambiClient->setSceneLevel(id, 0);
                if (ok) Console.printf("Scene %u OFF\n", id);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("slevel ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long level;
            if (parseControlArgs(SerialEntity::Scene, cmd.substring(7), id,
                                 true, 0, 255, level, "slevel <sceneId> <0-255>")) {
                bool ok = casambiClient->setSceneLevel(id, (uint8_t)level);
                if (ok) Console.printf("Scene %u level %ld\n", id, level);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
}

// Unit control: 'uon/uoff/ulevel/uvertical/ucolor/utemp/uslider <id> ...'.
static void cmdUnitCommand(const String& cmd) {
    if (cmd.startsWith("uon ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long v;
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(4), id,
                                 false, 0, 0, v, "uon <unitId>")) {
                bool ok = casambiClient->setUnitLevel(id, 255);
                if (ok) Console.printf("Unit %u ON\n", id);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("uoff ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long v;
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(5), id,
                                 false, 0, 0, v, "uoff <unitId>")) {
                bool ok = casambiClient->setUnitLevel(id, 0);
                if (ok) Console.printf("Unit %u OFF\n", id);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("ulevel ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long level;
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(7), id,
                                 true, 0, 255, level, "ulevel <unitId> <0-255>")) {
                bool ok = casambiClient->setUnitLevel(id, (uint8_t)level);
                if (ok) Console.printf("Unit %u level %ld\n", id, level);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("uvertical ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long vertical;
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(10), id,
                                 true, 0, 255, vertical, "uvertical <unitId> <0-255>")) {
                bool ok = casambiClient->setUnitVertical(id, (uint8_t)vertical);
                if (ok) Console.printf("Unit %u light balance %ld (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
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
                Console.println("Usage: ucolor <unitId> <r> <g> <b>  (each 0-255)");
            } else if (serialEntityExists(SerialEntity::Unit, (uint8_t)id)) {
                bool ok = casambiClient->setUnitColor((uint8_t)id, (uint8_t)r,
                                                      (uint8_t)g, (uint8_t)b);
                if (ok) Console.printf("Unit %ld color RGB(%ld,%ld,%ld)\n", id, r, g, b);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("utemp ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long kelvin;
            // Same range the REST API enforces for POST /api/units/:id/temperature.
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(6), id,
                                 true, 1000, 10000, kelvin, "utemp <unitId> <1000-10000>")) {
                bool ok = casambiClient->setUnitTemperature(id, (uint16_t)kelvin);
                if (ok) Console.printf("Unit %u temperature %ldK\n", id, kelvin);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("uslider ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long slider;
            if (parseControlArgs(SerialEntity::Unit, cmd.substring(8), id,
                                 true, 0, 255, slider, "uslider <unitId> <0-255>")) {
                bool ok = casambiClient->setUnitSlider(id, (uint8_t)slider);
                if (ok) Console.printf("Unit %u motor position %ld (0=up, 255=down)\n", id, slider);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
}

// Group control: 'glevel/gvertical/gslider <id> <0-255>'.
static void cmdGroupCommand(const String& cmd) {
    if (cmd.startsWith("glevel ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long level;
            if (parseControlArgs(SerialEntity::Group, cmd.substring(7), id,
                                 true, 0, 255, level, "glevel <groupId> <0-255>")) {
                bool ok = casambiClient->setGroupLevel(id, (uint8_t)level);
                if (ok) Console.printf("Group %u level %ld\n", id, level);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("gvertical ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long vertical;
            if (parseControlArgs(SerialEntity::Group, cmd.substring(10), id,
                                 true, 0, 255, vertical, "gvertical <groupId> <0-255>")) {
                bool ok = casambiClient->setGroupVertical(id, (uint8_t)vertical);
                if (ok) Console.printf("Group %u light balance %ld (0=top only, 127=both, 255=bottom only)\n", id, vertical);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
    else if (cmd.startsWith("gslider ")) {
        if (casambiClient && casambiClient->isAuthenticated()) {
            uint8_t id; long slider;
            if (parseControlArgs(SerialEntity::Group, cmd.substring(8), id,
                                 true, 0, 255, slider, "gslider <groupId> <0-255>")) {
                bool ok = casambiClient->setGroupSlider(id, (uint8_t)slider);
                if (ok) Console.printf("Group %u motor position %ld (0=up, 255=down)\n", id, slider);
                reportSerialSend(ok);
            }
        } else { Console.println("Not authenticated"); }
    }
}

// 'list units/groups/scenes'.
static void cmdList(const String& cmd) {
    String what = cmd.substring(5);
    if (what == "units") {
        Console.printf("\n=== Units (%d) ===\n", networkConfig.units.size());
        for (const auto& unit : networkConfig.units) {
            Console.printf("[%d] %s %s\n", unit.deviceId, unit.name.c_str(),
                          unit.on ? "(ON)" : "(OFF)");
        }
        Console.println();
    }
    else if (what == "groups") {
        Console.printf("\n=== Groups (%d) ===\n", networkConfig.groups.size());
        for (const auto& group : networkConfig.groups) {
            Console.printf("[%d] %s\n", group.groupId, group.name.c_str());
        }
        Console.println();
    }
    else if (what == "scenes") {
        Console.printf("\n=== Scenes (%d) ===\n", networkConfig.scenes.size());
        for (const auto& scene : networkConfig.scenes) {
            Console.printf("[%d] %s\n", scene.sceneId, scene.name.c_str());
        }
        Console.println();
    }
}

// ============================================================================
// COMMAND DISPATCHER
// ============================================================================

void handleCommand(const String& cmd) {
    // Echo the command, but never log credentials. `wifi set <ssid> <password>`
    // carries the WiFi password as a plain argument, so mask everything after
    // the subcommand for password-bearing commands.
    if (cmd.startsWith("wifi set")) {
        Console.println(">>> CMD: wifi set <redacted>");
    } else {
        Console.printf(">>> CMD: %s\n", cmd.c_str());
    }
    if (cmd == "help") {
        cmdHelp();
    }
    else if (cmd == "status") {
        printStatus();
    }
    else if (cmd == "blediag") {
        printBLEDiagnostics();
    }
    else if (cmd == "restart") {
        Console.println("Restarting...");
        EventLog::log(LOG_INFO, "Restart: requested via serial command");
        delay(500);
        ESP.restart();
    }
    else if (cmd == "refresh" || cmd.startsWith("refresh ")) {
        cmdRefresh(cmd.length() > 7 ? cmd.substring(8) : "");
    }
    else if (cmd == "clearconfig") {
        ConfigStore::clearAll();
        Console.println("Configuration cleared. Restarting...");
        EventLog::log(LOG_INFO, "Restart: configuration cleared (factory reset)");
        delay(1000);
        ESP.restart();
    }
    else if (cmd == "log" || cmd.startsWith("log ")) {
        cmdLog(cmd);
    }
    else if (cmd.startsWith("ntp")) {
        cmdNtp(cmd);
    }
    else if (cmd.startsWith("telnet")) {
        cmdTelnet(cmd);
    }
    else if (cmd == "setup") {
        if (apiClient) {
            runSetupWizard();
        } else {
            Console.println("Already configured. Use 'clearconfig' to reset.");
        }
    }
    else if (cmd == "scan") {
        if (casambiClient) {
            scanForDevices();
        } else {
            Console.println("Not in operation mode");
        }
    }
    else if (cmd.startsWith("connect ")) {
        cmdConnect(cmd);
    }
    else if (cmd == "disconnect") {
        if (casambiClient) {
            casambiClient->disconnect();
            Console.println("Disconnected");
        }
    }
    else if (cmd.startsWith("reconnect ")) {
        cmdReconnect(cmd);
    }
    else if (cmd.startsWith("autoconnect ")) {
        cmdAutoconnect(cmd);
    }
    else if (cmd.startsWith("wifi ")) {
        cmdWifi(cmd);
    }
    else if (cmd.startsWith("debug ")) {
        cmdDebug(cmd);
    }
    // Scene commands
    else if (cmd.startsWith("son ") || cmd.startsWith("soff ") || cmd.startsWith("slevel ")) {
        cmdSceneCommand(cmd);
    }
    // Unit commands
    else if (cmd.startsWith("uon ") || cmd.startsWith("uoff ") || cmd.startsWith("ulevel ") ||
             cmd.startsWith("uvertical ") || cmd.startsWith("ucolor ") || cmd.startsWith("utemp ") ||
             cmd.startsWith("uslider ")) {
        cmdUnitCommand(cmd);
    }
    // Group commands
    else if (cmd.startsWith("glevel ") || cmd.startsWith("gvertical ") || cmd.startsWith("gslider ")) {
        cmdGroupCommand(cmd);
    }
    // List commands
    else if (cmd.startsWith("list ")) {
        cmdList(cmd);
    }
    else {
        Console.println("Unknown command. Type 'help'");
    }
}

// ============================================================================
// SETUP WIZARD
// ============================================================================

void runSetupWizard() {
    Console.println("\n=== Casambi Setup Wizard ===\n");

    Console.println("Step 1: Scanning for Casambi networks...");
    Console.println("(Make sure your Casambi lights are powered on)\n");

    NimBLEDevice::init(DEVICE_NAME);

    scannedDevices.clear();
    NimBLEScan* pBLEScan = NimBLEDevice::getScan();

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setScanCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
    pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);

    Console.println("Scanning for 10 seconds...\n");
    pBLEScan->getResults(10000, false);  // blocking, duration in ms
    pBLEScan->clearResults();  // Free scan result memory

    if (scannedDevices.size() == 0) {
        Console.println("\nNo Casambi networks found!");
        Console.println("Make sure your lights are on and try again.");
        NimBLEDevice::deinit();
        return;
    }

    Console.printf("\nFound %d Casambi network(s):\n\n", scannedDevices.size());
    for (size_t i = 0; i < scannedDevices.size(); i++) {
        Console.printf("[%d] %s (%s) RSSI: %d, addr type: %s\n",
            i,
            scannedDevices[i].name.c_str(),
            scannedDevices[i].address.c_str(),
            scannedDevices[i].rssi,
            CasambiScan::addrTypeName(scannedDevices[i].addrType));
    }

    Console.println("\nSelect network (enter number):");
    Console.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String indexStr = Serial.readStringUntil('\n');
    indexStr.trim();
    int selectedIndex = indexStr.toInt();

    if (selectedIndex < 0 || selectedIndex >= (int)scannedDevices.size()) {
        Console.println("Invalid selection. Cancelled.");
        NimBLEDevice::deinit();
        return;
    }

    // Extract network UUID from MAC address (remove colons, lowercase)
    String networkUuid = scannedDevices[selectedIndex].address;
    networkUuid.replace(":", "");
    networkUuid.toLowerCase();

    Console.printf("\nSelected: %s\n", scannedDevices[selectedIndex].name.c_str());
    Console.printf("Network UUID: %s\n", networkUuid.c_str());

    // Clean up BLE for now (will reinit WiFi)
    NimBLEDevice::deinit();
    delay(500);

    // Step 2: Get network password
    Console.println("\nStep 2: Enter network password");
    Console.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String password = Serial.readStringUntil('\n');
    password.trim();
    if (password.length() == 0) { Console.println("Cancelled."); return; }

    // Step 3: Get WiFi credentials
    Console.println("\nStep 3: WiFi Configuration");
    Console.println("Enter WiFi SSID:");
    Console.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String ssid = Serial.readStringUntil('\n');
    ssid.trim();
    if (ssid.length() == 0) { Console.println("Cancelled."); return; }

    Console.println("\nEnter WiFi password:");
    Console.print("> ");
    while (!Serial.available()) {
        delay(10);
        esp_task_wdt_reset();
    }
    String wifiPassword = Serial.readStringUntil('\n');
    wifiPassword.trim();

    // Step 4: Connect to WiFi
    Console.println("\nStep 4: Connecting to cloud");
    Console.println("--- Connecting to WiFi ---");
    if (!apiClient->connectWiFi(ssid, wifiPassword)) {
        Console.printf("ERROR: WiFi connection failed: %s\n", apiClient->getLastError().c_str());
        return;
    }

    // Get network ID from UUID
    Console.println("--- Fetching network ID ---");
    String networkId;
    if (!apiClient->getNetworkId(networkUuid, networkId)) {
        Console.printf("ERROR: Failed to get network ID: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Create session
    Console.println("--- Creating session ---");
    String sessionToken;
    if (!apiClient->createSession(networkId, password, sessionToken)) {
        Console.printf("ERROR: Failed to create session: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Fetch network configuration
    Console.println("--- Downloading network configuration ---");
    if (!apiClient->fetchNetworkConfig(networkId, sessionToken, networkConfig)) {
        Console.printf("ERROR: Failed to fetch config: %s\n", apiClient->getLastError().c_str());
        apiClient->disconnectWiFi();
        return;
    }

    // Store network UUID and ID
    networkConfig.networkUuid = networkUuid;
    networkConfig.networkId = networkId;

    // Persist the network password so `refresh` can reuse it later
    networkConfig.casambiPassword = password;

    // Auto-connect target: the MAC of the unit picked in step 1. Without this
    // the wizard left autoConnectAddress empty, so the device came up with a
    // perfectly valid config and then never attempted a single connect — and
    // said nothing about it, because every "connect failed" trace sits behind
    // an attempt that never happened (issue #42). The web setup portal has
    // always stored it here, see setup_portal.cpp.
    networkConfig.autoConnectAddress = scannedDevices[selectedIndex].address;
    networkConfig.autoConnectEnabled = true;

    // Step 5: Save configuration
    Console.println("\nStep 5: Saving configuration");
    Console.println("--- Saving to flash ---");
    if (!ConfigStore::saveNetworkConfig(networkConfig)) {
        Console.println("ERROR: Failed to save configuration");
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

    Console.println("\n=== Setup Complete! ===");
    Console.printf("Network: %s\n", networkConfig.networkName.c_str());
    Console.printf("Units: %d\n", networkConfig.units.size());
    Console.printf("Groups: %d\n", networkConfig.groups.size());
    Console.printf("Scenes: %d\n", networkConfig.scenes.size());
    Console.printf("Auto-connect: %s\n", networkConfig.autoConnectAddress.c_str());
    Console.println("\nRestarting to enter operation mode...");
    delay(2000);
    ESP.restart();
}

void scanForDevices() {
    Console.println("\n=== Scanning for Casambi devices ===");
    Console.println("Scanning for 10 seconds...\n");

    scannedDevices.clear();

    NimBLEScan* pBLEScan = NimBLEDevice::getScan();

    // Fix memory leak: reuse scan callback instance
    if (scanCallbackInstance) delete scanCallbackInstance;
    scanCallbackInstance = new ScanCallbacks();
    pBLEScan->setScanCallbacks(scanCallbackInstance);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(BLE_SCAN_INTERVAL_MS);
    pBLEScan->setWindow(BLE_SCAN_WINDOW_MS);

    pBLEScan->getResults(10000, false);  // blocking, duration in ms

    Console.printf("\nFound %d Casambi device(s)\n", scannedDevices.size());
    Console.println("Use 'connect <n>' to connect to device n\n");

    pBLEScan->clearResults();
}

void connectToDevice(int index) {
    if (index < 0 || index >= (int)scannedDevices.size()) {
        Console.printf("Invalid device index. Use 0-%d\n", scannedDevices.size() - 1);
        return;
    }

    ScannedDevice& dev = scannedDevices[index];
    Console.printf("Connecting to %s (%s)...\n", dev.name.c_str(), dev.address.c_str());

    if (casambiClient->connect(dev.address)) {
        Console.println("Connected and authenticated successfully!");
        bleNoteConnected();  // clears the failure budget and the outage timestamp

        // Auto-save MAC address for auto-connect
        if (networkConfig.autoConnectAddress != dev.address) {
            configLock();
            networkConfig.autoConnectAddress = dev.address;
            configUnlock();
            ConfigStore::saveNetworkConfig(networkConfig);
            Console.printf("Saved MAC address for auto-connect: %s\n", dev.address.c_str());
        }
    } else {
        Console.printf("Connection failed (phase=%s, reason=%s, rc=%d) - "
                      "run 'blediag' for details\n",
                      casambiClient->getLastConnectPhase(),
                      disconnectReasonName(casambiClient->getLastDisconnectReason()),
                      casambiClient->getLastConnectError());
    }
}
