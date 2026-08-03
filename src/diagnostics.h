/**
 * Diagnostics: heap monitoring, status reports, version checks
 *
 * Split out of main.cpp. Everything here is read-only with respect to the
 * rest of the firmware — it samples state and prints it. The one exception is
 * monitorHeap(), which reboots the device when free heap falls below the
 * critical threshold; that is deliberate and is the reason it lives with the
 * heap tracking rather than in a pure "printers" module.
 *
 * CONCURRENCY: loop-task / serial-console only. The reports read state owned
 * by other tasks (BLE link state, WiFi counters) through the accessors those
 * modules expose, and take no locks of their own — they are diagnostics, so a
 * field sampled a few microseconds apart from its neighbour is acceptable and
 * must not be worth blocking the BLE or async_tcp task for.
 */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "cloud/network_config.h"

// Periodic heap sampling. Tracks the all-time minimum, logs on request
// ('debug heap on'), and restarts the device if free heap drops below
// HEAP_CRITICAL_THRESHOLD. Rate-limits itself, so it is cheap to call every
// loop iteration.
void monitorHeap();

// Lowest free heap observed since boot, for the status reports.
size_t minFreeHeapSeen();

// Seed the minimum at boot, before anything has had a chance to allocate.
void initHeapMonitor(size_t freeHeapAtBoot);

// 'status' — one-screen system overview (BLE, WiFi, web server, heap, uptime).
void printStatus();

// 'blediag' — BLE troubleshooting report: stored config, last connect phase
// and error, link quality, and a fresh advertisement probe.
void printBLEDiagnostics();

// Compares the Casambi protocol version of a freshly loaded config against the
// range this firmware is tested with, and warns on a mismatch.
void checkCasambiVersions(const NetworkConfig& cfg);

#endif // DIAGNOSTICS_H
