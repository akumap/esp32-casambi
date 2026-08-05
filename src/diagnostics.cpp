/**
 * Diagnostics: heap monitoring, status reports, version checks — see diagnostics.h.
 */

#include "diagnostics.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>   // the advertisement probe feeds the WDT while scanning
#include "config.h"
#include "app_state.h"
#include "console_out.h"
#include "log/event_log.h"
#include "web/webserver.h"
#include "ble/casambi_client.h"
#include "ble/casambi_scan.h"
#include "ble/packet.h"
#include "ble/reconnect_supervisor.h"
#include "net/wifi_manager.h"
#include "storage/config_store.h"

// Heap monitoring state
static unsigned long lastHeapCheck = 0;
static size_t minFreeHeap = UINT32_MAX;

size_t minFreeHeapSeen() { return minFreeHeap; }

void initHeapMonitor(size_t freeHeapAtBoot) { minFreeHeap = freeHeapAtBoot; }

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
        Console.printf("HEAP: free=%d, min=%d, largest_block=%d\n",
                      freeHeap, minFreeHeap, largestBlock);
    }

    // Critical heap handling with debounce: a single low reading is usually a
    // transient spike (web/WS TCP buffers, a BLE reconnect) that recovers on its
    // own. Only restart after HEAP_CRITICAL_CONSECUTIVE sustained low readings.
    static uint8_t lowHeapStreak = 0;
    if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
        lowHeapStreak++;
        Console.printf("*** Low heap %d < %d (%u/%u) ***\n",
                      freeHeap, HEAP_CRITICAL_THRESHOLD,
                      lowHeapStreak, HEAP_CRITICAL_CONSECUTIVE);
        // Record the onset of a low-heap episode once, for post-mortem analysis.
        if (lowHeapStreak == 1) {
            EventLog::log(LOG_WARN, "Low heap %u < %u (transient?)",
                          (unsigned)freeHeap, (unsigned)HEAP_CRITICAL_THRESHOLD);
        }
        if (lowHeapStreak >= HEAP_CRITICAL_CONSECUTIVE) {
            Console.println("*** Sustained low heap - restarting ESP32 ***");
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
    Console.println("\n=== System Status ===");

    // BLE status
    if (casambiClient) {
        Console.printf("BLE: %s\n",
            casambiClient->isAuthenticated() ? "Authenticated" :
            (casambiClient->getState() == ConnectionState::None ? "Disconnected" : "Connecting..."));

        if (casambiClient->isAuthenticated()) {
            unsigned long uptime = casambiClient->getConnectionUptime();
            Console.printf("  Uptime: %lu:%02lu:%02lu\n",
                          uptime / 3600000, (uptime / 60000) % 60, (uptime / 1000) % 60);
            Console.printf("  Packets received: %u\n", casambiClient->getReceivedPacketCount());
            Console.printf("  Connected to: %s\n", casambiClient->getConnectedAddress().c_str());
            Console.printf("  RSSI: %d dBm\n", casambiClient->getLastRssi());
        }

        // Parser counters. "partial" = the understood prefix was applied and
        // an undecoded tail dropped (likely a protocol element the reverse-
        // engineering does not cover yet — worth a look when it grows);
        // "malformed" = the packet yielded nothing usable.
        const PacketParseStats& ps = packetParseStats();
        if (ps.partial06.load() || ps.partial07.load() || ps.partial08.load()) {
            Console.printf("  Partially decoded packets: 0x06=%u 0x07=%u 0x08=%u\n",
                          ps.partial06.load(), ps.partial07.load(), ps.partial08.load());
        }
        if (ps.malformed06.load() || ps.malformed07.load() || ps.malformed08.load()) {
            Console.printf("  Malformed packets dropped: 0x06=%u 0x07=%u 0x08=%u\n",
                          ps.malformed06.load(), ps.malformed07.load(), ps.malformed08.load());
        }

        if (casambiClient->getLastDisconnectReason() != DisconnectReason::None) {
            Console.printf("  Last disconnect: reason=%d/%s, source=%s\n",
                          static_cast<int>(casambiClient->getLastDisconnectReason()),
                          disconnectReasonName(casambiClient->getLastDisconnectReason()),
                          casambiClient->getLastDisconnectSource());
        }
        if (!casambiClient->isAuthenticated()) {
            // Where the last attempt broke is the single most useful number
            // when the link never comes up ("link" = never reached the peer,
            // anything else = we talked to it and the handshake failed).
            Console.printf("  Last connect phase: %s (rc=%d)\n",
                          casambiClient->getLastConnectPhase(),
                          casambiClient->getLastConnectError());
            Console.printf("  Auto-connect: %s, MAC: %s\n",
                          networkConfig.autoConnectEnabled ? "enabled" : "disabled",
                          networkConfig.autoConnectAddress.length()
                              ? networkConfig.autoConnectAddress.c_str() : "(none)");
            Console.println("  Run 'blediag' for a full BLE diagnostic report");
        }
    } else {
        Console.println("BLE: Setup mode - no client");
    }

    // WiFi status
    Console.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    if (WiFi.status() == WL_CONNECTED) {
        Console.printf("  IP: %s, RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    // Web server
    Console.printf("Web Server: %s\n",
                  (webServer && webServer->isRunning()) ? "Running" : "Stopped");

    // System info
    Console.printf("Heap: free=%d, min=%d, largest=%d\n",
                  ESP.getFreeHeap(), minFreeHeap, ESP.getMaxAllocHeap());
    Console.printf("Uptime: %lu seconds\n", millis() / 1000);
    Console.printf("Reconnect failures: %d/%d\n", bleConsecutiveFailures(), MAX_RECONNECT_FAILURES);
    Console.printf("Auto-reconnect: %s\n", bleReconnectEnabled() ? "enabled" : "disabled");
    Console.println();
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
    Console.println("\n=== BLE Diagnostics ===");
    Console.printf("Firmware build: %d, uptime: %lus, heap: free=%u min=%u largest=%u\n",
                  FIRMWARE_BUILD, millis() / 1000,
                  ESP.getFreeHeap(), minFreeHeap, ESP.getMaxAllocHeap());
    Console.printf("Chip: %s rev %d, %d core(s)\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());

    // --- Configuration -----------------------------------------------------
    Console.println("\n-- Configuration --");
    Console.printf("Network: '%s' (uuid=%s)\n",
                  networkConfig.networkName.c_str(), networkConfig.networkUuid.c_str());
    Console.printf("Protocol version: %d (minimum %d)\n",
                  networkConfig.protocolVersion, MIN_PROTOCOL_VERSION);
    Console.printf("Units: %d, groups: %d, scenes: %d\n",
                  (int)networkConfig.units.size(), (int)networkConfig.groups.size(),
                  (int)networkConfig.scenes.size());
    Console.printf("Keys: %d\n", (int)networkConfig.keys.size());
    for (const auto& k : networkConfig.keys) {
        // Metadata only — never the key material itself.
        Console.printf("  key id=%d type=%d role=%d name='%s'\n",
                      k.id, k.type, k.role, k.name.c_str());
    }
    if (networkConfig.keys.empty()) {
        Console.println("  *** No keys: the gateway will connect but never authenticate "
                       "(re-run 'setup' or 'refresh') ***");
    }
    Console.printf("Auto-connect: %s, MAC: %s\n",
                  networkConfig.autoConnectEnabled ? "enabled" : "disabled",
                  networkConfig.autoConnectAddress.length()
                      ? networkConfig.autoConnectAddress.c_str() : "(none)");
    Console.printf("Auto-reconnect: %s, failures: %d, next backoff: %lu ms\n",
                  bleReconnectEnabled() ? "enabled" : "disabled",
                  bleConsecutiveFailures(), bleReconnectBackoffMs());
    Console.printf("Debug flags: ble=%s casambi=%s\n",
                  bleDebugEnabled ? "on" : "off", casambiDebugEnabled ? "on" : "off");

    // --- Client state ------------------------------------------------------
    Console.println("\n-- Link --");
    if (!casambiClient) {
        Console.println("No BLE client (setup mode)");
        Console.println();
        return;
    }

    Console.printf("State: %d (%s)\n", static_cast<int>(casambiClient->getState()),
                  casambiClient->isAuthenticated() ? "authenticated"
                      : (casambiClient->getState() == ConnectionState::None ? "disconnected"
                                                                            : "connecting"));
    Console.printf("Last connect phase: %s (NimBLE rc=%d)\n",
                  casambiClient->getLastConnectPhase(), casambiClient->getLastConnectError());
    Console.printf("Last disconnect: reason=%d/%s, source=%s\n",
                  static_cast<int>(casambiClient->getLastDisconnectReason()),
                  disconnectReasonName(casambiClient->getLastDisconnectReason()),
                  casambiClient->getLastDisconnectSource());
    Console.printf("Gateway: %s, rssi=%d dBm (accept threshold %d dBm)\n",
                  casambiClient->getConnectedAddress().length()
                      ? casambiClient->getConnectedAddress().c_str() : "(never connected)",
                  casambiClient->getLastRssi(), BLE_MIN_CONNECT_RSSI);
    Console.printf("Packets received: %u, link uptime: %lus, offline for: %lus\n",
                  casambiClient->getReceivedPacketCount(),
                  casambiClient->getConnectionUptime() / 1000,
                  bleLostAtMs() ? (millis() - bleLostAtMs()) / 1000 : 0);

    // --- What is actually out there ----------------------------------------
    Console.println("\n-- Advertisement probe --");
    if (casambiClient->isAuthenticated()) {
        // Scanning while connected can disturb a healthy link for no benefit.
        Console.println("Link is up - skipping the scan.");
        Console.println();
        return;
    }

    Console.println("Scanning 5 s for Casambi advertisers...");
    std::vector<CasambiScanResult> seen;
    CasambiScan::run(5, seen);
    esp_task_wdt_reset();

    if (seen.empty()) {
        Console.println("NO Casambi device is advertising.");
        Console.println("  - are the lights powered on and in range?");
        Console.println("  - is a phone with the Casambi app (or another gateway) already connected?");
        Console.println("    a Casambi unit accepts only ONE central at a time");
        Console.println();
        return;
    }

    bool targetSeen = false;
    Console.printf("Found %d advertiser(s):\n", (int)seen.size());
    for (const auto& r : seen) {
        bool isTarget = networkConfig.autoConnectAddress.length() &&
                        r.mac.equalsIgnoreCase(networkConfig.autoConnectAddress);
        targetSeen = targetSeen || isTarget;
        Console.printf("  %s type=%s rssi=%d name='%s'%s\n",
                      r.mac.c_str(), CasambiScan::addrTypeName(r.addrType), r.rssi,
                      r.name.c_str(), isTarget ? "  <-- configured gateway" : "");
        if (r.mfgData.length()) Console.printf("      mfg: %s\n", r.mfgData.c_str());
        if (r.svcData.length()) Console.printf("      svc: %s\n", r.svcData.c_str());
    }

    if (networkConfig.autoConnectAddress.length() && !targetSeen) {
        Console.printf("\n*** Configured MAC %s is NOT among them ***\n",
                      networkConfig.autoConnectAddress.c_str());
        Console.println("Every unit of a network advertises its own address, so a stored MAC");
        Console.println("disappears when that particular unit is switched off. Fix with:");
        Console.println("  scan, then connect <n>   (stores the MAC of a unit that is present)");
    }
    Console.println();
}

// ============================================================================
// VERSION CHECKS
// ============================================================================

// Warn on boot/refresh if protocol version or any unit firmware is below minimum.
// Unit firmware format: "Evolution/48.2" — we parse the numeric part after '/'.
void checkCasambiVersions(const NetworkConfig& cfg) {
    // Check Casambi BLE protocol version
    if (cfg.protocolVersion < MIN_PROTOCOL_VERSION) {
        Console.printf("*** WARNING: Casambi protocol v%d is below minimum v%d! ***\n",
                      cfg.protocolVersion, MIN_PROTOCOL_VERSION);
        Console.println("*** Update Casambi firmware or check network configuration. ***");
    } else if (cfg.protocolVersion > MAX_PROTOCOL_VERSION) {
        Console.printf("*** WARNING: Casambi protocol v%d exceeds maximum v%d — may be incompatible! ***\n",
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
            Console.printf("*** WARNING: Unit '%s' (id=%d) firmware %.1f < minimum %.1f ***\n",
                          unit.name.c_str(), unit.deviceId,
                          fwVersion, MIN_UNIT_FIRMWARE_VERSION);
        }
    }
}

