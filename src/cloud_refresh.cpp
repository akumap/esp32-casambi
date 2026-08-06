/**
 * Casambi cloud-configuration refresh — see cloud_refresh.h.
 */

#include "cloud_refresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "app_state.h"
#include "console_out.h"
#include "cloud/api_client.h"
#include "storage/config_store.h"
#include "log/event_log.h"
#include "net/telnet_console.h"
#include "net/wifi_manager.h"
#include "diagnostics.h"


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
        Console.println("ERROR: No network password available for refresh.");
        return;
    }

    // Persist the password to use after the reboot (saved one, or one newly
    // typed at the serial prompt).
    if (password != networkConfig.casambiPassword) {
        configLock();
        networkConfig.casambiPassword = password;
        configUnlock();
        if (!ConfigStore::saveNetworkConfig(networkConfig)) {
            Console.println("ERROR: Failed to store password for refresh");
            return;
        }
    }

    ConfigStore::setRefreshPending();
    Console.println("Cloud refresh scheduled. Restarting to apply...");
    // Close a telnet session cleanly first (E6) — the line above only reaches
    // the ring buffer, and nothing drains it once the reboot runs. Covers both
    // callers: the serial/telnet 'refresh' command and POST /api/refreshCasambi.
    telnetNotifyReboot("Cloud refresh scheduled. Restarting -- session closed.");
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
    Console.println("\n=== Scheduled Cloud Refresh ===");

    const String password = networkConfig.casambiPassword;
    if (password.length() == 0) {
        Console.println("ERROR: No stored network password; skipping refresh.");
        return;
    }

    // Bring up WiFi (nothing else is connected yet at this point in boot).
    WiFiCredentials wifiCreds;
    if (!ConfigStore::loadWiFiCredentials(wifiCreds)) {
        Console.println("ERROR: No WiFi credentials stored; skipping refresh.");
        return;
    }
    wifiLoadCachedCredentials();

    Console.printf("Connecting to WiFi: %s...\n", wifiCreds.ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiCreds.ssid.c_str(), wifiCreds.password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(100);
        esp_task_wdt_reset();
        Console.print(".");
    }
    Console.println();

    if (WiFi.status() != WL_CONNECTED) {
        Console.println("ERROR: WiFi connection failed; skipping refresh.");
        WiFi.disconnect(true);
        return;
    }
    Console.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // On any failure below, drop WiFi so setup() resumes from its normal
    // "BLE first, then WiFi" ordering with the existing config untouched.

    // Get network ID from stored UUID
    Console.println("--- Fetching network ID ---");
    String networkId;
    CasambiAPIClient tempClient;
    if (!tempClient.getNetworkId(networkConfig.networkUuid, networkId)) {
        Console.printf("ERROR: Failed to get network ID: %s\n", tempClient.getLastError().c_str());
        WiFi.disconnect(true);
        return;
    }

    // Create session
    Console.println("--- Creating session ---");
    String sessionToken;
    if (!tempClient.createSession(networkId, password, sessionToken)) {
        Console.printf("ERROR: Authentication failed: %s\n", tempClient.getLastError().c_str());
        WiFi.disconnect(true);
        return;
    }

    // Fetch fresh configuration
    Console.println("--- Downloading configuration ---");
    NetworkConfig freshConfig;
    if (!tempClient.fetchNetworkConfig(networkId, sessionToken, freshConfig)) {
        Console.printf("ERROR: Failed to fetch config: %s\n", tempClient.getLastError().c_str());
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
    Console.println("--- Saving to flash ---");
    if (!ConfigStore::saveNetworkConfig(freshConfig)) {
        Console.println("ERROR: Failed to save configuration; keeping existing config.");
        WiFi.disconnect(true);
        return;
    }

    Console.println("\n=== Refresh Complete! ===");
    Console.printf("Network: %s\n", freshConfig.networkName.c_str());
    Console.printf("Protocol: v%d (revision %d)\n", freshConfig.protocolVersion, freshConfig.revision);
    Console.printf("Units: %d\n", freshConfig.units.size());
    Console.printf("Groups: %d\n", freshConfig.groups.size());
    Console.printf("Scenes: %d\n", freshConfig.scenes.size());

    checkCasambiVersions(freshConfig);

    Console.println("\nConfiguration updated. Restarting to apply...");
    delay(2000);
    ESP.restart();
}

