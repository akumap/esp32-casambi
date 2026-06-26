# Konzept: Migration des BLE-Stacks auf NimBLE

Status: Vorschlag / noch nicht umgesetzt.
Branch: `claude/ble-nimbble-migration-uuwzu4`

## 1. Ziel und Motivation

Der ESP32-Casambi-Controller nutzt aktuell den in Arduino-ESP32 mitgelieferten
**Bluedroid**-BLE-Stack (`BLEDevice.h`, `BLEClient.h`, `BLEScan.h`). Bluedroid
belegt deutlich mehr RAM als der Alternativ-Host **NimBLE**. Genau dieser RAM
ist hier der Engpass: Die jüngere Projekt-Historie ist von Heap-Druck geprägt
(`heap-underrun-reboots`, „tight heap", `BLEDevice::deinit(true)` vor jedem
TLS-Handshake, weil sonst kein zusammenhängender Heap-Block für die
Cloud-Verbindung frei ist).

**NimBLE spart grob 40–100 KB RAM** gegenüber Bluedroid (je nach Konfiguration)
und reduziert auch die Flash-Größe. Für dieses Projekt heißt das konkret:

- Mehr freier Heap im Betrieb → weniger Risiko für Heap-Underrun-Reboots.
- Der zusammenhängende Block für den TLS-Handshake wird wahrscheinlicher
  verfügbar — möglicherweise sogar **ohne** vollständigen `deinit(true)`, was
  einen ganzen Workaround-Pfad entschärfen könnte (siehe 6.4, nicht Teil des
  Pflichtumfangs).

Das **Ziel dieses Konzepts** ist nicht die Migration selbst, sondern ein
**risikobegrenzter Migrationsplan**: gleiches Verhalten nach außen, kleine
prüfbare Schritte, jederzeit rückrollbar.

## 2. Ausgangslage (Ist-Zustand)

Der ESP32 ist ausschließlich **BLE-Central/Client** (er verbindet sich zu
Casambi-Geräten) und führt **passive/aktive Scans** durch. Es gibt **keinen
GATT-Server**, keine BLE-Advertising-Rolle, kein Pairing/Bonding und keine
Security-/Encryption-Funktionen des BLE-Stacks (die Casambi-Verschlüsselung
liegt vollständig in `src/crypto/` und ist BLE-Stack-unabhängig).

Das ist die **wichtigste risikomindernde Tatsache**: Wir nutzen nur den
kleinen, gut abgedeckten Central-Teil der API.

### 2.1 Berührungspunkte mit dem BLE-Stack

| Datei | Zeilen (ca.) | Nutzung |
|---|---|---|
| `src/ble/casambi_client.h` | 12–13, 162–163, 250 | Includes, `BLEClient*`, `BLERemoteCharacteristic*`, Notify-Callback-Signatur |
| `src/ble/casambi_client.cpp` | 60–80 | `createClient`, `connect`, `getService`, `getCharacteristic` |
| | 379, 635 | `registerForNotify`, statischer Notify-Callback |
| | 180, 345, 432, 617 | `readValue`, `writeValue` |
| `src/ble/casambi_scan.cpp` | 7–9, 25–57 | `BLEScan`, `BLEAdvertisedDeviceCallbacks::onResult`, `BLEUUID`-Vergleich, Adv-Daten |
| `src/ble/casambi_scan.h` | 8, 32 | API-Vertrag (Init/Deinit beim Aufrufer) |
| `src/main.cpp` | 9–10, 177, 603–608, 806, 1357–1372, 1415, 1525–1540 | `init`/`deinit`, zwei Scan-Stellen, Adv-Callback im Wizard |
| `src/web/setup_portal.cpp` | 9, 305, 327 | `init`/`deinit(true)` um den Portal-Scan |

→ Die BLE-Abhängigkeit ist **gut lokalisiert**: im Wesentlichen das Verzeichnis
`src/ble/` plus drei Lifecycle-Stellen (`main.cpp`, `setup_portal.cpp`). Keine
BLE-Typen lecken in `web/`, `cloud/`, `crypto/`, `storage/`.

