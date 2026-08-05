# Konzept: Serielle Konsole über TCP/IP (Telnet)

Status: **Entscheidungsgrundlage — nicht umgesetzt.** Dieses Dokument legt die
Variantenwahl, die Sicherheitsentscheidung und das Design fest; Code existiert
noch nicht.
Branch: `claude/serial-console-tcp-ip-yxnmy8`

Beantwortet die Frage: „Wie kann die serielle Konsole zusätzlich über das Netz
bedient werden, mit einem Standardtool auf dem Laptop?"

## 0. Kurzantwort

**Zielkonfiguration:** ESP32 am USB-Netzteil, frei platziert für gute
BLE-Bedingungen, in WLAN-Reichweite. Laptop im selben Netz bedient die Konsole
für `debug`, `refresh`, `status`, `log`. **Geflasht wird unverändert seriell
über USB.**

**Entscheidung:** Telnet-Server in der Firmware auf Port 23, Login mit dem
bereits vorhandenen **abgeleiteten API-Token** (nicht mit dem Casambi-Passwort).
Client auf dem Laptop: PuTTY, `telnet`, `nc`.

**SSH ist verworfen** — zwei unabhängige K.-o.-Kriterien (Heap, Lizenz),
Abschnitt 3.

| Variante | Firmware-Aufwand | Heap zur Laufzeit | Host-Tool | Live-Logs | erfüllt Zielkonfiguration |
|---|---|---|---|---|---|
| **A** `ser2net`/`socat` auf einem Host am USB-Port | keiner | 0 | `telnet`, `pio monitor rfc2217://` | ✅ | ❌ — setzt einen Rechner am USB-Kabel voraus |
| **B** Telnet-Server in der Firmware | mittel–hoch | wenige KB | **PuTTY**, `telnet`, `nc` | ✅ | ✅ **gewählt** |
| **C** Konsole über den vorhandenen WebSocket `/ws` | mittel | gering (Reuse) | `websocat`, Browser | ✅ | ⚠️ kein PuTTY |
| **D** `POST /api/console` + `curl` | gering | minimal | `curl` | ❌ | ⚠️ keine Konsole, nur Einzelkommandos |
| **E** SSH-Server in der Firmware | sehr hoch | ~40 KB+ zusammenhängend | `ssh`, PuTTY | ✅ | ❌ **verworfen**, Abschnitt 3 |

Variante A bleibt für die reine Entwicklungsarbeit am Schreibtisch die
schlankeste Lösung (null Firmware, überlebt WLAN-Ausfälle, behält den
`esp32_exception_decoder`) — sie scheidet nur aus, weil die Zielkonfiguration
bewusst *keinen* Rechner am Gerät vorsieht. Sie ist damit kein Ersatz, sondern
eine Ergänzung für die Werkbank.

## 1. Ausgangslage im Code

Drei Eigenschaften des bestehenden Codes bestimmen den gesamten Entwurf:

### 1.1 Ausgabe ist über den ganzen Code verstreut

662 `Serial.print*`-Aufrufe in 16 Dateien. Es gibt keinen zentralen
Ausgabekanal, den man umlenken könnte:

| Datei | Aufrufe | Task |
|---|---|---|
| `src/serial_console.cpp` | 223 | loop |
| `src/ble/casambi_client.cpp` | 143 | **NimBLE-Host-Task** (Callbacks) |
| `src/diagnostics.cpp` | 67 | loop |
| `src/cloud/api_client.cpp` | 51 | loop (bzw. Boot) |
| `src/main.cpp` | 42 | loop |
| `src/storage/config_store.cpp` | 34 | loop |
| `src/cloud_refresh.cpp` | 26 | Boot |
| `src/ble/packet.cpp` | 23 | NimBLE-Host-Task |
| `src/crypto/*`, `src/net/*`, `src/log/*`, `src/web/setup_portal.cpp` | je 3–12 | loop |
| `src/web/webserver.cpp` | 5 | **async_tcp-Task** |
| **Summe** | **662** | |

