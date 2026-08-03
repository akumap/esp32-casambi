/**
 * Casambi cloud-configuration refresh
 *
 * Split out of main.cpp. A refresh is deliberately a TWO-PHASE operation and
 * this header is the place that says why:
 *
 *   requestCloudRefresh()    stores the request (+ password) and reboots.
 *   runScheduledCloudRefresh() runs early at the next boot and downloads.
 *
 * Doing the download in-place would tear down the BLE stack and the
 * AsyncWebServer while the async_tcp task may still be processing the very
 * connection that asked for the refresh — the use-after-free behind issue #21.
 * At boot neither subsystem is up, so the TLS download gets a clean, large
 * heap with no concurrent tasks. Both entry points (serial 'refresh' and
 * POST /api/refreshCasambi) therefore go through the reboot.
 *
 * CONCURRENCY: loop-task only. The web handler does NOT call in here; it sets
 * a flag that the loop consumes (webserver.cpp: consumeRefreshRequest).
 */

#ifndef CLOUD_REFRESH_H
#define CLOUD_REFRESH_H

#include <Arduino.h>

// Schedule a refresh and reboot. Does not return on success.
void requestCloudRefresh(const String& password);

// Boot-path hook: if a refresh was scheduled, download and save the config.
// Call before BLE and the web server are started.
void runScheduledCloudRefresh();

#endif // CLOUD_REFRESH_H
