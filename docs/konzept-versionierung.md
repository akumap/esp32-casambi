# Konzept: Versionierung der Schnittstellen (Issue #29)

Status: umgesetzt (ESP-Firmware + FHEM-Modul + Tests + Doku, dieser Branch).
Issue: [#29](https://github.com/akumap/esp32-casambi/issues/29)
Branch: `claude/issue-29-solution-6su80s`

## 1. Ziel

Beide Schnittstellen des Systems sollen versioniert und überwacht werden:

1. **Casambi-Netzwerk ↔ ESP32**: Die Casambi-Netzwerkversion ist nicht
   beeinflussbar. Liegt sie außerhalb des vom ESP unterstützten Bereichs,
   soll eine **Warnung** angezeigt werden (keine Blockade).
2. **ESP32 ↔ FHEM**: Die REST/WebSocket-Schnittstelle erhält eine explizite
   **Major/Minor-Version**. Minor = abwärts-/aufwärtskompatible Erweiterung,
   Major = inkompatible Änderung (eine Seite muss aktualisiert werden).
3. **Anzeige**: Alle Versionsstände (Casambi-Netzwerkversion, unterstützter
   Bereich des ESP, API-Version des ESP, API-Version des FHEM-Moduls) werden
   als **FHEM-Readings** sichtbar; bei Mismatch gibt es Warn-Readings.

Grundprinzip in allen Fällen: **fail-operational** — Abweichungen werden
prominent gemeldet, der Betrieb läuft aber weiter. Das entspricht der
bestehenden toleranten Auslegung der Firmware (siehe `config.h:23-30`).

## 2. Ausgangslage (Ist-Zustand)

### 2.1 Casambi ↔ ESP32 — Erkennung vorhanden, Anzeige fehlt

- `config.h:29-30` definiert `MIN_PROTOCOL_VERSION 10` /
  `MAX_PROTOCOL_VERSION 11`.
- `checkCasambiVersions()` (`main.cpp:900`) warnt beim Boot und nach jedem
  Cloud-Refresh, wenn `protocolVersion` außerhalb des Bereichs liegt;
  zusätzlich warnen `config_store.cpp` (Laden/Speichern) und
  `casambi_client.cpp:517` (On-Air-Mismatch).
- **Lücke:** Alle Warnungen landen nur auf der seriellen Konsole. Weder die
  Netzwerkversion noch der unterstützte Bereich noch die Warnung werden über
  die API exponiert — FHEM kann nichts davon anzeigen.

### 2.2 ESP32 ↔ FHEM — nur implizite Versionierung über Buildnummer

- `FIRMWARE_BUILD` (`git rev-list --count`, injiziert durch
  `scripts/build_number.py`) wird in `/api/info` und der WebSocket-Hello
  mitgesendet.
- FHEM prüft dagegen `MIN_FIRMWARE_BUILD` (`98_CasambiGW.pm:69`) und setzt
  die Readings `esp32Build` / `esp32BuildWarning`.
- **Lücke:** Die Buildnummer ist eine *Implementierungs*-, keine
  *Schnittstellenversion* — sie steigt mit jedem Commit und sagt nichts über
  Kompatibilität aus.

## 3. Abgrenzung der Versionsbegriffe

| Version | Frage, die sie beantwortet | Quelle | bleibt/neu |
|---|---|---|---|
| `FIRMWARE_BUILD` | „Welcher Firmwarestand läuft?" | git-Commit-Zähler | bleibt unverändert |
| Casambi-Protokollversion | „Welche Version spricht das Netz?" | Cloud-Config | bleibt (wird zusätzlich exponiert) |
| `MIN/MAX_PROTOCOL_VERSION` | „Womit ist der ESP getestet?" | `config.h` | bleibt (wird zusätzlich exponiert) |
| **ESP↔FHEM-API-Version** | „Verstehen sich ESP und FHEM?" | neue Konstante beidseitig | **neu** |

Die Buildnummer-Prüfung bleibt parallel bestehen; sie beantwortet eine andere
Frage als die API-Version.

## 4. Teil A: Casambi ↔ ESP32

Die Erkennung existiert bereits vollständig (2.1); es ändert sich **nur die
Sichtbarkeit**. Die WebSocket-Hello (`_buildHelloMessage()`,
`webserver.cpp:348`) wird um drei Felder erweitert:

```json
{
  "type": "hello",
  "casambi_protocol_version": 11,
  "casambi_protocol_min": 10,
  "casambi_protocol_max": 11,
  ...
}
```

Begründung Transportweg: Die Hello erreicht nur **authentifizierte**
WebSocket-Clients. `/api/info` ist bewusst unauthentifiziert und auf zwei
Felder reduziert (`webserver.cpp:517-521`) — Details des Casambi-Netzes
gehören dort nicht hinein. Diese Datensparsamkeits-Entscheidung bleibt
unangetastet.

Die Bewertung (in/außerhalb Bereich) macht **FHEM** aus den drei Zahlen
selbst (siehe 6). Der ESP sendet nur Fakten — so liegt die Warnlogik nicht
doppelt vor, und die Firmware-Warnungen auf Serial bleiben unverändert.

## 5. Teil B: ESP↔FHEM-API-Version (Major/Minor)

### 5.1 Definition und Semantik

**Eine** gemeinsame Version für REST **und** WebSocket — beide leben im
selben Firmwarestand und im selben FHEM-Modul, getrennte Versionen wären
Pflegeaufwand ohne Nutzen.

- **Minor +1**: kompatible Erweiterung (neues Feld, neuer Endpoint, neuer
  WS-Nachrichtentyp). Beide Seiten müssen unbekannte Felder/Nachrichten
  weiterhin ignorieren (tun sie heute schon).
- **Major +1**, Minor auf 0: inkompatible Änderung (Feld entfernt/umbenannt,
  Semantik geändert, Auth geändert). Eine Seite muss aktualisiert werden.

**Startwert: 1.0.** Ein **fehlendes** Versionsfeld (ältere Firmware) wird von
FHEM **als 1.0 interpretiert** — damit löst sich das Henne-Ei-Problem
(das Versionsfeld ist selbst eine Schnittstellenerweiterung) ohne
Sonderbehandlung: Firmware vor diesem Konzept *ist* Schnittstellenstand 1.0.

### 5.2 Ablageort und Pflegeregel

ESP-Seite, in `config.h` neben den bestehenden Versionskonstanten:

```c
// ESP32 <-> FHEM interface version (REST + WebSocket, one shared version).
// Contract: bump MINOR for backwards/forwards-compatible extensions (new
// fields, endpoints, WS message types); bump MAJOR (and reset MINOR to 0)
// for incompatible changes. MUST be kept in sync with the constants in
// FHEM/98_CasambiGW.pm. A missing field on either side means 1.0.
#define FHEM_API_VERSION_MAJOR    1
#define FHEM_API_VERSION_MINOR    0
```

FHEM-Seite, in `98_CasambiGW.pm` neben `MIN_FIRMWARE_BUILD` (gleicher
Kommentar-Kontrakt, gespiegelt):

```perl
use constant API_VERSION_MAJOR => 1;   # keep in sync with src/config.h
use constant API_VERSION_MINOR => 0;
```

Da ESP-Firmware und FHEM-Modul im selben Repo liegen, ist die Pflegedisziplin
(„bei jeder Schnittstellenänderung mitzählen") der eigentliche Risikofaktor.
Der Kommentar-Kontrakt an beiden Konstanten plus ein Hinweis im README ist
hier der pragmatische Weg; CI-Tooling dafür lohnt sich (noch) nicht.

### 5.3 Transport

Zwei Stellen senden die Version als **Integer-Felder** (numerisch
vergleichbar, kein String-Parsing):

1. **`/api/info`** (unauthentifiziert, `webserver.cpp:522`): zusätzlich
   `"api_version_major": 1, "api_version_minor": 0`. Unbedenklich (verrät
   nichts über das Netz) und nützlich, weil FHEM die Kompatibilität so schon
   **vor** dem WebSocket-Aufbau kennt. Das Setup-Portal
   (`setup_portal.cpp:237`) sendet dieselben Felder, damit `/api/info` in
   beiden Modi dieselbe Form hat.
2. **WebSocket-Hello** (`webserver.cpp:348`): dieselben zwei Felder — die
   Hello ist die Stelle, auf die sich die Reading-Pflege in FHEM stützt.

### 5.4 Prüfregeln in FHEM

Ausgewertet in `CasambiGW_HandleHello` (`98_CasambiGW.pm:645`), analog zur
bestehenden Buildnummer-Prüfung:

| Situation | Verhalten |
|---|---|
| Feld fehlt | als 1.0 interpretieren, dann normale Prüfung |
| ESP-Major ≠ FHEM-Major | `apiVersionWarning` mit Klartext („incompatible, update …"), Log Level 2; **Betrieb läuft weiter** (fail-operational) |
| Major gleich, Minor unterschiedlich | kein Warn-Reading (`apiVersionWarning: ok`), nur Log Level 4 — Minor-Differenz ist per Definition kompatibel, egal welche Seite neuer ist |
| Versionen gleich | `apiVersionWarning: ok` |

Bewusst **keine** Verbindungsverweigerung bei Major-Mismatch: Ein degradiert
funktionierendes System mit deutlicher Warnung ist für den Anwender
diagnostizierbarer als ein totes.

## 6. FHEM-Readings (Teil C)

Neue bzw. bestehende Readings am Gateway-Device:

| Reading | Inhalt | Quelle | neu? |
|---|---|---|---|
| `casambiProtocolVersion` | z. B. `11` | Hello `casambi_protocol_version` | neu |
| `espCasambiVersionRange` | z. B. `10-11` | Hello `casambi_protocol_min/max` | neu |
| `casambiVersionWarning` | `ok` oder Klartext | von FHEM berechnet | neu |
| `espApiVersion` | z. B. `1.0` | Hello `api_version_major/minor` | neu |
| `fhemApiVersion` | z. B. `1.0` | Konstante im Modul | neu |
| `apiVersionWarning` | `ok` oder Klartext | von FHEM berechnet (5.4) | neu |
| `esp32Build` | Commit-Zähler | Hello `build` | bestehend |
| `esp32BuildWarning` | `ok` oder Klartext | bestehend | bestehend |

`casambiVersionWarning` wird gesetzt, wenn `casambiProtocolVersion` außerhalb
`espCasambiVersionRange` liegt — mit unterschiedlichem Text für „zu alt"
(Netz unter Minimum) und „neuer als getestet" (über Maximum), analog zu den
Serial-Warnungen in `checkCasambiVersions()`.

Damit sind alle im Issue geforderten Stände sichtbar: Casambi-Netzwerkversion,
ESP-Casambi-Min/Max, ESP-API-Version, FHEM-Schnittstellenversion — plus
Warnungen bei Mismatch.

## 7. Änderungsumfang

| Datei | Änderung |
|---|---|
| `src/config.h` | `FHEM_API_VERSION_MAJOR/MINOR` + Kommentar-Kontrakt |
| `src/web/webserver.cpp` | Hello: 5 neue Felder (5.3, 4); `/api/info`: 2 neue Felder |
| `src/web/setup_portal.cpp` | `/api/info`: dieselben 2 Felder |
| `FHEM/98_CasambiGW.pm` | Konstanten, Auswertung + Readings in `HandleHello`, Hilfsfunktion für den Versionsvergleich, commandref-Abschnitt |
| `FHEM/t/CasambiGW_helpers.t` | Tests für die Vergleichs-Hilfsfunktion (s. u.) |
| `README.md` | Versionskontrakt und neue Felder dokumentieren (die REST/WS-Schnittstelle ist im README spezifiziert, nicht in der Protokollreferenz — die behandelt das BLE-Protokoll) |

Die Versionsvergleiche in FHEM werden als **reine Perl-Hilfsfunktionen** ohne
FHEM-Laufzeitabhängigkeit geschnitten (z. B.
`CasambiGW_ApiVersionWarning($espMajor,$espMinor)` und
`CasambiGW_CasambiVersionWarning($ver,$min,$max)`, Rückgabe `"ok"` oder
Warntext), damit sie im bestehenden Testfile `CasambiGW_helpers.t` direkt
testbar sind — gleiches Muster wie die dortigen Prozent/Byte-Helfer.

Dieses Konzept selbst ist eine **Minor-Erweiterung**: Es fügt nur Felder
hinzu. Die Version bleibt beim Startwert **1.0** (die neuen Felder *sind* die
Definition von 1.0; Firmware ohne die Felder wird ja ebenfalls als 1.0
gelesen und ist dazu kompatibel).

## 8. Testkonzept

1. **Unit-Tests (Perl):** Vergleichs-Helfer mit den Fällen aus 5.4 und 6
   (fehlend→1.0, Major-Mismatch beidseitig, Minor-Differenz beidseitig,
   Casambi-Version unter/über/im Bereich).
2. **Kompatibilität alt→neu:** neues FHEM-Modul gegen alte Firmware (ohne
   Versionsfelder) → Readings zeigen `1.0`, `apiVersionWarning: ok`, keine
   Fehler im Log.
3. **Manuell:** neue Firmware + neues Modul → alle Readings gefüllt; einmalig
   testweise `API_VERSION_MAJOR` in FHEM verstellen → Warnung erscheint,
   Steuerung funktioniert weiter.

## 9. Nicht-Ziele

- **Keine Blockade** bei Versions-Mismatch (weder Casambi- noch API-Seite) —
  nur Warnungen.
- **Keine getrennten Versionen** für REST und WebSocket.
- **Kein CI-Zwang** zur Versionspflege — Kommentar-Kontrakt genügt beim
  aktuellen Repo-Zuschnitt (beide Seiten in einem Repo).
- **Keine Änderung** an der bestehenden Buildnummer-Prüfung und an der
  Datensparsamkeit von `/api/info` (Netzdetails bleiben authentifizierten
  Clients vorbehalten).
