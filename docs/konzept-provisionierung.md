# Konzept: ESP32-Casambi-Provisionierung ohne Serial

Status: Konzept (noch keine Implementierung)
Branch: `claude/esp32-ap-rest-config-hejbf7`

## 1. Ziel

Nach dem Flashen soll **keine Konfiguration per Serial-Eingabe** mehr nötig
sein. Die **gesamte interaktive Erstkonfiguration** findet in **einer**
Weboberfläche statt — dem **offenen SoftAP-Portal** des ESP32:

- WLAN-Auswahl + WLAN-Passwort
- Casambi-Gateway-Auswahl (BLE-Scan) + Casambi-Netzwerk-Passwort

Der Casambi-Cloud-Abruf läuft direkt im Anschluss mit **Live-Fortschritt**.
FHEM verbindet sich danach nur noch mit dem **fertig konfigurierten** Gerät.

Der bestehende Serial-Wizard bleibt als **Fallback** erhalten.

## 2. Ausgangslage (Ist-Zustand)

- Erstkonfiguration läuft komplett über Serial (`runSetupWizard`, `main.cpp:1297`):
  BLE-Scan → Auswahl → Casambi-Passwort → WiFi-SSID/Passwort → Cloud-Download →
  speichern → Reboot.
- WiFi (STA) und Webserver starten erst, **wenn** bereits eine gültige Config
  vorliegt.
- FHEM-Modul `98_CasambiGW.pm` verbindet sich per WebSocket zu einer fest
  einkonfigurierten IP (`define <name> CasambiGW <ip>`) und steuert über
  HTTP-POST an `/api/units/...`.

Die Cloud-Schritte sind bereits Serial-unabhängig gekapselt
(`api_client.cpp`: `getNetworkId`, `createSession`, `fetchNetworkConfig`).

## 3. Randbedingungen

### 3.1 Heap vs. BLE beim Cloud-Zugriff
Der TLS-Handshake zur Casambi-Cloud braucht einen großen zusammenhängenden
Heap-Block, der nur frei ist, wenn der BLE-Stack **nicht** initialisiert ist
(siehe Kommentare bei `refresh`, `main.cpp:738`, und im Wizard). Während des
Cloud-Schritts muss BLE also deinitialisiert sein.

### 3.2 Woher kommt die `networkUuid`?
Der Cloud-Zugriff braucht aus dem BLE-Scan **genau eine** Information: die
`networkUuid`. Diese ist aktuell die **BLE-MAC** des gewählten Geräts
(`main.cpp:1353`, Doppelpunkte entfernt, lowercase). Der erste Cloud-Call
`GET /networks/uuid/<networkUuid>` (`api_client.cpp:45`) nimmt diese UUID als
Eingabe. Es gibt keine andere Quelle für die UUID als den Scan.
→ Daraus folgt zwingend die Reihenfolge **Scan vor Cloud**.

### 3.3 Was im AP-Modus geht und was nicht

| Aktivität | reiner AP-Modus | Grund |
|---|---|---|
| WLAN-Liste (`WiFi.scanNetworks`) | ✅ | kein Internet nötig |
| **BLE-Scan** (Casambi-GW-Liste) | ✅ | BLE + WiFi-AP koexistieren; **kein TLS → kein Heap-Konflikt** |
| WLAN-PW + Casambi-PW eingeben/speichern | ✅ | reine Eingabe |
| **Casambi-Cloud-Fetch** | ❌ | braucht Internet (STA) **und** BLE aus (Heap) |

Konsequenz: **Eingaben einsammeln** geht komplett im AP-Portal; der **Cloud-
Fetch** braucht zusätzlich eine STA-Verbindung und freien Heap. Lösung:
**AP+STA-Modus** (siehe 6.3).

## 4. Reihenfolge (Prinzip)

```
1. WLAN-Auswahl    (AP-Portal)
2. BLE-SCAN        (im AP-Modus → networkUuid-Kandidaten zur Auswahl)   ← BLE an
   → BLEDevice::deinit()                                                ← BLE aus (Heap frei)
3. CASAMBI-CLOUD   (uuid + Passwort → Keys/Config; AP+STA)              ← BLE aus, TLS braucht Heap
   → save + reboot
4. BLE-VERBINDUNG  (authentifiziert, mit Cloud-Keys)                    ← Betriebsmodus
```

Wichtige Unterscheidung: **BLE-Scan ≠ BLE-Verbindung.**

| BLE-Aktivität | braucht | liefert | Zeitpunkt |
|---|---|---|---|
| **Scan** (Discovery, passiv) | nichts | `networkUuid` (+ Name/RSSI) | **vor** Cloud (im AP) |
| **Verbindung** (authentifiziert) | Cloud-**Keys** | Steuerung der Lampen | **nach** Cloud (Betrieb) |

