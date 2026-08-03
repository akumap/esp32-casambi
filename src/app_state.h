/**
 * Shared application state
 *
 * main.cpp owns these objects; the modules split out of it (net/, ble/,
 * diagnostics, cloud_refresh, serial_console) reach them through here.
 *
 * This extends the pattern config.h already uses for the debug flags rather
 * than introducing a second one: the definitions stay in main.cpp, everyone
 * else sees an extern declaration.
 *
 * CONCURRENCY
 * -----------
 * These are plain globals, not synchronised containers. The rule that makes
 * that safe is unchanged by the split and must stay that way:
 *
 *   - The POINTERS (casambiClient, webServer, ...) are assigned during setup()
 *     on the loop task and then only read. Nothing re-seats them at runtime —
 *     that is why 'wifi set' reboots instead of rebuilding the web server
 *     (see cmdWifi), and why a cloud refresh reboots too.
 *   - The runtime-mutable String fields of networkConfig are guarded by
 *     g_configMutex. Every writer runs on the loop task; the async_tcp task
 *     copies them under the lock (webserver.cpp: ConfigLock/lockedCopy).
 *
 * So: hold g_configMutex around any String mutation in networkConfig, and do
 * not add code that swaps a pointer here while other tasks are running.
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>
#include "cloud/network_config.h"

class CasambiClient;
class CasambiAPIClient;
class CasambiWebServer;
class SetupPortal;

// ---------------------------------------------------------------------------
// Core objects (defined in main.cpp)
// ---------------------------------------------------------------------------

extern NetworkConfig     networkConfig;
extern CasambiClient*    casambiClient;
extern CasambiAPIClient* apiClient;
extern CasambiWebServer* webServer;
extern SetupPortal*      setupPortal;

// Guards the runtime-mutable NetworkConfig String fields. Non-null after
// setup(); the helpers below tolerate a null handle so early-boot callers
// (and the host tests) do not need a special case.
extern SemaphoreHandle_t g_configMutex;

// Take/give g_configMutex around a String mutation on the loop task. Prefer
// the RAII ConfigLock in webserver.cpp where an allocation can throw between
// the two; these are for the straight-line loop-task paths.
void configLock();
void configUnlock();

// ---------------------------------------------------------------------------
// BLE scan results (defined in main.cpp)
// ---------------------------------------------------------------------------

struct ScannedDevice {
    String address;
    String name;
    int rssi;
    // Advertised address type. The reconnect path always connects as "public",
    // so a device listed here as "random" can be discovered but never reached.
    uint8_t addrType;

    ScannedDevice() : rssi(0), addrType(0) {}
};

extern std::vector<ScannedDevice> scannedDevices;

#endif // APP_STATE_H
