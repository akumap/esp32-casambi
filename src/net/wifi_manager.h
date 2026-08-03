/**
 * WiFi station management: credentials cache, disconnect diagnostics, reconnect
 *
 * Split out of main.cpp.
 *
 * CONCURRENCY
 * -----------
 * This module is the one place in the firmware where state is genuinely
 * written from two tasks, so the split had to preserve that carefully:
 *
 *   - onWiFiEvent() runs on the ARDUINO WIFI EVENT TASK, not the loop task.
 *   - checkAndReconnectWiFi() runs on the loop task.
 *
 * The three variables both touch (last RSSI, disconnect reason, outage start)
 * are therefore std::atomic and stay that way. Everything else here is
 * loop-task only. Do not add a plain (non-atomic) field that onWiFiEvent
 * writes — on the dual-core targets the event task and the loop task run on
 * different cores in parallel.
 */

#ifndef NET_WIFI_MANAGER_H
#define NET_WIFI_MANAGER_H

#include <Arduino.h>
#include "../storage/config_store.h"

// Register the event handler that records WHY the link dropped. Call once,
// before WiFi.begin().
void wifiInstallEventHandler();

// Periodic check + non-blocking reconnect nudge. Cheap to call every loop
// iteration; it rate-limits itself to WIFI_RECONNECT_INTERVAL_MS.
void checkAndReconnectWiFi();

// Cached credentials, loaded once at boot to avoid repeated LittleFS reads in
// the reconnect path. Returns false when nothing is stored.
bool wifiLoadCachedCredentials();
bool wifiHaveCachedCredentials();
const WiFiCredentials& wifiCachedCredentials();

// Marks the link as up without waiting for the next periodic check, so the
// boot path does not immediately log a spurious "connection lost".
void wifiNoteConnected();

// Reason code of the current disconnect episode (0 = link is up) and its
// human-readable name, for 'wifi status' and the diagnostics report.
uint8_t     wifiLastDisconnectReason();
const char* wifiDisconnectReasonName(uint8_t reason);

#endif // NET_WIFI_MANAGER_H