### 2.2 Sensible Stellen (höchstes Regressionsrisiko)

1. **Notify-Callback-Threading.** `_notifyCallback` läuft im BLE-Host-Task,
   nicht im Loop-Task. Die Synchronisation über `_mutex`/`_encMutex` und der
   Kommentar bei `_sendEncryptedPacket` (Antwort-Notification kann **synchron**
   während `writeValue` im selben Tick feuern) sind exakt auf das
   Bluedroid-Timing abgestimmt. NimBLE liefert Notifications über einen
   **eigenen Task** (Default) — Reihenfolge und Reentrancy können sich ändern.
2. **`deinit(true)` ist nicht umkehrbar** innerhalb eines Boots (Controller-RAM
   wird freigegeben). Beide Stacks teilen dieses Verhalten — die bestehende
   „danach rebooten"-Logik bleibt gültig.
3. **WiFi/BLE-Koexistenz und Heap-Timing.** Reihenfolge „BLE vor WiFi"
   (`main.cpp:176`) und der TLS-Heap-Workaround hängen am realen
   Speicherverbrauch — der ändert sich mit NimBLE (gewollt), muss aber
   **gemessen** werden.

## 3. Zielbild

- Bibliothek: **`h2zero/NimBLE-Arduino`**, Version **gepinnt** in
  `platformio.ini` (siehe 4.1).
- `src/ble/` nutzt NimBLE-Typen; das **öffentliche Interface** von
  `CasambiClient` und `CasambiScan` bleibt **unverändert** (keine NimBLE-Typen
  in den Headern, soweit möglich → siehe 5.1).
- Nach außen identisches Verhalten: Scan-Ergebnisse, Verbindungsaufbau,
  Key-Exchange, Auth, Steuerbefehle, Auto-Reconnect, WebSocket-Push.

## 4. Risikobegrenzung: Leitplanken

### 4.1 Version pinnen und bewusst wählen

NimBLE-Arduino **2.x** ist die aktuelle Linie (zur Zeit der Erstellung 2.1.0)
und unterstützt Arduino-ESP32 sowohl der 2.x- als auch der 3.x-Core-Reihe. Die
**1.4.x**-Linie hat eine API, die dem alten `BLEDevice` etwas näher ist.

