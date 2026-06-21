# Konzept: ESP32-Casambi-Provisionierung ohne Serial

Status: Konzept (noch keine Implementierung)
Branch: `claude/esp32-ap-rest-config-hejbf7`

## 1. Ziel

Nach dem Flashen soll **keine Konfiguration per Serial-Eingabe** mehr nötig sein:

1. **WiFi** wird per **offenem SoftAP + Captive Portal** eingerichtet.
2. Die übrigen Konfigurationsdaten (Casambi-Netz + Passwort) liefert das
   **FHEM-Modul** über die **Web-/REST-API**, sobald es den ESP32 im Heimnetz
   erreichen kann.

Der bestehende Serial-Wizard bleibt als **Fallback** erhalten.

## 2. Ausgangslage (Ist-Zustand)

- Erstkonfiguration läuft komplett über Serial (`runSetupWizard`, `main.cpp:1297`):
  BLE-Scan → Auswahl → Casambi-Passwort → WiFi-SSID/Passwort → Cloud-Download →
  speichern → Reboot.
- WiFi (STA) und Webserver starten erst, **wenn** bereits eine gültige Config
  vorliegt. Ohne Config gibt es kein WiFi und damit keine Web-/REST-Oberfläche.
- Das FHEM-Modul `98_CasambiGW.pm` verbindet sich per WebSocket zu einer fest
  einkonfigurierten IP (`define <name> CasambiGW <ip>`) und steuert über
  HTTP-POST an `/api/units/...`.

Die Cloud-Schritte sind bereits Serial-unabhängig gekapselt
(`api_client.cpp`: `getNetworkId`, `createSession`, `fetchNetworkConfig`) und
lassen sich hinter REST-Endpunkte hängen.

## 3. Randbedingungen

### 3.1 Heap vs. BLE beim Cloud-Zugriff
Der TLS-Handshake zur Casambi-Cloud braucht einen großen zusammenhängenden
Heap-Block, der nur frei ist, wenn der BLE-Stack **nicht** initialisiert ist
(siehe Kommentare bei `refresh`, `main.cpp:738`, und im Wizard). Während der
Cloud-Schritt läuft, muss BLE also deinitialisiert sein.

### 3.2 Woher kommt die `networkUuid`?
Der Cloud-Zugriff braucht aus dem BLE-Scan **genau eine** Information: die
`networkUuid`. Diese ist aktuell die **BLE-MAC** des gewählten Geräts
(`main.cpp:1353`, Doppelpunkte entfernt, lowercase). Der erste Cloud-Call
`GET /networks/uuid/<networkUuid>` (`api_client.cpp:45`) nimmt diese UUID als
Eingabe. Es gibt keine andere Quelle für die UUID als den Scan.

→ Daraus folgt zwingend die Reihenfolge **Scan vor Cloud**.

## 4. Korrigierte Reihenfolge

```
1. WiFi            (Phase 1, offener SoftAP + Captive Portal)
2. BLE-SCAN        (Discovery → networkUuid-Kandidaten zur Auswahl)   ← BLE an
   → BLEDevice::deinit()                                              ← BLE aus (Heap frei)
3. CASAMBI-CLOUD   (uuid + Passwort → Keys/Config)                    ← BLE aus, TLS braucht Heap
   → save + reboot
4. BLE-VERBINDUNG  (authentifiziert, mit Cloud-Keys)                  ← Betriebsmodus
```

Wichtige Unterscheidung: **BLE-Scan ≠ BLE-Verbindung.**

| BLE-Aktivität | braucht | liefert | Zeitpunkt |
|---|---|---|---|
| **Scan** (Discovery, passiv) | nichts | `networkUuid` (+ Name/RSSI zur Auswahl) | **vor** Cloud |
| **Verbindung** (authentifiziert) | Cloud-**Keys** | Steuerung der Lampen | **nach** Cloud (Betrieb) |

Scan und Verbindung liegen bewusst um den Cloud-Schritt herum: Scan davor
(liefert die UUID), Verbindung danach (braucht die Keys). Das entspricht der
Sequenz, die der heutige Serial-Wizard bereits nutzt.

## 5. Boot-Zustandsmaschine (3 Zustände)

`setup()` entscheidet anhand des Speicherzustands:

| Zustand | Bedingung | Modus | BLE |
|---|---|---|---|
| **A. Unprovisioniert** | keine WiFi-Credentials | SoftAP (offen) + Captive Portal | aus |
| **B. WiFi ok, keine Casambi-Config** | WiFi vorhanden, `!hasValidConfig()` | Setup-Modus (STA) + mDNS + REST | temporär an (nur Scan) |
| **C. Vollständig** | WiFi + gültige Config | Betriebsmodus (heute) | an |

## 6. Phase 1 — WiFi per offenem SoftAP + Captive Portal

Trigger: keine `wifi.json` vorhanden.

1. `WiFi.softAP("Casambi-Setup-XXXX")` **ohne Passwort** (offener AP).
   XXXX = 4 Hex-Ziffern aus `ESP.getEfuseMac()`.