Die Task-Spalte ist der wichtige Teil und war beim ersten Durchsehen nicht
offensichtlich: **Ausgaben entstehen auf mindestens drei Tasks.**
`Serial.print` ist dabei unkritisch, weil der Arduino-`HardwareSerial` intern
serialisiert. Ein Socket-Write ist das nicht — direkt aus dem Tee heraus auf
denselben `WiFiClient` zu schreiben, würde lwIP aus drei Tasks parallel
bedienen. Das schließt den naiven Ansatz „Tee schreibt sofort in den Socket"
aus und führt direkt zum Ringpuffer in Abschnitt 4.2.

### 1.2 Eingabe kommt ausschließlich aus dem loop-Task

`main.cpp:329-335` liest `Serial.readStringUntil('\n')` und ruft
`handleCommand()` synchron auf. `serial_console.h:8-14` hält ausdrücklich fest,
dass das eine tragende Invariante ist: Wizard, `scan`, `connect` und die
Reboot-Kommandos dürfen mit nichts anderem parallel laufen; die REST-API
umgeht diese Pfade deshalb bewusst und reiht stattdessen `BleCommand`-Einträge
für den loop-Task ein.

Ein in `loop()` gepollter Telnet-Server erfüllt diese Invariante **ohne
Zusatzaufwand** — er ruft `handleCommand()` auf demselben Task auf. Das ist der
Hauptgrund für `WiFiServer` (synchron) statt AsyncTCP.

### 1.3 Fünf blockierende Eingabestellen

`serial_console.cpp:242, 887, 916, 928, 938` warten mit
`while (!Serial.available()) { esp_task_wdt_reset(); … }` hart auf `Serial` —
Setup-Wizard und `connect`. Über Telnet würde die Session dort hängen. Siehe
Entscheidung E3.

## 2. Warum überhaupt Telnet und nicht C/D

Der Nutzen einer Netzwerkkonsole steht und fällt mit den **asynchronen**
Ausgaben: BLE-Reconnects, 0x06-Broadcasts, Heap-Monitor, Parse-Warnungen. Wer
nur Kommandos absetzen und deren direkte Antwort sehen will, braucht keine
Konsole — das kann `curl` gegen die bestehende REST-API bereits (Variante D).
Genau deshalb ist der mechanische Rename aus Abschnitt 4.1 unvermeidlich: ohne
ihn sieht die Netzwerkkonsole nur das, was `handleCommand()` selbst ausgibt,
und ist damit eine umständliche Variante von `curl`.

Variante C (Konsole über `/ws`) wäre firmwareseitig kleiner, weil Auth,
Heap-Guards und Client-Cleanup schon existieren — scheitert aber am
Standardtool: `websocat` ist auf einem frischen Laptop nicht vorhanden, ein
Browser-Panel im Dashboard wäre ein eigenes Feature. Zusätzlich wäre es eine
Änderung am Wire-Format und damit `FHEM_API_VERSION`-pflichtig (CLAUDE.md).
Telnet ist von beidem frei.

## 3. SSH — verworfen

Die Motivation ist berechtigt: bei Telnet läuft das Login-Geheimnis im Klartext
über das LAN. Trotzdem ist SSH auf diesem Gerät nicht der Weg.

### 3.1 Heap — der harte Blocker

Gemessene Werte (aus `docs/konzept-asynctcp-churn-stabilitaet.md`, Stresstests
auf echter Hardware, via `/api/status`):

| Situation | `free_heap` | `largest_block` |
|---|---|---|
| Leerlauf | 90–96 KB | 37–83 KB |
| unter Misch-Last (HTTP + WS) | 37–58 KB | **7–27 KB** |
| `min_free_heap` im Stresstest | 7–16 KB | — |

Dazu `HEAP_CRITICAL_THRESHOLD` = 20 000 B (`config.h:225`) → dreimal in Folge
unterschritten ⇒ Reboot (`diagnostics.cpp:54-67`). Kein PSRAM.

Entscheidend ist die Spalte `largest_block`: unter Last stehen zeitweise nur
7–27 KB **zusammenhängend** zur Verfügung. Genau daran scheitern schon heute
TLS-Handshakes zur Laufzeit — `README.md:1126` dokumentiert `HTTP -1` bei
aktivem BLE-Stack, und `cloud_refresh.cpp:20-28` verschiebt den Cloud-Download
deshalb auf den nächsten Boot, *bevor* BLE und Webserver starten.

