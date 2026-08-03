# Konzept: Apple Home / HomeKit — Footprint-Abschätzung

Status: **abgeschlossen — nicht weiterverfolgt.** Reine Machbarkeits- und
Footprint-Analyse; es wurde nichts implementiert und es ist auch nichts
geplant. Die Anbindung an Apple Home läuft bereits über den Raspberry Pi
(Variante A, Abschnitt 3); das native HAP im ESP32 (Variante B) ist mit dem
hier ermittelten Heap-Bedarf und dem Webserver-Blocker aus 4.4 **verworfen**.
Das Dokument bleibt als Entscheidungsgrundlage stehen — falls die Frage
später mit anderer Hardware (ESP32-S3 mit PSRAM) wiederkommt, stehen die
Zahlen und die Messanleitung (Abschnitt 6) bereit.

Beantwortet die Frage „Wie groß wäre der Footprint (Flash und Heap) für eine
Homebridge, die ich in Apple Home einbinden kann?"
Branch: `claude/homebridge-apple-home-footprint-gnouc8`

## 0. Kurzantwort

Die Frage hat zwei sehr verschiedene Antworten, je nachdem **wo** die Bridge
läuft:

| Variante | Flash auf dem ESP32 | Heap auf dem ESP32 | Machbarkeit auf ATOM Lite |
|---|---|---|---|
| **A — Homebridge auf dem Raspberry Pi**, Plugin spricht die vorhandene REST-/WS-Schnittstelle | **0 B** | **0 B** | ✅ sofort, ohne Firmware-Änderung |
| **B — natives HAP im ESP32** (HomeSpan), Gerät erscheint selbst als Bridge in Apple Home | **+150–250 KB** (unkritisch) | **+40–60 KB dauerhaft**, Peaks bis ~75 KB | ❌ passt nicht ins Heap-Budget; zusätzlich ein Architektur-Blocker (Abschnitt 4.4) |

**Flash ist in beiden Varianten kein Problem.** Der Engpass bei Variante B ist
der Heap — und zwar nicht knapp, sondern um etwa den Faktor 2 zu klein, wenn
Web-UI, WebSocket-Push und BLE-Stack erhalten bleiben sollen.

> Zahlenherkunft: Der Ist-Zustand (Abschnitt 1) ist im Repo gemessen. Die
> HomeSpan-Zahlen sind teils fremd-gemessen (Flash), teils aus dem
> HomeSpan-Quelltext hergeleitet (Heap, Abschnitt 4.2) — **nicht** auf dieser
> Hardware nachgemessen. Abschnitt 6 beschreibt, wie man sie in ~1 h verifiziert.

## 1. Ausgangslage (gemessen)

### 1.1 Flash

| Segment | belegt | Kapazität | frei |
|---|---|---|---|
| App (`huge_app`, `app0` = 0x300000) | 1 596 065 B (1,52 MiB) | 3 145 728 B (3 MiB) | **1,48 MiB** |
| statisches RAM (Linker) | 59 572 B | 532 480 B | — |