2. `DNSServer` auf Port 53 → alle Anfragen auf `192.168.4.1` umleiten
   (Captive-Portal-Effekt).
3. HTML-Formular (`PROGMEM`, kein LittleFS-Upload nötig):
   - `GET /` → WLAN-Liste via `WiFi.scanNetworks()` + Passwortfeld.
   - `POST /wifi` `{ssid, password}` → `ConfigStore::saveWiFiCredentials()`
     → „gespeichert, Neustart" → `ESP.restart()`.
4. Nach Reboot: WiFi-Creds vorhanden → Zustand B.

Neue Dateien: `src/web/setup_portal.{h,cpp}`.

## 7. Phase 2 — Casambi-Setup über REST, getrieben von FHEM

Trigger: WiFi verbunden, aber `!ConfigStore::hasValidConfig()`. BLE startet
**nicht** automatisch — nur kurz für den Scan.

### 7.1 Lange Operationen laufen in `loop()`, nicht im Async-Handler
BLE-Scan (~10 s) und Cloud-Fetch (mehrere Sekunden HTTPS) dürfen den
AsyncTCP-Task der Webserver-Handler nicht blockieren. Daher:

- REST-Handler setzen nur ein **Auftrags-Flag** + Parameter und kehren sofort
  zurück.
- Eine **Setup-Zustandsmaschine in `loop()`** erledigt die Arbeit (mit
  `esp_task_wdt_reset()`).
- FHEM **pollt** den Fortschritt über `/api/setup/status`.

### 7.2 REST-Endpunkte (Setup-Modus)

| Methode | Pfad | Body / Antwort | Zweck |
|---|---|---|---|
| `GET` | `/api/info` | `{configured:false, build, hostname, mac, ip}` | Gerät identifizieren / Setup-Status |
| `POST` | `/api/setup/scan` | `202 Accepted` | BLE-Scan anstoßen |
| `GET` | `/api/setup/scan` | `{state:"scanning\|done", devices:[…]}` | Scan-Ergebnis abholen |
| `POST` | `/api/setup/network` | `{networkUuid, casambiPassword}` → `202` | Cloud-Fetch anstoßen |
| `GET` | `/api/setup/status` | `{state, msg, networkName?}` | Fortschritt / Ergebnis |

### 7.3 Scan-Ergebnis (pro gefundenem Gerät)

```json
{
  "uuid": "a1b2c3d4e5f6",      // MAC ohne Doppelpunkte = networkUuid-Kandidat
  "mac":  "a1:b2:c3:d4:e5:f6",
  "name": "Wohnzimmer",        // Advertised Name (oft = Netzwerkname)
  "rssi": -62,
  "mfgData": "...",            // Manufacturer Data (hex), falls vorhanden
  "svcData": "..."             // Service Data (hex), falls vorhanden
}
```

`ScanCallbacks::onResult` wird dafür erweitert (heute nur address/name/rssi) um
`getManufacturerData()` / `getServiceData()`. Name/RSSI helfen bei der Auswahl,
mfg/svc-Data als zusätzliches Unterscheidungsmerkmal.

### 7.4 Ablauf der Setup-Zustandsmaschine
1. **idle** → Webserver + mDNS aktiv, BLE aus.
2. `POST /api/setup/scan` → **scanning**: `BLEDevice::init()` → 10 s Scan →
   Liste füllen → **`BLEDevice::deinit(true)`** → **scan_done**.
3. FHEM holt Ergebnis (`GET /api/setup/scan`), Nutzer wählt Gateway.
4. `POST /api/setup/network {networkUuid, casambiPassword}` → **fetching**
   (BLE ist aus → Heap frei):
   - `getNetworkId(uuid)` → `createSession(pw)` → `fetchNetworkConfig()`
   - `networkUuid/networkId/casambiPassword` setzen
   - **`autoConnectAddress` = MAC aus `uuid`** (Doppelpunkte einfügen),
     `autoConnectEnabled = true`
   - `saveNetworkConfig()` → **done** → Antwort → `ESP.restart()` → Zustand C
5. Fehler → **error** mit Meldung in `/api/setup/status`; BLE bleibt aus, kein
   Reboot, FHEM kann erneut scannen/senden.

## 8. Mehrere Casambi-Geräte / Robustheit

### 8.1 Warum oft nur ein Gerät erscheint
Es ist **primär eine Casambi-Mesh-Eigenschaft**, kein Firmware-Limit: Die
Teilnehmer eines Casambi-Netzes handeln aus, **welche Einheit gerade als BLE-
Gateway „connectable" advertised** — meist nur eine zur Zeit. Der Scan sammelt
aber bereits **alle** advertisenden Casambi-Geräte in `scannedDevices`
(dedupliziert per MAC); mehrere Einträge erscheinen, sobald mehrere gleichzeitig
advertisen.

### 8.2 Robustheitsproblem mit fester MAC
Die Firmware merkt sich Identität **und** Auto-Connect-Ziel als **eine feste
MAC** (`networkUuid` = `autoConnectAddress` = MAC). Schaltet man genau diese
Einheit aus, übernimmt eine andere Einheit das Advertising — **mit anderer
MAC** — und der Reconnect auf die feste MAC schlägt fehl, obwohl das Netz noch
erreichbar wäre.

