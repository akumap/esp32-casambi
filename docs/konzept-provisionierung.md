# Konzept: ESP32-Casambi-Provisionierung ohne Serial

Status: in Umsetzung — SoftAP-Portal + Cloud-Provisionierung + mDNS + `/api/info`
implementiert; **Auto-Hangeln (Abschnitt 7) bewusst zurückgestellt** (Reconnect
bleibt vorerst auf fester MAC wie bisher).
Branch: `claude/esp32-ap-rest-config-hejbf7`

## 1. Ziel

Nach dem Flashen soll **keine Konfiguration per Serial-Eingabe** mehr nötig
sein. Die **gesamte interaktive Erstkonfiguration** findet in **einer**
Weboberfläche statt — dem **offenen SoftAP-Portal** des ESP32:

- WLAN-Auswahl + WLAN-Passwort
- Casambi-Netzwerk-Passwort (BLE-Scan im Hintergrund; eine **Gateway-Auswahl**
  ist nur nötig, wenn mehrere Casambi-Netze gefunden werden — siehe 6.5)

Der Casambi-Cloud-Abruf läuft direkt im Anschluss mit **Live-Fortschritt**.
FHEM verbindet sich danach nur noch mit dem **fertig konfigurierten** Gerät.

Im Betrieb wählt der ESP das Gateway **automatisch** und wechselt bei Bedarf zur
nächsten erreichbaren Einheit desselben Netzes (siehe 7) — eine feste
GW-Auswahl durch den Nutzer entfällt damit.

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
4. Cloud: `getNetworkId(uuid)` → `createSession(pw)` → `fetchNetworkConfig()`
   (Auswahl des `uuid` siehe 6.5 — bei nur einem Netz automatisch).
5. `networkUuid/networkId/casambiPassword` setzen;
   **`autoConnectAddress` = MAC aus `uuid`** (Doppelpunkte einfügen) als
   *bevorzugter* erster Versuch, `autoConnectEnabled = true`;
   `saveNetworkConfig()` (enthält über `unit.address` die MAC-Liste **aller**
   Einheiten → Grundlage für das Auto-Hangeln im Betrieb, siehe 7).
6. Status → **done**, Portal zeigt den cloud-bestätigten `networkName` →
   Reboot in Zustand C.
7. **Fehler** an jeder Stelle (falsches Casambi-PW, Cloud nicht erreichbar,
   falsches WLAN-PW) → Status **error** mit Meldung; zurück in den reinen
   AP-Modus, Seite bleibt erreichbar, Nutzer korrigiert und sendet erneut.

### 6.5 Das richtige Netz wählen (Disambiguierung beim Setup)
Beim Erst-Setup hat der ESP die Unit-MAC-Liste noch **nicht** (die kommt erst
mit der Cloud-Config), also greift der Netz-Filter aus Abschnitt 7 hier noch
nicht. Stattdessen:

