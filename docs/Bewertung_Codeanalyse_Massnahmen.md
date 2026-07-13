# Bewertung des Prüfberichts „Codeanalyse: esp32-casambi"

**Gegenstand:** Nachvollziehen und Bewerten der neun Maßnahmen (S-01 … S-09) aus dem
Prüfbericht vom 13. Juli 2026.
**Geprüfter Stand:** `main` · `e0075639a1614c8d2476ebf48fef54b34ba60e4e` — dies ist
zugleich der aktuelle HEAD des Repositories. Der Bericht analysiert also den
aktuellen Code; keine der neun Maßnahmen ist bislang umgesetzt (der frühere
Commit „implement all findings from the July 2026 code analysis" bezieht sich auf
eine **andere**, vorangegangene Analyse).
**Methodik der Bewertung:** Für jede Maßnahme wurden die im Bericht genannten
Code-Belegstellen im aktuellen Quellcode aufgesucht und der behauptete Kontroll-
bzw. Datenfluss verifiziert.

## Gesamturteil

Der Bericht ist **fachlich fundiert und in allen neun Punkten korrekt
nachvollziehbar.** Sämtliche Zeilenangaben stimmen mit dem aktuellen Code
überein, die beschriebenen Fehlerpfade existieren wie dargestellt, und die
Prioritäten sind angemessen gewählt. Auch die im Abschnitt „Positive
Ausgangslage" genannten Schutzmechanismen sind belegbar (CMAC-Selbsttest
`main.cpp:335`, Reboot/Refresh-Delegation `main.cpp:370/971`, Streaming großer
Logs `webserver.cpp:908`, Build-Matrix `ci.yml:13-14`).

Es wurde **eine sachliche Ungenauigkeit** gefunden (S-01, Detail zum Key-Array,
siehe unten). Sie ändert die Stoßrichtung und Priorität der Maßnahme nicht.

| ID | Priorität lt. Bericht | Nachvollzogen | Bewertung |
|----|----------------------|---------------|-----------|
| S-01 | Hoch | ✅ | Bestätigt (eine Detail-Ungenauigkeit, s. u.) |
| S-02 | Hoch | ✅ | Bestätigt |
| S-03 | Hoch | ✅ | Bestätigt |
| S-04 | Mittel | ✅ | Bestätigt |
| S-05 | Mittel | ✅ | Bestätigt |
| S-06 | Niedrig | ✅ | Bestätigt |
| S-07 | Mittel | ✅ | Bestätigt |
| S-08 | Mittel | ✅ | Bestätigt |
| S-09 | Mittel | ✅ | Bestätigt |

---

## Einzelbewertung

### S-01 · Konfiguration atomar und validiert speichern — **Hoch, bestätigt**

- `hasValidConfig()` prüft ausschließlich `LittleFS.exists(CONFIG_FILE_PATH)`
  (`config_store.cpp:24-27`). Keine Syntax-, Pflichtfeld- oder Schlüsselprüfung.
  Bestätigt.
- `saveNetworkConfig()` und `saveWiFiCredentials()` öffnen die Live-Datei direkt
  mit Modus `"w"` (`config_store.cpp:124`, `:294`). Ein Abbruch nach dem Öffnen
  kürzt/zerstört die einzige Kopie. Bestätigt (nicht atomar, kein Backup).
