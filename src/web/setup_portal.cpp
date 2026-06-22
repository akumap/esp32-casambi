/**
 * Setup Portal - implementation
 */

#include "setup_portal.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

#include "../config.h"
#include "../cloud/api_client.h"
#include "../storage/config_store.h"

// Seconds spent in each BLE discovery scan.
#define PORTAL_BLE_SCAN_SECONDS   8
// How long to wait for the home WLAN to connect during provisioning.
#define PORTAL_WIFI_TIMEOUT_MS    15000

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static String deviceSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[5];
    sprintf(buf, "%04x", (unsigned)(mac & 0xFFFF));
    return String(buf);
}

// "a1b2c3d4e5f6" → "a1:b2:c3:d4:e5:f6"
static String macFromUuid(const String& uuid) {
    String mac;
    for (size_t i = 0; i + 1 < uuid.length(); i += 2) {
        if (i) mac += ":";
        mac += uuid.substring(i, i + 2);
    }
    mac.toLowerCase();
    return mac;
}

// ---------------------------------------------------------------------------
// Portal HTML (single page, served from PROGMEM)
// ---------------------------------------------------------------------------

static const char PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Casambi Setup</title>
<style>
body{font-family:sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#f4f4f4}
h1{font-size:1.3em}fieldset{margin:12px 0;border:1px solid #ccc;border-radius:8px;background:#fff}
legend{font-weight:bold}label{display:block;margin:8px 0 2px}
select,input,button{width:100%;padding:8px;box-sizing:border-box;margin-top:4px;font-size:1em}
button{background:#0a6;color:#fff;border:0;border-radius:6px;cursor:pointer;margin-top:10px}
button.sec{background:#37a}#status{white-space:pre-wrap;margin-top:12px;font-weight:bold}
small{color:#666}
</style></head><body>
<h1>Set up Casambi gateway</h1>
<fieldset><legend>1. Wi-Fi</legend>
<button class="sec" onclick="scanWifi()">Scan Wi-Fi</button>
<label>Network</label><select id="ssid"><option value="">– scan first –</option></select>
<label>Wi-Fi password</label><input id="wpw" type="password">
</fieldset>
<fieldset><legend>2. Casambi network</legend>
<button class="sec" onclick="scanBle()">Scan Casambi gateways</button>
<label>Gateway / network</label><select id="net"><option value="">– scan first –</option></select>
<small>If several appear, just enter the password – only the matching network authenticates.</small>
<label>Casambi network password</label><input id="cpw" type="password">
</fieldset>
<button onclick="provision()">Set up</button>
<div id="status"></div>
<script>
const S=document.getElementById('status');
function j(u,o){return fetch(u,o).then(r=>r.json())}
async function scanWifi(){
  S.textContent='Scanning for Wi-Fi…';
  for(let i=0;i<15;i++){
    let d=await j('/api/wifi-scan');
    if(d.state==='done'){
      let s=document.getElementById('ssid');s.innerHTML='';
      d.networks.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
        let o=document.createElement('option');o.value=n.ssid;
        o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.enc?'':' [open]');s.appendChild(o)});
      S.textContent=d.networks.length+' Wi-Fi network(s) found.';return;
    }
    await new Promise(r=>setTimeout(r,1000));
  }
  S.textContent='Wi-Fi scan is taking too long – please try again.';
}
async function scanBle(){
  S.textContent='Scanning for Casambi (approx. 10 s)…';
  await j('/api/ble-scan',{method:'POST'});
  for(let i=0;i<20;i++){
    await new Promise(r=>setTimeout(r,1500));
    let d=await j('/api/ble-scan');
    if(d.state==='done'){
      let s=document.getElementById('net');s.innerHTML='';
      if(!d.devices.length){S.textContent='No Casambi gateway found.';return;}
      d.devices.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
        let o=document.createElement('option');o.value=n.uuid;
        o.textContent=(n.name||n.uuid)+' ('+n.rssi+' dBm)';s.appendChild(o)});
      S.textContent=d.devices.length+' gateway(s) found.';return;
    }
  }
  S.textContent='Casambi scan is taking too long – please try again.';
}
async function provision(){
  let body={ssid:document.getElementById('ssid').value,
    wifiPassword:document.getElementById('wpw').value,
    casambiPassword:document.getElementById('cpw').value,
    networkUuid:document.getElementById('net').value};
  if(!body.ssid){S.textContent='Please select a Wi-Fi network.';return;}
  if(!body.casambiPassword){S.textContent='Please enter the Casambi password.';return;}
  S.textContent='Setup started…';
  await j('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)});
  const T={connecting_wifi:'Connecting to Wi-Fi…',fetching_cloud:'Downloading configuration from the cloud…'};
  for(let i=0;i<120;i++){
    await new Promise(r=>setTimeout(r,2000));
    let d=await j('/api/provision/status');
    if(d.state==='done'){S.textContent='Done! Network: '+(d.networkName||'?')+
      '\nThe device is restarting.';return;}
    if(d.state==='error'){S.textContent='Error: '+(d.msg||'unknown');return;}
    S.textContent=T[d.state]||('Status: '+d.state);
  }
  S.textContent='Timed out – please check the status.';
}
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SetupPortal::SetupPortal()
    : _server(nullptr), _bleInited(false),
      _scanRequested(false), _scan(ScanState::Idle),
      _provisionRequested(false), _prov(ProvState::Idle),
      _rebootAt(0) {}

SetupPortal::~SetupPortal() {
    if (_server) {
        _server->end();
        delete _server;
        _server = nullptr;
    }
    _dns.stop();
}

bool SetupPortal::begin() {
    String ssid = "Casambi-Setup-" + deviceSuffix();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid.c_str());          // open AP, no password
    IPAddress ip = WiFi.softAPIP();

    _dns.start(53, "*", ip);            // captive portal: catch every host

    _server = new AsyncWebServer(80);
    _setupRoutes();
    _server->begin();

    Serial.println("\n=== Setup Portal active ===");
    Serial.printf("Connect to open WiFi '%s', then open http://%s/\n",
                  ssid.c_str(), ip.toString().c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

void SetupPortal::_setupRoutes() {
    // Portal page (and captive-portal catch-all)
    auto sendPage = [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", PORTAL_HTML);
    };
    _server->on("/", HTTP_GET, sendPage);
    _server->onNotFound(sendPage);

    _server->on("/api/info", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument d;
        d["configured"] = false;
        d["build"]      = FIRMWARE_BUILD;
        d["hostname"]   = "casambi-" + deviceSuffix();
        d["mac"]        = WiFi.macAddress();
        d["ip"]         = WiFi.softAPIP().toString();
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    _server->on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = WiFi.scanComplete();
        JsonDocument d;
        if (n == WIFI_SCAN_FAILED) {        // -2: not started yet
            WiFi.scanNetworks(true);
            d["state"] = "scanning";
        } else if (n == WIFI_SCAN_RUNNING) {  // -1
            d["state"] = "scanning";
        } else {
            d["state"] = "done";
            JsonArray a = d["networks"].to<JsonArray>();
            for (int i = 0; i < n; i++) {
                JsonObject o = a.add<JsonObject>();
                o["ssid"] = WiFi.SSID(i);
                o["rssi"] = WiFi.RSSI(i);
                o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            }
            WiFi.scanDelete();
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    _server->on("/api/ble-scan", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (_scan != ScanState::Running) {
            _scan = ScanState::Idle;
            _scanRequested = true;
        }
        req->send(202, "application/json", "{\"state\":\"accepted\"}");
    });
    _server->on("/api/ble-scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", _bleScanJson());
    });

    _server->on("/api/provision/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", _statusJson());
    });

    // Provisioning request carries a JSON body → parse in the body handler.
    _server->on("/api/provision", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            // Reached only when there was no body to consume.
            if (req->contentLength() == 0) {
                req->send(400, "application/json", "{\"error\":\"empty body\"}");
            }
        },
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len,
               size_t index, size_t total) {
            if (index == 0) _provBody = "";
            for (size_t i = 0; i < len; i++) _provBody += (char)data[i];
            if (index + len < total) return;   // wait for the rest

            JsonDocument d;
            DeserializationError err = deserializeJson(d, _provBody);
            _provBody = "";
            if (err) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            _ssid       = d["ssid"]            | "";
            _wifiPw     = d["wifiPassword"]    | "";
            _casambiPw  = d["casambiPassword"] | "";
            _chosenUuid = d["networkUuid"]     | "";

            if (_ssid.isEmpty() || _casambiPw.isEmpty()) {
                req->send(400, "application/json", "{\"error\":\"missing fields\"}");
                return;
            }

            _prov = ProvState::Connecting;
            _provMsg = "";
            _provNetworkName = "";
            _provisionRequested = true;
            req->send(202, "application/json", "{\"state\":\"accepted\"}");
        });
}