## 5. Boot-Zustandsmaschine (2 Zustände)

`setup()` entscheidet anhand von `ConfigStore::hasValidConfig()`:

| Zustand | Bedingung | Modus | BLE |
|---|---|---|---|
| **A. Setup** | `!hasValidConfig()` | offenes SoftAP-Portal (AP, bei Bedarf AP+STA) | für Scan an, für Cloud aus |
| **C. Betrieb** | gültige Config | Betriebsmodus (heute) + mDNS | an |

Es gibt **keinen** separaten „WiFi-ok-aber-keine-Casambi-Config"-Zustand mehr:
Die Config gilt erst als gültig, wenn der gesamte Portal-Ablauf inklusive
Cloud-Fetch erfolgreich war. Bricht der Vorgang ab (z. B. Stromausfall mitten
im Setup), startet das Gerät wieder im Portal; bereits gespeicherte
WLAN-Credentials werden im Formular vorausgefüllt.

## 6. Setup-Portal (Zustand A)

### 6.1 Start
1. `WiFi.softAP("Casambi-Setup-XXXX")` **ohne Passwort** (offener AP).
   XXXX = 4 Hex-Ziffern aus `ESP.getEfuseMac()`.
2. `DNSServer` auf Port 53 → Captive-Portal-Umleitung auf `192.168.4.1`.
3. AsyncWebServer liefert die Single-Page (HTML in `PROGMEM`, kein
   LittleFS-Upload nötig).

### 6.2 Endpunkte des Portals

| Methode | Pfad | Zweck |
|---|---|---|
| `GET` | `/` | Single-Page mit beiden Bereichen (WLAN, Casambi) |
| `GET` | `/api/wifi-scan` | WLAN-Liste (`WiFi.scanNetworks`) als JSON |
| `POST` | `/api/ble-scan` | BLE-Scan anstoßen (Antwort `202`) |
| `GET` | `/api/ble-scan` | `{state:"scanning\|done", devices:[…]}` |
| `POST` | `/api/provision` | `{ssid, wifiPassword, networkUuid, casambiPassword}` (Antwort `202`) |
| `GET` | `/api/provision/status` | `{state, msg, networkName?}` für Live-Fortschritt |

Lange Operationen (BLE-Scan ~10 s, STA-Connect, Cloud-Fetch) laufen **nicht**
im AsyncTCP-Handler, sondern in einer **Zustandsmaschine in `loop()`** (mit
`esp_task_wdt_reset()`); die Handler setzen nur Auftrag + Parameter, die
Portal-Seite pollt den Status.

### 6.3 Ablauf nach „Absenden" (AP+STA mit Live-Fortschritt)
1. WLAN-Credentials speichern.
2. **`BLEDevice::deinit(true)`** (BLE-Scan fertig → Heap frei).
3. `WiFi.mode(WIFI_AP_STA)`, STA-Verbindung zum Heim-WLAN aufbauen — der **AP
   bleibt aktiv**, die Fortschrittsseite im Browser bleibt erreichbar.
4. Cloud: `getNetworkId(uuid)` → `createSession(pw)` → `fetchNetworkConfig()`.
5. `networkUuid/networkId/casambiPassword` setzen;
   **`autoConnectAddress` = MAC aus `uuid`** (Doppelpunkte einfügen),
   `autoConnectEnabled = true`; `saveNetworkConfig()`.
6. Status → **done**, Portal zeigt Erfolg → Reboot in Zustand C.
7. **Fehler** an jeder Stelle (falsches Casambi-PW, Cloud nicht erreichbar,
   falsches WLAN-PW) → Status **error** mit Meldung; zurück in den reinen
   AP-Modus, Seite bleibt erreichbar, Nutzer korrigiert und sendet erneut.

### 6.4 Scan-Ergebnis (pro Gerät)
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
`ScanCallbacks::onResult` wird dafür erweitert (heute nur address/name/rssi).

## 7. Mehrere Casambi-Geräte / Robustheit

### 7.1 Warum oft nur ein Gerät erscheint
Primär eine **Casambi-Mesh-Eigenschaft**, kein Firmware-Limit: Die Teilnehmer
eines Casambi-Netzes handeln aus, **welche Einheit gerade als BLE-Gateway
„connectable" advertised** — meist nur eine zur Zeit. Der Scan sammelt aber
bereits **alle** advertisenden Casambi-Geräte (dedupliziert per MAC); mehrere
Einträge erscheinen, sobald mehrere gleichzeitig advertisen. Das Portal stellt
alle gefundenen Geräte zur Auswahl.

