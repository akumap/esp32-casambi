# Codeanalyse: Stabilität, Funktion, Optimierungen (Firmware + FHEM)

Stand: Juli 2026, `main` @ 12904d9 (nach PR #25 Stability-Review und Issue-#11-Hardening).
Umfang: gesamte Firmware (`src/`, ~9.000 Zeilen) und beide FHEM-Module
(`FHEM/98_CasambiGW.pm`, `FHEM/98_CasambiUnit.pm`, ~1.400 Zeilen), dazu
`platformio.ini`, CI-Workflow und Konzept-Dokumente.

## Umsetzungsstand

Alle Befunde sind auf diesem Branch umgesetzt (ein Folge-Commit dieses
Dokuments), mit zwei bewussten Abweichungen:

| Befund | Status |
|--------|--------|
| S1 Keepalive vs. WDT | ✅ WDT 45 s **und** Keepalive nur nach ≥60 s Funkstille (`BLE_KEEPALIVE_IDLE_MS`) — erledigt zusammen mit O1 |
| S2 `_connectedAddress`-Race | ✅ Schreiben/Lesen unter `g_configMutex` |
| S3 WS-Queue | ✅ Tiefe **64** (Nutzerentscheidung statt 32) + Resync-Hello bei Drop |
| S4 Restart-Politik | ✅ Restart nur bei 10 internen Fehlern in Folge; Peer-Abwesenheit → Dauer-Backoff |
| S5 Log-Snapshot vs. Dateiwechsel | ✅ `EventLog::generation()`-Prüfung im Chunk-Streaming; zusätzlich msgLen-Clamp in `writeEntryJson` |
| S6 RTC-Magic | ✅ Layout-versioniert (`sizeof(LogEntry)`/Kapazität eingemischt) |
| F1 deviceSuffix | ✅ letzte zwei MAC-Oktette (main.cpp + setup_portal.cpp). **Achtung:** SSID/mDNS-Suffix bestehender Geräte ändert sich einmalig |
| F2 mDNS | ✅ idempotent, Aufruf in allen WiFi-/Webserver-Recovery-Pfaden |
| F3 `network`-Reading | ✅ `networkName` im hello, Reading im FHEM-Modul, README |
| F4 `set on` = 100 % | ✅ stellt letzte Helligkeit wieder her (`LAST_BRIGHTNESS`), Fallback 100 % |
| F5 WS-Reject 404 | ✅ `GET /ws` ohne Token → 401 |
| F6 Portal-/api/info | ✅ nur noch `{configured, build}` |
| F7 ID-Truncation | ✅ `parseIdSegment` validiert 0–255, sonst 400 |
| F8 0x07-Echo | — dokumentierter Zustand, bewusst keine Änderung |
| O1 Keepalive-Funk | ✅ siehe S1 |
| O2 Body-Parsing-Duplikate | ✅ Helper (`_checkBle`, `_parseBody`, `_requireUint8`, `_*FromPath`); webserver.cpp ~290 Zeilen kleiner |
| O3 ESP32-C3 | ⏸️ **bleibt in Build+CI** (Nutzerentscheidung: Board nur mangels Hardware ungetestet); als build-only dokumentiert |
| O4 NimBLE-Init-Name | ✅ `DEVICE_NAME` |
| O5 Oversize-Bodies | ✅ >4 KiB → Verbindung wird geschlossen, ≤4 KiB weiterhin sauberes 413 |
| P1 Handshake-Backoff | ✅ 30 s-Backoff, Log-Degradierung nach 1. Fehlschlag, 401-Hinweis auf Passwort |
| P2 Poll-Ketten | ✅ Dedupe + Stale-Guard in `InfoCb` |
| P3 SendCommand-Statuscodes | ✅ `$param->{code}` geprüft, Log mit 401-Hinweis |
| P4 `wsState` nach EOF | ✅ Reset in `Read` |
| P5 `UPDATING_STATUS` | ✅ `local`-Guard (Unit + Vertical) |
| P6 Passwort-Klartext | ✅ `set <gw> password` (Key-Store); Attribut als Legacy-Fallback erhalten |
| P7 Negativ-Cache | ✅ known-unknown-IDs in `UNIT_BY_ID` als `undef` |
| P8a JSON-Guard | ✅ `require JSON` mit Fehlermeldung in Define; unnötiges `use JSON` aus 98_CasambiUnit.pm entfernt |
| P8b FIN-Bit | ✅ als Grenze kommentiert |
| P8c save-Hinweis | ✅ Log nach strukturellen Änderungen + README |
| P8d =pod-Drift | ✅ CasambiVertical-Doku korrigiert |

## Gesamteinschätzung

Die Codebasis ist für ein Projekt dieser Art ungewöhnlich reif: Die
Task-Grenzen (loopTask / NimBLE-Host / async_tcp) sind durchgängig dokumentiert
und mit Mutex-Disziplin abgesichert, kritische Pfade (Cloud-Refresh vor
BLE-Start, Broadcast-Queue statt `textAll()` aus dem BLE-Task, Body-Buffering
nur für bekannte Endpunkte) sind bewusst konstruiert, und das zweistufige
Event-Log (RTC + LittleFS) macht Abstürze nachvollziehbar. Die groben
Stabilitätsrisiken (AsyncTCP-Churn, Double-Response-Leak, String-Races auf
`NetworkConfig`) sind durch die vorangegangenen Reviews behoben.

Die verbleibenden Befunde sind überwiegend klein. Die lohnendsten Fixes:

| # | Bereich | Befund | Schwere |
|---|---------|--------|---------|
| F1 | Firmware | `deviceSuffix()` liefert OUI statt gerätespezifischer MAC-Bytes → mDNS-/SSID-Kollision bei mehreren Gateways | mittel |
| F2 | Firmware | mDNS startet nur im Boot-Pfad — nie, wenn WLAN erst später kommt | mittel |
| P1 | FHEM | Keine Backoff-Bremse bei fehlgeschlagenem WS-Handshake (fehlendes/falsches `casambiPassword`) → Dauerschleife im Sekundentakt | mittel |
| S1 | Firmware | GATT-Read im Keepalive: 30-s-ATT-Timeout trifft exakt den 30-s-Task-WDT | mittel (selten) |
| S3 | Firmware | WS-Broadcast-Queue (Tiefe 8) kann bei Szenen mit >8 Units Zustands-Pushes verlieren — FHEM bleibt dann bis zum nächsten Event stale | klein–mittel |

---

## 1. Firmware — Stabilität

### S1: Keepalive-GATT-Read kann den Task-Watchdog treffen (mittel, selten)

`CasambiClient::sendKeepalive()` (`src/ble/casambi_client.cpp:266`) macht alle
30 s einen synchronen `_authChar->readValue()` auf dem loopTask. NimBLE bricht
eine unbeantwortete GATT-Prozedur erst nach dem ATT-Transaktions-Timeout von
**30 s** ab — exakt der Wert des Task-WDT (`WDT_TIMEOUT_SECONDS 30`,
`src/config.h:144`). Genau das Szenario, für das der Keepalive existiert
(Link auf LL-Ebene lebendig, ATT antwortet nicht), kann den loopTask also bis
an die WDT-Grenze blockieren. Dieselbe Race-Klasse wurde für `connect()`
bereits erkannt und mit `setConnectTimeout(10 s)` entschärft
(`casambi_client.cpp:128-131`) — der Read-Pfad blieb offen.

Empfehlung (eine der Optionen):
- WDT auf 45–60 s erhöhen (einfachste, robusteste Variante), oder
- den Keepalive nur senden, wenn `_lastNotificationTime` älter als z. B. 60 s
  ist (spart nebenbei Funkverkehr, siehe O1), oder
- statt `readValue()` einen Health-Check ohne ATT-Prozedur verwenden.

### S2: String-Race auf `_connectedAddress` (niedrig, theoretisch)

`getConnectedAddress()` (`casambi_client.h:133`) kopiert die Arduino-`String`
ohne Lock; geschrieben wird sie in `_connectLocked()`
(`casambi_client.cpp:111`) auf dem loopTask, gelesen u. a. von
`/api/status` und `_buildHelloMessage()` auf dem async_tcp-Task. Der
`isAuthenticated()`-Check davor ist ein TOCTOU: Zwischen Check und Kopie kann
der loopTask einen RSSI-Re-Roll starten und die String reassignen. Praktisch
entschärft, weil Reconnects fast immer dieselbe (gleich lange) Adresse
schreiben (kein Realloc) — aber `connect <n>` per Serial auf eine andere
Adresse öffnet das Fenster real. Die identische Problemklasse wurde für
`NetworkConfig`-Strings mit `g_configMutex`/`lockedCopy()` gelöst
(`webserver.cpp:20-25`); dieselbe Behandlung für `_connectedAddress` (oder ein
gelocktes Getter-Pendant) wäre konsequent und billig.

### S3: WS-Broadcast-Queue Tiefe 8 — Verlust ohne Selbstheilung (klein–mittel)

`WS_BROADCAST_QUEUE_DEPTH 8` (`config.h:184`): Ein einzelnes 0x06-Paket einer
Gruppen-/Szenenschaltung enthält einen Record **pro betroffener Unit** (bei
MTU ~247 B bis zu ~40 Records). `_applyUnitStates` feuert pro Record einen
Callback → `broadcastUnitState` enqueued im NimBLE-Task, während der loopTask
u. U. gerade in `delay(10)` oder einem LittleFS-Flush steckt. Ab dem 9.
Eintrag wird gedroppt (`webserver.cpp:365-368`). Der Code-Kommentar nimmt an,
„der nächste BLE-Broadcast trägt den Zustand nach" — für die *gedroppte* Unit
kommt aber kein weiterer Broadcast, bis sich ihr Zustand erneut ändert oder
FHEM neu verbindet (hello). Ein verpasstes `off` einer Leuchte bleibt in FHEM
also potenziell dauerhaft als `on` stehen.

Empfehlung: Tiefe auf 32 erhöhen (Kosten: 24 × 4 B Queue-Slots — vernachlässigbar;
die `String`-Payloads existieren ohnehin nur kurz) und/oder beim Drop einen
„resync nötig"-Marker setzen, der im nächsten `loop()` ein frisches
Hello/Vollstatus-Broadcast auslöst.

### S4: Restart-Politik bei dauerhaft abwesendem BLE-Peer (klein)

Nach `MAX_RECONNECT_FAILURES 10` wird der ESP neu gestartet
(`main.cpp:545-551`). Sind die Leuchten schlicht stromlos (Wandschalter über
Nacht aus), ergibt das einen Endlos-Zyklus: ~8–10 min Backoff-Versuche →
Reboot → von vorn. Der Neustart hilft nur gegen einen verklemmten BLE-Stack,
nicht gegen einen abwesenden Peer, und erzeugt pro Zyklus Flash-Writes
(NVS-Boot-Counter, Log-Einträge). Empfehlung: Verbindungs-Timeouts
(`BLELinkLoss` aus `connect()`) vom Restart-Zähler ausnehmen und dauerhaft im
60-s-Backoff bleiben; Restart nur bei internen Fehlern (Auth-/Stack-Fehler,
die auf einen wedged Zustand deuten).

### S5: `/api/log`-Streaming vs. Ping-Pong-Dateiwechsel (Randfall)

`snapshotNewest()` friert Dateigrößen/Indizes ein (`webserver.cpp:872`,
`event_log.cpp:409`), die Chunks werden aber über mehrere Ticks gestreamt.
Wechselt währenddessen die aktive Log-Datei (Datei voll → andere geleert),
verrutschen die globalen Indizes: Die Antwort kann einmalig lückenhaft oder
doppelt sein. Kein Crash, kein Leak — als bekannter Randfall dokumentieren
oder eine Generation-Nummer im Snapshot prüfen.

### S6: RTC-Log-Layout ohne Versionierung (Randfall)

`rtcLogMagic` (`event_log.cpp:17-23`) bleibt über ein Firmware-Update hinweg
gültig. Ändert ein Update das `LogEntry`-Layout (`LOG_MSG_MAX`,
Feld-Reihenfolge), werden beim ersten Boot einmalig Müll-Einträge nach
LittleFS geflusht. Billiger Fix: Layout-Version (z. B. `sizeof(LogEntry)`) in
die Magic einmischen.

### Positiv (Stabilität)

- Lock-Hierarchie `_mutex` → `_encMutex` dokumentiert und eingehalten;
  `_sendOperation` nutzt korrekt die `*Locked`-Varianten (kein Deadlock).
- `_sendEncryptedPacket` gibt `_encMutex` **vor** `writeValue()` frei, weil die
  Antwort-Notification synchron feuern kann — subtil und richtig gelöst.
- Cloud-Refresh als Reboot-Marker (`REFRESH_FLAG_PATH`) statt Runtime-Teardown
  eliminiert die Use-after-free-Klasse aus Issue #21 vollständig; Marker wird
  vor dem Download gelöscht → kein Bootloop bei Fehlschlag.
- Heap-Restart mit Debounce (3 aufeinanderfolgende Tiefstände), WiFi-Reconnect
  nicht-blockierend, WDT-Feeds in allen Warteschleifen.
- Event-Log: Flash-Writes nur vom Owner-Task, Fremd-Tasks parken im RTC-Ring —
  saubere Entkopplung.

---

## 2. Firmware — Funktion

### F1: `deviceSuffix()` ist nicht gerätespezifisch (mittel)

`main.cpp:98-103` und `setup_portal.cpp:26-31`:

```cpp
uint64_t mac = ESP.getEfuseMac();
sprintf(buf, "%04x", (unsigned)(mac & 0xFFFF));
```

`ESP.getEfuseMac()` legt `mac[0]` (erstes Oktett = OUI/Herstellerpräfix) ins
niederwertigste Byte. `mac & 0xFFFF` liefert damit die **ersten** beiden
MAC-Oktette — bei Boards derselben Charge identisch. Folgen:

- Zwei Gateways erzeugen dieselbe Setup-SSID `Casambi-Setup-XXXX` und
  denselben mDNS-Namen `casambi-XXXX.local` — genau das
  Multi-Gateway-Szenario, das README und FHEM-Modul unterstützen sollen,
  kollidiert.
- README-Angabe „last 4 hex digits of the chip MAC" stimmt nicht.

Fix: die letzten beiden Oktette verwenden, z. B.

```cpp
uint8_t m[6]; esp_efuse_mac_get_default(m);
sprintf(buf, "%02x%02x", m[4], m[5]);
```

(Beide `deviceSuffix()`-Kopien anpassen; Suffix-Änderung bricht bestehende
`casambi-XXXX.local`-Definitionen in FHEM — im Changelog erwähnen.)

### F2: mDNS startet nur im Boot-Pfad (mittel)

`startMDNS()` wird ausschließlich in `setup()` aufgerufen (`main.cpp:364`),
und nur wenn WLAN **beim Boot** verbunden ist. Kommt das WLAN erst später
(Router bootet nach Stromausfall langsamer als der ESP — ein realistisches
24/7-Szenario), erstellt `checkAndReconnectWiFi()` zwar den Webserver
(`main.cpp:576-583`), aber mDNS wird nie gestartet → `casambi-XXXX.local`
bleibt dauerhaft unauffindbar. Gleiches gilt für den Webserver-Restart im
BLE-Reconnect-Pfad (`main.cpp:527-533`) und nach `wifi set`
(`main.cpp:1273-1280`). Fix: `startMDNS()` idempotent machen (Guard-Flag) und
in der Recovered-Transition von `checkAndReconnectWiFi()` mit aufrufen.

### F3: FHEM-Reading `network` ist tot (klein)

Seit dem Security-Hardening liefert `/api/info` nur noch
`{configured, build}` (`webserver.cpp:425-431`), und die Hello-Message enthält
keinen Netznamen (`_buildHelloMessage`, `webserver.cpp:299-337`). Das
FHEM-Modul wertet `$info->{network}` aber weiterhin aus
(`98_CasambiGW.pm:195`) und die README dokumentiert das Reading `network` —
es wird nie mehr gesetzt. Empfehlung: `networkName` in die (authentifizierte)
Hello-Message aufnehmen und im `hello`-Handler setzen; die unauthentifizierte
`/api/info` bewusst schlank lassen.

### F4: `set <unit> on` erzwingt 100 % (klein, UX)

`/api/units/:id/on` sendet `SetLevel 255` (`webserver.cpp:1097`), FHEM-`on`
ebenso (`98_CasambiGW.pm:755-757`). Nach dem Dimmen auf 20 % springt ein
HomeKit-„Ein" also auf 100 %, während die Casambi-App das letzte Level
wiederherstellt. `CasambiVertical` macht es bereits besser (letzten
`pct`-Wert wiederherstellen, `98_CasambiUnit.pm:373-383`) — dieselbe Logik im
`CasambiUnit`-`on` (letzte `brightness`-Reading senden, Fallback 100) wäre
konsistenter.

### F5: WS-Auth-Ablehnung liefert 404 statt 401 (klein, Diagnose)

Ein Upgrade mit falschem/fehlendem Token fällt durch den Filter
(`webserver.cpp:76-79`) in `onNotFound` → `404 Endpoint not found`
(`webserver.cpp:498-551`). Im FHEM-Log steht dann nur
„handshake failed: HTTP/1.1 404" — der eigentliche Grund (Auth) ist nicht
erkennbar. Ein explizites 401 für `GET /ws` ohne gültigen Token würde die
Fehlersuche deutlich verkürzen (siehe auch P1).

### F6: Portal-`/api/info` verrät weiterhin hostname/mac/ip (klein)

Die Betriebs-Variante wurde auf `{configured, build}` reduziert, die
Portal-Variante (`setup_portal.cpp:189-198`) liefert im offenen AP weiterhin
`hostname`, `mac`, `ip`. Geringe Sensitivität, aber inkonsistent zum
Hardening-Konzept — angleichen.

### F7: ID-Parsing der REST-Routen ohne Bereichsprüfung (kosmetisch)

`path.substring(...).toInt()` wird auf `uint8_t` gekürzt
(z. B. `webserver.cpp:979`): `/api/units/300/on` adressiert Unit 44 (300 mod
256). Meist endet das im 404 (`getUnitById`), kann aber mit realen IDs
kollidieren. Werte > 255 explizit mit 404 ablehnen.

### F8: 0x07-Echo aktualisiert nur SetLevel/Unit (dokumentierter Zustand)

`casambi_client.cpp:920-946`: Temperature-/Vertical-Echos anderer Controller
ändern den lokalen State nicht; in der Praxis folgt ein 0x06-Broadcast.
Kein Handlungsbedarf, nur als Verhaltensgrenze festgehalten.

---

## 3. Firmware — Optimierungen

- **O1 — Keepalive-Funkverkehr:** Der 30-s-GATT-Read läuft auch, wenn laufend
  Notifications ankommen. `_lastNotificationTime` existiert bereits — den
  Read nur bei > 60 s Funkstille auslösen spart Funk, Mutex-Kontention und
  entschärft S1 gleich mit.
- **O2 — ~400 Zeilen dupliziertes Body-Parsing:** Die 10 Body-POST-Handler in
  `webserver.cpp` wiederholen identisch „_tempObject prüfen → JSON parsen →
  freigeben → Feld validieren". Ein Helper
  (`bool parseBody(request, doc)` + `bool requireUint8(doc, key, out)`)
  reduziert die Datei um ~300 Zeilen und verhindert Divergenz der Kopien.
- **O3 — ESP32-C3 halb entfernt:** README führt das Board seit 1137bbf nicht
  mehr, `platformio.ini:64-85` und die CI-Matrix (`ci.yml:14`) bauen es
  weiterhin. Entweder als „ungetestet, build-only" dokumentieren oder Umgebung
  + CI-Eintrag entfernen (spart CI-Zeit).
- **O4 — `NimBLEDevice::init("ESP32-Casambi")`** vs. `DEVICE_NAME`-Konstante
  („ESP32 Casambi") — Duplikat, eine Quelle verwenden.
- **O5 — Oversized-Bodies früh abbrechen:** `onNotFound` lehnt > 512 B erst
  nach vollständigem Empfang ab; bei absurden Content-Lengths könnte der
  Handler die Verbindung direkt schließen. Nur relevant gegen mutwillige
  Clients im LAN — niedrige Priorität.

---

## 4. FHEM-Module

### P1: Keine Backoff-Bremse nach Handshake-Fehlschlag (mittel)

Ablauf bei fehlendem/falschem `casambiPassword` (oder generell abgelehntem
Upgrade): Handshake schlägt fehl → `CasambiGW_StartInfoPoll`
(`98_CasambiGW.pm:356-363`) → Poll nach **+1 s** (`:147`) → `/api/info` meldet
`configured:true` → sofort neuer WS-Versuch → Fehlschlag → von vorn. Ergebnis:
Dauerschleife im ~1–2-s-Takt, unbegrenzt, mit Level-2-Log pro Runde
(„WebSocket handshake failed: HTTP/1.1 404 …") und entsprechender Last auf dem
ESP. Empfehlung:

- Nach einem Handshake-Fehlschlag mit `INFO_POLL_OFFLINE` (30 s) statt 1 s
  weiterpollen (Fehlschlag-Flag in `StartInfoPoll` übergeben).
- In Kombination mit F5 (401 statt 404) den Auth-Fall erkennen und einmalig
  klartextlich loggen: „casambiPassword-Attribut prüfen".

### P2: Poll-Timer-Ketten können sich verdoppeln (klein)

`StartInfoPoll` entfernt zwar anstehende Timer, aber eine bereits laufende
`/api/info`-Anfrage (HttpUtils, nicht abbrechbar) plant in ihrem Callback
(`:178`, `:187`, `:208`) eine **zweite** Kette. FHEM dedupliziert
`InternalTimer` nicht — nach mehreren `set reconnect` während laufender Polls
läuft die Abfrage mehrfach parallel (harmlos, aber unnötiger Traffic/Log).
Fix: `RemoveInternalTimer($hash, "CasambiGW_Poll")` am Anfang von
`CasambiGW_InfoCb`, oder eine Poll-Generation im Hash mitführen.

### P3: `CasambiGW_SendCommand` ignoriert HTTP-Statuscodes (klein)

Der Callback (`:782-786`) loggt nur Transportfehler (`$err`). Antwortet der
ESP mit `503 Not connected to BLE gateway` oder `401 Unauthorized`, verschwindet
das Kommando spurlos — der Nutzer sieht in FHEM ein optimistisch gesetztes
`state on`, die Lampe bleibt aus. Fix: `$param->{code} != 200` prüfen und mit
Gerätename/Kommando loggen (Level 3); bei 401 Hinweis auf `casambiPassword`.

### P4: `wsState` bleibt nach Read-EOF „connected" (kosmetisch)

Liefert `DevIo_SimpleRead` undef (`:339-340`), ruft DevIo intern
`DevIo_Disconnected`, aber `$hash->{wsState}` bleibt „connected", bis der
nächste Handshake es überschreibt. Der Ping-Timer feuert derweil ins Leere
(selbstheilend über den Pong-Timeout, aber verwirrend beim Debuggen). In
`CasambiGW_Read` beim undef-Return `wsState = "disconnected"` setzen.

### P5: `UPDATING_STATUS` ohne Exception-Schutz (Randfall)

`CasambiUnit_UpdateFromState` setzt das Flag und löscht es am Ende
(`98_CasambiUnit.pm:167/195`). Stirbt dazwischen etwas (Notify-Handler in der
Event-Kette von `readingsEndUpdate` können beliebigen User-Code ausführen),
bleibt das Flag 1 und das Gerät ignoriert **dauerhaft alle set-Kommandos**
(SetFn: `return undef if $hash->{UPDATING_STATUS}`). Robust:
`local $hash->{UPDATING_STATUS} = 1;` (automatisches Zurücksetzen auch bei
die) — gleiche Stelle in `CasambiVertical_UpdateFromParent`.

### P6: `casambiPassword` als Klartext-Attribut (Hardening-Hinweis)

Das Passwort steht in `fhem.cfg`/`list`-Ausgaben. FHEM-üblich für Credentials
ist der `setKeyValue`/`getKeyValue`-Store (uniqueID-verschlüsselt) mit einem
`set <gw> password <pw>`-Kommando. Da der abgeleitete Token ohnehin im
LAN-Klartext läuft, ist das kein akutes Risiko — aber konsistent mit dem
Firmware-Hardening.

### P7: Unbekannte `unit_state`-IDs erzeugen wiederholt Vollscans (perf, klein)

Wurden pending-Units per `discardChanges` verworfen, läuft für jede weitere
`unit_state`-Push dieser Units der Rebuild-Pfad (`:687-700`) inkl.
`sort keys %defs` über **alle** FHEM-Geräte — bei großen Installationen und
gesprächigen Units unnötige Dauerlast. Fix: negative Treffer im
`UNIT_BY_ID`-Hash als `undef` cachen (bis zum nächsten hello).

### P8: Kleinigkeiten

- `use JSON;` direkt (`:2`) — FHEM-Konvention wäre ein eval-Guard bzw. die
  `json2nameValue`-freie Nutzung von `JSON::XS` mit Fallback; auf Systemen
  ohne JSON-Modul bricht sonst das Laden des Moduls.
- Fragmentierte WS-Frames (FIN-Bit) werden nicht reassembliert
  (`:375-415`) — die ESP-Seite fragmentiert praktisch nie; als bekannte
  Grenze im Kommentar festhalten.
- Nach `applyChanges` sind neue Geräte erst nach `save` persistent —
  ein Log-Hinweis (oder optionales Autosave) erspart Verwunderung nach
  FHEM-Neustarts.
- `=pod`-Doku von `CasambiVertical` nennt `genericDeviceType dimmer` und ein
  altes `homebridgeMapping`-Format — der Code setzt `light` und das
  `cmdOn/cmdOff`-Format (`:275-280`). Doku angleichen.

### Positiv (FHEM)

- MAC-Regex-Validierung vor `fhem("define …")` verhindert Command-Injection
  über manipulierte hello-Daten (`98_CasambiGW.pm:725`).
- Pending-Changes-Konzept (explizites `applyChanges`) schützt vor
  Autocreate-/Delete-Unfällen durch transiente Netzzustände.
- MAC-basierte Identität + FUUID-Schonung hält HomeKit-Zuordnungen über
  Casambi-Rekonfigurationen stabil.
- 300-ms-Debounce der Slider-Kommandos, Feedback-Loop-Guard, `/api/info`-Gate
  vor dem WS-Connect — durchdachte Integrationsdetails.

---

## 5. Priorisierte Empfehlungen

1. **F1** `deviceSuffix()` auf die letzten MAC-Oktette umstellen (2 Stellen).
2. **F2** `startMDNS()` idempotent machen und im WiFi-Recovery-Pfad aufrufen.
3. **P1 + F5** Handshake-Fehlschlag: Firmware 401 für `/ws`, FHEM 30-s-Backoff
   und einmaliger Auth-Hinweis.
4. **S3** `WS_BROADCAST_QUEUE_DEPTH` auf 32; optional Resync-Marker bei Drop.
5. **S1/O1** Keepalive nur bei Funkstille senden (oder WDT > 30 s).
6. **P3** HTTP-Statuscodes in `CasambiGW_SendCommand` loggen.
7. **S4** Restart-Zähler auf interne Fehler beschränken.
8. **P5** `local`-Guard für `UPDATING_STATUS`.
9. **F3** `networkName` ins hello + FHEM-Reading reaktivieren (README-Abgleich).
10. Rest (S2, S5, S6, F4, F6, F7, O2–O5, P2, P4, P6–P8) nach Gelegenheit.