- **Boot-Pfad:** Wenn `hasValidConfig()` zwar `true` liefert, `loadNetworkConfig()`
  aber an einem JSON-Parsefehler scheitert, wird **nur** `"ERROR: Failed to load
  configuration"` geloggt (`main.cpp:472-474`) und der Zweig fällt durch — das
  Setup-Portal wird ausschließlich im `else`-Zweig (`main.cpp:475-485`, Datei
  fehlt) geöffnet. Das Gerät landet also in einem unbrauchbaren Zustand ohne
  Portal. Die Bewertung des Berichts („robuste Rückkehr … nicht eindeutig
  abgesichert") ist damit sogar eher untertrieben.
- **Korrektur einer Detailaussage:** Der Bericht schreibt, beim Laden
  unvollständiger Hex-Schlüssel könne „der Rest des Zielarrays undefiniert
  bleiben". Tatsächlich initialisiert der `CasambiKey()`-Konstruktor das Array
  per `memset(key, 0, AES_KEY_SIZE)` (`network_config.h:25-27`) — der Rest bleibt
  **Null**, nicht uninitialisiert. Das Grundproblem (ein zu kurzer/nicht-hexer
  Schlüssel wird stillschweigend als gültiger, falscher Schlüssel akzeptiert)
  bleibt bestehen; nur die Formulierung „undefiniert" ist zu scharf.

**Fazit:** Berechtigt und mit Recht als höchste Priorität eingestuft. Der
empfohlene Ansatz (Temp-Datei → Validierung → atomares Umbenennen → Backup) ist
angemessen.

### S-02 · Request-Größe im Setup-Portal begrenzen — **Hoch, bestätigt**

Im Body-Handler von `POST /api/provision` wird bei `index == 0`
`buf->reserve(total)` aufgerufen (`setup_portal.cpp:263`), wobei `total` die vom
Client gemeldete `Content-Length` ist — **ohne Obergrenze und vor jeder
JSON-Prüfung.** Das Portal läuft auf dem offenen SoftAP. Der beschriebene
Heap-Erschöpfungs-/Reset-Vektor ist real. Der per-Request-`_tempObject`-Puffer
mit `onDisconnect`-Cleanup ist korrekt beschrieben. Bestätigt.

### S-03 · BLE-Sendeergebnis bis zur REST-API zurückführen — **Hoch, bestätigt**

- `_sendEncryptedPacket()` ist `void` und bricht bei Mutex-Timeout
  (`casambi_client.cpp:713`), fehlender Verschlüsselung (`:718`) und leerem
  Ciphertext (`:733`) still ab; der Rückgabewert von `writeValue()` (`:740`) wird
  **nicht** ausgewertet.
- `_sendOperation()` erhöht `_outPacketCount++` (`:688`) **nach** dem void-Aufruf,
  also unabhängig vom Erfolg. `_buildOperation()` erhöht `_origin++` (`:701`)
  bereits beim Aufbau.
- Alle öffentlichen Setter (`setUnitLevel`, `setSceneLevel`, … ) sind `void`
  (`casambi_client.h:172-180`).
- Web-Handler senden nach dem Aufruf pauschal Erfolg, z. B.
  `_client->setUnitLevel(unitId, 255); … _sendJsonSuccess(request);`
  (`webserver.cpp:1124-1127`). `_checkBle()` prüft nur den Zustand **vor** dem
  Senden.

Ergebnis: Ein Schaltbefehl kann HTTP 200 melden, obwohl kein Paket übertragen
wurde; Paket-/Origin-Zähler laufen bei Sendefehler weiter. Alles bestätigt. Mit
Recht Priorität Hoch, da externe Automatisierung (FHEM) auf die falsche
Erfolgsmeldung vertraut.

### S-04 · Lokale Einstellungen beim Cloud-Refresh erhalten — **Mittel, bestätigt**

Bei `runScheduledCloudRefresh()` werden aus `networkConfig` nach
`freshConfig` übernommen: `autoConnectEnabled`, `autoConnectAddress` und die fünf
Debug-Flags (`main.cpp:1047-1053`) sowie `casambiPassword` (`:1044`). **Nicht**
übernommen werden `gatewayName` und `ntpServer` (`network_config.h:99-106`) — sie
fallen auf die Konstruktor-Defaults (`""` bzw. `NTP_SERVER_DEFAULT`,
`network_config.h:123-124`) zurück. Ein benutzerdefinierter NTP-Server und der
Gatewayname gehen also nach einem Refresh verloren. Bestätigt. Die Empfehlung,
die lokalen Felder in `preserveLocalSettings()` zu kapseln, ist sinnvoll und
verhindert die Wiederholung des Fehlers bei neuen Feldern.

### S-05 · HTTP-Status im FHEM-Refreshcallback auswerten — **Mittel, bestätigt**

Der Callback (`98_CasambiGW.pm:334-347`) unterscheidet nur `$err`
(Transportfehler) vom Erfolgsfall. Im `else`-Zweig wird bedingungslos
`"refreshCasambi accepted by ESP: $data"` geloggt (`:345`) — `$param->{code}`
wird nicht ausgewertet. Ein 401/403/409/500 mit Fehler-JSON erschiene damit als
„accepted". Bestätigt.

### S-06 · Helligkeitsumrechnung — **Niedrig, bestätigt**

`my $level = int(($value // 0) * 2.55);` (`98_CasambiGW.pm:857`). Da `2.55` in
IEEE-754-double als `2.5499999…` dargestellt wird, ergibt `100 * 2.55 ≈
254.9999…`, `int()` schneidet auf **254** ab. Der `on`-Befehl sendet dagegen
`255` (`:852`). Die Diskrepanz ist real. Die vorgeschlagene Ersatzformel
`int($value * 255 / 100 + 0.5)` liefert die im Bericht genannten Sollwerte
(0→0, 1→3, 50→128, 100→255) korrekt. Bestätigt.

### S-07 · EventLog-Löschung aus dem async_tcp-Task delegieren — **Mittel, bestätigt**

`_handleDeleteLog()` ruft `EventLog::clear()` **direkt** im async-Webcallback auf
(`webserver.cpp:919`). `clear()` nimmt den Mutex mit `portMAX_DELAY`
(unbegrenzt, `event_log.cpp:274`) und führt drei `LittleFS.remove()`-Operationen
aus (`:276-278`). Blockierendes Flash-I/O im TCP-Task, wie beschrieben — und das
Reboot/Refresh-Muster zum Delegieren an den Loop-Task existiert bereits im
Projekt (wiederverwendbar). Bestätigt.

### S-08 · Cloud-Session-Token nicht vollständig protokollieren — **Mittel, bestätigt**

`Serial.printf("API: Session created: %s\n", sessionToken.c_str());`
(`api_client.cpp:175`) gibt das vollständige, noch gültige Session-Token auf der
seriellen Schnittstelle aus. Bestätigt. Da serielle Logs häufig geteilt werden,
ist die Redaction-Empfehlung berechtigt.

### S-09 · CI über reine Kompilierung hinaus ausbauen — **Mittel, bestätigt**

`ci.yml` führt ausschließlich `pio run -e devkit-v4` und `pio run -e esp32-c3`
aus (`ci.yml:13-14, 33-34`). Keine Unit-/Regressionstests, kein `perl -c` für die
FHEM-Module, keine Python-Syntaxprüfung der Skripte. Die im Bericht genannten
Regressionen (100 %→254, HTTP 200 trotz BLE-Fehler, Zählerdrift, fehlendes
Body-Limit) blieben in einer reinen Build-CI unentdeckt. Bestätigt. Die
Kopplungshinweise zu S-01/S-03 (Test-Seams) sind konsistent.

---

## Empfehlung zur Umsetzungsreihenfolge

Die im Bericht vorgeschlagene Reihenfolge ist plausibel und wird bestätigt:

1. **Phase 1 (Ausfall-/Datenverlustrisiken):** S-01, S-02, S-03 — höchste
   Priorität, jeweils mit Hardwaretest.
2. **Phase 2 (Konsistenz/Task-Sicherheit):** S-04, S-05, S-07.
3. **Phase 3 (Sicherheit/Genauigkeit):** S-06, S-08.
4. **Querschnitt:** S-09 begleitend, da S-01/S-03 ohnehin testbare
   Schnittstellen erfordern.

Die „Quick Wins" mit sehr niedrigem Aufwand und klarer Korrektheit — **S-06**
(deterministische Formel), **S-08** (eine Logzeile) und **S-05** (isolierte
FHEM-Korrektur) — können auch vorgezogen werden, da sie kein Hardwarerisiko
tragen.

---

## Umsetzung

Alle neun Maßnahmen wurden anschließend umgesetzt.

| ID | Umsetzung | Zentrale Dateien |
|----|-----------|------------------|
| S-01 | Atomares Speichern (Temp → Validierung → Backup-Swap), Recovery aus Backup beim Laden, Semantik-/Hex-Schlüssel-Validierung, Setup-Portal-Fallback bei Ladefehler | `storage/config_store.cpp`, `storage/config_validation.h`, `config.h`, `main.cpp` |
| S-02 | Body-Obergrenze (4 KB) + Allokationsprüfung vor `reserve`, 413/503 | `web/setup_portal.cpp` |
| S-03 | Sende-Ergebnis als `bool` durch BLE- und Web-Schicht; Zähler-Rollback bei Fehler; 503 statt 200 | `ble/casambi_client.{h,cpp}`, `web/webserver.{h,cpp}` |
| S-04 | `preserveLocalSettings()` inkl. `gatewayName`/`ntpServer` | `cloud/network_config.h`, `main.cpp` |
| S-05 | `CasambiGW_ClassifyRefreshResponse()` wertet `$param->{code}` aus | `FHEM/98_CasambiGW.pm` |
| S-06 | `CasambiGW_PercentToByte()` mit gerundeter Ganzzahlarithmetik + Clamping | `FHEM/98_CasambiGW.pm` |
| S-07 | `DELETE /api/log` setzt nur ein Flag (202); `EventLog::clear()` läuft im Loop-Task | `web/webserver.{h,cpp}`, `main.cpp` |
| S-08 | Session-Token wird nicht mehr protokolliert (nur Länge) | `cloud/api_client.cpp` |
| S-09 | Native Unit-Tests (`test/`), Perl-Tests (`FHEM/t/`), CI um `pio test -e native`, `perl -c`, `py_compile` erweitert | `platformio.ini`, `.github/workflows/ci.yml`, `test/`, `FHEM/t/` |

**Verifikation (host-seitig, ohne Zielhardware):**
- `config_validation.h` gegen echtes ArduinoJson kompiliert und geprüft — 15/15 Checks bestanden.
- `FHEM/t/CasambiGW_helpers.t` — 17/17 Tests bestanden (u. a. 100 % → 255, HTTP-Klassifizierung).
- `perl -c` beider FHEM-Module und `py_compile` der Skripte fehlerfrei.
- Firmware-Build (ESP32) und `pio test -e native` laufen in der CI; lokal war die
  PlatformIO-Registry durch die Netzwerk-Policy gesperrt, daher wurde die
  Validierungslogik standalone gegen ArduinoJson verifiziert.

> Hinweis zu S-01: `hasValidConfig()` bleibt bewusst ein günstiger
> Existenz-Check (Live- oder Backup-Datei), weil es auch im asynchronen
> Web-Handler (`POST /api/refreshCasambi`) läuft — eine vollständige
> Flash-Parse dort würde genau das durch S-07 adressierte Problem
> (Flash-I/O im async_tcp-Task) neu einführen. Die echte Parse- und
> Semantikprüfung sowie die Recovery erfolgen in `loadNetworkConfig()`, das
> die eigentliche Boot-Entscheidung (Betrieb vs. Setup) trägt.