Ein SSH-Key-Exchange braucht einen vergleichbaren zusammenhängenden
Krypto-Arbeitsbereich — aber **zur Laufzeit, bei jedem Login, mit aktivem BLE
und async_tcp**. Das ist exakt die Konstellation, die das Projekt bereits als
nicht funktionierend dokumentiert hat. Ein Login, der zufällig mit einem
BLE-Reconnect zusammenfällt, würde fehlschlagen oder das Gerät unter die
Reboot-Schwelle drücken — auf einem Gerät, das man gerade fernwarten wollte.
Der Trick aus `cloud_refresh` (auf den Boot verschieben) ist hier
definitionsgemäß nicht anwendbar.

### 3.2 Lizenz — der zweite, unabhängige Blocker

Das Repo steht unter **MIT** (`LICENSE`).

| Bibliothek | Lizenz | Bewertung |
|---|---|---|
| **wolfSSH** (+ wolfSSL) | GPLv3 oder kommerziell | statisch gelinkt ⇒ Firmware-Binary wird GPLv3, kollidiert mit der MIT-Distribution |
| **libssh** (ESP32-Portierungen existieren) | LGPL | Relinking-Auflage bei statisch gelinkter Firmware praktisch kaum erfüllbar |
| **Dropbear / OpenSSH** | — | setzen POSIX-Prozesse voraus, nicht sinnvoll portierbar |

mbedTLS ist zwar im Build (`webserver.cpp:16`), liefert aber nur Primitive —
SSH-Transportschicht, KEX, Userauth und Channels sind nicht enthalten. Es gibt
keinen „Flag umlegen"-Pfad.

### 3.3 Auch der Komfortgewinn ist fraglich

- Host-Key muss erzeugt und in LittleFS persistiert werden; RSA-Keygen dauert
  auf einem ESP32 im Minutenbereich, nur Ed25519/ECDSA ist gangbar.
- Aktuelle OpenSSH-/PuTTY-Versionen lehnen veraltete KEX-/Cipher-Sätze ab. Kann
  die Embedded-Lib nur einen schmalen Satz, landet man beim Client bei
  `-oKexAlgorithms=+…` — dann ist auch SSH kein „einfach so"-Standardtool mehr.
- Das Blockier-/Backpressure-Problem aus 4.2 bliebe identisch, nur mit einer
  teureren Krypto-Schicht im Schreibpfad.

### 3.4 Die Verhältnismäßigkeit

`/api/*`, `/ws` und das Dashboard laufen in diesem Projekt bereits
unverschlüsselt über HTTP, mit dem Token im Klartext in jedem Request-Header —
ESPAsyncWebServer kann auf dem ESP32 kein TLS. SSH würde ausgerechnet die
Debug-Konsole zur einzigen verschlüsselten Fläche machen, während die
vollständige Steuer-API daneben offen im LAN liegt. Wer mitliest, braucht die
Konsole gar nicht. Das deckt sich mit der Schutzziel-Tabelle in
`konzept-security-hardening.md`, Abschnitt 1: „Schutz gegen passiven
L2-Sniffer" ist dort bereits als **nur teilweise im Scope** eingestuft.

### 3.5 Wenn doch Verschlüsselung gebraucht wird

Ohne Firmware-Änderung: Läuft irgendwo im Netz ein Linux-Host (z. B. der
FHEM-Raspberry-Pi, auch wenn der ESP32 nicht mehr an dessen USB hängt), dann
`ssh pi` und von dort `telnet casambi-xxxx.local`. Verschlüsselt bis zum Pi,
Klartext nur auf dem letzten Hop — und von außerhalb des LAN nutzbar.

## 4. Design

Neues Modul `src/net/telnet_console.*`, in `loop()` gepollt.

### 4.1 Ausgabe: `Console`-Tee + mechanischer Rename

Ein von `Print` abgeleitetes globales Objekt `Console`, dessen `write()` an
`Serial` **und** an den Telnet-Ringpuffer fächert. Dazu eine
`sed`-Ersetzung `Serial.print` → `Console.print` an 662 Stellen.