// ---------------------------------------------------------------------------
// State machine (runs from loop())
// ---------------------------------------------------------------------------

void SetupPortal::loop() {
    _dns.processNextRequest();

    if (_scanRequested && _scan != ScanState::Running) {
        _scanRequested = false;
        _runScan();
    }

    if (_provisionRequested) {
        _provisionRequested = false;
        _runProvision();
    }

    if (_prov == ProvState::Done && _rebootAt && millis() >= _rebootAt) {
        Serial.println("Setup complete - restarting into operation mode");
        delay(200);
        ESP.restart();
    }
}

void SetupPortal::_runScan() {
    _scan = ScanState::Running;
    Serial.println("Portal: BLE scan...");

    // Keep BLE initialised across repeated scans. Releasing the controller
    // memory (deinit(true)) cannot be undone within the same boot, so doing it
    // here would make every scan after the first one find nothing. The memory
    // is released later, once, right before the cloud TLS handshake.
    if (!_bleInited) {
        BLEDevice::init("Casambi-Setup");
        _bleInited = true;
    }
    CasambiScan::run(PORTAL_BLE_SCAN_SECONDS, _scanResults);

    Serial.printf("Portal: BLE scan done, %d device(s)\n", _scanResults.size());
    _scan = ScanState::Done;
}

