/**
 * Casambi cloud-configuration refresh — see cloud_refresh.h.
 */

#include "cloud_refresh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "app_state.h"
#include "cloud/api_client.h"
#include "storage/config_store.h"
#include "log/event_log.h"
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
    wifiLoadCachedCredentials();

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