**Nur `print`/`println`/`printf`.** Die sechs Eingabe- und Setup-Stellen
(`Serial.begin`, `Serial.available`, `Serial.readStringUntil`) bleiben auf dem
echten `Serial`. Damit ist der Diff ein reiner, mechanisch überprüfbarer
Rename, und der serielle Kanal bleibt unverändert funktionsfähig.

Bewusst *nicht* gewählt: `#define Serial gConsole` in einem zentralen Header.
Ein Ein-Zeilen-Trick, aber eine versteckte Umdefinition eines globalen Symbols —
passt nicht zum Stil dieses Repos und wäre bei Compilerfehlern extrem
verwirrend.

### 4.2 Der Ringpuffer löst drei Probleme auf einmal

`Console::write()` fasst den Socket **nie** an. Es schreibt in einen statisch
allozierten Ringpuffer; `loop()` drainiert ihn in den Telnet-Client.

Das löst gleichzeitig:

1. **Cross-Task-Sicherheit** (Abschnitt 1.1) — Ausgaben vom NimBLE-Host-Task
   und vom async_tcp-Task landen im Puffer, nicht im Socket. Der Puffer wird
   unter einer kurzen kritischen Sektion beschrieben, lwIP wird ausschließlich
   vom loop-Task bedient.
2. **Backpressure ohne Watchdog-Risiko** — liest der Client nicht mehr (Laptop
   zugeklappt, halbtote Session), läuft das TCP-Fenster voll. Ein blockierendes
   `client.write()` würde den loop-Task anhalten; bei 45 s WDT ist das ein
   Reboot. Der Drainer prüft vor jedem Chunk mit einem **`select()` mit
   Timeout 0** auf dem Socket-Descriptor, ob überhaupt gesendet werden kann,
   und **verwirft** bei Überlauf die ältesten Daten, statt zu warten. Ein
   Drop-Zähler wird beim nächsten erfolgreichen Schreiben als
   `[… N Bytes verworfen …]` ausgegeben, damit stille Lücken erkennbar sind.

   > **Nicht `availableForWrite()` verwenden.** `WiFiClient` überschreibt die
   > Methode auf dem ESP32-Arduino-Core nicht, erbt also die Default-
   > Implementierung aus `Print`, die konstant **0** liefert. Ein Gate der Form
   > `if (availableForWrite() > 0)` sendet damit *nie* — das hat in der ersten
   > Fassung sämtliche Kommandoausgabe verschluckt (Banner, Echo und Prompt
   > waren weiterhin sichtbar, weil sie direkt geschrieben werden und nicht
   > durch den Ringpuffer laufen) und zugleich die Liveness-Probe aus E6b
   > stillgelegt. Zusätzlich wird der **Rückgabewert von `write()`** ausgewertet
   > und der Cursor über den nicht gesendeten Rest zurückgesetzt, damit ein
   > Teilschreibvorgang nichts verliert.
3. **Scrollback beim Login** — der Client bekommt beim Verbinden einen
   Lese-Index auf den ältesten noch gültigen Eintrag statt auf das Pufferende.
   Damit sieht man die letzten ~4 KB Ausgabe *vor* dem Login. Auf einem Gerät,
   das man genau deshalb übers Netz besucht, weil vorher etwas schiefging, ist
   das der Unterschied zwischen brauchbar und nutzlos.

Ein Mechanismus, drei Probleme — das ist der Grund, warum der Ringpuffer nicht
optionales Beiwerk ist (Entscheidung E5).

### 4.3 Eingabe: Zeilen- und IAC-Parser

Die Bytes vom Client durchlaufen einen Parser, der
- Telnet-`IAC`-Sequenzen ab `0xFF` verwirft (3-Byte `WILL/WONT/DO/DONT` und
  Subnegotiation `IAC SB … IAC SE`) — PuTTY sendet die beim Verbinden; ohne
  Filter landen sie im Zeilenpuffer und das erste Kommando ist Müll. `nc`
  sendet gar nichts, der Filter muss also tolerant sein;
