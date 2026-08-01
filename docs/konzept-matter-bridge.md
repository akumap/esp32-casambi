# Konzept: Matter-Bridge neben dem REST-API (Issue #44)

Status: **Konzept / Entscheidungsvorlage** — noch keine Implementierung.
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

**Ergebnis dieses Konzepts vorweg:** Der Nutzen ist klar, der Weg ist gangbar —
aber **nicht auf der heutigen Zielhardware** (ESP32-WROOM-32, 320 KB DRAM) und
**nicht mit der heutigen Toolchain** (Arduino Core 2.x). Beides ist mit Zahlen
in Abschnitt 4 belegt. Empfohlen wird ein gestufter Weg mit einem
zeitbegrenzten Spike als Entscheidungstor (Abschnitt 10).

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

### 4.1 RAM: der eigentliche Showstopper auf dem ESP32-WROOM-32

Espressifs eigene Messwerte für das **Licht-Beispiel** (nur Matter, sonst
nichts) auf einem ESP32-C3, Standardkonfiguration:

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
56 KB frei.

**Fazit:** Matter + Casambi-BLE + Webserver passen auf einem klassischen ESP32
mit 320 KB DRAM **nicht** zusammen. Ein Build für `devkit-v4` ist kein
realistisches Ziel.

**Konsequenz:** Der Matter-Build braucht ein RAM-stärkeres Target —
**ESP32-S3** (512 KB SRAM, optional PSRAM) als Primärziel, **ESP32-C6** als
Alternative. Der bestehende `devkit-v4`-Build bleibt **unverändert und ohne
Matter**; Matter wird ein eigenes Environment hinter einem Feature-Flag
(`-DFEATURE_MATTER`), damit die heutige Zielhardware kein Risiko erbt.

### 4.2 Flash und Partitionen

Matter allein bringt ~1,5 MB Code mit. Zusammen mit der bestehenden Firmware
(NimBLE + AsyncWebServer + TLS-Cloud-Client + PROGMEM-UI) ist `huge_app.csv`
(3 MB App, keine OTA) bestenfalls knapp. Empfehlung: Matter-Target mit
**8 MB oder 16 MB Flash** und eigener Partitionstabelle (größerer NVS für
Fabrics/Attribute). Für die Attestation wird die in `esp_matter` mitgelieferte
**Test-DAC** verwendet (keine `fctry`-Partition, kein Fertigungsprozess) —
Konsequenzen dazu in 7.5.

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
LAN. Das passt inhaltlich perfekt zum Ziel „ohne Konfiguration": Der
BLE-Commissioning-Pfad existiert nur, um WLAN-Credentials zu übertragen — was
hier schon erledigt ist.

**Offener Punkt (Spike):** Nicht jedes Ökosystem-App findet on-network-Geräte
gleich zuverlässig. Muss in Stufe 1 gegen Apple Home, Google Home und Home
Assistant real geprüft werden. Fallback wäre BLE-Advertising nur bei
getrennter Casambi-Verbindung — deutlich unangenehmer, deshalb erst, wenn
IP-only nachweislich scheitert.

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
Limit ist also unvermeidbar. Vorschlag: `MATTER_MAX_BRIDGED_UNITS` (Default 16,
im Spike gegen den gemessenen Heap kalibriert), Auswahl **deterministisch nach
`deviceId` aufsteigend**, Überzählige werden im Dashboard und im Event-Log
sichtbar gemeldet — nicht stillschweigend verschluckt.

## 5. Optionen