(README, Abschnitt „Tested Hardware", Environment `devkit-v4`.)

### 1.2 Heap im Betrieb

Aus `docs/konzept-asynctcp-churn-stabilitaet.md` (Stresstest-Läufe auf echter
Hardware, Werte aus `/api/status`):

| Situation | `free_heap` | `largest_block` |
|---|---|---|
| Leerlauf / nach Cooldown | 90–96 KB | 37–83 KB |
| unter Misch-Last (HTTP + WS) | 37–58 KB | 7–27 KB |
| `min_free_heap` im Stresstest | **7–16 KB** | — |

Dazu die harten Grenzen aus `src/config.h`:

- `HEAP_CRITICAL_THRESHOLD` = 20 000 B → 3× in Folge unterschritten ⇒ **Reboot**
- `WS_SEND_HEAP_MARGIN` = 4 096 B, `HTTP_EXPENSIVE_GET_HEAP_FLOOR` = 8 192 B
  (Admission-Control, damit teure Antworten gar nicht erst begonnen werden)
- kein PSRAM auf dem ATOM Lite (ESP32-PICO-D4)

Das **operativ verfügbare Polster** ist also nicht „90 KB", sondern grob
**30–40 KB oberhalb der Reboot-Schwelle unter Last** — und genau in dieses
Polster müsste HAP hinein.

## 2. Was „Homebridge" hier bedeuten kann

1. **Homebridge** (Node.js, github.com/homebridge/homebridge) — läuft auf einem
   Rechner im LAN, präsentiert sich Apple Home als HAP-Bridge und übersetzt auf
   beliebige APIs. Der ESP32 bleibt unverändert ein REST-/WS-Gerät.
2. **Native HomeKit-Bridge im ESP32** — die Firmware implementiert HAP selbst
   (HomeSpan, esp-homekit-sdk), das Gerät taucht direkt in Apple Home auf, kein
   Zwischenrechner.

Beides ergibt in der Home-App dasselbe Bild (eine Bridge mit N Leuchten).
Der Footprint unterscheidet sich fundamental.

## 3. Variante A — Homebridge auf dem Pi

**ESP32-Footprint: 0 Flash, 0 Heap.** Die Firmware braucht keine Zeile Änderung;
alles, was eine HomeKit-Bridge benötigt, ist bereits da:

| HomeKit braucht | vorhandene Schnittstelle |
|---|---|
| Geräteliste + Fähigkeiten | `GET /api/units` (`controls[]`, `hasCCT`, `hasVertical`, `numChannels`, `cctMin/Max`) |
| stabile Identität (UUID pro Accessory) | `uuid` bzw. `address` im `hello`-Snapshot |
| Zustand ohne Polling | WebSocket `/ws`: `hello` + `unit_state` bei jeder Änderung, < 100 ms |
| Schalten/Dimmen | `POST /api/units/:id/on\|off\|level\|temperature\|vertical` |
| atomares Mehrkanal-Setzen | `POST /api/units/:id/state` (API ≥ 1.1) |
| Erreichbarkeit | `online` je Unit, `connection_state` (BLE-Link) |
| Auth | `X-API-Key` = SHA-256(`casambi-api:` + Passwort), für WS auch `?k=<token>` |

### 3.1 Mapping auf HomeKit-Characteristics

| Casambi | HomeKit | Umrechnung |
|---|---|---|
| `on` | `On` (Lightbulb) | 1:1 |
| `level` 0–255 | `Brightness` 0–100 % | `round(level / 255 * 100)`, zurück `round(pct / 100 * 255)` |
| `temperature` (`colorTemp` 0–255 + `cctMin/cctMax`) | `ColorTemperature` in **Mired** 140–500 | `kelvin = cctMin + colorTemp/255*(cctMax-cctMin)`; `mired = 1e6/kelvin`; HomeKit-Range aus `cctMin/cctMax` ableiten (`setProps({minValue, maxValue})`) — die Home-App zeigt sonst einen Regelbereich, den die Leuchte nicht kann |
| `vertical` | kein HomeKit-Pendant | pragmatisch: zweite `Lightbulb` („… Indirekt") mit `Brightness`, oder `Fan`-`RotationSpeed`; Custom-Characteristics zeigt die Home-App nicht an |
| mehrere `dimmer`-Kanäle (Oligo Grace) | zwei `Lightbulb`-Services in einem Accessory | Schreiben über `/state` gebündelt, sonst setzt die Leuchte den jeweils anderen Kanal zurück |
| `online: false` | `SERVICE_COMMUNICATION_FAILURE` | Fehlerstatus statt „aus" melden |

### 3.2 Aufwand und Footprint auf dem Pi

- Plugin: ~300–500 Zeilen TypeScript (`homebridge` + `ws`), eine WS-Verbindung
  für alle Accessories, `updateValue()` bei `unit_state` → keine Pollinglast.
  Achtung: Der ESP32 lässt **3 gleichzeitige WS-Clients** zu
  (`WS_MAX_CLIENTS`); FHEM belegt bereits einen.
- Der Debounce beim Dimmen gehört ins Plugin (Home-App feuert beim Slider viele
  `Brightness`-Writes) — zusammenfassen und über `/state` als ein
  BLE-Telegramm schicken.
- Pi-Ressourcen: Homebridge selbst ~80–150 MB RSS (Node.js), Plugin ~1 MB.
  Auf einem Pi irrelevant, dort steht ohnehin schon die Build-/FHEM-Umgebung.
- Ohne Eigenentwicklung geht es auch: generische HTTP-Plugins oder der
  HomeKit-Bridge-Integration von Home Assistant — dann allerdings ohne den
  Push-Pfad, also mit Polling und träger Statusanzeige.

## 4. Variante B — natives HAP im ESP32 (HomeSpan)

Referenzimplementierung ist [HomeSpan](https://github.com/HomeSpan/HomeSpan)
(Arduino-ESP32, MIT). Espressifs `esp-homekit-sdk` ist reines ESP-IDF und
passt nicht in den Arduino-Framework-Build dieses Projekts;
`Arduino-HomeKit-ESP32` ist ausdrücklich unmaintained.

### 4.1 Flash: +150–250 KB — unkritisch

Belastbarster Fremdwert: In
[HomeSpan-Issue #591](https://github.com/HomeSpan/HomeSpan/issues/591) meldet
ein Nutzer für dieselbe Skizze **1 244 957 B ohne** und **1 430 061 B mit**
`homeSpan.poll()` — die Differenz von **185 104 B** ist praktisch genau der
inkrementelle HAP-Code (HAP-Statemaschine, TLV8, SRP-6A, HKDF, die
Characteristic-Tabellen sowie libsodium für Ed25519/Curve25519/
ChaCha20-Poly1305), da alles Übrige ohne den Poll-Aufruf wegoptimiert wird.

| Posten | grob |
|---|---|
| HomeSpan-Kern + libsodium + mbedTLS-Ergänzungen (SHA-512, Bignum) | ~150–200 KB |
| Casambi→HAP-Adapter (Accessory-Aufbau, Update-Callbacks, Mapping) | ~10–20 KB |
| ggf. Portierung des Webservers auf die synchrone `WebServer`-Lib (4.4) | ±0, eher −20 KB |
| **Summe** | **~150–250 KB** |

Ergebnis: **1,52 MiB → ~1,75 MiB von 3 MiB**, weiterhin ~1,2 MiB frei. Flash
ist bei `huge_app` schlicht kein Thema. (Der übliche HomeSpan-Fallstrick „passt
nicht in 1,3 MB" betrifft das Default-Partitionsschema — dieses Projekt nutzt
bereits `huge_app.csv`.)

### 4.2 Heap: +40–60 KB dauerhaft — der eigentliche Engpass

Hergeleitet aus dem HomeSpan-Quelltext (`src/HAP.h`, `src/HomeSpan.h`,
`src/SRP.h`, Stand master):

**a) Grundlast, unabhängig von der Netzgröße**

| Posten | Beleg | grob |
|---|---|---|
| `HapOut`-Streampuffer (Klartext + verschlüsselt, je 1 KB) | `HAP.h:174 bufSize=1024` | ~2 KB |
| Controller-Liste (bis `MAX_CONTROLLERS=16`, LTPK + ID je Eintrag) | `HAP.h:91` | ~1–2 KB |
| mDNS-Advertising mit HAP-TXT-Records | ESPmDNS | ~2–4 KB |
| HomeSpan-Interna (Config, SHA-384-Hash, NVS-Caches, Listen) | — | ~2–4 KB |
| **Summe Grundlast** | | **~8–12 KB** |

**b) Accessory-Datenbank — skaliert mit der Anzahl Leuchten**

Jede Leuchte wird ein Accessory aus `SpanAccessory` +
`AccessoryInformation` (6 Characteristics) + `LightBulb`
(`On`, `Brightness`, `ColorTemperature`). `SpanCharacteristic` ist ~112 B
(`HomeSpan.h:627 ff.`: 3× `UVal` für min/max/step, `UVal` value + newValue,
6 Zeiger, `EVLIST`-Vector) — mit Malloc-Header, Service-Vectors und den
heap-kopierten Namensstrings landet man bei:

| | pro Leuchte |
|---|---|
| SpanAccessory + 2 Services inkl. `req`/`opt`-Vectors | ~0,4 KB |
| 9–10 Characteristics à ~124 B | ~1,2 KB |
| Strings (Name, Hersteller, Modell, Seriennummer, Firmware) | ~0,15 KB |
| **Summe** | **~1,5–2 KB** |

Eine Leuchte mit `vertical` (zweiter Lightbulb-Service) oder zwei Dimmern
kostet ~0,5 KB mehr.

| Netzgröße | Accessory-DB |
|---|---|
| 5 Leuchten | ~8–10 KB |
| 10 Leuchten | ~15–20 KB |
| 20 Leuchten | ~30–40 KB |
| 30 Leuchten | ~45–60 KB |

**c) Pro HAP-Verbindung**

HomeSpan hält standardmäßig 8 Verbindungsslots (`setMaxConnections()`).
Realistisch offen sind 1–3 dauerhafte (Home-Hub, HomePod/Apple TV) plus
transiente von iPhones/iPads.

| Posten | Beleg | grob |
|---|---|---|
| `HAPClient` + Session-Keys (a2c/c2a, Curve25519) | `HAP.h:107–116` | ~0,3 KB |
| lwIP-Socket (PCB, pbufs, Sendefenster) | ESP-IDF-Defaults | ~1–2 KB Ruhe, 5–6 KB unter Transfer |
| HTTP-Requestpuffer, `TempBuffer<uint8_t> httpBuf` | `HAP.h:90 MAX_HTTP=8096`, `HAP.cpp:123` | bis 8 KB **transient** |
| **je aktiver Controller** | | **~2 KB Ruhe, 6–12 KB im Zugriff** |

**d) Einmalige Peaks**