- CR, LF, CRLF und **CR NUL** gleichermaßen als Zeilenende behandelt
  (Telnet-Clients senden CR NUL; `readStringUntil('\n')` + `trim()` deckt das
  heute nur zufällig ab);
- Backspace/DEL für das lokale Echo verarbeitet;
- die Zeilenlänge hart begrenzt (Schutz gegen einen Client, der nie `\n`
  sendet).

Diese Logik ist Arduino-frei und gehört nach der Repo-Konvention in einen
eigenen Header `src/net/telnet_line.h` mit Host-Tests unter
`test/test_telnet_line` — analog zu `serial_args.h` / `packet_parse.h`
(Entscheidung E10).

### 4.4 Kommando-Ausführung

Eine fertige Zeile geht direkt an `handleCommand()` — auf dem loop-Task, damit
unverändert konform zu `serial_console.h:8-14`. Keine Queue, kein zweiter Task.

Blockierende Kommandos (`scan`, `connect`, `refresh` mit anschließendem Reboot)
blockieren dann auch die Telnet-Session für ihre Dauer — genau wie bei der
seriellen Konsole. Der WDT wird in diesen Schleifen bereits gefüttert.

### 4.5 Login

```
token = hex( SHA-256( "casambi-api:" + <Casambi-Netzwerk-Passwort> ) )
```

Das Gerät leitet aus der eingetippten Eingabe denselben Wert ab und vergleicht
konstantzeitig gegen den gespeicherten Token. Die Bausteine existieren
vollständig: `webserver.cpp:250-267` (`_deriveApiToken`) und
`_constantTimeEquals`. Begründung siehe Entscheidung E1.

Ablauf: Verbindung → `IAC WILL ECHO` + `IAC WILL SGA` → Prompt `Password:`
(Echo bewusst unterdrückt) → 3 Versuche mit Verzögerung nach Fehlversuch →
danach Verbindung schließen. Nach erfolgreichem Login: Scrollback-Replay,
Banner mit Build-Nummer, dann Prompt.

### 4.6 Idle-Timeout und Liveness

Der Timeout hat zwei Zwecke: die Sitzung eines vergessenen Logins beenden und
— wichtiger — den **einzigen Session-Slot** (E6) freiräumen, wenn der Client
ohne FIN verschwindet (Laptop-Deckel zu, WLAN weg). Eine halboffene
TCP-Verbindung erkennt das Gerät sonst nie und man sperrt sich selbst aus.

Die beiden Zwecke werden **getrennt** gelöst, damit `timeout 0` gefahrlos ist:

**Timeout** = Zeit seit der letzten **vollständig empfangenen Kommandozeile**.
Diese Definition ist nicht beliebig:

- *Nicht* „seit dem letzten empfangenen Byte": PuTTY kann eigene Keepalives
  senden (Einstellung *Connection → Seconds between keepalives*, verschickt
  Telnet-NOPs, um NAT-Zustände offenzuhalten). Die würden den Timer dauerhaft
  zurücksetzen — der Mechanismus wäre wirkungslos, ohne dass es auffällt.
- *Nicht* „kein Verkehr in beide Richtungen": Dann hielte zwar ein
  Nacht-Mitschnitt sich selbst am Leben, aber eine 15-minütige Ruhephase des
  Geräts um 3 Uhr würde die Aufzeichnung mittendrin und unbemerkt abbrechen.
  Eingabe-basiert plus ein bewusst gesetztes `0` ist vorhersagbar.

**Liveness** = unabhängig vom Timeout. lwIP-Keepalive auf dem Socket
(`SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT` via `setsockopt`; ob in der
IDF-Konfiguration dieses Builds aktiv, ist bei der Umsetzung zu prüfen),
ersatzweise ein `IAC NOP` alle 60 s vom Gerät aus — für den Client unsichtbar,
läuft aber bei totem Peer in einen TCP-Fehler und gibt den Slot frei. Damit
bleibt der Slot auch bei abgeschaltetem Timeout selbstheilend.

Bedienung, im Stil der vorhandenen Kommandos (`debug …`, `wifi …`,
`ntp status`):

