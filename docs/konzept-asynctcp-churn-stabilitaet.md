# Konzept: Stabilität unter WebSocket-/TCP-Verbindungs-Churn (Issue #18)

Status: **Vorschlag / noch nicht umgesetzt.** Adressiert die in #18 dokumentierten
Reboots unter hochfrequentem Verbindungs-Churn. Diagnose-Instrumentierung
(`WSDBG`, `WiFiRC:`) und das Stress-Harness sind bereits in `main` (aus PR #19);
dieses Konzept beschreibt den **Fix**, nicht die Diagnose.
Branch: `claude/issue-19-body-comments-3fa3ju`

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

### 4.2 WiFi-Reconnect watchdog-sicher machen (gegen B)

Unabhängig vom Library-Tausch und **vorrangig**, da es den harten Reboot in
einen sanften Retry verwandelt — selbst bei niedrigem Heap.

Aktueller Pfad `checkAndReconnectWiFi()` (`main.cpp:417`) ruft im loopTask
nacheinander `WiFi.disconnect()` → `delay(100)` → **`WiFi.begin()`** → Warte-
Schleife. `WiFi.begin()` ist hier die blockierende Stelle.

Ansatz: **nicht-blockierender Reconnect**, der auf das bereits aktive
`WiFi.setAutoReconnect(true)` (`main.cpp:238`) setzt, statt `WiFi.begin()` im
loopTask zu blockieren — der loopTask kehrt sofort zurück und füttert weiter den
Watchdog; der nächste `checkAndReconnectWiFi()`-Tick prüft den Status erneut.
Die `WiFiRC:`-Checkpoints bleiben zur Verifikation drin.

> Hinweis: Da (B) heap-getrieben ist, kann der Leak-Fix aus PR #19 den Auslöser
> bereits entschärfen. Der watchdog-sichere Reconnect ist dennoch nötig, weil er
> das Verhalten auch unter Heap-Druck graceful hält.

## 5. Abnahme (muss bestehen, bevor #18 als gelöst gilt)

Flashen, `debug heap on`, dann mit `scripts/stress_test.py`:

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

## 6. Aufräumen nach Verifikation

- `WSDBG`-WS-Trace und `WiFiRC:`-Checkpoints (Untersuchungs-Gerüst) entfernen —
  oder gegated lassen; bei Merge entscheiden.
- `platformio.ini` auf **feste Version-Tags** (nicht floatend) für reproduzier-
  bare Builds setzen.

## 7. Betroffene Dateien (geplant)

- `platformio.ini` — `lib_deps` auf ESP32Async, Versions-Pins, ggf.
  `CONFIG_ASYNC_TCP_*`-Flags
- `src/web/webserver.cpp` / `.h` — API-Anpassungen an den neuen Stack,
  Re-Verifikation des Single-Response-Verhaltens
- `src/main.cpp` — nicht-blockierender, watchdog-sicherer WiFi-Reconnect

## 8. Referenzen

- Crash-Backtraces: #18-Body (`_accept` AsyncTCP.cpp:1421, `_lwip_fin`
  AsyncTCP.cpp:920).
- AsyncTCP-Close-/FIN-Fixes: ESP32Async/AsyncTCP-Releases v3.4.5–v3.4.10.
- `_accept`-Fix-Status: nicht in den Release-Notes bestätigt → per WS-Churn-Test
  beweisen.
- Verwandt (gleiche Crash-Familie, im esphome-Fork ungelöst): esphome/issues#5676.