- **Pairing (SRP-6A, 3072 Bit, HAP §5.5):** `SRP6A` hält 14 `mbedtls_mpi`
  à bis zu 384 B plus `_rr`-Helper und `Verification` (400 B) — mit den
  mbedTLS-Temporaries **~10–15 KB transient**, davon ein Teil als
  zusammenhängender Block. Die Hardware-MPI-Einheit des ESP32 entschärft das
  gegenüber reiner Software-Exponentiation; Vergleichswert aus dem
  Arduino-HomeKit-Umfeld: Pairing gelingt ab ~14 KB freiem Heap. Fällt nur beim
  erstmaligen Koppeln an.
- **`GET /accessories`:** aktuelle HomeSpan-Versionen streamen die Antwort in
  1-KB-Records (`HapOut`, zweifacher Durchlauf für Content-Length), es gibt also
  **keinen** Großblock proportional zur Netzgröße mehr. Das war in älteren
  Versionen der limitierende Faktor (vgl.
  [Issue #684](https://github.com/HomeSpan/HomeSpan/issues/684): Nutzer stießen
  bei ~40 KB `largest_block` an die Wand). Bei einem Update-Fenster ohne
  Streaming wäre die Bewertung deutlich schlechter.

### 4.3 Bilanz gegen das vorhandene Budget

Szenario: 15 Leuchten, 2 dauerhafte Controller-Verbindungen.

| | Heap |
|---|---|
| Grundlast | ~10 KB |
| Accessory-DB (15 × ~1,7 KB) | ~26 KB |
| 2 dauerhafte Verbindungen (Ruhe) | ~4 KB |
| **dauerhaft** | **~40 KB** |
| + gleichzeitiger HAP-Request (8 KB Puffer + Socket) | **~50 KB Peak** |
| + Pairing | **~55 KB Peak** |

Dagegen der Ist-Zustand:

| Situation | heute frei | nach HAP (−40 KB) |
|---|---|---|
| Leerlauf | 90–96 KB | ~50–56 KB — ginge |
| unter Misch-Last (HTTP + WS) | 37–58 KB | **−3 … +18 KB** |
| `min_free_heap` im Stresstest | 7–16 KB | **weit unter 0** |

Unter Last liegt das Ergebnis **unterhalb** von `HEAP_CRITICAL_THRESHOLD`
(20 KB) — also im Bootschleifen-Bereich, und das schon bevor der
`largest_block` (unter Last 7–27 KB) gegen den 8-KB-HTTP-Puffer und den
SRP-Block gehalten wird. Die Aussage ist robust gegen Detailfehler in der
Schätzung: selbst bei halbem Bedarf bliebe kein sinnvolles Polster.

Passen würde es nur, wenn parallel etwas Großes wegfällt — was bei Variante B
ohnehin ansteht (4.4) — oder auf einem Board **mit PSRAM**: HomeSpan allokiert
`SpanAccessory`/`SpanService`/`SpanCharacteristic`/`SRP6A` über `HS_MALLOC`
(`HomeSpan.h`, `src/PSRAM.h`) explizit bevorzugt aus PSRAM. Auf einem ESP32-S3
mit 2–8 MB PSRAM verschwindet Posten (b) und der größte Teil von (d) aus dem
internen Heap. Der ATOM Lite (PICO-D4) hat keinen.

### 4.4 Der harte Blocker jenseits des Speichers

HomeSpan dokumentiert im
[ProgrammableHub-Beispiel](https://github.com/HomeSpan/ProgrammableHub)
ausdrücklich: *„ESPAsyncWebServer requires a different TCP stack and cannot be
used with HomeSpan"* — der gezeigte Weg ist die **synchrone**
`WebServer`-Library, und HomeSpans TCP-Slots müssen dafür von 8 auf 5 reduziert
werden.

Dieses Projekt steht komplett auf `AsyncTCP` + `ESPAsyncWebServer`:
`src/web/webserver.cpp` (1 956 Zeilen), `src/web/setup_portal.cpp` (612 Zeilen),
die WebSocket-Push-Schicht und die gesamte Stabilitätsarbeit aus #18
(`docs/konzept-asynctcp-churn-stabilitaet.md`, `scripts/stress_test.py`,
`scripts/verify_tcp_stack.py`). Variante B hieße:

- Portierung von REST-API, Dashboard und Setup-Portal auf die synchrone
  `WebServer`-Lib — die blockierend im `loop()` läuft, in dem auch der
  BLE-Client und der Watchdog (45 s) hängen,
- **Verlust des WebSocket-Push** (die synchrone Lib hat keinen) — damit bricht
  die FHEM-Integration in ihrer heutigen Form,
- erneutes Durchlaufen der kompletten Churn-/Stabilitätsverifikation,
- dazu Kleinigkeiten: Portkonflikt (HAP will 80, `setPortNum()` nötig),
  HomeSpan-master verlangt Arduino-ESP32-Core ≥ 3.3.0 (`src/version.h`) —
  `platform = espressif32` in `platformio.ini` ist unpinned, das ist vorab zu
  prüfen; `huge_app` hat keinen zweiten OTA-Slot, HAP-Geräte will man aber
  eher over-the-air aktualisieren.

Der Speicher ist also nicht einmal das teuerste Argument.

## 5. Entscheidung

**Variante A — und zwar bereits im Einsatz.** Die Apple-Home-Anbindung läuft
über den Raspberry Pi; am ESP32 ändert sich dadurch nichts. **Variante B ist
verworfen** und wird nicht weiterverfolgt: der Heap-Bedarf passt auf dem ATOM
Lite nicht (4.3), und der Webserver-Blocker (4.4) würde den Preis auch dann
nicht rechtfertigen, wenn er es täte. Die folgenden Absätze halten fest,
*warum* — nicht, was noch zu tun wäre.

Variante A kostet auf dem ESP32 exakt nichts, nutzt mit dem
WebSocket-Push genau den Pfad, für den er gebaut wurde, und lässt die
BLE-/Web-Architektur unangetastet. Die HomeKit-Logik (Mired-Umrechnung,
Debounce, Fehlerstatus) liegt dort, wo sie billig zu ändern ist — auf dem Pi,
neben FHEM.

Zwei Wege blieben, falls die Frage später doch noch einmal aufkommt — beide
sind ausdrücklich **nicht** eingeplant und hier nur festgehalten, damit die
Analyse nicht erneut geführt werden muss:

- **ESP32-S3 mit PSRAM** als Zielboard; dann trägt PSRAM die Accessory-DB, und
  es bleibt „nur" die Webserver-Portierung aus 4.4.
- **Zweiter ESP32 als reine HAP-Bridge**, der den bestehenden Controller über
  dessen REST-/WS-API anspricht — funktional identisch zu Variante A, nur ohne
  Pi. Dort ist der Heap frei, weil kein BLE-Stack und kein Casambi-Protokoll
  darauf laufen.

## 6. Messanleitung (nicht ausgeführt)

Die Zahlen in 4.1/4.2 sind hergeleitet, nicht auf dieser Hardware gemessen —
und werden es nach der Entscheidung in Abschnitt 5 auch nicht. Wer sie doch
einmal hart machen will, braucht dafür etwa eine Stunde:

1. **Flash:** `homespan/HomeSpan` in ein Wegwerf-Environment aufnehmen, eine
   minimale Bridge mit 15 Lightbulb-Accessories bauen, `pio run -t size`
   gegen den heutigen Wert (1 596 065 B) halten. Erwartung: +150–250 KB.
2. **Heap:** dieselbe Skizze flashen und `ESP.getFreeHeap()` /
   `heap_caps_get_largest_free_block()` an drei Punkten loggen — nach
   `homeSpan.begin()`, nach Aufbau der Accessory-DB, während `GET /accessories`
   eines gekoppelten Controllers. Erwartung: ~10 KB / ~35 KB / ~50 KB Delta.
3. Erst wenn (2) deutlich besser ausfällt als hier geschätzt, lohnt die Frage
   nach 4.4 überhaupt.