| # | Ansatz | Aufwand | Hardware | „ohne Konfiguration" | Bewertung |
|---|---|---|---|---|---|
| **A** | Matter-Bridge **in dieser Firmware**, eigenes Environment für S3/C6 | hoch (Core-3-Migration + Bridge) | neue Platine nötig, `devkit-v4` bleibt ohne Matter | ja (QR im Dashboard) | **Empfohlen** als Zielbild — ein Gerät, keine Zusatzinfrastruktur |
| **B** | Zweiter MCU als Matter-Companion, spricht das REST/WS-API | mittel | zwei Boards | teilweise (Companion muss ESP finden) | Verschiebt das RAM-Problem sauber, verdoppelt aber Hardware und Update-Pfad |
| **C** | Externe Bridge-Software (Matterbridge / Home-Assistant-Matter-Hub) auf vorhandenem Server, nutzt das REST/WS-API | gering (Plugin/Adapter, kein Firmware-Risiko) | Dauerläufer-Host nötig | nein (Host + Plugin einrichten) | **Sofort verfügbar**, gute Zwischenlösung — als dokumentierter Weg, nicht als Ersatz für A |

**Empfehlung:** A als Zielbild, C sofort dokumentieren, B nur, falls der Spike
in Stufe 1 auch auf S3 scheitert.

## 6. Zielarchitektur (Option A)

### 6.1 Modulschnitt

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

### 6.2 Abbildung Casambi → Matter

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

### 6.3 Stabile Endpoint-IDs

Ökosysteme hängen Raumzuordnung, Namen und Automationen an der **Endpoint-ID**.
Wandern IDs nach einem Cloud-Refresh, vertauschen sich beim Nutzer die Lampen.
Deshalb: `endpoint_map` als kleine LittleFS-Datei
(`unitId` + `controlName` → `endpointId`), Regeln:

- bestehende Zuordnung gewinnt immer,
- neue Units bekommen die nächste freie ID (nie eine wiederverwendete),
- verschwundene Units behalten ihren Slot zunächst und werden `Reachable=false`;
  Freigabe nur über einen expliziten Matter-Reset.

### 6.4 Zustandsfluss

```
BLE-Notify-Task → (bestehender UnitStateCallback) → Event-Queue
                → loop-Task: WebSocket-Broadcast  (unverändert)
                           + Matter-Attribut-Update (neu, nur wenn Wert sich ändert)
```

Kein `esp_matter`-Aufruf aus dem BLE-Task. Attribut-Updates werden entprellt
(nur bei tatsächlicher Änderung), damit ein Dimm-Rampen-Broadcast aus dem Mesh
keine Update-Flut in den Fabrics auslöst.

### 6.5 Kommandofluss

```
Matter-Task (CHIP) → command_queue (BleCommand) → loop-Task → CasambiClient
```

