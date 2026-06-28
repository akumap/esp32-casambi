# Konzept: Stabilität unter WebSocket-/TCP-Verbindungs-Churn (Issue #18)

Status: **in Umsetzung.** Adressiert die in #18 dokumentierten Reboots unter
hochfrequentem Verbindungs-Churn. Diagnose-Instrumentierung (`WSDBG`, `WiFiRC:`)
und das Stress-Harness sind bereits in `main` (aus PR #19).
Branch: `claude/migrate-async-tcp-stack-esp32async`

**Umsetzungsstand:**
- ✅ `platformio.ini`: Async-Stack auf `ESP32Async/AsyncTCP#v3.4.10` +
  `ESP32Async/ESPAsyncWebServer#v3.11.1` umgestellt (feste Git-Tags).
- ✅ `src/main.cpp`: WiFi-Reconnect ist jetzt nicht-blockierend / watchdog-sicher.
- ✅ `src/web/webserver.h`: obsoleten `HTTP_GET/POST/DELETE`-Makro-Workaround
  **entfernt** — der neue Stack deklariert die Methoden selbst (Details in 4.1.1).
  Sonst keine Änderung an `webserver.cpp` (alle übrigen Berührungspunkte stabil).
- ✅ Compile-Fix nach erstem Build-Versuch auf dem Host (Makro-Kollision mit dem
  Enum des neuen Stacks).
- ⏳ Ausstehend: erfolgreicher Build auf dem PlatformIO-Host + zweistufige Abnahme
  (Abschnitt 6) auf echter Hardware.

## 1. Ziel

Das Gerät darf unter **missbräuchlichem Verbindungs-Churn** (viele
WebSocket-Connects/-Disconnects pro Sekunde, z. B. ein Client in einer schnellen
Reconnect-Schleife) **nicht mehr rebooten**. Der Fix gilt erst als erledigt,
wenn die WS-Churn-Reproduktion mit dem vorhandenen Stress-Harness sauber
durchläuft — **kein** `Guru Meditation`, **kein** `REBOOT DETECTED`, Heap und
größter Block erholen sich.

Zwei Symptome mit teils gemeinsamer Wurzel sind abzudecken:

- **(A) Panic im AsyncTCP-Stack** beim Annehmen/Schließen von TCP-Verbindungen
  (primär, reproduzierbar) — tritt **bei gesundem Heap** auf.
- **(B) Watchdog-Hang in `WiFi.begin()`** nach einem last-bedingten
  WiFi-Abriss (ursprüngliche Meldung von #18) — tritt **bei knappem Heap** auf.

## 2. Ausgangslage (Ist-Zustand)

Dies ist **getrennt** vom Double-Response-Heap-Leck der dynamischen POST-Routen
— das war ein Anwendungsfehler und ist in PR #19 behoben. #18 liegt **nicht** im
Anwendungscode.

Aktuelle Async-Abhängigkeiten (`platformio.ini:8`):

```
esphome/ESPAsyncWebServer-esphome @ 3.2.2
esphome/AsyncTCP-esphome          @ 2.1.4
build_flags = -DCORE_DEBUG_LEVEL=0 -DCONFIG_ASYNC_TCP_RUNNING_CORE=1 -DCONFIG_ASYNC_TCP_USE_WDT=0
```

Beide `*-esphome`-Forks sind **nicht aktiv gepflegt**.

## 3. Kernursache

### 3.1 (A) Races in `AsyncTCP-esphome`
Ein reiner WS-Churn-Lauf (nur schnelle Connects/Disconnects, kein GET/POST)
rebootet, obwohl der Heap **gesund** ist (free ~100 KB, größter Block ~80 KB
durchgehend). Zwei Absturzstellen, beide im lwip-`tcpip_thread`, beide in
Library-Code:

```
LoadProhibited, EXCVADDR=0x30
  AsyncServer::_accept(tcp_pcb*, signed char)   AsyncTCP.cpp:1421
LoadProhibited, EXCVADDR=0x00
  AsyncClient::_lwip_fin(tcp_pcb*, signed char)  AsyncTCP.cpp:920
```

Das sind **Null-Deref-/Use-after-free-Races** beim Accept (`_accept`) bzw. bei
der FIN-Verarbeitung (`_lwip_fin`) unter Churn. Die `WSDBG`-Instrumentierung
belegt: Client-Zahl bleibt im Cap und pendelt normal, der Heap erholt sich nach
**jedem** Connect/Disconnect-Zyklus (kein WS-Leck) — also eine **Logik-Race**,
kein OOM.

### 3.2 (B) Blockierendes `WiFi.begin()`
In einem gemischten Lauf wurde der Heap zuvor durch eine separate Churn-Episode
niedrig getrieben (`free ~36 KB, largest_block 7156`, ~1 min festhängend),
danach brach WiFi ab und der Reconnect-Pfad hing. Die `WiFiRC:`-Checkpoints
(`main.cpp:436`) zeigen `-> begin()` als letzte Zeile, `-> wait loop` wird nie
erreicht → **`WiFi.begin()` blockierte loopTask die vollen 30 s** → WDT-Reboot.

Wahrscheinliche Kette: **niedriger/fragmentierter Heap → WiFi-Subsystem kann
nicht allozieren → Link fällt → `WiFi.begin()` stallt unter Heap-Druck →
Watchdog-Reboot.**

> Wichtig: Realistische Last löst nichts davon aus. Das FHEM-Gateway hält **eine
> persistente WebSocket ohne Churn**; das `realistic`-Profil lief fehlerfrei
> (0 Fehler, Heap stabil). Für (A)/(B) braucht es ein abusives Churn-Muster.

## 4. Lösung

Zwei voneinander **unabhängige** Maßnahmen — getrennt umsetzbar und testbar.

### 4.1 Async-Stack auf ESP32Async migrieren (gegen A)

Wechsel von den `*-esphome`-Forks auf die aktiv gepflegte **ESP32Async**-Org:

```
ESP32Async/ESPAsyncWebServer   (aktuell, ~v3.11.x)
ESP32Async/AsyncTCP            (aktuell, ~v3.4.10+)
```

Begründung: Der ESP32Async/AsyncTCP-Changelog enthält **gezielte Fixes am
Close-/FIN-Pfad** — v3.4.10 „Defer close on fin to async task", v3.4.6
„Error/closing stability", v3.4.7 (abort/dispose + memleak), v3.4.5 („Replace
closed_slots with double indirection"). Diese zielen direkt auf den
**`_lwip_fin`**-Crash. Für **`_accept` gibt es keinen bestätigten Fix** → dieser
Pfad ist unverifiziert und **muss** durch die WS-Churn-Reproduktion abgesichert
werden.

→ Dies ist eine **eigenständige, sorgfältig getestete Abhängigkeitsmigration**
(wie zuvor BLE/NimBLE), keine schnelle Änderung. Branch-basiert rückrollbar; die
`*-esphome`-Versionen oben für ein schnelles Zurück-Pinnen notiert halten.

#### Schritte
1. Eigener Branch von `main` (nach Merge von PR #19, damit der Double-Response-
   Fix in der Basis liegt).
2. `lib_deps` auf die ESP32Async-Libs umstellen; beide `esphome/*`-Einträge raus.
3. Empfohlene `CONFIG_ASYNC_TCP_*`-Build-Flags aus der ESP32Async/AsyncTCP-README
   prüfen (Queue-Größe, Priorität, Stack-Größe, Running-Core, Max-Ack-Time);
   `RUNNING_CORE=1` beibehalten. Wechselwirkung mit dem knappen Heap
   (NimBLE-Koexistenz) gegenprüfen.
4. API-/Compile-Änderungen in `src/web/webserver.cpp` / `.h` abklären — kritische
   Berührungspunkte:
   - `request->_tempObject` (Body-Pufferung) — Feld/Semantik bestätigen.
   - `request->onDisconnect([...])` (Cleanup abgebrochener Bodys).
   - `request->beginChunkedResponse(...)` (Streaming-Quelle für `/api/log`).
   - `AsyncWebSocket::cleanupClients(WS_MAX_CLIENTS)`, `textAll`, `count()`,
     `client->text()/close()`.
   - `DefaultHeaders::Instance().addHeader(...)`.
   - **Catch-all-Routing** (`onNotFound` + `onRequestBody`): im neuen Fork kann
     das Zusammenspiel anders sein → **erneut verifizieren, dass pro POST genau
     ein `request->send()` erfolgt** (Kern des PR-#19-Fix). Die Oversize-/
     Control-/Invalid-Isolation erneut laufen lassen, damit kein Per-Request-Leck
     zurückkehrt.

#### 4.1.1 Ergebnis des API-Reviews (umgesetzt)
Eine Quelländerung war nötig: der **`HTTP_GET/POST/DELETE`-Makro-Workaround** in
`webserver.h` musste **entfernt** werden. Der alte `*-esphome`-Fork umschloss
sein Methoden-Enum mit `#ifndef HTTP_ANY`, sodass vorab gesetzte Makros es
unterdrückten. Der ESP32Async-Stack deklariert die Methoden dagegen im Enum
`AsyncWebRequestMethod` (Werte u. a. `HTTP_DELETE=1<<0`, `HTTP_GET=1<<1`) **ohne**
diesen Guard und exportiert sie selbst global (`using namespace`, abschaltbar via
`ASYNCWEBSERVER_NO_GLOBAL_HTTP_METHODS`). Die alten Makros zerstörten dieses Enum
beim Kompilieren (`expected identifier before numeric constant`) — daher raus.
Kollision mit dem `HTTPMethod`-Enum des Arduino-Cores (`<HTTPClient.h>`, via
`cloud/api_client.h`) entsteht nur am tatsächlichen Verwendungsort; in den
relevanten Übersetzungseinheiten wird kein nacktes `HTTP_*` neben `HTTPClient`
referenziert, daher ist keine Qualifizierung nötig.

Alle übrigen Berührungspunkte in `webserver.cpp` sind unverändert (gegen v3.11.1
geprüft):
- `request->_tempObject` (`void*`), `request->onDisconnect(...)`,
  `request->beginChunkedResponse("application/json", filler)` mit
  `size_t(uint8_t*, size_t, size_t)`-Lambda — unverändert.
- `AsyncWebSocket`: `cleanupClients`, `textAll`, `count`, `client->text/close/id`
  und die Event-Callback-Signatur — unverändert.
- `DefaultHeaders::Instance().addHeader(...)` — unverändert.
- Der `HTTP_GET/POST/DELETE`-Makro-Workaround in `webserver.h` (vor dem Include,
  greift via `#ifndef HTTP_ANY`-Guard der Lib) bleibt gültig: die
  `WebRequestMethod`-Enumwerte (`GET=1`, `POST=2`, `DELETE=4`) sind identisch.

**Eine relevante Verhaltensänderung** (kein Compile-Bruch): Ab
ESPAsyncWebServer v3.11.0 ist `CloseClientOnQueueFull` per Default **`false`** —
ein WS-Client mit voller Sende-Queue wird **nicht** mehr getrennt, sondern die
Nachricht verworfen. Das passt zu unserem Broadcast-Modell (jede `unit_state`-/
`connection_state`-Nachricht ist ein **vollständiger** Zustands-Snapshot; eine
verworfene Nachricht wird von der nächsten korrigiert — dieselbe Drop-Toleranz
wie unsere `broadcastUnitState`-Queue). Daher **bewusst beim Default belassen**;
auf der Hardware ist in der Abnahme zu bestätigen, dass ein langsamer Client
keinen Queue-Aufbau erzeugt (WS-Cap = 3, geringes Broadcast-Volumen).

### 4.2 WiFi-Reconnect watchdog-sicher machen (gegen B)

Unabhängig vom Library-Tausch und **vorrangig**, da es den harten Reboot in
einen sanften Retry verwandelt — selbst bei niedrigem Heap.

Der alte Pfad `checkAndReconnectWiFi()` rief im loopTask nacheinander
`WiFi.disconnect()` → `delay(100)` → **`WiFi.begin()`** → eine 5-s-Warte-Schleife.
`WiFi.begin()` war die blockierende Stelle.

**Umgesetzt** (`src/main.cpp`): nicht-blockierender Reconnect.
- Die blockierende `begin()`+Warte-Schleife ist **entfernt**.
- Primäre Recovery über das bereits beim Erstconnect aktive
  `WiFi.setAutoReconnect(true)` (`main.cpp:238`) — der IDF-WiFi-Task verbindet
  selbst neu.
- Pro 30-s-Tick nur ein **nicht-blockierender Nudge** via `WiFi.reconnect()`
  (nutzt die gespeicherte Config, leichter als `begin()`, kehrt sofort zurück);
  der neue Status wird auf einem späteren Tick beobachtet, nicht hier abgewartet.
- Die Nacharbeiten bei Wiederverbindung (NTP-Re-Arm, defensiver Web-Server-
  Restart) laufen jetzt **einmalig** beim Übergang *disconnected → connected*.
- Die `WiFiRC:`-Checkpoints bleiben zur Verifikation drin (jetzt
  `-> reconnect() [non-blocking]` bzw. `<- reconnected`).

> Hinweis: Da (B) heap-getrieben ist, kann der Leak-Fix aus PR #19 den Auslöser
> bereits entschärfen. Der watchdog-sichere Reconnect ist dennoch nötig, weil er
> das Verhalten auch unter Heap-Druck graceful hält.

## 5. Risikomitigation

Eine Stack-Migration ist riskant, weil sich das Verhalten **subtil** ändern
kann, ohne dass etwas offensichtlich bricht: die `*-esphome`- und die
ESP32Async-Forks teilen dieselbe API-Oberfläche, aber das **Verhalten** der
Catch-all-Callbacks, des Chunked-Response-Pfads oder der WS-Client-Verwaltung
kann abweichen. Reine Stress-Tests (`stress_test.py`) finden Abstürze und Lecks,
prüfen aber **nicht**, ob jede Funktion auf dem neuen Stack noch *korrekt*
arbeitet. Genau diese Lücke schließt die Mitigation.

### 5.1 Strategie
- **Branch-basiert rückrollbar** (wie BLE/NimBLE): die `*-esphome`-Versionen
  bleiben oben notiert; bei Regression Branch verwerfen und zurück-pinnen.
- **Kleine, prüfbare Schritte**: erst nur `lib_deps`/Flags tauschen und bauen,
  dann Compile-Fixes, dann funktionale Verifikation, erst zuletzt Stress.
- **Zwei-stufige Abnahme**: eine **funktionale** Verifikation der
  migrations-empfindlichen Punkte (5.2) *vor* der bereits vorhandenen
  **Stress-/Churn**-Abnahme (Abschnitt 6). Funktion zuerst — ein Stack, der
  unter Last nicht abstürzt, aber Bodies falsch puffert, ist keine Lösung.

### 5.2 Funktions-Verifikationsskript `scripts/verify_tcp_stack.py`

Neues, eigenständiges Skript (reine stdlib, gleiche WS-/HTTP-Helfer wie
`stress_test.py`). Es **belastet das Gerät nicht**, sondern prüft gezielt die
**API-Berührungspunkte, die sich durch den Stack-Wechsel verschieben können** —
jeder Punkt aus 4.1 Schritt 4 bekommt eine eigene Assertion. CI-tauglich:
Exit-Code 0 = alle Checks grün, 1 = mindestens einer rot.

| ID | Prüft | Migrations-Bezug |
|---|---|---|
| T1 | CORS-Header auf jeder Antwort | `DefaultHeaders::Instance().addHeader` |
| T2 | GET-Routen liefern gültiges JSON | `server.on()`-GET-Pfade |
| T3 | `/api/log` streamt gültiges JSON-Array (`?n=`, `?n=0`) | `beginChunkedResponse` |
| T4 | POST-Body erreicht den Handler (kein „missing body") | `onRequestBody` → `_tempObject` → `onNotFound`-Dispatch |
| T5 | Kein Per-POST-Leck über einen Burst | **Single-Response** (PR-#19-Regression darf nicht zurückkehren) |
| T6 | Übergroßer POST → 413 | `contentLength()`-Reject in `onNotFound` |
| T7 | Ungültiges JSON / leerer Body → 400 | saubere Einzelantwort der Handler |
| T8 | Abgebrochener Body wird freigegeben | `request->onDisconnect()`-Cleanup |
| T9 | WS-Handshake + `hello`-Snapshot beim Connect | `AsyncWebSocket`-Upgrade + `client->text()` |
| T10 | WS-Client-Cap wird durchgesetzt | `cleanupClients(WS_MAX_CLIENTS)` |

T5 und T8 vergleichen den Heap (`free_heap` aus `/api/status`) vor/nach einem
Burst, damit auch ein langsames Leck auffällt. Am Ende prüft das Skript per
`boot_count`, dass das Gerät während der Checks **nicht rebootet** ist.

Aufruf:
```
python3 scripts/verify_tcp_stack.py --host <ip> --ws-max-clients 3
```
Wird vor BLE-Verbindung ausgeführt, ist T4 mit `503` (statt `200`) zufrieden —
entscheidend ist nur, dass der Body ankommt, nicht der BLE-Erfolg.

## 6. Abnahme (muss bestehen, bevor #18 als gelöst gilt)

**Stufe 1 — Funktion** (siehe 5.2): `verify_tcp_stack.py` muss mit Exit-Code 0
durchlaufen. Erst dann Stufe 2.

**Stufe 2 — Stabilität:** Flashen, `debug heap on`, dann mit
`scripts/stress_test.py`:

```
# WS-Churn — das, was _accept / _lwip_fin vorher zum Absturz brachte
python3 scripts/stress_test.py --host <ip> --profile medium \
        --skip-get --skip-post --skip-abort --duration 120

# Volle Mischlast
python3 scripts/stress_test.py --host <ip> --profile medium --duration 120

# Realistik-Regression — muss perfekt bleiben
python3 scripts/stress_test.py --host <ip> --profile realistic --duration 120
```

Pass-Kriterien: kein `REBOOT DETECTED`, kein `Guru Meditation` (insb. kein
`_accept` / `_lwip_fin`), freier Heap erholt sich, „largest block recovered".
Der WSDBG-Trace zeigt keine Client-Akkumulation.

## 7. Aufräumen nach Verifikation

- `WSDBG`-WS-Trace und `WiFiRC:`-Checkpoints (Untersuchungs-Gerüst) entfernen —
  oder gegated lassen; bei Merge entscheiden.
- `platformio.ini` auf **feste Version-Tags** (nicht floatend) für reproduzier-
  bare Builds setzen.

## 8. Betroffene Dateien (geplant)

- `platformio.ini` — `lib_deps` auf ESP32Async, Versions-Pins, ggf.
  `CONFIG_ASYNC_TCP_*`-Flags
- `src/web/webserver.cpp` / `.h` — API-Anpassungen an den neuen Stack,
  Re-Verifikation des Single-Response-Verhaltens
- `src/main.cpp` — nicht-blockierender, watchdog-sicherer WiFi-Reconnect
- `scripts/verify_tcp_stack.py` — **neu**: funktionale Verifikation der
  migrations-empfindlichen API-Punkte (Risikomitigation, siehe 5.2)

## 9. Referenzen

- Crash-Backtraces: #18-Body (`_accept` AsyncTCP.cpp:1421, `_lwip_fin`
  AsyncTCP.cpp:920).
- AsyncTCP-Close-/FIN-Fixes: ESP32Async/AsyncTCP-Releases v3.4.5–v3.4.10.
- `_accept`-Fix-Status: nicht in den Release-Notes bestätigt → per WS-Churn-Test
  beweisen.
- Verwandt (gleiche Crash-Familie, im esphome-Fork ungelöst): esphome/issues#5676.
