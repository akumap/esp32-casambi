# Konzept: Webserver-Stabilität unter Hochfrequenz-Last

Status: **umgesetzt** — Leak-Fix, WS-Client-Cap, Heap-/Fragmentierungs-Diagnose
und Stress-Test-Harness sind in `main` (PR #19, schließt #17). Ein
verbleibendes Library-Problem ist als #18 ausgegliedert und **nicht** Teil
dieses Konzepts.
Branch: `claude/issue-19-body-comments-3fa3ju`

## 1. Ziel

Der Webserver muss auch unter **dauerhafter, hochfrequenter Last** stabil
laufen — sowohl unter der realen FHEM-Produktionslast (eine persistente
WebSocket plus sporadische Steuer-POSTs) als auch unter missbräuchlicher
Überlast (viele parallele Clients, GET-Fluten, Schrott-POSTs). „Stabil" heißt
hier konkret:

- **Kein Heap-Leck**: Der freie Heap kehrt nach einer Lastphase auf das
  Ausgangsniveau zurück; er sinkt nicht monoton bis zum Absturz.
- **Keine Fragmentierung als Dauerschaden**: Auch der größte zusammenhängende
  Block erholt sich, nicht nur die freie Gesamtmenge.
- **Reproduzierbarkeit**: Ein wiederholbares Last-/Mess-Werkzeug, das einen
  echten Leak von einer transienten Delle und von einem absturzbedingten
  Reboot zuverlässig unterscheidet.

## 2. Ausgangslage (Ist-Zustand vor dem Fix)

Issue #17 meldete, dass der ESP32 unter starker, schneller Web-Last nach einer
Weile abstürzt. Der Heap lief langsam leer, danach krachte AsyncTCP.

Der Webserver (`src/web/webserver.cpp`) nutzt **ESPAsyncWebServer**. Die
statischen Routen werden über `server.on()` registriert; die **dynamischen
POST-Routen mit ID im Pfad** (`/api/units|groups|scenes/<id>/...`, `/api/ntp`)
haben **keinen** eigenen `server.on()`-Handler. Solche Routen landen in der
Catch-all-Behandlung.

## 3. Kernursache: doppelte HTTP-Antwort pro POST (das Leck)

ESPAsyncWebServer behandelt eine Route ohne passenden `server.on()`-Handler
über **zwei** Callbacks:

- `onRequestBody` — wird (ggf. mehrfach, chunkweise) aufgerufen, während der
  Request-Body eintrifft.
- `onNotFound` — wird danach **genau einmal** pro Request aufgerufen.

Der alte Code sendete die Antwort bereits aus `onRequestBody`. Anschließend
lief `onNotFound` und sendete eine **zweite** Antwort (seinen 404-Fall-Through).
Das erste Response-Objekt wurde dabei überschrieben, **ohne freigegeben zu
werden** → **~216 B Leck pro abgeschlossenem POST**. Bei FHEM-typischer
Frequenz summiert sich das, bis der Heap erschöpft ist und AsyncTCP abstürzt.

Zusätzlich gab es ein zweites, kleineres Leck: Bei **abgebrochenen**
POST-Verbindungen (Body kommt nie vollständig an) wurde der bereits angelegte
Body-Puffer (`_tempObject`) nie freigegeben, weil `onNotFound` für einen nie
abgeschlossenen Request gar nicht läuft.

## 4. Lösung: genau ein Antwortpunkt

Grundprinzip: **`onNotFound` ist der einzige Ort, der antwortet.**
`onRequestBody` **puffert nur** und sendet nie.

### 4.1 `onRequestBody` — nur puffern
(`webserver.cpp:391`)

- Greift nur für `HTTP_POST` auf `/api/...`.
- Bodys > 512 B werden **nicht** gepuffert (unsere JSON-Payloads sind < 64 B);
  die Ablehnung übernimmt `onNotFound` per `contentLength()`.
- Beim ersten Chunk (`index == 0`) wird der Puffer als `String*` in
  `request->_tempObject` angelegt **und** ein `request->onDisconnect()`-Handler
  scharfgeschaltet, der den Puffer freigibt, falls der Request nie abschließt.
  Das ist für abgebrochene Bodys der einzige Aufräumpfad.
- Folge-Chunks werden an den Puffer angehängt.

### 4.2 `onNotFound` — dispatchen und genau einmal antworten
(`webserver.cpp:327`)

- Nicht-POST → einmaliger 404.
- **Body-lose Steuer-Endpunkte** (`/on`, `/off`) → sofort antworten.
- **Body-Endpunkte** (`/level`, `/color`, `/temperature`, `/slider`,
  `/vertical`, `/api/ntp`):
  - Übergröße (`contentLength() > 512`) → `_tempObject` freigeben, **413**
    zurückgeben (immer noch der einzige Antwortpunkt → kein Doppel-Send).
  - Sonst den jeweiligen Handler aufrufen. Jeder Handler liest den gepufferten
    Body, gibt `_tempObject` frei und sendet **genau eine** Antwort (fehlender
    Body / ungültiges JSON → 400).
- Alles andere → einmaliger 404.

Die eigentlichen Handler (`_handleUnitLevel` usw.) blieben inhaltlich
unverändert; verschoben hat sich nur **wer wann antwortet**.

## 5. WebSocket-Client-Begrenzung

Gleichzeitige WS-Clients sind auf **3** begrenzt (`WS_MAX_CLIENTS`,
`config.h:139`). Durchgesetzt wird das in `loop()` über
`_ws->cleanupClients(WS_MAX_CLIENTS)` (`webserver.cpp:89`) — der älteste Client
über dem Limit wird verdrängt.

Eine Variante mit **hartem Reject zur Connect-Zeit** wurde getestet, leckte
aber unter Verbindungs-Churn Client-Strukturen und wurde zugunsten des
bewährten `cleanupClients`-Mechanismus wieder verworfen.

> Hinweis zur Thread-Trennung (Bestand, nicht Teil des Fix, aber relevant für
> die Stabilität): Status-Broadcasts aus dem BLE-Task werden über eine
> FreeRTOS-Queue (`WS_BROADCAST_QUEUE_DEPTH`, `config.h:147`) an den Loop-Task
> übergeben und erst dort mit `_ws->textAll()` gesendet. So wird die
> WS-Client-Verwaltung nie aus einem BLE-Callback heraus angefasst.

## 6. Diagnose & Instrumentierung

### 6.1 Fragmentierung sichtbar machen
`GET /api/status` liefert zusätzlich zu `free_heap`:

- `largest_block` (`ESP.getMaxAllocHeap`) — größter allozierbarer Block; deckt
  Fragmentierung auf, die `free_heap` allein verbirgt.
- `min_free_heap` (`ESP.getMinFreeHeap`) — schlimmster Tiefpunkt seit Boot.

Beide speisen den Recovery-Verdikt des Stress-Tests.

### 6.2 Gezielte, schaltbare Traces
- **`WSDBG`** (`webserver.cpp:105`): je eine Zeile pro WS-CONNECT/DISCONNECT/
  ERROR mit Client-Zahl und Heap, hinter `debug heap on`. Lokalisiert einen
  Churn-Leak (Heap fällt pro Zyklus, erholt sich nie) bzw. die letzte Zeile vor
  einem Reboot.
- **`WiFiRC:`** (`main.cpp`): WiFi-Reconnect-Checkpoints, um die verbleibenden
  Library-Abstürze (#18) einzugrenzen. Bewusst drin gelassen, um einen späteren
  Fix verifizieren zu können.

## 7. Stress-Test-Harness (`scripts/stress_test.py`)

Gestufte Last-Profile, vom Sanften zum Aggressiven:

| Profil | Charakter |
|---|---|
| `realistic` | Eine persistente WS wie FHEM, nur sporadische POSTs. **Produktionslast — muss felsenfest sein.** |
| `light` | realistic + etwas HTTP-Polling + eine langsame WS-Reconnect-Schleife. |
| `medium` | Mehrere parallele GET/POST-Worker + WS-Churn + 1 persistente WS. |
| `heavy` | Aggressiv: viele Worker, schneller WS-Churn, abgebrochene POSTs — das „excessive use"-Szenario aus #17, soll den Bruchpunkt finden. |

Eigenschaften:

- **Ramp-up** der Worker, **Rate-Limit pro Worker**, granulare Skip-Flags
  (`--skip-ws`, `--skip-abort`, …).
- FHEM-ähnlicher **persistenter WS-Worker**.
- **Cooldown-Phase** nach der Last mit getrenntem **Heap- und
  Fragmentierungs-Recovery-Verdikt**.
- **Reboot-Erkennung mitten im Lauf** (über `boot_count`), damit ein
  absturzbedingter Reboot kein falsches „recovered" vortäuschen kann.

## 8. Verifikation (auf dem Gerät)

Isolierte Läufe nach dem Fix (je 120 s Last + 30 s Cooldown):

| Pfad | Vorher | Nachher |
|---|---|---|
| Oversize-POST | ~64 KB verloren, fragmentiert | erholt, größter Block zurück |
| Control / Invalid-POST | leckte | erholt |
| GET-Flut (keep-alive & close) | — | erholt |
| Realistische Last (1 persistente WS) | — | 0 Fehler, Heap stabil |

## 9. Abgrenzung — bekanntes Restproblem (#18)

Unter **missbräuchlichem WS-Verbindungs-Churn** rebootet das Gerät weiterhin —
Ursache ist jedoch die `AsyncTCP-esphome`-Library (Null-Deref-Races in
`AsyncServer::_accept` und `AsyncClient::_lwip_fin`), und das tritt **bei
gesundem Heap** auf, also nicht im Anwendungscode. Realistische Last (eine
persistente FHEM-WS, kein Churn) löst es nicht aus. Tracking in #18 mit
Backtraces; empfohlener Fix ist die Migration auf den gepflegten
ESP32Async-Stack als separate, getestete Änderung.

## 10. Betroffene Dateien

- `src/web/webserver.cpp`, `src/web/webserver.h`, `src/config.h` — Leak-Fix,
  WS-Cap, Diagnose, Instrumentierung
- `src/main.cpp` — WiFi-Reconnect-Checkpoint-Tracing
- `scripts/stress_test.py` — neues, gestuftes Stress-Test-Harness
- `README.md` — Hinweis zur WS-Client-Begrenzung