```
telnet status              # Timeout, aktive Session, verworfene Bytes
telnet timeout <sekunden>  # 0 = aus, sonst 60..86400
```

Persistiert als Feld in `NetworkConfig`, wie die Debug-Flags
(`network_config.h:181-186`, `config_store.cpp:101-104`). **Wichtig:** der neue
Wert muss in die Merge-Liste `network_config.h:266-271` aufgenommen werden,
sonst setzt der nächste Cloud-`refresh` ihn still zurück — dieselbe Fußangel,
die dort bereits für die sechs Debug-Flags bedacht wurde.

Auf der Laptop-Seite braucht es für lange Mitschnitte nichts Zusätzliches:
PuTTY → *Session → Logging → All session output* schreibt direkt in eine Datei.

## 5. Entscheidungen (die zuvor offenen Fragen)

| # | Frage | Entscheidung | Begründung |
|---|---|---|---|
| **E1** | Login mit Netzwerk-Passwort oder abgeleitetem Token? | **Token** (64 Hex-Zeichen, einmal im PuTTY-Session-Profil hinterlegt) | Das Casambi-Passwort ist ein **Cloud-Account-Credential** — damit kommt man an App und Cloud-API, weit über dieses Gerät hinaus. Der Token ist rein gerätelokal. Exakt dafür wurde `API_TOKEN_PREFIX` eingeführt (`config.h:187-195`). Kostet keine zusätzliche Zeile Firmware gegenüber der Passwort-Variante. Ehrlich dazu: der Token bleibt ein Bearer-Credential im Klartext und ist im LAN abgreifbar/replaybar — ein Mitleser hat dann aber nur, was er über die ohnehin offene REST-API auch hätte, **keinen Zugang zum Cloud-Konto**. |
| **E2** | SSH statt Telnet? | **verworfen** | Heap (3.1) und Lizenz (3.2) sind zwei unabhängige K.-o.-Kriterien, keins davon durch besseren Code umgehbar. |
| **E3** | Setup-Wizard über Telnet? | **gesperrt** — `setup` und `wifi set` antworten über Telnet mit „nur über die serielle Konsole verfügbar" | Erspart den kompletten Umbau der fünf blockierenden Eingabepfade (1.3). Kein realer Verlust: geflasht wird ohnehin per USB, die Erstprovisionierung läuft über das SoftAP-Portal. `wifi set` würde zusätzlich die eigene Session unter den Füßen wegreißen. Und beide geben Zugangsdaten aus — über Klartext-Telnet ein echtes Leck (`serial_console.cpp:744-748` maskiert `wifi set` heute nur im Kommando-Echo, nicht die Wizard-Ausgabe). |
| **E4** | Echo selbst negotiieren oder PuTTY-Einstellungen dokumentieren? | **selbst negotiieren** (`WILL ECHO`, `WILL SGA`), Gerät echot Zeichen inkl. Backspace | PuTTY steht per Default auf „Local echo/line editing: Auto"; ohne Negotiation tippt man zeichenweise und blind. Ausschlaggebend ist aber die Passworteingabe: Echo gezielt *nicht* zu senden geht nur, wenn das Gerät die Echo-Hoheit hat. Die PuTTY-Doku-Variante („Local echo auf Force on") kommt zusätzlich ins README als Fallback für Clients, die nicht negotiieren. |
| **E5** | Scrollback-Ringpuffer ja/nein, wie groß? | **ja, 4 KB, statisch alloziert** | Kein optionales Beiwerk, sondern der Mechanismus, der auch Cross-Task-Sicherheit und Backpressure löst (4.2). **Statisch** ist der Punkt: statisches RAM ist reichlich frei (59 572 B von 532 480 B belegt), der Heap ist die knappe Ressource — ein `static uint8_t[4096]` kostet dort **nichts**. |
| **E6** | Mehrere gleichzeitige Sessions? | **eine**, weitere Verbindungen werden mit Hinweis abgewiesen | Sonst konkurrieren zwei Bediener um blockierende Kommandos wie `connect`. Session wird bei Reboot-Kommandos geschlossen. |
| **E6a** | Idle-Timeout fest oder konfigurierbar? | **konfigurierbar und abschaltbar**: `telnet timeout <s>`, Default 900, `0` = aus; persistiert in `NetworkConfig` | Lange Mitschnitte über Nacht sind ein reales Szenario, bei dem stundenlang nichts getippt wird. Gemessen wird die Zeit seit der letzten **Kommandozeile**, nicht seit dem letzten Byte (PuTTY-Keepalives würden den Timer sonst dauerhaft zurücksetzen) und nicht „kein Verkehr in beide Richtungen" (eine Ruhephase des Geräts würde den Mitschnitt still abbrechen). Details und Begründung in 4.6. |
| **E6b** | Was ersetzt den Timeout bei `0`? | **Keepalive/`IAC NOP` als eigenständige Liveness-Prüfung** | Der Timeout räumt auch den einzigen Session-Slot frei, wenn der Client ohne FIN verschwindet. Ohne Ersatz würde `timeout 0` bedeuten, dass ein zugeklappter Laptop das Gerät dauerhaft blockiert. Getrennt gelöst ⇒ `timeout 0` ist gefahrlos. |
| **E7** | Verhalten bei totem Client | **nie blockieren**, ältestes verwerfen, Drop-Zähler ausgeben | Ein blockierendes `client.write()` im loop-Task ist bei 45 s WDT ein Reboot — das einzige Szenario, in dem diese Erweiterung die Gerätestabilität gefährdet. |
| **E8** | Port und Aktivierung | **Port 23**, Server startet **nur, wenn ein Token vorhanden ist** | Port 23 ist PuTTYs Telnet-Default. Die Kopplung an den Token ist eine Sicherheitsentscheidung: `webserver.cpp:281` behandelt „kein Passwort gespeichert" als *Auth aus* — für die REST-API vertretbar, für eine Konsole mit `clearconfig`/`restart` **nicht**. Ohne Token bleibt der Port also zu, auch im Setup-Modus. |
| **E9** | API-Versionierung | **kein Bump** von `FHEM_API_VERSION_MAJOR/MINOR` | Telnet ist kein REST-/WebSocket-Wire-Format; die Regel in CLAUDE.md und `config.h` greift nicht. README-Abschnitt + Eintrag in der Feature-Übersicht sind trotzdem fällig. |
| **E10** | Wo lebt die Parser-Logik? | eigener Header `src/net/telnet_line.h` + `test/test_telnet_line` | Repo-Konvention: Arduino-freie Logik wird host-testbar ausgelagert (CLAUDE.md, „Conventions"). IAC-Filter und Zeilenzerlegung sind genau das. |
| **E11** | Challenge-Response statt Klartext-Token? | **zurückgestellt** | Nonce + `HMAC(token, nonce)` wäre nicht replaybar und kryptografisch solide — aber in PuTTY nicht von Hand tippbar. Man bräuchte ein Helferskript auf dem Laptop und verlöre damit das Standardtool, also den ganzen Zweck. Als spätere Ausbaustufe notiert, falls das Bedrohungsmodell sich ändert. |
| **E12** | mDNS-Service `_telnet._tcp` registrieren? | **nein** | Zu trennen: die *Namensauflösung* `casambi-xxxx.local` kommt aus `MDNS.begin()` (`time_sync.cpp:38`), existiert bereits und ist alles, was PuTTY braucht. Eine *Service-Ankündigung* (PTR/SRV/TXT, wie `addService("http","tcp",80)` in Zeile 40-43) macht das Gerät zusätzlich in Dienst-Browsern (`avahi-browse -a`, Bonjour Browser, Finder) auffindbar. PuTTY durchsucht kein mDNS — der praktische Gewinn ist null, während der Eintrag jedem LAN-Gerät aktiv mitteilt, dass hier eine administrative Konsole auf Port 23 lauscht. `setup_portal.cpp:233` zeigt denselben Gedanken: bei offenem Portal werden Hostname/MAC/IP bewusst nicht ausgegeben. **Mittelweg**, falls Auffindbarkeit später doch gewünscht ist: TXT-Record `telnet=23` am bestehenden HTTP-Service statt eines eigenen Eintrags. |

## 6. Aufwand und Risiken

| Posten | Umfang |
|---|---|
| Neues Modul `net/telnet_console.*` | ~250–350 Zeilen |
| Parser-Header + Host-Tests | ~80 + ~120 Zeilen |
| Mechanischer Rename `Serial.print` → `Console.print` | 662 Zeilen in 16 Dateien, reiner Rename |
| Einhängen in `loop()`, Kommandos `telnet status` / `telnet timeout` | wenige Zeilen |
| Timeout-Feld in `NetworkConfig` + `config_store` + Merge-Liste (E6a) | wenige Zeilen, leicht zu vergessen |
| mDNS | keine Änderung (E12) |
| README-Abschnitt (Bedienung, PuTTY-Einstellungen, Token-Ermittlung) | — |
| **Heap zur Laufzeit** | Listen-Socket + 1 Client ≈ wenige KB lwIP; Ringpuffer 0 (statisch) |
| **Flash** | unkritisch (1,48 MiB frei im `huge_app`-Layout) |

**Hauptrisiko:** blockierende Writes auf einen toten Client → Watchdog-Reboot.
Durch E7 adressiert, muss aber gezielt getestet werden (Session öffnen, Laptop
in den Ruhezustand, Gerät unter Ausgabelast beobachten).

**Zweitrisiko:** Der Rename berührt jede Datei mit Ausgaben. Er sollte als
**eigener Commit vor** der Funktionalität laufen, damit der Feature-Diff
lesbar bleibt.

**Nicht adressiert:** Fällt das WLAN aus, ist die Konsole weg — also
ausgerechnet bei WLAN-Problemen. Fallback bleibt USB, was in dieser
Konfiguration ohnehin für das Flashen nötig ist. Der persistente Event-Log
(`log/event_log.*`, Kommando `log 2`) überlebt Reboots und deckt die grobe
Nachschau ab.

## 7. Testplan (nicht ausgeführt)

1. **Host-Tests** — `pio test -e native -f test_telnet_line`: IAC-Filter
   (3-Byte und Subnegotiation), CR/LF/CRLF/CR-NUL, Backspace, Überlauf der
   maximalen Zeilenlänge.
2. **Client-Matrix** — PuTTY (Telnet **und** Raw), Linux/macOS `telnet`, `nc`.
   Jeweils: Login-Prompt lesbar, Passwort nicht sichtbar, erstes Kommando
   unverfälscht.
3. **Auth** — falscher Token 3× ⇒ Verbindung geschlossen; kein Token
   gespeichert ⇒ Port 23 geschlossen (`nc -vz`).
4. **Backpressure** — Session öffnen, Client-Prozess `SIGSTOP`en, Gerät unter
   Ausgabelast (`debug ble on` + BLE-Aktivität): kein Reboot, Drop-Zähler
   erscheint nach dem Fortsetzen.
5. **Sperren** — `setup` und `wifi set` über Telnet ⇒ Hinweis, keine Ausführung;
   dieselben Kommandos seriell ⇒ unverändert funktionsfähig.
6. **Idle-Timeout** — mit kurzem Testwert (`telnet timeout 60`): Session fällt
   ohne Eingabe weg; mit aktivierten PuTTY-Keepalives fällt sie **trotzdem**
   weg (beweist, dass auf Kommandozeilen und nicht auf Bytes gemessen wird);
   bei laufender Ausgabe fällt sie ebenfalls weg. `telnet timeout 0` ⇒ Session
   überlebt eine Nacht. Wert überlebt Reboot **und** einen `refresh`.
7. **Session-Slot** — Client hart abschießen (Netzwerkkabel/WLAN aus, kein FIN)
   bei `timeout 0`: Slot wird durch Keepalive/NOP wieder frei, neue Verbindung
   ist möglich.
8. **Regression seriell** — kompletter Kommando-Durchlauf über `/dev/ttyUSB0`
   nach dem Rename (Verfahren siehe CLAUDE.md, „Serial monitor").
9. **Stabilität** — `scripts/stress_test.py` mit offener Telnet-Session, um
   Wechselwirkung mit async_tcp und Heap zu prüfen.
