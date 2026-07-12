/**
 * Setup Portal - open SoftAP + captive portal for first-time provisioning
 *
 * Started when no valid configuration exists. Serves a single-page web UI that
 * collects WLAN credentials, the Casambi network password and (when several
 * networks are visible) the gateway selection. On submit it switches to AP+STA,
 * frees BLE, downloads the network config from the Casambi cloud and reboots
 * into operation mode.
 *
 * Long-running work (BLE scan, WiFi connect, cloud fetch) is executed from
 * loop() — the async HTTP handlers only set requests and return immediately.
 *
 * HTTP endpoints:
 *   GET  /                      Single-page portal
 *   GET  /api/info              {configured:false, build, hostname, mac, ip}
 *   GET  /api/wifi-scan         async WLAN scan: {state, networks:[...]}
 *   POST /api/ble-scan          start BLE scan (202)
 *   GET  /api/ble-scan          {state, devices:[...]}
 *   POST /api/provision         {ssid, wifiPassword, casambiPassword, networkUuid?}
 *   GET  /api/provision/status  {state, msg?, networkName?}
 */

#ifndef SETUP_PORTAL_H
#define SETUP_PORTAL_H

#include <Arduino.h>
#include <DNSServer.h>
#include <vector>
#include "../ble/casambi_scan.h"

class AsyncWebServer;
class AsyncWebServerRequest;

class SetupPortal {
public:
    SetupPortal();
    ~SetupPortal();

    /** Start the open SoftAP, captive-portal DNS and the web server. */
    bool begin();

    /** Drive DNS + the provisioning state machine. Call from loop(). */
    void loop();

private:
    enum class ScanState { Idle, Running, Done };
    enum class ProvState { Idle, Connecting, Fetching, Done, Error };

    AsyncWebServer* _server;
    DNSServer       _dns;
    bool            _bleInited;       // BLE stack currently initialised?

    // Guards the members shared between the async_tcp request handlers and
    // the loop-task state machine: _scanResults and the provisioning Strings
    // below. Held only for short copy/swap sections, never across the scan or
    // the cloud requests themselves.
    SemaphoreHandle_t _mutex;

    // BLE scan
    volatile bool _scanRequested;
    volatile ScanState _scan;
    std::vector<CasambiScanResult> _scanResults;   // guarded by _mutex

    // Provisioning
    volatile bool _provisionRequested;
    volatile ProvState _prov;
    String        _provMsg;           // guarded by _mutex
    String        _provNetworkName;   // guarded by _mutex
    String        _ssid, _wifiPw, _casambiPw, _chosenUuid;  // guarded by _mutex
    unsigned long _rebootAt;

    void _setupRoutes();
    void _runScan();
    void _runProvision();

    String _bleScanJson() const;
    String _statusJson() const;
};

#endif // SETUP_PORTAL_H