### 7.2 Robustheitsproblem mit fester MAC
Die Firmware merkt sich Identität **und** Auto-Connect-Ziel als **eine feste
MAC** (`networkUuid` = `autoConnectAddress` = MAC). Schaltet man diese Einheit
aus, übernimmt eine andere Einheit das Advertising — **mit anderer MAC** — und
der Reconnect auf die feste MAC schlägt fehl, obwohl das Netz erreichbar wäre.

### 7.3 Verbesserung (separat, abhängig von 9.1)
Beim Verbinden nicht strikt an die MAC pinnen, sondern auf **irgendeine gerade
advertisende Einheit desselben Netzes** gehen. Dafür müsste das Netz über die
**Service-Data im Advertisement** identifiziert werden statt über die MAC.

## 8. mDNS / FHEM-Anbindung

- **Betriebsmodus (Zustand C):** mDNS-Hostname `casambi-XXXX` (XXXX = 4 Hex aus
  `ESP.getEfuseMac()`) → `casambi-a1b2.local`, Service `_http._tcp` (Port 80)
  mit TXT-Records `configured=1`, `build=<n>`, `network=<name>`. So sind
  **mehrere Gateways** im WLAN eindeutig unterscheidbar.
- **FHEM-Define** akzeptiert Hostname **oder** IP:
  - `define gw1 CasambiGW casambi-a1b2.local`
  - `define gw1 CasambiGW 192.168.178.111` (empfohlen mit DHCP-Reservierung am
    Router über die ESP-MAC)
- **FHEM-Modul-Änderungen sind minimal:** keine `scanNetworks`/`casambiSetup`-
  Befehle, kein Status-Polling — die gesamte Einrichtung passiert im Portal.
  Optional: `GET /api/info` (`{configured, build, hostname, mac, ip}`), damit
  FHEM erkennt und meldet, falls es auf ein noch nicht konfiguriertes Gerät
  zeigt.

## 9. Offene Verifikationspunkte

1. **`networkUuid` = MAC oder Service-Data?** Aktuell wird die Geräte-MAC als
   `networkUuid` genutzt und funktioniert für den Cloud-Lookup. Ob die Cloud
   stattdessen einen aus der Service-Data abgeleiteten Netz-Identifier
   akzeptiert (was MAC-unabhängigen Reconnect ermöglichen würde), ist an echter
   Hardware zu prüfen. Davon hängt Abschnitt 7.3 ab.
2. **AP+STA + TLS heapseitig:** Mit deinitialisiertem BLE sollte der TLS-Heap
   verfügbar sein; AP+STA selbst braucht wenig Heap. An Hardware bestätigen.
3. **Scan-Dauer:** fester 10-s-BLE-Scan, auf Knopfdruck im Portal — vorgeschlagen.

## 10. ESP-Firmware-Änderungen im Überblick

1. `main.cpp` — `setup()` als 2-Zustands-Maschine (Setup-Portal / Betrieb);
   Provisionierungs-Zustandsmaschine (idle/wifi-scan/ble-scan/connecting/
   fetching/done/error) in `loop()`. Serial-Wizard bleibt als Fallback.
2. `src/web/setup_portal.{h,cpp}` (neu) — offener SoftAP, DNSServer,
   Single-Page-HTML, Portal-Endpunkte, AP+STA-Umschaltung.
3. `ScanCallbacks` erweitern (mfg/svc-Data); Scan-Ergebnis als gemeinsame
   Struktur für Serial- und Portal-Pfad.
4. Cloud-Schritte aus `runSetupWizard()` in `provisionFromCloud(uuid, pw, &cfg)`
   herauslösen (von Portal **und** Serial genutzt).
5. `webserver.{h,cpp}` — mDNS im Betriebsmodus, optional `/api/info`.
6. `config.h` — Präfixe/Konstanten (AP-SSID, mDNS-Hostname).
7. `98_CasambiGW.pm` — optional `/api/info`-Auswertung; sonst unverändert.
8. README aktualisieren.

## 11. Umsetzungsreihenfolge

1. `provisionFromCloud()` aus dem Wizard herauslösen (Refactor, verhaltensneutral).
2. `ScanCallbacks` erweitern + gemeinsame Scan-Struktur.
3. SoftAP-/Captive-Portal-Modul mit Single-Page + Portal-Endpunkten.
4. Provisionierungs-Zustandsmaschine in `loop()` (inkl. AP+STA + Cloud-Fetch).
5. 2-Zustands-`setup()`; mDNS im Betriebsmodus.
6. Optional `/api/info` + FHEM-Auswertung.
7. README aktualisieren.