Wichtig: Matter-Level-Kommandos (`MoveToLevel` mit Transition, Slider-Wischen in
der App) kommen in Serie. Der BLE-Pfad ist um Größenordnungen langsamer.
Deshalb **Coalescing pro Unit** („letzter Wert gewinnt", ~150 ms), umgesetzt in
`command_queue`, sodass **auch das REST-API davon profitiert**. Mehrkanalige
Änderungen gehen als ein `setUnitState` (`casambi_client.h:208`) raus.

### 6.6 Verhältnis zu FHEM/WebSocket

Da alle Wege durch dieselbe Queue laufen, sieht FHEM jede über Matter ausgelöste
Änderung ganz normal als `unit_state`-Push — und umgekehrt. Es gibt keinen
zweiten „Wahrheitsstand" und keine Sonderfälle im FHEM-Modul.

## 7. „Ohne Konfiguration" — konkret

### 7.1 Pairing-Daten erzeugt das Gerät selbst

Discriminator und Setup-Passcode werden **beim ersten Matter-Start einmalig
zufällig erzeugt** (`esp_random()`), in NVS abgelegt und danach unverändert
wiederverwendet.

Bewusst **nicht** aus der eFuse-MAC abgeleitet: Die MAC ist über WLAN und BLE
öffentlich sichtbar; ein daraus berechenbarer Passcode wäre für jeden in
Funkreichweite erratbar und würde das Commissioning-Geheimnis entwerten. Die
Verbotswerte des Standards (`00000000`, `11111111`, …, `12345678`, `87654321`)
werden beim Erzeugen ausgeschlossen.

### 7.2 Anzeige

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

### 7.3 Commissioning-Fenster

Solange keine Fabric existiert, ist das Fenster automatisch offen (Basic
Commissioning Mode) — der Nutzer muss nichts drücken. Nach dem ersten Beitritt
schließt es; für ein zweites Ökosystem (Multi-Admin) gibt es einen Knopf im
Dashboard bzw. `POST /api/matter/commission-window`, der es befristet wieder
öffnet.

### 7.4 Reset

`POST /api/matter/reset` (und `matter reset` seriell) entfernt alle Fabrics und
erzeugt neue Pairing-Daten. `clearconfig` löscht zusätzlich die Endpoint-Map,
damit ein frisch provisioniertes Gerät keine Altzuordnungen erbt.

### 7.5 Wo „ohne Konfiguration" endet — ehrlich benannt

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

Diese Asymmetrie ist der wichtigste Erwartungspunkt gegenüber dem Issue-Titel
„… i.e., Google Home".

## 8. Auswirkungen auf die versionierte Schnittstelle

Neue Endpunkte (`GET /api/matter`, `POST /api/matter/reset`,
`POST /api/matter/commission-window`) sind rein additiv → `FHEM_API_VERSION_MINOR`
+1, `MAJOR` bleibt (`config.h:92`, Regeln siehe `konzept-versionierung.md`).
Optional später ein `matter`-Feld in der WebSocket-Hello und ein Reading im
FHEM-Modul; beides nicht Voraussetzung für Stufe 1–3.

## 9. Sicherheitsbetrachtung

| Punkt | Bewertung |
|---|---|
| Pairing-Passcode | zufällig, persistent, nicht aus öffentlichen Werten ableitbar (7.1) |
| Sichtbarkeit des Codes | nur über authentifizierte API/Dashboard-Sitzung |
| Angriffsfläche | Matter-Knoten lauscht dauerhaft im LAN — neue Netzwerkoberfläche, die es vorher nicht gab; im Spike mit begrenzter Fabric-Zahl testen |
| Fabric-Verwaltung | jede gekoppelte Fabric darf alle gebrückten Lampen schalten — dieselbe Vertrauensstufe wie ein API-Token, in der README benennen |
| Test-Attestation | signalisiert Ökosystemen „nicht zertifiziert" — bewusst akzeptiert (7.5) |

## 10. Umsetzung in Stufen (mit Abbruchkriterien)

| Stufe | Inhalt | Abschluss-/Abbruchkriterium |
|---|---|---|
| **0** | README-Abschnitt: Casambi über eine **externe** Matter-Bridge (Option C) am bestehenden REST-API | Nutzer kann heute Google Home anbinden, ohne dass Firmware geändert wird |
| **1 — Spike (zeitbegrenzt)** | Core-3.x-Plattform + Environment `matter-s3`; Minimal-Matter (2 Endpoints) **gleichzeitig** mit NimBLE-Casambi-Verbindung und Webserver; IP-only Commissioning gegen Apple Home, Google Home, HA; mDNS-Konflikt prüfen | **Gate:** ≥ 60 KB freier Heap im Betrieb, `min_free_heap` ≥ 30 KB über 24 h, Casambi-Link stabil, mind. ein Ökosystem koppelt on-network. **Sonst:** Option A verwerfen, auf B oder C umschwenken |
| **2** | Bridge-Kern: `command_queue` herauslösen, Aggregator + Bridged Nodes aus `NetworkConfig`, Zustands- und Kommandopfad, Endpoint-Map | Alle Units bis `MATTER_MAX_BRIDGED_UNITS` schaltbar/dimmbar, Zustand folgt Änderungen aus der Casambi-App |
| **3** | Zero-Config-UX: QR + Zahlencode im Dashboard, `/api/matter`, Reset, Serial-Kommando | Neues Gerät ist ohne Doku-Lektüre koppelbar (außer Google-Console-Schritt) |
| **4** | CCT/RGB/vertical/mehrkanalige Fixtures, optional Gruppen/Szenen | Mapping-Tabelle 6.2 vollständig |
| **5** | README, FHEM-Hinweise, CI-Environment `matter-s3` | CI baut den Matter-Build mit |

Die Core-3.x-Migration wird als **eigenes Issue** geführt (4.4) und ist formal
Voraussetzung für Stufe 1.

## 11. Tests und Abnahme

**Host-Tests** (`pio test -e native`, Muster wie `test/test_state_codec`):
Level-Umrechnung `1..254 ⇄ 0..255` inkl. Randwerte, Mireds⇄Kelvin mit Klemmung,
Auswahl des Device Types aus `controls[]`, Endpoint-Map (Persistenz,
Nie-Wiederverwendung, Verhalten bei Refresh mit neuen/entfallenen Units),
Passcode-Generator (Verbotsliste).

**Hardware-Abnahme:** Commissioning in mindestens zwei Ökosystemen; Schalten aus
Ökosystem, Casambi-App und REST-API gemischt, ohne Zustandsdivergenz; Verhalten
bei BLE-Abriss (`Reachable=false`, Erholung); 24-h-Heap-Lauf; Cloud-Refresh mit
geänderter Unit-Liste ohne Vertauschen der Zuordnungen.

## 12. Risiken und offene Fragen

| Risiko / Frage | Auswirkung | Umgang |
|---|---|---|
| RAM reicht auch auf S3 nicht | Option A tot | Stufe-1-Gate vor jeder weiteren Investition |
| On-network-Commissioning wird von Google Home schlecht unterstützt | Hauptzielökosystem fällt aus | im Spike explizit prüfen; Notnagel: BLE-Advertising bei getrenntem Casambi-Link |
| CHIP-mDNS kollidiert mit vorhandenem Responder | Gerät unauffindbar | Plattform-mDNS konfigurieren (4.5) |
| Core-3.x-Migration bringt Regressionen im bestehenden Stack (#18-Stabilität, TLS-Heap) | trifft auch Nicht-Matter-Nutzer | eigenes Issue, eigener Merge, Matter hinter Feature-Flag |
| Endpoint-Limit < Netzgröße | große Netze nur teilweise gebrückt | dokumentiertes, deterministisches Limit + sichtbare Meldung (4.6) |
| Zwei Bauziele (mit/ohne Matter) | Pflegeaufwand, CI-Zeit | Matter strikt in `src/matter/` kapseln, Rest per Flag unberührt |
| Test-VID/PID vs. Zertifizierung | „nicht zertifiziert"-Hürden je Ökosystem | in README benennen (7.5); Zertifizierung explizit außerhalb des Umfangs |

## 13. Was jetzt zu entscheiden ist

1. **Hardware:** Ist ein ESP32-S3-Board (≥ 8 MB Flash) als *zusätzliches*
   Matter-Target akzeptabel? Ohne das ist Option A nicht umsetzbar.
2. **Toolchain:** Core-3.x-Migration als eigenes Issue starten?
3. **Reichweite Stufe 1:** Spike als reine Machbarkeitsmessung (empfohlen) oder
   direkt mit Bridge-Kern?
4. **Zwischenlösung:** Soll Stufe 0 (externe Bridge am REST-API) sofort
   dokumentiert werden, unabhängig vom Rest?

## Quellen

- Espressif, *Configuration options to optimize RAM and Flash* (esp-matter),
  Speicherwerte des Licht-Beispiels:
  <https://github.com/espressif/esp-matter/blob/main/docs/en/optimizations.rst>
- Matter-Bridge-Beispiel (Aggregator, dynamische Endpoints):
  <https://project-chip.github.io/connectedhomeip-doc/examples/bridge-app/esp32/README.html>
- Espressif Developer Portal, *Matter: Bridge for Non-Matter Devices*:
  <https://developer.espressif.com/blog/matter-bridge-for-non-matter-devices/>
- Google Home Developers, Test einer Matter-Integration (Test-VID/PID,
  Developer-Console-Projekt): <https://developers.home.google.com/matter/test>
- Arduino-Matter-Beispiele (kein Aggregator/Bridge-Beispiel vorhanden):
  <https://github.com/espressif/arduino-esp32/tree/master/libraries/Matter/examples>