### 8.3 Geplante Maßnahmen
- **Auswahl:** Im Setup alle gefundenen Casambi-Geräte an FHEM melden (Liste
  existiert bereits), nicht nur das erste.
- **Robusterer Reconnect (Verbesserung):** Beim Verbinden nicht strikt an die
  gespeicherte MAC pinnen, sondern auf **irgendeine gerade advertisende Einheit
  desselben Netzes** gehen. Dafür müsste das Netz über die **Service-Data im
  Advertisement** identifiziert werden statt über die MAC (siehe offener Punkt
  9.1).

## 9. Offene Verifikationspunkte

1. **`networkUuid` = MAC oder Service-Data?** Aktuell wird die Geräte-MAC als
   `networkUuid` genutzt und funktioniert für den Cloud-Lookup. Ob die Cloud
   stattdessen einen aus der Service-Data abgeleiteten Netz-Identifier
   akzeptiert (was MAC-unabhängigen Reconnect ermöglichen würde), ist an echter
   Hardware zu prüfen. Davon hängt ab, ob Abschnitt 8.3 (robuster Reconnect)
   vollständig umsetzbar ist.
2. **Scan-Dauer/Trigger:** fester 10-s-Scan mit Polling — vorgeschlagen.
3. **Setup-Endpunkte nach Abschluss:** in Zustand C deaktivieren (nur
   `/api/info` behalten) plus optional `set <gw> reconfigure`, das ohne
   `clearconfig` zurück in den Setup-Modus schaltet.

## 10. mDNS / Discovery (mehrere Gateways)

- **Eindeutiger Hostname:** `casambi-XXXX` (XXXX = 4 Hex aus
  `ESP.getEfuseMac()`) → `casambi-a1b2.local`.
- mDNS-Service `_http._tcp` (Port 80) mit TXT-Records: `configured=0|1`,
  `build=<n>`, `network=<name>` (nach Config) → mehrere ESPs klar
  unterscheidbar.
- **FHEM-Define** akzeptiert Hostname **oder** IP:
  - `define gw1 CasambiGW casambi-a1b2.local`
  - `define gw1 CasambiGW 192.168.178.111` (empfohlen mit DHCP-Reservierung am
    Router über die ESP-MAC)
- Gleicher Präfix für SoftAP-SSID (`Casambi-Setup-XXXX`).

## 11. FHEM-Modul-Erweiterungen (`98_CasambiGW.pm`)

1. **Statuserkennung:** Beim Verbindungsaufbau zuerst `GET /api/info`. Bei
   `configured:false` → Setup-Zustand statt WebSocket-Handshake; Reading
   `setupState`.
2. **Neue Set-Befehle:**
   - `set <gw> scanNetworks` → `POST /api/setup/scan`, dann Polling von
     `GET /api/setup/scan`; Ergebnis als Reading `casambiNetworks`
     (Liste „uuid – name (rssi)").
   - `set <gw> casambiSetup <uuid> <password>` → `POST /api/setup/network`,
     danach Polling `GET /api/setup/status`; bei `done` Reboot abwarten und
     automatisch auf WebSocket umschalten.
3. **Mehrere Gateways:** über getrennte FHEM-Devices mit je eigener
   IP/Hostname; keine Sonderlogik nötig.
4. Nach `done` + Reboot erkennt FHEM `configured:true` und baut die
   WebSocket-Verbindung wie gehabt auf.

## 12. ESP-Firmware-Änderungen im Überblick

1. `main.cpp` — `setup()` als 3-Zustands-Maschine; Setup-Zustandsmaschine
   (idle/scanning/scan_done/fetching/done/error) in `loop()`. Serial-Wizard
   bleibt als Fallback.
2. `src/web/setup_portal.{h,cpp}` (neu) — offener SoftAP, DNSServer, HTML.
3. `webserver.{h,cpp}` — Setup-Modus (Client darf `null` sein), neue
   `/api/info` + `/api/setup/*`-Routen, mDNS.
4. `ScanCallbacks` erweitern (mfg/svc-Data); Scan-Ergebnis als gemeinsame
   Struktur für Serial- und REST-Pfad.
5. Cloud-Schritte aus `runSetupWizard()` in
   `provisionFromCloud(uuid, pw, &cfg)` herauslösen (von REST **und** Serial
   genutzt).
6. `config.h` — Präfixe/Konstanten (AP-SSID, mDNS-Hostname).
7. `98_CasambiGW.pm` — `/api/info`-Erkennung, `scanNetworks` + `casambiSetup`,
   neue Readings.
8. README aktualisieren.

## 13. Umsetzungsreihenfolge

1. `provisionFromCloud()` aus dem Wizard herauslösen (Refactor, verhaltensneutral).
2. Zustandsmaschine in `setup()`.
3. Setup-REST-Endpunkte + mDNS in `webserver.cpp`.
4. SoftAP-/Captive-Portal-Modul.
5. FHEM-Modul: `/api/info`-Erkennung + Setup-Set-Befehle.
6. README aktualisieren.
