# Konzept: Matter-Bridge neben dem REST-API (Issue #44)

Status: **Konzept / Entscheidungsvorlage** — noch keine Implementierung.
**Entscheidung des Auftraggebers: Die Hardware bleibt der M5Stack ATOM Lite
(ESP32-PICO-D4, 4 MB Flash, kein PSRAM), und ein zweites Gerät kommt nicht in
Frage.** Damit entfallen alle in diesem Dokument untersuchten Wege zu einer
Matter-Bridge — die Begründung steht in 4.1/5.7, die Konsequenzen in 14.
Abschnitt 15 bewertet die Nachfrage nach IFTTT und nennt die Schnittstellen-
erweiterung, die auf dieser Hardware tatsächlich trägt.
Issue: [#44](https://github.com/akumap/esp32-casambi/issues/44)
Branch: `claude/matter-bridge-no-config-zmie1w`

## 1. Ziel

Die erkannten Casambi-Geräte sollen **zusätzlich zum bestehenden REST/WebSocket-
API** als Matter-Geräte erscheinen, damit Ökosysteme wie Google Home, Apple Home
oder SmartThings sie direkt einbinden können — **möglichst ohne Konfiguration**:

- kein zusätzlicher Einrichtungsschritt im Setup-Portal,
- keine Datei, kein Mapping, keine IDs, die der Nutzer pflegen muss,
- der Nutzer scannt im Ökosystem-App einen QR-Code (bzw. tippt den Zahlencode
  ein), den das Gerät selbst erzeugt und auf seiner Weboberfläche anzeigt.

Das REST/WebSocket-API und FHEM bleiben unverändert nutzbar; Matter ist ein
**zweiter, gleichberechtigter Konsument** derselben internen Schnittstellen —
kein Ersatz und kein Umbau des bestehenden Pfades.

**Ergebnis dieses Konzepts vorweg:**

1. Mit dem **Standard-Stack** (`esp_matter`/CHIP in Standardkonfiguration, Arduino
   als Framework) passt Matter **nicht** neben die bestehende Firmware — nicht auf
   dem ESP32-WROOM-32. Belegt mit Espressifs eigenen Messwerten in Abschnitt 4.1.
2. Es gibt aber **erhebliche Speicherhebel** (Abschnitt 5): Espressif dokumentiert
   in Summe ~85–100 KB zusätzlichen freien Heap allein aus Konfiguration, dazu die
   Verlagerung der Matter-BSS-Segmente in **PSRAM**. Damit wird der On-Device-Weg
   realistisch — der Preis ist ein **PSRAM-Board** und ein Build als
   *ESP-IDF mit Arduino als Komponente*.
3. Es gibt eine **deutlich leichtere Matter-Implementierung** als CHIP: Tasmotas
   Berry-Umsetzung (~209 KB Flash, minimaler RAM-Bedarf, IP-Commissioning,
   Bridge-Modus). Sie lässt sich nicht als Bibliothek herauslösen, taugt aber als
   **Companion-Gerät** — der schnellste Weg zu Google Home ohne jedes Risiko für
   diese Firmware.

## 2. Ausgangslage (Ist-Zustand)

### 2.1 Was schon da ist und für eine Bridge passt

| Baustein | Datei | Warum er für Matter passt |
|---|---|---|
| Generisches Gerätemodell | `cloud/network_config.h` (`CasambiUnit`, `UnitControl`, `controlName()`) | Fähigkeiten kommen aus der Cloud-Fixture, nicht aus hartkodierten Typen — genau das, was ein „generischer" Bridge-Mapper braucht |
| Kommando-Queue | `web/webserver.h:72` (`BleCommand`), `web/webserver.cpp:61`/`:149` | Ein einziger Serialisierungspunkt: REST-Handler stellt ein, der loop-Task führt aus. Ein zweiter Producer (Matter) fügt sich ohne neue Nebenläufigkeit ein |
| Zustands-Callback | `casambiClient->setUnitStateCallback(...)` (`main.cpp:452`) | Push-Quelle für Attribut-Updates; heute schon per Queue vom BLE-Task entkoppelt |
| Atomarer Mehrkanal-Write | `ble/casambi_client.h:208` (`setUnitState`) | Matter-Kommandos, die mehrere Kanäle betreffen (Level + CCT), gehen als **ein** Telegramm raus |
| mDNS läuft bereits | `main.cpp:140` (`startMDNS`) | Matter braucht DNS-SD — aber Achtung, Konfliktpotenzial (4.5) |

Die Bridge muss also **keine** Casambi-Logik neu bauen. Sie ist reiner
Protokoll-Adapter: Matter-Cluster ⇄ vorhandenes Unit-Modell + Kommando-Queue.

### 2.2 Speicherlage heute

`/api/status` meldet im Normalbetrieb ca. **56 KB freien Heap**, der Tiefstand
(`min_free_heap`) liegt bei ca. **31 KB** (README, Abschnitt „Status &
Discovery"). Unter 20 KB startet die Firmware sich selbst neu
(`HEAP_CRITICAL_THRESHOLD`, `config.h:204`). Das ist der Kern der
Machbarkeitsfrage in Abschnitt 4.

### 2.3 Toolchain

`platformio.ini` verwendet `platform = espressif32` ohne Pin, und der Code
benutzt `esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true)` (`main.cpp:341`) — die
**zweiargumentige** Form, die es nur bis ESP-IDF 4.4 / **Arduino Core 2.x**
gibt. Die Firmware baut heute also gegen Core 2.x. Matter (`esp_matter`) gibt es
für Arduino erst ab **Core 3.x**.

## 3. Was „Matter-Bridge" technisch bedeutet

Eine Bridge ist im Matter-Datenmodell ein einzelner Knoten mit:

- **Endpoint 0** — Root Node (Basic Information, Netzwerk-, Fabric-Verwaltung),
- **Endpoint 1** — **Aggregator** (Device Type `0x000E`), dessen
  `Descriptor.PartsList` alle gebrückten Geräte listet,
- **je gebrücktem Gerät ein weiterer Endpoint** mit dem passenden Device Type
  (z. B. Dimmable Light) **plus** dem Cluster
  *Bridged Device Basic Information* (`NodeLabel` = Casambi-Name,
  `Reachable` = online-Flag).

Dazu kommt das **Commissioning**: Discriminator (12 bit), Setup-Passcode
(27 bit), VID/PID, daraus der Onboarding-Payload (`MT:…` als QR bzw.
11-stelliger Zahlencode), Auffindbarkeit per DNS-SD (`_matterc._udp` vor,
`_matter._tcp` nach dem Commissioning) und **Device Attestation** (DAC).
IPv6 (mindestens Link-Local) ist Pflicht.

Für diese Firmware relevant: **die dynamische Endpoint-Verwaltung**. Die
Casambi-Unit-Liste steht erst nach dem Cloud-Refresh fest und ändert sich
später wieder — die Endpoints müssen zur Laufzeit aus `NetworkConfig` gebaut
werden. Das ist genau der Mechanismus des CHIP-Bridge-Beispiels
(`NUM_DYNAMIC_ENDPOINTS`), Kosten laut Espressif **≈ 550 Byte DRAM je
Endpoint-Slot**, Standardwert 16 Slots.

## 4. Randbedingungen — die harten Grenzen

### 4.1 RAM: warum der Standard-Stack auf dem WROOM-32 nicht passt

Espressifs eigene Messwerte für das **Licht-Beispiel** (nur Matter, sonst
nichts) auf einem ESP32-C3, **Standardkonfiguration**:

| Größe | Wert |
|---|---|
| Flash (bin) | 1 476 960 Byte |
| statisch D/IRAM | 195 080 Byte |
| freier Heap beim Boot | 35 976 Byte |
| freier Heap **nach** dem Commissioning | 101 580 Byte |

Der Sprung von 36 KB auf 101 KB entsteht durch
`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING`: **nach dem Commissioning wird der
BLE-Speicher freigegeben.**

Genau das ist hier **nicht möglich** — BLE ist die Casambi-Verbindung und läuft
dauerhaft. Für diese Firmware gilt also eher die „Boot"-Zeile: ~36 KB frei bei
einer Anwendung, die *nur* Matter macht. Unsere Firmware braucht daneben
gleichzeitig NimBLE als Central, WiFi-STA, AsyncTCP/AsyncWebServer mit
WebSocket-Clients, LittleFS und den Event-Log — und hat damit heute schon nur
56 KB frei (2.2).

Dominant ist dabei **nicht** der Heap-Bedarf zur Laufzeit, sondern der
**statische Anteil**: ~195 KB D/IRAM von 320 KB, bevor die erste Zeile eigener
Code läuft. Genau dort setzen die Hebel in Abschnitt 5 an — deshalb ist die
Aussage „passt nicht" eine Aussage über die *Standardkonfiguration*, nicht über
Matter an sich.

### 4.2 Flash und Partitionen

Matter allein bringt ~1,5 MB Code mit. Zusammen mit der bestehenden Firmware
(NimBLE + AsyncWebServer + TLS-Cloud-Client + PROGMEM-UI) ist `huge_app.csv`
(3 MB App, keine OTA) bestenfalls knapp. Empfehlung: Matter-Target mit
**8 MB oder 16 MB Flash** und eigener Partitionstabelle (größerer NVS für
Fabrics/Attribute). Für die Attestation wird die in `esp_matter` mitgelieferte
**Test-DAC** verwendet (keine `fctry`-Partition, kein Fertigungsprozess) —
Konsequenzen dazu in 8.5.

### 4.3 BLE-Koexistenz: kein BLE-Commissioning

Matter-Geräte werben normalerweise während des Commissionings **per BLE**. Hier
belegt NimBLE den Controller dauerhaft als **Central** zum Casambi-Gateway, und
Matters BLE-Manager initialisiert/deinitialisiert den Stack selbst — ein
Übernahmekonflikt, der im schlechtesten Fall die Casambi-Verbindung abreißen
lässt.

**Entwurfsentscheidung: IP-only Commissioning.** Das Gerät ist ohnehin schon im
WLAN (die Casambi-Provisionierung erfolgt vorher, siehe
`konzept-provisionierung.md`) und braucht keine WLAN-Zugangsdaten mehr vom
Commissioner. Es wirbt also ausschließlich per DNS-SD (`_matterc._udp`) im
LAN.

Diese Entscheidung ist **praktisch erprobt**: Tasmota commissioniert seine
Matter-Geräte ausschließlich über IP/DNS-SD, mit derselben Begründung („BLE
dient vor allem der WLAN-Übergabe — die ist bei uns schon erledigt"). Das
senkt das Risiko dieses Punktes deutlich, ersetzt aber nicht die Prüfung
gegen die konkreten Ökosystem-Apps in Stufe 1.

### 4.4 Toolchain-Migration auf Arduino Core 3.x

Vorbedingung, kein Nebeneffekt (siehe 2.3). Betroffen:

| Punkt | Bewertung |
|---|---|
| Plattform-Paket | Wechsel auf ein Core-3.x-Paket (pioarduino), da das offizielle `espressif32` bei Core 2.x stehengeblieben ist |
| `esp_task_wdt_init` | Signaturwechsel (`esp_task_wdt_config_t`) — `main.cpp:341` |
| NimBLE-Arduino 2.1.0 | unterstützt Core 3.x, voraussichtlich unkritisch |
| ESP32Async AsyncTCP/ESPAsyncWebServer (gepinnt) | unterstützen Core 3.x, Pins ggf. anheben |
| WiFi-/LittleFS-/HTTPClient-API | punktuelle Anpassungen, dazu Neuverifikation von TLS-Heap und `#18`-Stabilität |

Das ist Aufwand und Regressionsrisiko **für die gesamte bestehende Firmware**
und sollte als **eigenes Issue** geführt werden — nicht versteckt in der
Matter-Umsetzung. Bis dahin bleibt Matter auf sein eigenes Environment
beschränkt.

### 4.5 mDNS-Doppelbelegung

Die Firmware registriert bereits einen mDNS-Responder (`main.cpp:140`). CHIP
bringt standardmäßig eine eigene „minimal mDNS"-Implementierung mit, die
ebenfalls Port 5353 bedient. Mitigation: CHIP auf die Plattform-mDNS
konfigurieren (`CONFIG_USE_MINIMAL_MDNS=n`) oder im Spike verifizieren, dass
beide koexistieren. Symptom bei Fehlkonfiguration: Gerät ist im Ökosystem
unauffindbar, obwohl es läuft.

### 4.6 Mengengerüst

`CLOUD_MAX_UNITS` ist 250 (`config.h:298`), CHIPs Bridge-Beispiel bringt
standardmäßig 16 dynamische Endpoint-Slots mit (≈550 B DRAM je Slot). Ein
Limit ist also unvermeidbar — Tasmota empfiehlt für seinen Bridge-Modus aus
Performancegründen sogar nur ~8 gebrückte Endpoints. Vorschlag:
`MATTER_MAX_BRIDGED_UNITS` (Default 16, im Spike gegen den gemessenen Heap
kalibriert), Auswahl **deterministisch nach `deviceId` aufsteigend**,
Überzählige werden im Dashboard und im Event-Log sichtbar gemeldet — nicht
stillschweigend verschluckt.

## 5. Speicher: Hebel und leichtere Grundlagen

Die Frage „geht es nicht sparsamer?" hat vier belastbare Antworten, in
steigender Wirkung.

### 5.1 Am eigenen Stack sparen (≈ 10–25 KB, hilfreich, nicht entscheidend)

Begrenzung gleichzeitiger WebSocket-Clients, kleinere AsyncTCP-Queues und
-Stacks, kleinere Event-Log-Puffer, NimBLE-Feintuning (nur Central-Rolle, eine
Verbindung), `WS_HELLO_MAX_UNITS` senken. Alles machbar, aber es verschiebt die
Größenordnung nicht: gegen ~195 KB statisches Matter-BSS sind 20 KB Rundung.

### 5.2 esp-matter konfigurieren (Espressifs eigene Zahlen)

Espressif dokumentiert die folgenden Einsparungen (gemessen an C3/H2,
Licht-Beispiel):

| Option | Flash | D/IRAM | freier Heap (Boot) | für uns nutzbar? |
|---|---|---|---|---|
| `CONFIG_ENABLE_CHIP_SHELL=n` | −55…−67 KB | −0,8…−2,2 KB | +9,9…+10,5 KB | ja |
| BLE-Controller-Code in Flash (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`) | — | −19,4…−19,9 KB | +19,8…+23,1 KB | ja, aber Latenz-/Durchsatzkosten am BLE — **im Spike gegen die Casambi-Stabilität messen** |
| FreeRTOS-Funktionen in Flash | — | −9,1…−10,3 KB | +9,1…+9,5 KB | ja (Performance-Trade-off) |
| SPI-Flash-ROM-Implementierung | — | −9,5…−12,7 KB | +8,3…+12,6 KB | ja |
| Log-Event-Puffer auf 256 B | — | −5,4 KB | +6,5…+7,0 KB | ja |
| Endpoint-/Device-Type-Count senken | — | −6,6 KB | +6,0…+6,8 KB | teilweise — wir *brauchen* Endpoints (≈550 B je Slot) |
| Ringbuf-Funktionen in Flash | — | −4,7 KB | +4,0…+4,6 KB | ja |
| ungenutzte Cluster ausschließen | −37 KB | −3,7 KB | +3,9 KB | ja |
| Task-Stacks verkleinern | — | — | +3,3…+3,9 KB | vorsichtig, wir haben eigene Tasks |
| Newlib-Nano-Formatierung | −47 KB | — | +1,9…+2,4 KB | ja |
| BLE-Optimierungen (max. Verbindungen, Rollen abschalten) | −23 KB | −1,7…−2,2 KB | +9,8…+19,1 KB | **nur teilweise** — die Central-Rolle ist unsere Casambi-Verbindung und darf nicht weg |

In Summe liegen realistisch **~70–90 KB** zusätzlicher freier Heap drin — die
36-KB-Zeile aus 4.1 wird damit zu ~110 KB. Das ist die Größenordnung, in der
unsere Firmware daneben existieren kann.

**Der Haken:** Das sind **IDF-Kconfig-Optionen**. Im PlatformIO-Arduino-Framework
kommt der Core als *vorkompilierte* Bibliothek mit fixer `sdkconfig` — diese
Schalter sind dort schlicht nicht erreichbar. Wer sie nutzen will, muss den
Matter-Build als **ESP-IDF mit Arduino als Komponente** fahren. Das ist ein gut
dokumentierter, aber deutlich größerer Toolchain-Schritt als die reine
Core-3.x-Migration aus 4.4 — und er beträfe nur das Matter-Environment, nicht
den `devkit-v4`-Build.

### 5.3 PSRAM — der größte Einzelhebel

Espressif beschreibt explizit, die **BSS-Segmente von `libCHIP.a` und
`libesp_matter.a` per Linker-Fragment in externes RAM zu verlagern**
(`CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). Damit verschwindet genau der
Posten, der in 4.1 dominiert — der statische Anteil — aus dem internen RAM.

Kandidaten:

| Modul | RAM | Bewertung |
|---|---|---|
| **ESP32-WROVER** (klassischer ESP32 + 4/8 MB PSRAM) | 320 KB intern + PSRAM | Nächster Verwandter der heutigen Hardware; `platformio.ini` hat die PSRAM-Flags bereits (auskommentiert) vorbereitet. Aber: Cache-Workaround (`-mfix-esp32-psram-cache-issue`) kostet Performance, PSRAM ist nicht DMA-fähig, WLAN-/BLE-Puffer müssen intern bleiben |
| **ESP32-S3 N16R8** | 512 KB intern + 8 MB PSRAM | Sauberer (Octal-PSRAM, kein Cache-Bug), mehr internes RAM, reichlich Flash — **empfohlenes Matter-Target** |
| ESP32-C6 (ohne PSRAM) | 512 KB intern | denkbar, aber ohne den Hebel aus 5.3 knapper als S3+PSRAM |

Mit 5.2 + 5.3 zusammen ist der On-Device-Weg nach heutigem Kenntnisstand
**komfortabel machbar**; das Stufe-1-Gate (Abschnitt 11) bleibt trotzdem
bestehen, weil unsere Kombination aus dauerhaftem BLE-Central, AsyncTCP und
Matter in keiner Espressif-Messung vorkommt.

### 5.4 Eine leichtere Implementierung: Tasmotas Berry-Matter

Tasmota hat Matter **komplett neu in Berry implementiert**, statt CHIP zu
verwenden:

- **~209 KB Flash** statt ~1,5 MB — laut Autor „sehr wenig im Vergleich zu
  connectedhomeip";
- **sehr geringer RAM-Bedarf**, weil der Berry-Code als Bytecode *im Flash*
  liegt („solidified") und nicht ins RAM geladen wird;
- **IP-Commissioning** über DNS-SD, kein BLE (siehe 4.3);
- Krypto über BearSSL statt mbedTLS;
- läuft auf **allen ESP32-Varianten**, in den Standard-`tasmota32`-Builds
  enthalten;
- hat einen **Bridge-Modus** mit „virtuellen Endpoints", die per Berry-Code
  getrieben werden — also frei an eine fremde HTTP-Schnittstelle koppelbar.
  Empfohlenes Limit: ~8 gebrückte Endpoints.

Das ist der Beleg, dass Matter selbst nicht schwer ist — schwer ist *CHIP*.

Zwei Nutzungsarten, eine sinnvoll, eine nicht:

- **Technisch tragfähig, hier aber ausgeschlossen — Companion-Gerät
  (Option B in Abschnitt 6; der Auftraggeber will kein zweites Gerät
  einrichten, siehe Abschnitt 14):** ein zweiter,
  billiger ESP32 mit **Standard-Tasmota** plus einem Berry-Skript, das unsere
  REST-Endpunkte und den WebSocket-Push auf virtuelle Matter-Endpoints
  abbildet. Keine Firmware-Änderung hier, kein Core-3.x-Zwang, kein PC/Server
  nötig, Hardwarekosten im einstelligen Euro-Bereich. Der Preis: ein zweites
  Gerät, ein zweiter Update-Pfad, und das Berry-Skript ist zu pflegen.
- **Nicht sinnvoll — Herauslösen:** Tasmotas Matter hängt an der Berry-VM und
  an Tasmota-Infrastruktur. „Nur die Matter-Klassen übernehmen" hieße, Berry
  und halb Tasmota in diese Firmware zu ziehen. Wer das will, betreibt in
  Wahrheit Tasmota.

### 5.5 Eigene Minimal-Implementierung — ausdrücklich nicht empfohlen

Reizvoll, aber der Aufwand liegt nicht im Datenmodell, sondern in PASE/SPAKE2+,
CASE, Sigma-Handshake, TLV, Interaction Model, Subscriptions, DNS-SD und den
Eigenheiten der Ökosysteme. Das sind Mannmonate, und Fehler liegen genau dort,
wo man sie am schlechtesten debuggt. Tasmota hat es gemacht — und braucht dafür
eine eigene Sprachumgebung und jahrelange Pflege.

### 5.6 Hilft es, dass Casambi nur Leuchten kennt?

Naheliegender Gedanke: Wir brauchen kein Thermostat, keine Türschlösser, keine
Kameras — nur Dimmer, Farbtemperatur, vereinzelt RGB. Müsste ein derart
zugeschnittener Matter-Knoten nicht viel kleiner sein?

**Bei CHIP: nein, nur am Rand.** Espressif hat genau diese Optimierung gemessen
(„ungenutzte Cluster ausschließen"): **−37 KB Flash, −3,7 KB D/IRAM,
+3,9 KB freier Heap**. Flash spart es spürbar, RAM praktisch nicht.

Der Grund: Der Speicher geht nicht ins *Datenmodell*, sondern in die
**Protokollmaschinerie**, und die ist für eine einzelne Lampe identisch mit der
für ein Smart Home voller Geräte. Jeder Matter-Knoten muss mitbringen:

- **MRP** (Message Reliability Protocol) über UDP/IPv6 — Retransmits, Acks, Backoff;
- **TLV**-Kodierung und den Message-Layer;
- **PASE**-Commissioning mit **SPAKE2+** auf P-256;
- **CASE**-Sitzungsaufbau (Sigma1/2/3) inklusive X.509-Operationszertifikaten,
  NOC/ICAC/RCAC-Kettenprüfung, ECDH und Signaturen;
- **Attestation** (DAC/PAI/CD, CSR-Request) — auch mit Testzertifikaten;
- das **Interaction Model** mit Read/Write/Invoke **und Subscriptions** inklusive
  Report-Engine samt Min-/Max-Intervall-Timern (Ökosysteme abonnieren, sie pollen nicht);
- **Access Control** (ACL-Durchsetzung je Fabric), Fabric-Tabelle, Session-Store;
- **DNS-SD**-Werbung in zwei Betriebsarten;
- die Pflicht-Cluster Basic Information, Descriptor, General/Network Commissioning,
  Operational Credentials, Administrator Commissioning, General Diagnostics.

Erst danach kommen On/Off, Level Control und Color Control — der Teil, den unser
enges Gerätespektrum überhaupt berührt.

**Was tatsächlich mit dem Zuschnitt skaliert** (und deshalb trotzdem
mitgenommen werden sollte):

| Stellschraube | Ersparnis | Anmerkung |
|---|---|---|
| ungenutzte Cluster ausschließen | ~3,9 KB Heap, 37 KB Flash | gemessen (s. o.) |
| Endpoint-Slots exakt dimensionieren | ~550 B je Slot | 8 statt 16 Slots ≈ 4,4 KB |
| Color Control weglassen (Phase 1 nur Dimmer) | einige hundert Byte je Endpoint | der fetteste Licht-Cluster |
| max. Fabrics senken (Default 5 → 2–3) | einige KB | Zertifikate + ACL-Einträge je Fabric |
| Subscriptions/Sessions/Exchanges begrenzen | einige KB | Puffer je offener Subscription |

In Summe **~10–20 KB** — willkommen, aber es ist nicht die Größenordnung, die
über den WROOM-32 entscheidet. Die Untergrenze setzt CHIP, nicht der
Gerätekatalog.

**Wo der Zuschnitt wirklich zählt:** wenn man CHIP *ersetzt*. Tasmotas 209 KB
gegenüber ~1,5 MB (5.4) zeigen das Potenzial. Aber Vorsicht bei der
Schlussfolgerung: Diese Ersparnis stammt aus der schlankeren *Umsetzung der
Maschinerie*, nicht daraus, dass nur Lampen unterstützt werden. Von einer
Eigenimplementierung entfielen auf „es sind nur Leuchten" grob 15 % der
Arbeit — die verbleibenden 85 % (SPAKE2+, CASE, MRP, TLV, IM mit
Subscriptions, ACL) sind unabhängig vom Gerätespektrum zu leisten. Bewertung
unverändert 5.5: nicht verhältnismäßig.

### 5.7 Fazit der Speicherbetrachtung

| Weg | zusätzlicher Speicherbedarf hier | Hardware | Aufwand |
|---|---|---|---|
| CHIP/esp-matter, Standardkonfig | passt nicht | — | — |
| CHIP/esp-matter + 5.2 (IDF-Build) | grenzwertig ohne PSRAM | S3/C6 | hoch |
| CHIP/esp-matter + 5.2 + 5.3 + 5.6 | komfortabel | S3 mit PSRAM | hoch |
| eigene Subset-Implementierung (5.5) | vermutlich ausreichend | vorhandene Hardware | sehr hoch, Monate, riskanteste Fehlerklasse |
| Tasmota-Companion (5.4) | **null** in dieser Firmware | zweiter ESP32 | gering — **aber zweites Gerät, vom Auftraggeber ausgeschlossen** |

## 6. Optionen im Überblick

| # | Ansatz | Aufwand | Hardware | „ohne Konfiguration" | Bewertung |
|---|---|---|---|---|---|
| **A** | Matter-Bridge **in dieser Firmware**, eigenes Environment, IDF+Arduino-as-Component, PSRAM-Target | hoch (Core-3-Migration + IDF-Build + Bridge) | **ein** Gerät — ESP32-S3 mit PSRAM statt des bisherigen Boards | ja (QR im Dashboard) | **Empfohlen** — einziger Weg, der „ein Gerät, ohne Konfiguration" wirklich erfüllt |
| **B** | **Tasmota-Companion**: zweiter ESP32 mit Standard-Tasmota + Berry-Skript auf unser REST/WS-API | gering (Skript, kein Firmware-Risiko) | zwei Geräte | nein (Flashen, WLAN, Token, Skript) | **ausgeschlossen** — der Auftraggeber will kein zweites Gerät einrichten. Bleibt nur als Rückfallebene dokumentiert, falls A am Gate scheitert |
| **C** | Externe Bridge-Software (Matterbridge / Home-Assistant-Matter-Hub) auf vorhandenem Server | gering | Dauerläufer-Host | nein | Randnotiz für HA-Nutzer, die den Host ohnehin betreiben |
| **D** | Eigene Subset-Implementierung von Matter in dieser Firmware, ohne CHIP (5.5, 5.6) | sehr hoch (Monate) | vorhandenes Board | ja | **nicht verhältnismäßig** — der Zuschnitt auf Leuchten spart nur ~15 % der Arbeit |

**Empfehlung:** A. Wichtig zur Einordnung: A bedeutet **kein zweites Gerät**,
sondern **ein anderes Board für dasselbe Gerät** — die komplette Firmware
inklusive Casambi-BLE, REST-API und Matter läuft auf einem einzigen ESP32-S3
mit PSRAM. Der Einrichtungsaufwand für den Nutzer bleibt der von heute, plus
QR-Code scannen.

## 7. Zielarchitektur (Option A)

### 7.1 Modulschnitt

```
src/control/command_queue.h   (neu)  BleCommand + Queue + Executor, aus webserver.* herausgelöst
src/matter/matter_bridge.h/.cpp (neu) Matter-Node, Endpoint-Aufbau, Attribut-Sync, Kommando-Mapping
src/matter/endpoint_map.h/.cpp  (neu) persistente Zuordnung unitId <-> endpointId
src/web/webserver.*           (Änderung) nutzt command_queue statt eigener Queue; + /api/matter
src/web/dashboard.h           (Änderung) Matter-Kachel mit QR + Zahlencode + Fabric-Status
```

Kernprinzip: **ein** Kommandopfad. REST-Handler und Matter-Handler sind beide
nur Producer auf `command_queue`; ausgeführt wird weiterhin ausschließlich im
loop-Task (`webserver.cpp:149` wandert mit). Damit bleibt die BLE-Serialisierung
unverändert, und Matter erbt automatisch Backpressure, Fehlerprotokollierung und
die vorhandenen Sicherheits-/Timeout-Eigenschaften.

Der Schnitt lohnt sich unabhängig von Matter — er ist auch die Voraussetzung
dafür, dass ein **Companion (Option B)** dieselbe Semantik sieht wie das
Dashboard.

### 7.2 Abbildung Casambi → Matter

Getrieben von `controls[]` / `controlName()` (`cloud/network_config.h`), also
**ohne** Fixture-Sonderfälle:

| Casambi-Controls der Unit | Matter Device Type | Cluster |
|---|---|---|
| nur `dimmer` | Dimmable Light (`0x0101`) | On/Off, Level Control |
| `dimmer` + `temperature` | Color Temperature Light (`0x010C`) | + Color Control (CT, Mireds) |
| `dimmer` + `rgb`/`xy` | Extended Color Light (`0x010D`) | + Color Control (HS/XY) |
| zusätzliche `dimmer1`, `vertical`, `slider` | je ein **weiterer** Dimmable-Light-Endpoint, `NodeLabel` = `"<Name> <controlName>"` | On/Off, Level Control |
| Gruppen | optional je ein Dimmable-Light-Endpoint (Phase 4) | — |
| Szenen | Phase 4, als On/Off-Endpoint (Matter kennt keinen „Szene"-Device-Type für Bridges) | — |

Umrechnungen (reine Funktionen, host-testbar):
Level Matter `1..254` ⇄ Casambi `0..255`; `OnOff` ⇄ Level 0/letzter Wert;
Mireds ⇄ Kelvin mit Klemmung auf `cctMinKelvin`/`cctMaxKelvin`; Kelvin ⇄
normalisierter Casambi-Wert existiert bereits (`cloud/state_codec.h`).

`Reachable` = `unit.online`; fällt die Casambi-Verbindung aus, werden **alle**
gebrückten Endpoints auf `Reachable=false` gesetzt (Signal an die Ökosysteme,
statt still veraltete Werte zu zeigen).

Diese Tabelle gilt **auch für Option B** — sie ist die fachliche Abbildung, nicht
die technische Umsetzung.

### 7.3 Stabile Endpoint-IDs

Ökosysteme hängen Raumzuordnung, Namen und Automationen an der **Endpoint-ID**.
Wandern IDs nach einem Cloud-Refresh, vertauschen sich beim Nutzer die Lampen.
Deshalb: `endpoint_map` als kleine LittleFS-Datei
(`unitId` + `controlName` → `endpointId`), Regeln:

- bestehende Zuordnung gewinnt immer,
- neue Units bekommen die nächste freie ID (nie eine wiederverwendete),
- verschwundene Units behalten ihren Slot zunächst und werden `Reachable=false`;
  Freigabe nur über einen expliziten Matter-Reset.

### 7.4 Zustandsfluss

```
BLE-Notify-Task → (bestehender UnitStateCallback) → Event-Queue
                → loop-Task: WebSocket-Broadcast  (unverändert)
                           + Matter-Attribut-Update (neu, nur wenn Wert sich ändert)
```

Kein `esp_matter`-Aufruf aus dem BLE-Task. Attribut-Updates werden entprellt
(nur bei tatsächlicher Änderung), damit ein Dimm-Rampen-Broadcast aus dem Mesh
keine Update-Flut in den Fabrics auslöst.

### 7.5 Kommandofluss

```
Matter-Task (CHIP) → command_queue (BleCommand) → loop-Task → CasambiClient
```

Wichtig: Matter-Level-Kommandos (`MoveToLevel` mit Transition, Slider-Wischen in
der App) kommen in Serie. Der BLE-Pfad ist um Größenordnungen langsamer.
Deshalb **Coalescing pro Unit** („letzter Wert gewinnt", ~150 ms), umgesetzt in
`command_queue`, sodass **auch das REST-API davon profitiert**. Mehrkanalige
Änderungen gehen als ein `setUnitState` (`casambi_client.h:208`) raus.

### 7.6 Verhältnis zu FHEM/WebSocket

Da alle Wege durch dieselbe Queue laufen, sieht FHEM jede über Matter ausgelöste
Änderung ganz normal als `unit_state`-Push — und umgekehrt. Es gibt keinen
zweiten „Wahrheitsstand" und keine Sonderfälle im FHEM-Modul.

## 8. „Ohne Konfiguration" — konkret

### 8.1 Pairing-Daten erzeugt das Gerät selbst

Discriminator und Setup-Passcode werden **beim ersten Matter-Start einmalig
zufällig erzeugt** (`esp_random()`), in NVS abgelegt und danach unverändert
wiederverwendet.

Bewusst **nicht** aus der eFuse-MAC abgeleitet: Die MAC ist über WLAN und BLE
öffentlich sichtbar; ein daraus berechenbarer Passcode wäre für jeden in
Funkreichweite erratbar und würde das Commissioning-Geheimnis entwerten. Die
Verbotswerte des Standards (`00000000`, `11111111`, …, `12345678`, `87654321`)
werden beim Erzeugen ausgeschlossen.

### 8.2 Anzeige

- **Dashboard** (`/`): Kachel „Matter" mit QR-Code, 11-stelligem Handeingabe-Code
  und Fabric-Status. QR wird clientseitig aus dem Payload-String gerendert
  (kleiner eingebetteter Encoder — **kein CDN**, das Gerät muss offline
  funktionieren); der Zahlencode ist der Fallback, falls der QR nicht rendert.
- **`GET /api/matter`** (authentifiziert): `{enabled, commissioned, fabrics,
  qr_payload, manual_code, endpoints:[{unit_id, control, endpoint_id, reachable}],
  units_over_limit}`.
- **Serial**: `matter` zeigt dasselbe, für den Fallback ohne Browser.

Der Pairing-Code ist ein **Geheimnis** und wird wie der API-Token behandelt: nur
über authentifizierte Endpunkte, nie in `/api/info` (das bewusst offen ist).

### 8.3 Commissioning-Fenster

Solange keine Fabric existiert, ist das Fenster automatisch offen (Basic
Commissioning Mode) — der Nutzer muss nichts drücken. Nach dem ersten Beitritt
schließt es; für ein zweites Ökosystem (Multi-Admin) gibt es einen Knopf im
Dashboard bzw. `POST /api/matter/commission-window`, der es befristet wieder
öffnet.

### 8.4 Reset

`POST /api/matter/reset` (und `matter reset` seriell) entfernt alle Fabrics und
erzeugt neue Pairing-Daten. `clearconfig` löscht zusätzlich die Endpoint-Map,
damit ein frisch provisioniertes Gerät keine Altzuordnungen erbt.

### 8.5 Wo „ohne Konfiguration" endet — ehrlich benannt

Ohne Matter-Zertifizierung (CSA-Mitgliedschaft, Test-Häuser, VID) läuft das
Gerät mit einer **Test-VID/PID** (`0xFFF1`/…). Folgen:

- **Google Home** lässt nicht zertifizierte Geräte nur zu, wenn in der *Google
  Home Developer Console* ein Projekt mit **genau dieser Test-VID/PID** angelegt
  und das Konto als Tester eingetragen ist. Das ist eine einmalige Aktion **im
  Google-Konto des Nutzers** — sie liegt außerhalb dessen, was die Firmware
  wegautomatisieren kann. Das Ziel „ganz ohne Konfiguration" ist für Google Home
  also **nicht vollständig erreichbar**; die Anleitung dazu gehört in die
  README.
- **Apple Home / SmartThings** koppeln in der Regel mit einer
  „nicht zertifiziert"-Warnung, ohne Entwicklerkonto.
- **Home Assistant** koppelt ohne Einschränkung.

Das gilt **unabhängig vom gewählten Weg** — auch ein Tasmota-Companion (Option B)
ist ein nicht zertifiziertes Matter-Gerät.

## 9. Auswirkungen auf die versionierte Schnittstelle

Neue Endpunkte (`GET /api/matter`, `POST /api/matter/reset`,
`POST /api/matter/commission-window`) sind rein additiv → `FHEM_API_VERSION_MINOR`
+1, `MAJOR` bleibt (`config.h:92`, Regeln siehe `konzept-versionierung.md`).
Optional später ein `matter`-Feld in der WebSocket-Hello und ein Reading im
FHEM-Modul; beides nicht Voraussetzung für Stufe 1–3.

## 10. Sicherheitsbetrachtung

| Punkt | Bewertung |
|---|---|
| Pairing-Passcode | zufällig, persistent, nicht aus öffentlichen Werten ableitbar (8.1) |
| Sichtbarkeit des Codes | nur über authentifizierte API/Dashboard-Sitzung |
| Angriffsfläche | Matter-Knoten lauscht dauerhaft im LAN — neue Netzwerkoberfläche, die es vorher nicht gab; im Spike mit begrenzter Fabric-Zahl testen |
| Fabric-Verwaltung | jede gekoppelte Fabric darf alle gebrückten Lampen schalten — dieselbe Vertrauensstufe wie ein API-Token, in der README benennen |
| Test-Attestation | signalisiert Ökosystemen „nicht zertifiziert" — bewusst akzeptiert (8.5) |
| Companion (Option B) | braucht einen API-Token dieser Firmware; das Token liegt dann im Klartext auf dem Tasmota-Gerät — bei der Empfehlung mitschreiben |

## 11. Umsetzung in Stufen (mit Abbruchkriterien)

| Stufe | Inhalt | Abschluss-/Abbruchkriterium |
|---|---|---|
| **0** | Beschaffung/Auswahl eines S3-Boards mit PSRAM; Core-3.x-Migration als eigenes Issue starten (4.4) | Firmware baut unverändert auf Core 3.x, `devkit-v4` bleibt grün |
| **1 — Spike (zeitbegrenzt)** | **IDF-mit-Arduino-als-Komponente**, Optimierungssatz aus 5.2, PSRAM-BSS-Verlagerung nach 5.3, Zuschnitt nach 5.6; Minimal-Matter (2 Endpoints) **gleichzeitig** mit NimBLE-Casambi und Webserver | **Gate:** ≥ 60 KB freier Heap im Betrieb, `min_free_heap` ≥ 30 KB über 24 h, Casambi-Link stabil (insbesondere mit BLE-Code im Flash, 5.2), mDNS-Konflikt geklärt. **Sonst:** Rückfallebene Option B/C, Entscheidung beim Auftraggeber |
| **2** | Bridge-Kern: `command_queue` herauslösen, Aggregator + Bridged Nodes aus `NetworkConfig`, Zustands-/Kommandopfad, Endpoint-Map | Alle Units bis `MATTER_MAX_BRIDGED_UNITS` schaltbar/dimmbar, Zustand folgt Änderungen aus der Casambi-App |
| **3** | Zero-Config-UX: QR + Zahlencode im Dashboard, `/api/matter`, Reset, Serial-Kommando | Neues Gerät ohne Doku-Lektüre koppelbar (außer Google-Console-Schritt) |
| **4** | CCT/RGB/vertical/mehrkanalige Fixtures, optional Gruppen/Szenen | Mapping-Tabelle 7.2 vollständig |
| **5** | README, FHEM-Hinweise, CI-Environment für den Matter-Build | CI baut den Matter-Build mit |

Die Core-3.x-Migration (4.4) ist formal Voraussetzung für Stufe 1 und wird als
**eigenes Issue** geführt — sie betrifft die gesamte bestehende Firmware und
darf nicht im Matter-Branch versteckt werden.

Stufe 1 ist bewusst der erste inhaltliche Schritt: Sie kostet wenig, beantwortet
aber die einzige Frage, an der das ganze Vorhaben hängt — passt Matter neben
dauerhaftem BLE-Central und AsyncTCP auf **ein** Board?

## 12. Tests und Abnahme

**Host-Tests** (`pio test -e native`, Muster wie `test/test_state_codec`):
Level-Umrechnung `1..254 ⇄ 0..255` inkl. Randwerte, Mireds⇄Kelvin mit Klemmung,
Auswahl des Device Types aus `controls[]`, Endpoint-Map (Persistenz,
Nie-Wiederverwendung, Verhalten bei Refresh mit neuen/entfallenen Units),
Passcode-Generator (Verbotsliste).

**Hardware-Abnahme:** Commissioning in mindestens zwei Ökosystemen; Schalten aus
Ökosystem, Casambi-App und REST-API gemischt, ohne Zustandsdivergenz; Verhalten
bei BLE-Abriss (`Reachable=false`, Erholung); 24-h-Heap-Lauf; Cloud-Refresh mit
geänderter Unit-Liste ohne Vertauschen der Zuordnungen.

## 13. Risiken und offene Fragen

| Risiko / Frage | Auswirkung | Umgang |
|---|---|---|
| RAM reicht auch mit 5.2 + PSRAM nicht | Option A tot, und die Ein-Geräte-Vorgabe ist nicht erfüllbar | Gate in Stufe 1 — früh und billig; danach Entscheidung zwischen Rückfallebene (B/C) und Verzicht |
| BLE-Controller-Code im Flash (5.2) verschlechtert die Casambi-Verbindung | Kernfunktion leidet | im Spike als eigenes Messkriterium führen, notfalls Option abwählen und Heap anders holen |
| IDF-mit-Arduino-als-Komponente | zweiter Build-Pfad neben PlatformIO-Arduino | strikt auf das Matter-Environment begrenzen; `devkit-v4` bleibt wie er ist |
| PSRAM auf klassischem ESP32 (WROVER) | Cache-Workaround kostet Performance, kein DMA | S3 bevorzugen |
| On-network-Commissioning wird von Google Home schlecht unterstützt | Hauptzielökosystem fällt aus | im Stufe-1-Spike gegen die realen Apps prüfen, bevor der Bridge-Kern gebaut wird; Tasmota-Praxis spricht dafür (4.3) |
| CHIP-mDNS kollidiert mit vorhandenem Responder | Gerät unauffindbar | Plattform-mDNS konfigurieren (4.5) |
| Core-3.x-Migration bringt Regressionen im bestehenden Stack (#18-Stabilität, TLS-Heap) | trifft auch Nicht-Matter-Nutzer | eigenes Issue, eigener Merge, Matter hinter Feature-Flag |
| Endpoint-Limit < Netzgröße | große Netze nur teilweise gebrückt | dokumentiertes, deterministisches Limit + sichtbare Meldung (4.6) |
| Zwei Bauziele (mit/ohne Matter) | Pflegeaufwand, CI-Zeit | Matter strikt in `src/matter/` kapseln, Rest per Flag unberührt |
| Test-VID/PID vs. Zertifizierung | „nicht zertifiziert"-Hürden je Ökosystem | in README benennen (8.5); Zertifizierung explizit außerhalb des Umfangs |

## 14. Was jetzt zu entscheiden ist

Beide offenen Hardwarefragen sind inzwischen beantwortet:

| Frage | Entscheidung |
|---|---|
| Zweites Gerät (Option B, Companion)? | **nein** |
| Boardwechsel auf ESP32-S3 mit PSRAM (Option A)? | **nein**, es bleibt beim ATOM Lite (ESP32-PICO-D4, 4 MB Flash, kein PSRAM) |

Damit ist der Sachstand eindeutig: **Auf dieser Hardware ist keine
Matter-Bridge realisierbar.** Es liegt nicht an fehlender Optimierung —
Abschnitt 5 hat alle bekannten Hebel durchgerechnet:

- Konfiguration (5.2) und Zuschnitt auf Leuchten (5.6) bringen zusammen
  ~80–110 KB, erfordern aber den IDF-Build und reichen ohne PSRAM nicht;
- der entscheidende Hebel (5.3, Matter-BSS ins externe RAM) setzt PSRAM voraus,
  das der PICO-D4 nicht hat;
- dazu kommt die Flash-Grenze: ~1,5 MB CHIP-Code neben der bestehenden Firmware
  passen nicht in 4 MB (4.2);
- eine Eigenimplementierung (5.5/Option D) ist nicht verhältnismäßig.

**Konsequenz für Issue #44:** Das Ziel „Casambi-Geräte erscheinen als
Matter-Geräte" ist mit unveränderter Hardware und ohne Zusatzgerät nicht
erreichbar. Das Issue sollte entweder zurückgestellt werden (bis ein
Hardwarewechsel ansteht) oder mit dieser Begründung geschlossen werden.
Die verbleibenden Wege zu Google Home & Co. laufen zwingend über ein Gerät im
Netz, das Matter bzw. die Ökosystem-Anbindung übernimmt — dazu Abschnitt 15.

Dieses Dokument bleibt als Analyse bestehen: Sollte die Hardwarefrage später neu
bewertet werden, ist der Weg samt Zahlen, Stufenplan und Gates hier beschrieben.

## 15. Nachgefragt: IFTTT — und was auf dieser Hardware wirklich trägt

### 15.1 Warum IFTTT das Ziel nicht erreicht

IFTTT ist ein **Cloud-Dienst**. Für unseren Zweck scheitert es an vier Punkten,
von denen jeder einzelne genügt:

| Punkt | Sachstand |
|---|---|
| **Erreichbarkeit** | Für Steuerung *zum Gerät hin* müsste IFTTT unseren ESP aufrufen — er steht im LAN. Das ginge nur über Portfreigabe (inakzeptabel: unverschlüsseltes HTTP, ein aus dem Casambi-Passwort abgeleitetes Token, kein Rate-Limit) oder über einen Relay im Netz — dann braucht man wieder das Gerät, das man vermeiden wollte |
| **Google Assistant** | Genau die Funktion, die wir bräuchten, ist weg: Seit dem 31.08.2022 unterstützt IFTTTs Google-Assistant-Dienst **keine Trigger mit variabler Eingabe** mehr („Sage einen Satz mit einer Zahl") und keine eigenen Antworten. Dimmen auf einen Prozentwert per Sprache ist damit ausgeschlossen; Googles „Conversational Actions" wurden am 12.06.2023 ganz eingestellt |
| **Tarif** | Webhooks — der einzige technisch brauchbare Baustein — sind erst ab **IFTTT Pro** verfügbar; der kostenlose Tarif erlaubt 2 einfache Applets mit bis zu **60 Minuten Verzögerung**. Pro deckt 20 Applets ab. Bei einem Applet je Lampe und Aktion sind das gut eine Handvoll Leuchten |
| **Architektur** | Ein Cloud-Dienst im Steuerpfad widerspricht der Grundidee dieser Firmware: nach der Provisionierung **lokal und offline** (BLE + LAN, keine Cloud). Latenz, Ausfall und Fremdabhängigkeit kämen ohne funktionalen Gegenwert dazu |

Und selbst wenn all das anders wäre: IFTTT macht aus den Lampen **keine Geräte
in Google Home**. Sie tauchen nicht in der Geräteliste auf, haben keine
Raumzuordnung und keinen Zustand — es bleiben Sprach-Trigger auf feste Sätze.
Als Ersatz für Matter taugt das nicht.

### 15.2 Was als Schnittstellenerweiterung sinnvoll wäre

Sinnvoll ist die Frage trotzdem — nur zeigt sie in eine andere Richtung.

**a) Ausgehende Ereignis-Webhooks (klein, sofort machbar).**
Bei Zustandsänderung, BLE-Verbindungsverlust oder Heap-Warnung ein HTTP-POST mit
JSON an eine konfigurierbare URL. Nutzen: Benachrichtigungen und Automationen in
FHEM, Node-RED, n8n oder Home Assistant — und, wer mag, IFTTT-Webhooks (Pro).
Aufwand gering, Speicherbedarf minimal.
**Randbedingung:** Ausgehendes **HTTPS** ist auf diesem Gerät die heikle
Variante — der TLS-Handshake braucht einen großen zusammenhängenden Heapblock,
weshalb der Cloud-Refresh heute bewusst vor dem BLE-Start läuft (README,
„Troubleshooting"). Empfehlung deshalb: **HTTP an Empfänger im LAN**, TLS nur
optional mit Heap-Prüfung und dokumentiertem Risiko.

**b) MQTT-Client mit Home-Assistant-Auto-Discovery (die eigentliche Empfehlung).**
Der ESP verbindet sich **ausgehend** zu einem Broker im LAN, meldet je Unit die
Discovery-Nachricht und veröffentlicht Zustände; Kommandos kommen über
`.../set`-Topics zurück. Vorteile:

- **beide Richtungen**, ohne Portfreigabe, ohne Cloud, ohne zweites Gerät;
- Home Assistant legt die Leuchten **automatisch** an — dasselbe
  „ohne Konfiguration"-Prinzip wie beim QR-Code, nur eine Ebene höher;
- über HA sind Google Home, Apple Home und Alexa erreichbar — genau das Ziel aus
  #44, nur eben mit HA als Vermittler statt Matter im Gerät;
- Speicherbedarf: eine TCP-Verbindung und ein kleiner Client — Größenordnung
  wenige KB, unkritisch auf dem ATOM Lite;
- der Kommandopfad wäre derselbe wie bei REST und Matter (7.1), MQTT also ein
  weiterer Producer auf `command_queue`.

**Ehrlich zur Grenze:** Auch b) braucht einen Broker und eine HA-Instanz im
Netz. Wenn im Haushalt **kein** Dauerläufer existiert und weder Zusatzgerät noch
Boardwechsel in Frage kommen, gibt es keinen Weg in Google Home — das ist eine
Hardwaregrenze, keine Frage der Schnittstellengestaltung.

### 15.3 Empfehlung

1. IFTTT **nicht** verfolgen (15.1).
2. Wenn eine Home-Assistant-Instanz existiert oder aufgesetzt werden kann:
   **MQTT mit Auto-Discovery** als eigenes Issue — das ist die tragfähige
   Erweiterung auf unveränderter Hardware und erreicht das ursprüngliche Ziel
   über einen Umweg.
3. Unabhängig davon: **ausgehende Ereignis-Webhooks** als kleines, nützliches
   Feature für lokale Automatisierung.
4. Issue #44 zurückstellen oder mit der Begründung aus Abschnitt 14 schließen.

## Quellen

- Espressif, *Configuration options to optimize RAM and Flash* (esp-matter) —
  Speicherwerte des Licht-Beispiels und alle Optimierungsschalter inkl.
  BSS-Verlagerung nach PSRAM:
  <https://github.com/espressif/esp-matter/blob/main/docs/en/optimizations.rst>
- Matter-Bridge-Beispiel (Aggregator, dynamische Endpoints):
  <https://project-chip.github.io/connectedhomeip-doc/examples/bridge-app/esp32/README.html>
- Espressif Developer Portal, *Matter: Bridge for Non-Matter Devices*:
  <https://developer.espressif.com/blog/matter-bridge-for-non-matter-devices/>
- ESP-IDF, *Support for External RAM* (PSRAM-Nutzung, Einschränkungen):
  <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html>
- Tasmota, *Adventures with Matter protocol on Tasmota* (Berry-Implementierung,
  ~209 KB Flash, IP-Commissioning):
  <https://github.com/arendst/Tasmota/discussions/17872>
- Tasmota, *Matter* / *Matter Internals* (Bridge-Modus, virtuelle Endpoints,
  Endpoint-Empfehlung): <https://tasmota.github.io/docs/Matter/> ·
  <https://tasmota.github.io/docs/Matter-Internals/>
- Google Home Developers, Test einer Matter-Integration (Test-VID/PID,
  Developer-Console-Projekt): <https://developers.home.google.com/matter/test>
- IFTTT, *Google Assistant changes* (Wegfall der Trigger mit variabler Eingabe
  zum 31.08.2022): <https://ifttt.com/explore/google-assistant-changes>
- Google, Einstellung der Conversational Actions zum 12.06.2023:
  <https://en.wikipedia.org/wiki/Actions_on_Google>
- IFTTT-Tarife (Webhooks erst ab Pro, kostenlos 2 Applets mit bis zu 60 min
  Verzögerung): <https://ifttt.com/plans>
- Arduino-Matter-Beispiele (kein Aggregator/Bridge-Beispiel vorhanden):
  <https://github.com/espressif/arduino-esp32/tree/master/libraries/Matter/examples>