**Empfehlung: NimBLE-Arduino 2.x, exakt gepinnt** (z. B. `h2zero/NimBLE-Arduino
@ 2.1.0`). Begründung: 2.x wird gepflegt, ist mit aktuellen Cores kompatibel,
und der einmalige Mehraufwand der 2.x-Scan-/Subscribe-API ist klein gegenüber
der Arbeit, später erneut migrieren zu müssen. Ein offizieller
[1.x→2.x-Migration-Guide](https://github.com/h2zero/NimBLE-Arduino) existiert.

> Achtung Nebenrisiko: `platform = espressif32` ist in `platformio.ini`
> **nicht gepinnt**. Im Zuge der Migration sollte die Plattformversion
> ebenfalls gepinnt werden, damit Core- und NimBLE-Version reproduzierbar
> zusammenpassen.

### 4.2 Parallele Build-Umgebung statt Big-Bang

Eine neue PlatformIO-Umgebung `env:nimble` (erbt von `devkit-v4`, fügt die
NimBLE-Lib und `-DUSE_NIMBLE` hinzu) **ohne** die bestehenden Umgebungen zu
verändern. So bleibt der alte Bluedroid-Build während der gesamten Migration
**jederzeit baubar und flashbar** — sofortiger Vergleich und Rollback.

Optional als Brücke: die paar Aufrufe in `src/ble/` per `#ifdef USE_NIMBLE`
umschalten. Das hält **einen** Branch baubar für **beide** Stacks, bis NimBLE
verifiziert ist; danach werden die `#ifdef`s und der alte Pfad entfernt.

### 4.3 Kleine, prüfbare Schritte (siehe 7)

Scan zuerst (zustandslos, gut isolierbar), dann Client. Nach jedem Schritt
bauen + auf echter Hardware testen, bevor der nächste beginnt.

### 4.4 Heap vor/nach messen

Vor der Migration auf dem Bluedroid-Build den freien Heap an drei definierten
Punkten protokollieren (nach `BLEDevice::init`, nach Auth, direkt vor dem
TLS-Handshake). Nach der Migration dieselben Punkte. Damit wird der RAM-Gewinn
belegt **und** geprüft, ob der TLS-Heap-Workaround weiter nötig ist. Das
vorhandene Heap-Logging (`heapDebugEnabled`) eignet sich als Basis.

### 4.5 Rollback-Kriterium vorab definieren

Die Migration gilt als gescheitert (→ Branch nicht mergen), wenn eines zutrifft:
Verbindungsaufbau/Auth schlägt reproduzierbar fehl, Unit-State-Notifications
gehen verloren, neue Reboots/Watchdog-Resets, oder der freie Heap ist **nicht**
besser als vorher. Rückrollen = bei der `devkit-v4`-Umgebung bleiben; der
NimBLE-Branch wird verworfen oder pausiert.

## 5. Architektur der Umstellung

### 5.1 Header von NimBLE-Typen freihalten

`casambi_client.h` exponiert heute `BLEClient*` und `BLERemoteCharacteristic*`
als private Member sowie die Bluedroid-Notify-Signatur. Zur Risikominderung und
um Include-Streuung zu vermeiden:

- **Bevorzugt:** Member auf NimBLE-Pendants umstellen, aber die `#include`s nur
  in die `.cpp` ziehen und im Header **vorwärts-deklarieren**
  (`class NimBLEClient; class NimBLERemoteCharacteristic;`). Dann ist NimBLE ein
  reines Implementierungsdetail von `src/ble/`.
- Das öffentliche API (Methoden, Enums, Callback-`std::function`-Typen) bleibt
  **byte-identisch** — `main.cpp`, `web/`, `cloud/` müssen nicht angefasst
  werden, außer den drei Lifecycle-Aufrufen (Init/Deinit, 5.3).

### 5.2 API-Mapping (Bluedroid → NimBLE 2.x)

| Bluedroid (heute) | NimBLE 2.x | Hinweis |
|---|---|---|
| `BLEDevice::init(name)` | `NimBLEDevice::init(name)` | gleich |
| `BLEDevice::deinit(true)` | `NimBLEDevice::deinit(true)` | gleich; Controller-RAM frei |
| `BLEDevice::createClient()` | `NimBLEDevice::createClient()` | NimBLE kann Clients poolen/wiederverwenden |
| `BLEClient::connect(BLEAddress)` | `NimBLEClient::connect(NimBLEAddress)` | Adress-Typ (public/random) ggf. relevant |
| `getService(BLEUUID)` | `getService(NimBLEUUID)` | gleich |
| `getCharacteristic(BLEUUID)` | `getCharacteristic(NimBLEUUID)` | gleich |
| `chr->readValue()` → `std::string` | `chr->readValue()` → `NimBLEAttValue` | `NimBLEAttValue` konvertiert zu `std::string`; Längenprüfungen prüfen |
| `chr->writeValue(data, len)` | `chr->writeValue(data, len, response)` | `response`-Flag explizit setzen (Write **with/without** response — Verhalten verifizieren!) |
| `chr->registerForNotify(cb)` | `chr->subscribe(true, cb)` | **Callback-Signatur geändert** (5.2.1) |
| `BLEScan* = BLEDevice::getScan()` | `NimBLEScan* = NimBLEDevice::getScan()` | gleich |
| `setAdvertisedDeviceCallbacks(BLEAdvertisedDeviceCallbacks*)` | `setScanCallbacks(NimBLEScanCallbacks*)` | **Klasse + Methode umbenannt** |
| `onResult(BLEAdvertisedDevice dev)` (per Wert) | `onResult(const NimBLEAdvertisedDevice* dev)` (Zeiger) | Zugriff via `->` statt `.` |
| `scan->start(sec, false)` | `scan->getResults(sec*1000, false)` bzw. `start(...)` | Signatur/Einheit (ms) prüfen |
| `dev.getServiceUUID().equals(uuid)` | `dev->getServiceUUID() == uuid` | Vergleichsoperator |
| `dev.haveName()/getName()` etc. | identische Methoden auf Zeiger | gleich |

#### 5.2.1 Notify-Callback

Bluedroid:
`void cb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool isNotify)`.
NimBLE 2.x:
`void cb(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool isNotify)`
— gleiche Parameter, nur der Typ des ersten Arguments ändert sich. Der
bestehende statische-Trampolin-Aufbau (`g_clientInstance`) bleibt nutzbar.

### 5.3 Lifecycle-Stellen

`main.cpp` (177, 806, 1357 ff., 1415, 1525 ff.) und `setup_portal.cpp`
(305, 327) tauschen nur `BLEDevice::` → `NimBLEDevice::`. Die Semantik von
`init`/`deinit(true)` ist äquivalent; die „danach rebooten"-Logik bleibt.

## 6. Funktionsspezifische Risiken und Maßnahmen

### 6.1 Notify-Timing/Threading (höchstes Risiko)
NimBLE ruft Notify-Callbacks aus einem dedizierten Task auf. Der Kommentar bei
`_sendEncryptedPacket` beschreibt ein **synchrones** Bluedroid-Verhalten
(Antwort feuert während `writeValue`). Unter NimBLE feuert sie eher
**asynchron** kurz danach.
→ Maßnahme: Die `_encMutex`-Logik ist bereits auf „Callback aus fremdem Task"
ausgelegt und sollte tragen. Trotzdem: Auth- und Key-Exchange-Flow (die auf
`_totalReceivedPackets`-Polling mit Timeouts warten) auf echter Hardware
gezielt testen. Die Polling-Timeouts (2 s/5 s) bleiben als Sicherheitsnetz.

### 6.2 Write with/without response
Casambi-Steuerung schreibt via `writeValue`. Ob „write with response" oder
„without" — bisher implizit durch Bluedroid bestimmt. Unter NimBLE ist das
explizit. → Beide Varianten testen; das alte Verhalten als Default
reproduzieren, um Timing/Zuverlässigkeit identisch zu halten.

### 6.3 Adress-Typ beim Connect
Casambi-Geräte werden über feste MAC verbunden. NimBLE unterscheidet
public/random Adressen schärfer. → Falls `connect` fehlschlägt, Adress-Typ aus
dem Scan-Ergebnis übernehmen statt String-Adresse blind zu verwenden.

### 6.4 Optionaler Folgenutzen: TLS-Heap-Workaround
Wenn NimBLE genug RAM spart, ist evtl. kein voller `deinit(true)` mehr vor dem
Cloud-Zugriff nötig. **Bewusst außerhalb des Pflichtumfangs** — erst messen
(4.4), separat und nur wenn die Kernmigration stabil ist. Risiko sonst: zwei
Variablen gleichzeitig geändert.

### 6.5 Größe von NimBLE-Buffers / MTU
NimBLE hat eigene Buildtime-Defaults (max. Verbindungen, MTU,
ATT-Buffer-Größen) über `nimconfig`/Build-Flags. Defaults genügen für 1
Verbindung; MTU-Verhalten beim Lesen der Device-Info (21 Byte) und der
Packets verifizieren.

## 7. Umsetzungsreihenfolge (jeder Schritt einzeln testbar)

1. **Vorbereitung/Messung.** Heap an den drei Punkten (4.4) auf dem
   Bluedroid-Build protokollieren als Referenz. Platform-Version pinnen.
2. **Build-Umgebung.** `env:nimble` in `platformio.ini` mit gepinnter
   NimBLE-Lib und `-DUSE_NIMBLE`. Leerer Build muss durchlaufen.
3. **Scan migrieren** (`casambi_scan.cpp`, plus die zwei Scan-Stellen in
   `main.cpp` und Portal-Scan). Zustandslos, kleinste Einheit. Test:
   Portal-/Wizard-Scan findet dieselben Casambi-Geräte wie vorher.
4. **Client migrieren** (`casambi_client.*`): Connect → Service/Char →
   Notify-Subscribe → readValue/writeValue. Test: Key-Exchange, Auth,
   Steuerbefehl (Level setzen), eingehende Unit-State-Notification,
   Auto-Reconnect nach Link-Loss.
5. **Lifecycle/Deinit** an allen Stellen umstellen; Cloud-Refresh und
   Wizard-Pfad testen (BLE frei → TLS erfolgreich → Reboot in Betriebsmodus).
6. **Heap-Vergleich** gegen die Referenz aus Schritt 1; Akzeptanzkriterien (8)
   prüfen.
7. **Aufräumen.** `#ifdef`-Brücken und Bluedroid-Pfad entfernen, alte
   Build-Umgebungen auf NimBLE umstellen, Doku/README aktualisieren.

## 8. Akzeptanzkriterien

- Scan findet dieselben Casambi-Netze (Anzahl/MACs/Namen) wie der
  Bluedroid-Build.
- Voller Verbindungs-Lebenszyklus funktioniert: Connect → ECDH → Auth →
  Steuerbefehle → eingehende State-Updates → sauberer Disconnect →
  Auto-Reconnect.
- Cloud-Refresh und Erst-Provisionierung (Portal + Wizard) laufen inkl.
  BLE-Deinit/TLS/Reboot durch.
- **Freier Heap nach Auth messbar höher** als auf dem Bluedroid-Build.
- Über einen mehrstündigen Dauerlauf **keine** neuen Watchdog-/Heap-Reboots.
- Öffentliches API von `CasambiClient`/`CasambiScan` unverändert (keine
  Änderungen außerhalb `src/ble/` außer den Lifecycle-Aufrufen).

## 9. Rollback

Solange die Brücken-Variante (4.2) aktiv ist: einfach die `devkit-v4`-
(Bluedroid-)Umgebung flashen. Der NimBLE-Code lebt isoliert in `src/ble/`
hinter `#ifdef`/eigener Build-Umgebung; ein fehlgeschlagener Versuch berührt den
produktiven Bluedroid-Pfad nicht. Branch `claude/ble-nimbble-migration-uuwzu4`
bleibt bis zur Abnahme separat.

## 10. Aufwandsschätzung (grob)

| Schritt | Aufwand |
|---|---|
| Build-Umgebung + Lib pinnen | klein |
| Scan-Migration | klein |
| Client-Migration (inkl. Notify-Timing-Tests) | mittel — Kern des Risikos |
| Lifecycle/Deinit + Cloud-/Wizard-Tests | klein–mittel |
| Heap-Messung + Dauerlauf | mittel (v. a. Wartezeit) |
| Aufräumen/Doku | klein |

Der mit Abstand sensibelste Block ist die **Client-Migration mit
Notify-Timing** (6.1) — dort liegt der Testschwerpunkt.

## Quellen

- [h2zero/NimBLE-Arduino (GitHub, inkl. 1.x→2.x Migration Guide)](https://github.com/h2zero/NimBLE-Arduino)
- [NimBLE-Arduino 2.1.0 Release Notes](https://newreleases.io/project/github/h2zero/NimBLE-Arduino/release/2.1.0)
- [esp-nimble-cpp v2.0.0 Changelog (ESP Component Registry)](https://components.espressif.com/components/h2zero/esp-nimble-cpp/versions/2.0.0/changelog?language=en)
