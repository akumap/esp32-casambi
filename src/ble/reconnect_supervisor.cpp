/**
 * BLE auto-reconnect supervisor — see reconnect_supervisor.h.
 */

#include "reconnect_supervisor.h"

#include <WiFi.h>
#include "../config.h"
#include "../console_out.h"
#include "../app_state.h"
#include "../log/event_log.h"
#include "../web/webserver.h"
#include "../net/time_sync.h"
#include "casambi_client.h"

// BLE reconnect state
static unsigned long lastBLEReconnectAttempt = 0;
static unsigned long bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
static uint8_t consecutiveReconnectFailures = 0;
static bool g_bleReconnectEnabled = true;  // Can be disabled via command

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

bool          bleReconnectEnabled()    { return g_bleReconnectEnabled; }
uint8_t       bleConsecutiveFailures() { return consecutiveReconnectFailures; }
unsigned long bleReconnectBackoffMs()  { return bleReconnectInterval; }
unsigned long bleLostAtMs()            { return bleLostAt; }

void setBleReconnectEnabled(bool enabled) {
    g_bleReconnectEnabled = enabled;
    if (enabled) {
        consecutiveReconnectFailures = 0;
        bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
    }
}

// Runs on the NimBLE host task — timestamps and backoff only, nothing heavy.
void bleNoteLinkLost() {
    if (bleLostAt == 0) bleLostAt = millis();
    bleReconnectInterval = BLE_RECONNECT_INTERVAL_MS;
    lastBLEReconnectAttempt = millis();
}

void bleNoteConnected() {
    consecutiveReconnectFailures = 0;
    bleLostAt = 0;   // manual/auto recovery — don't attribute a later loss to it
}

void bleNoteConnectAttempt() {
    lastBLEReconnectAttempt = millis();
}


// Explain, at most once a minute, why the link is down and nothing is being
// done about it. `reason` is a static string.
static void noteBLEIdle(const char* reason, const char* remedy) {
    unsigned long now = millis();
    if (lastBLEIdleNotice != 0 && now - lastBLEIdleNotice < BLE_IDLE_NOTICE_INTERVAL_MS) return;
    lastBLEIdleNotice = now;

    Console.printf("BLE: not connected and NOT attempting to reconnect - %s (%s)\n", reason, remedy);
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

    if (!g_bleReconnectEnabled) {
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

    Console.printf("BLE: Auto-reconnect attempt #%d to %s (backoff: %lu ms, offline %lus, heap=%u)...\n",
                  consecutiveReconnectFailures + 1,
                  networkConfig.autoConnectAddress.c_str(),
                  bleReconnectInterval,
                  bleLostAt ? (millis() - bleLostAt) / 1000 : 0,
                  ESP.getFreeHeap());

    if (casambiClient->connect(networkConfig.autoConnectAddress)) {
        Console.println("BLE: Reconnect successful!");
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
                Console.printf("Web API restarted at: http://%s/api\n",
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
        Console.printf("BLE: Reconnect failed (#%d, %s, phase=%s, reason=%d/%s, rc=%d). "
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
            Console.println("*** Too many internal BLE failures! Restarting ESP32 ***");
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