void SetupPortal::_runProvision() {
    _prov = ProvState::Connecting;

    // Persist WLAN credentials early so a later operation boot can reconnect.
    WiFiCredentials wc;
    wc.ssid = _ssid;
    wc.password = _wifiPw;
    ConfigStore::saveWiFiCredentials(wc);

    // Release the BLE controller memory now (once) so the TLS handshake has a
    // large enough contiguous heap block. After this BLE is unusable until the
    // next reboot — fine, we reboot into operation mode on success.
    if (_bleInited) {
        BLEDevice::deinit(true);
        _bleInited = false;
        delay(100);
    }

    // Bring up STA alongside the AP so the portal page stays reachable.
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(_ssid.c_str(), _wifiPw.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < PORTAL_WIFI_TIMEOUT_MS) {
        delay(100);
        esp_task_wdt_reset();
    }
    if (WiFi.status() != WL_CONNECTED) {
        _prov = ProvState::Error;
        _provMsg = "WLAN-Verbindung fehlgeschlagen";
        return;
    }
    Serial.printf("Portal: WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());

    _prov = ProvState::Fetching;

    // Candidate uuids: the chosen one, or every scanned device (password probe).
    std::vector<String> candidates;
    if (!_chosenUuid.isEmpty()) {
        candidates.push_back(_chosenUuid);
    } else {
        for (const auto& r : _scanResults) candidates.push_back(r.uuid);
    }
    if (candidates.empty()) {
        _prov = ProvState::Error;
        _provMsg = "Kein Casambi-Gateway gefunden";
        return;
    }

    CasambiAPIClient api;
    NetworkConfig cfg;
    String usedUuid, lastErr;
    bool ok = false;

    for (const auto& uuid : candidates) {
        esp_task_wdt_reset();
        String nid;
        if (!api.getNetworkId(uuid, nid)) { lastErr = api.getLastError(); continue; }
        esp_task_wdt_reset();
        String token;
        if (!api.createSession(nid, _casambiPw, token)) { lastErr = api.getLastError(); continue; }
        esp_task_wdt_reset();
        NetworkConfig tmp;
        if (!api.fetchNetworkConfig(nid, token, tmp)) { lastErr = api.getLastError(); continue; }
        cfg = tmp;
        cfg.networkId   = nid;
        cfg.networkUuid = uuid;
        usedUuid = uuid;
        ok = true;
        break;
    }

    if (!ok) {
        _prov = ProvState::Error;
        _provMsg = "Authentifizierung/Cloud fehlgeschlagen";
        if (lastErr.length()) _provMsg += ": " + lastErr;
        return;
    }

    cfg.casambiPassword    = _casambiPw;
    cfg.autoConnectAddress = macFromUuid(usedUuid);   // preferred first target
    cfg.autoConnectEnabled = true;

    // Remember the advertised name of the chosen gateway (the connected BLE
    // address is usually a random static address and won't resolve to a unit).
    for (const auto& r : _scanResults) {
        if (r.uuid == usedUuid) { cfg.gatewayName = r.name; break; }
    }

    if (!ConfigStore::saveNetworkConfig(cfg)) {
        _prov = ProvState::Error;
        _provMsg = "Speichern fehlgeschlagen";
        return;
    }

    _provNetworkName = cfg.networkName;
    _provMsg = "Fertig";
    _prov = ProvState::Done;
    _rebootAt = millis() + 4000;       // let the browser poll the success state
    Serial.printf("Portal: provisioned network '%s'\n", cfg.networkName.c_str());
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------

String SetupPortal::_bleScanJson() const {
    JsonDocument d;
    d["state"] = (_scan == ScanState::Done) ? "done"
               : (_scan == ScanState::Running || _scanRequested) ? "scanning"
               : "idle";
    if (_scan == ScanState::Done) {
        JsonArray a = d["devices"].to<JsonArray>();
        for (const auto& r : _scanResults) {
            JsonObject o = a.add<JsonObject>();
            o["uuid"] = r.uuid;
            o["mac"]  = r.mac;
            o["name"] = r.name;
            o["rssi"] = r.rssi;
            if (r.mfgData.length()) o["mfgData"] = r.mfgData;
            if (r.svcData.length()) o["svcData"] = r.svcData;
        }
    }
    String out; serializeJson(d, out);
    return out;
}

String SetupPortal::_statusJson() const {
    const char* s =
        _prov == ProvState::Connecting ? "connecting_wifi" :
        _prov == ProvState::Fetching   ? "fetching_cloud"  :
        _prov == ProvState::Done       ? "done"            :
        _prov == ProvState::Error      ? "error"           : "idle";
    JsonDocument d;
    d["state"] = s;
    if (_provMsg.length())         d["msg"] = _provMsg;
    if (_provNetworkName.length()) d["networkName"] = _provNetworkName;
    String out; serializeJson(d, out);
    return out;
}