- **Ein** gefundener Casambi-Advertiser → kein Auswählen, direkt Cloud-Versuch.
- **Mehrere** Advertiser → das **Passwort ist die eindeutige Probe**: Der ESP
  probiert die Kandidaten der Reihe nach (`getNetworkId` → `createSession`).
  `createSession` liefert nur beim passenden Netz `200`, sonst `401/403`
  („Invalid password", `api_client.cpp:117`). Der authentifizierende Kandidat
  gewinnt; sein über `fetchNetworkConfig` bestätigter `networkName` wird im
  Portal angezeigt.
- Zusätzlich wird der advertiste **Name** (BLE „Complete Local Name",
  `main.cpp:557`) in der Liste angezeigt, damit der Nutzer den wahrscheinlich
  richtigen vorab wählen und Versuche sparen kann.

Vorbehalte (Hardware-Prüfpunkte, siehe 9):
- **Zwei Netze mit identischem Passwort** → beide authentifizieren → echte
  Mehrdeutigkeit; dann beide `networkName` anzeigen und wählen lassen (selten).
- **Zwei Einheiten desselben Netzes** als Kandidaten → ob beide Unit-MACs im
  Cloud-Lookup `getNetworkId` auflösen, ist offen; eine nicht auflösende MAC
  scheidet einfach aus, der funktionierende Kandidat bleibt.

Kosten: jeder Versuch = ein Cloud-Roundtrip (BLE hier ohnehin aus → Heap frei),
bei wenigen Advertisern unkritisch.

**Wiederholtes Scannen zur gezielten Gateway-Auswahl (praxiserprobt):**
Weil das Casambi-Mesh aushandelt, welche Einheit gerade als „connectable"
advertised, zeigt jeder erneute BLE-Scan u. U. eine **andere** Einheit desselben
Netzes. Man kann den Scan im Portal daher **so oft wiederholen, bis das
gewünschte Gateway erscheint**, und gezielt eine Einheit wählen, die
**dauerhaft mit Strom versorgt und nicht vom Netz getrennt** wird.
Das ist aktuell wichtig, weil der Reconnect noch auf die gespeicherte MAC pinnt
(Auto-Hangeln, Abschnitt 7, ist zurückgestellt): Eine stabile, ortsfeste
Einheit als Gateway zu wählen erhöht die Betriebszuverlässigkeit, bis das
Auto-Hangeln implementiert ist.

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

## 7. Automatisches Gateway-Hangeln im Betrieb

### 7.1 Grundlage: netzweite Keys + bekannte MAC-Liste
Zwei Eigenschaften machen automatisches Umschalten möglich:

1. **Netzweite Authentifizierung.** `connect()` nutzt den über
   `_config->getBestKey()` geladenen **Netzwerk-Key** (`casambi_client.cpp:112`),
   nicht gerätespezifische Keys; der ECDH-Austausch ist pro Verbindung frisch.
   Mit demselben Key kann man sich gegen **jede** Einheit des Netzes
   authentifizieren — die MAC ist kryptografisch nicht besonders.
2. **Bekannte MAC-Liste.** Nach dem Setup kennt der ESP über `unit.address`
   (`api_client.cpp:299`, persistiert `config_store.cpp:87`) die MACs **aller**
   Einheiten seines Netzes.

> ⚠ **Hardware-Befund (widerlegt Annahme 2 teilweise):** Die tatsächlich
> verbundene Gateway-Adresse ist eine **zufällige (random static) BLE-Adresse**
> (beobachtet z. B. `9e:d8:2b:33:15:44` — oberste zwei Bits `10`) und **stimmt
> nicht** mit `unit.address` aus der Cloud überein. Damit funktioniert der
> geplante **Netz-Filter über die bekannte Unit-MAC-Liste NICHT** (Punkt 9.1
> ist also negativ beantwortet). Zudem kann sich die Adresse nach einem
> Geräte-Neustart ändern — was sogar den heutigen Reconnect über die feste
> `autoConnectAddress` brechen kann. Konsequenz für das Hopping: Netz-Zuordnung
> muss über das **Advertisement (Service-Data / advertisten Namen)** oder über
> einen **Auth-Versuch mit dem Netzwerk-Key** erfolgen, nicht über Unit-MACs.

### 7.2 Verfahren
Statt an eine feste MAC zu pinnen, hangelt sich der ESP zur nächsten
erreichbaren Einheit:

```
Verbindung weg
   → BLE-Scan (Service-UUID 0xFE4D)
   → schneiden mit bekannter Unit-MAC-Liste        ← Netz-Filter, kein Fremdnetz
   → besten verfügbaren Kandidaten verbinden
   → ECDH + Auth mit Netzwerk-Key
```

- **Priorisierung:** zuletzt genutzter GW (`autoConnectAddress`) zuerst, dann
  zuletzt-online + stärkstes RSSI.
- **Netz-Filter ohne Service-Data:** Der Schnitt mit der bekannten MAC-Liste
  schließt fremde Casambi-Netze automatisch aus — Punkt 9.1 wird damit für den
  Betrieb **gegenstandslos** (er bleibt nur für die Setup-Disambiguierung
  relevant, siehe 6.5).
- **Proaktiver Scan:** Online/Offline-Events kommen über den
  Unit-State-Callback (`unit.online`, `main.cpp:182`). Sie helfen, Kandidaten zu
  priorisieren und bei Topologie-Änderungen früher zu scannen (kürzere
  Detektion). Einschränkung: Diese Events laufen über die **aktuelle**
  Verbindung — fällt der aktuelle GW weg, ist das der Transportweg selbst; für
  *andere* Einheiten funktioniert es, sonst dient die volle MAC-Liste als
  Fallback.

### 7.3 Warum oft nur ein Gerät erscheint / Wechseldauer
Es ist eine **Casambi-Mesh-Eigenschaft**: Die Teilnehmer handeln aus, **welche
Einheit gerade als „connectable" advertised** — meist nur eine zur Zeit. Der
ESP kann nur zu einer **gerade connectable** advertisenden Einheit verbinden;
die MAC-Liste sagt ihm nur, *welche* dazugehören und online sind, kann aber
keine Einheit zwingen, GW zu werden.

Wechseldauer (aus dem Code abgeschätzt):

| Phase | Zeit | Quelle |
|---|---|---|
| Verlust erkennen | sauber ~0–10 s (Callback / `CONNECTION_CHECK_INTERVAL_MS`), stiller Hänger bis ~30 s (Keepalive) | `config.h:111`, `main.cpp:285` |
| Neuen Advertiser finden | Sekunden Scan **+ Mesh-Neuwahl (variabel, Casambi-seitig)** | unbekannt |
| Connect + Key-Exchange + Auth | typ. ~1–3 s | `config.h:94` |

- **Mehrere Einheiten gleichzeitig connectable:** Umschalten quasi sofort
  → **~3–8 s**.
- **Nur eine zur Zeit:** Warten auf Mesh-Neuwahl → variabel, der Flaschenhals.
- Worst Case (stiller Hänger + träge Neuwahl): bis ~30–60 s; zusätzlich greifen
  Backoff (`BLE_RECONNECT_INTERVAL_MS`=5 s → max 60 s) und nach
  `MAX_RECONNECT_FAILURES`=10 der Neustart.

Ob mehrere Einheiten gleichzeitig connectable advertisen, ist der eine Punkt,
der sich nur an echter Hardware messen lässt (siehe 9) — er entscheidet zwischen
„nahezu unterbrechungsfrei" und „spürbare Pause".

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

1. **Advertisende MAC == `unit.address`? → NEIN (an Hardware geklärt).** Die
   verbundene Gateway-Adresse ist eine **random static** BLE-Adresse (z. B.
   `9e:d8:2b:33:15:44`) und matcht **keine** `unit.address`. Der geplante
   Netz-Filter über die Unit-MAC-Liste (7.1/7.2) ist damit hinfällig; die
   Namensauflösung Gateway-MAC → Unit-Name funktioniert ebenfalls nicht
   (Workaround: advertisten Namen beim Provisionieren merken + Netzname als
   Fallback, umgesetzt). **Offen/neu:** Wechselt diese Adresse bei einem
   Geräte-Neustart? Falls ja, bricht auch der Reconnect über die feste
   `autoConnectAddress` → Hopping muss übers Advertisement/Auth-Probe gehen.
2. **Mehrere Einheiten gleichzeitig connectable?** Entscheidet die Wechseldauer
   (7.3): nahezu unterbrechungsfrei vs. Warten auf Mesh-Neuwahl. Nur an echter
   Hardware messbar.
3. **Setup-Disambiguierung (6.5):** ob bei mehreren Einheiten desselben Netzes
   beide Unit-MACs im Cloud-`getNetworkId` auflösen; und Verhalten bei zwei
   Netzen mit identischem Passwort.
4. **AP+STA + TLS heapseitig:** Mit deinitialisiertem BLE sollte der TLS-Heap
   verfügbar sein; AP+STA selbst braucht wenig Heap. An Hardware bestätigen.
5. **Scan-Dauer:** fester 10-s-BLE-Scan im Portal — vorgeschlagen.

> Hinweis: Der frühere Punkt „networkUuid = MAC oder Service-Data?" ist für den
> **Betrieb gegenstandslos**, da der Netz-Filter über die bekannte MAC-Liste
> läuft (7.2), nicht über Service-Data.

## 10. Ressourcenbedarf (überschlägig)

**Keine neuen externen Libraries:** `DNSServer` und `ESPmDNS` sind Teil des
ESP32-Arduino-Cores; `ESPAsyncWebServer`/`AsyncTCP` sind bereits eingebunden.

### 10.1 Flash (Programm)

| Komponente | grob |
|---|---|
| DNSServer (Captive Portal) | ~2–5 KB |
| ESPmDNS | ~5–10 KB |
| Portal-HTML in `PROGMEM` | ~3–8 KB |
| Setup-Zustandsmaschine, Refactor, Auto-Hangeln | wenige KB |
| **Summe** | **~15–30 KB** |

Unkritisch: Die Partition ist `huge_app.csv` (`platformio.ini`), Flash-Platz ist
reichlich vorhanden.

### 10.2 RAM/Heap
Der sensible Posten (kein PSRAM, `HEAP_CRITICAL_THRESHOLD` = 20 KB):

| Situation | Zusatzbedarf | Bewertung |
|---|---|---|
| Betrieb (Zustand C): mDNS dauerhaft | ~2–4 KB | unkritisch |
| Auto-Hangeln: BLE-Scan nur bei Verbindungsverlust | transient, bestehende Scan-Infrastruktur | unkritisch |
| Setup: AP + Portal + BLE-Scan | moderat, **kein TLS gleichzeitig** | unkritisch |
| **Setup-Peak: AP+STA + TLS-Handshake (BLE aus)** | AP_STA ~10–30 KB **+** TLS ~30–50 KB | **kritisch prüfen** |

Einziger realer Engpass ist der **transiente Peak beim Cloud-Fetch im
AP+STA-Modus**. Weil BLE dann deinitialisiert ist (6.3), sollte es passen — das
ist Verifikationspunkt 9.4 und an Hardware zu messen.

> Ausweg, falls der Peak zu knapp wird: den AP **vor** dem TLS-Schritt schließen
> (reines STA), Fortschritt dann erst nach Reboot zeigen — opfert das
> Live-Feedback gegen ~10–30 KB Heap.

Insgesamt: moderater Flash-Zuwachs, im Betrieb vernachlässigbarer RAM-Zuwachs;
einziges Risiko ist der kurze Setup-Peak, abgesichert durch das BLE-Deinit.

## 11. ESP-Firmware-Änderungen im Überblick

1. `main.cpp` — `setup()` als 2-Zustands-Maschine (Setup-Portal / Betrieb);
   Provisionierungs-Zustandsmaschine (idle/wifi-scan/ble-scan/connecting/
   fetching/done/error) in `loop()`. Serial-Wizard bleibt als Fallback.
2. `src/web/setup_portal.{h,cpp}` (neu) — offener SoftAP, DNSServer,
   Single-Page-HTML, Portal-Endpunkte, AP+STA-Umschaltung.
3. `ScanCallbacks` erweitern (mfg/svc-Data); Scan-Ergebnis als gemeinsame
   Struktur für Serial- und Portal-Pfad.
4. Cloud-Schritte aus `runSetupWizard()` in `provisionFromCloud(uuid, pw, &cfg)`
   herauslösen (von Portal **und** Serial genutzt); Mehr-Netz-Disambiguierung
   per Passwort-Probe (6.5).
5. **Auto-Hangeln im Betrieb** (`main.cpp` `checkAndReconnectBLE` +
   `casambi_client`): Reconnect von „feste MAC" auf „Scan → Schnitt mit bekannter
   Unit-MAC-Liste → besten Kandidaten verbinden" umstellen; `autoConnectAddress`
   nur noch als bevorzugter Erstversuch; Online/RSSI-Priorisierung.
6. `webserver.{h,cpp}` — mDNS im Betriebsmodus, optional `/api/info`.
7. `config.h` — Präfixe/Konstanten (AP-SSID, mDNS-Hostname).
8. `98_CasambiGW.pm` — optional `/api/info`-Auswertung; sonst unverändert.
9. README aktualisieren.

## 12. Risiko-Einschätzung (Debugging-Aufwand)

| Bereich | Risiko | Wo Debugging anfällt |
|---|---|---|
| `provisionFromCloud()` herauslösen (Refactor) | Niedrig | verhaltensneutral, gegen Serial-Wizard testbar |
| SoftAP + DNSServer + Portal-HTML | Niedrig | Standard-Muster, ohne Casambi testbar |
| `wifi-scan`, mDNS, Scan-Struct | Niedrig | gut isoliert testbar |
| Lange Ops in `loop()` statt Async-Handler | Mittel | Nebenläufigkeit/WDT, Muster klar |
| BLE-Scan → `deinit` → Cloud-Fetch | Mittel | bekannt empfindlich; Wizard beweist Machbarkeit |
| **AP+STA + TLS-Heap-Peak** | Mittel | Heap-Messung (9.4); Fallback „AP vor TLS schließen" |
| **Auto-Hangeln im Betrieb** | Höher (hardwareabh.) | Mesh-/Advertising-Verhalten nur am echten Netz prüfbar (9.1/9.2) |
| Setup-Disambiguierung (6.5) | Niedrig–Mittel | Edge Cases (9.3) nur mit echtem Account; Fehlerpfade gutmütig |

**Gesamtbild:** Die Firmware-Mechanik ist niedriges bis mittleres Risiko und
kein architektonisches Neuland — der bestehende Wizard demonstriert die harten
Teile (BLE-vor-Cloud, TLS, Auth) bereits funktionierend. Der echte
Debugging-Aufwand konzentriert sich auf zwei On-Hardware-Themen: den
Heap-Peak im AP+STA+TLS-Moment (messbar, mit Fallback) und das
Casambi-Mesh-/Advertising-Verhalten beim Auto-Hangeln (iteratives Testen).

**Risiko-Minimierung:**
- Serial-Wizard bleibt als Fallback erhalten.
- Gestaffelte Umsetzung (Abschnitt 13): Refactor zuerst, dann Portal, dann
  Auto-Hangeln **separat** und **nachgelagert** — so liegt das größte
  Einzelrisiko nicht im kritischen Pfad der Erstinbetriebnahme.
- Vorhandene Werkzeuge nutzen: `heapDebug`, `bleDebug`, nichtflüchtiges
  Event-Log.

## 13. Umsetzungsreihenfolge

1. `provisionFromCloud()` aus dem Wizard herauslösen (Refactor, verhaltensneutral).
2. `ScanCallbacks` erweitern + gemeinsame Scan-Struktur.
3. SoftAP-/Captive-Portal-Modul mit Single-Page + Portal-Endpunkten.
4. Provisionierungs-Zustandsmaschine in `loop()` (inkl. AP+STA + Cloud-Fetch +
   Disambiguierung 6.5).
5. 2-Zustands-`setup()`; mDNS im Betriebsmodus.
6. Auto-Hangeln im Reconnect (7) umsetzen.
7. Optional `/api/info` + FHEM-Auswertung.
8. README aktualisieren.
