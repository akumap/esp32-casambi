/**
 * WiFi station management — see wifi_manager.h.
 */

#include "wifi_manager.h"

#include <WiFi.h>
#include <esp_task_wdt.h>
#include <atomic>
#include "../config.h"
#include "../app_state.h"
#include "../log/event_log.h"
#include "../web/webserver.h"
#include "time_sync.h"

// Cached WiFi credentials — loaded once at boot and updated by 'wifi set'.
// Avoids repeated LittleFS reads in the 30 s reconnect loop.
static WiFiCredentials g_wifiCreds;
static bool g_wifiCredsLoaded = false;

// True while WiFi is known-up, so we only log the WiFi-loss event once per drop.
static bool g_wifiWasConnected = false;

// WiFi monitoring state
static unsigned long lastWiFiCheck = 0;

bool wifiLoadCachedCredentials() {
    g_wifiCredsLoaded = ConfigStore::loadWiFiCredentials(g_wifiCreds);
    return g_wifiCredsLoaded;
}

bool wifiHaveCachedCredentials() { return g_wifiCredsLoaded; }

const WiFiCredentials& wifiCachedCredentials() { return g_wifiCreds; }

void wifiNoteConnected() { g_wifiWasConnected = true; }

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

uint8_t wifiLastDisconnectReason() { return g_lastWifiDiscReason.load(); }

const char* wifiDisconnectReasonName(uint8_t reason) {
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

void wifiInstallEventHandler() {
    WiFi.onEvent(onWiFiEvent);
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
