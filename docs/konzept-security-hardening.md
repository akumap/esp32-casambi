# Konzept: Security-Hardening (Issue #11)

Status: Konzept (noch nicht umgesetzt).
Branch: `claude/issue-11-concept-scgdix`
Bezug: Issue #11 „Harden device from security point of view" inkl. Security-Review
(14 Findings) im Issue-Kommentar.

## 1. Ziel und Schutzziele

Die Firmware wurde ursprünglich für den **DMZ-/Bastel-Betrieb** entworfen: alle
Schnittstellen offen, keine Authentifizierung, sensible Daten im Klartext im
Flash. Issue #11 verlangt, das Gerät für den **normalen Heimnetz-Betrieb** zu
härten.

Der **zentrale Baustein** dieses Konzepts ist die **Authentifizierung der
Web-Schnittstelle (REST + WebSocket)**. Als Geheimnis bietet sich das
**Casambi-Cloud-/Netzwerk-Passwort** an, das nach dem Provisioning ohnehin auf
dem ESP32 liegt (`NetworkConfig::casambiPassword`, persistiert in
`casambi_config.json`, `config_store.cpp:54/188`). Dadurch ist **kein neues,
separat zu verwaltendes Passwort** nötig.

Schutzziele (realistisch für ein LAN-Gerät ohne TLS):

| Ziel | Im Scope | Begründung |
|---|---|---|
| **Zugriffskontrolle** — kein Steuern/Auslesen durch beliebige LAN-Geräte und Browser-JS | ✅ Kern | Hauptangriffsfläche nach „DMZ → Heimnetz" |
| **Sensible Daten at-rest** (WLAN-PW, AES-Keys, Casambi-PW) | ✅ | Flash-Auslesen bei physischem Zugriff |
| **Robustheit** (kein unauth. Reboot/DoS, keine Leaks/Crashes) | ✅ | bereits im Review als High markiert |
| **Schutz gegen passiven L2-Sniffer** (Mitlesen des Klartext-LAN) | ⚠️ nur teilweise | echte Abwehr braucht TLS — auf dem ESP32 schwer; siehe 3.4 |
| **Vertraulichkeit gegenüber LAN-fähiger Malware mit Root** | ❌ | außerhalb des realistischen Schutzumfangs |

## 2. Ausgangslage / Befunde (gruppiert)

Der Security-Review listet 14 Findings. Gruppiert nach Thema und mit
aktuellem Code-Stand abgeglichen:

### 2.1 Fehlende Web-Authentifizierung (Kern)
- **#1 [Critical]** Keine Auth auf dem REST-API; CORS-Wildcard
  `Access-Control-Allow-Origin: *` (`webserver.cpp:271`) erlaubt zusätzlich
  beliebigem Browser-JS Zugriff.
- **#9 [Medium]** WebSocket `/ws` unauthentifiziert — sendet beim Connect den
  vollständigen Unit-Snapshot (`_buildHelloMessage`, `webserver.cpp:124`).
- **#2 [High]** `POST /api/reboot` unauthentifiziert **und** mit blockierendem
  `delay(1000)` im Async-Callback (`webserver.cpp:319-323`).
- **#12 [Low]** `/api/status` gibt Netzwerktopologie (SSID, IP, RSSI,
  Gateway-MAC, Heap) ohne Auth preis (`webserver.cpp:476`).

→ Behandelt in **Abschnitt 3**.

### 2.2 Daten at-rest
- **#4 [High]** WLAN-Passwort (`wifi_config.json`) und AES-Keys/Casambi-PW
  (`casambi_config.json`) liegen als **Klartext-JSON** im LittleFS
  (`config_store.cpp`).

→ Behandelt in **Abschnitt 4**.

### 2.3 BLE-/Krypto-Implementierung
- **#5 [High]** Transport-Key per **XOR-Fold** aus SHA-256
  (`key_exchange.cpp:212`). ⚠️ **Protokollzwang prüfen**: Die Ableitung bildet
  das reverse-engineerte Casambi-Protokoll nach (Kommentar „Python code reverses
  the bytes", `key_exchange.cpp:196`). Ein Ändern würde die Interoperabilität
  brechen. → wahrscheinlich **kein echtes Finding**, sondern Protokolltreue;
  vor jeder Änderung gegen reale Hardware verifizieren.
- **#6 [High]** CMAC-Vergleich nicht constant-time — Early-Exit-Schleife
  (`encryption.cpp:76-81`). Echtes, billig zu behebendes Finding.
- **#10 [Medium]** AES-CTR-Counter-Reset bei Reconnect → Nonce-Reuse-Risiko bei
  identischem Device-Nonce.
- **#13 [Low]** ECDH Shared Secret + SHA-Hash nicht aus dem Stack genullt
  (`key_exchange.cpp:192-208`).
- **#14 [Low]** `_leftShift`/CMAC nicht gegen RFC-4493-Testvektoren validiert.

→ Behandelt in **Abschnitt 5**.

### 2.4 Logging / Eingabevalidierung
- **#7 [Medium]** `X-Forwarded-For` wird in `_getClientIP()` blind vertraut
  (`webserver.cpp:1409-1414`) → IP-Spoofing in Logs.
- **#8 [Medium]** BLE-MAC unsanitisiert im `fhem()`-Define
  (`98_CasambiGW.pm:690`) → mögliche Command-Injection.
- **#11 [Medium]** WiFi-Passwort wird über blankes Command-Echo auf Serial
  geloggt (`main.cpp`).

→ Behandelt in **Abschnitt 6**.

### 2.5 Bereits behoben
- **#3 [High]** `_tempObject`-Leak bei abgebrochenem POST: Im aktuellen Code
  registriert `onRequestBody` einen `onDisconnect`-Handler, der `_tempObject`
  freigibt (`webserver.cpp:432-437`). **Gilt als erledigt** — nur noch als
  Regressions-Checkpunkt führen.

## 3. Kernmaßnahme: Web-Authentifizierung mit dem Casambi-Passwort

### 3.1 Geheimnis-Quelle und Geltungsbereich
- Geheimnis = `_config->casambiPassword`. Auth wird **nur erzwungen, wenn ein
  Passwort gesetzt ist** (`casambiPassword.length() > 0`). Für Altkonfigurationen
  ohne gespeichertes PW (Feld defaultet auf `""`, `config_store.cpp:188`) bleibt
  das Gerät offen, bis einmal `refreshCasambi`/Neu-Provisioning das PW ablegt —
  so bricht das Update **keine** bestehende Installation.
- Das **Setup-Portal** (Erstinbetriebnahme, noch kein PW vorhanden) bleibt
  notwendigerweise **offen**; Härtung dort über kurze Lebensdauer/Reichweite
  (offener SoftAP, siehe `konzept-provisionierung.md`), nicht über ein Passwort.

### 3.2 Abgeleitetes Token statt Klartext-Passwort
Das **rohe Casambi-Cloud-Passwort gehört nicht auf die Leitung** und nicht im
Klartext in die FHEM-Konfiguration — sonst könnte ein Mitleser sich damit auch
gegen die **Casambi-Cloud** authentifizieren. Stattdessen ein **abgeleitetes
API-Token**:

```
apiToken = hex( SHA-256( "casambi-api:" || casambiPassword ) )
```

- Der ESP berechnet das Token einmalig beim Laden der Config (mbedTLS SHA-256 ist
  bereits eingebunden, vgl. `key_exchange.cpp`).
- FHEM berechnet dasselbe Token aus dem im Define/Attribut hinterlegten
  Passwort (Perl `Digest::SHA`). Damit ist das eigentliche Cloud-PW nie auf der
  Leitung und nicht direkt aus dem FHEM-Klartext-Token rückrechenbar.
- Vergleich auf dem ESP **constant-time** (gleiches Muster wie #6, Abschnitt 5).

### 3.3 Transport der Auth
- **REST:** Header `X-API-Key: <apiToken>` vor jedem geschützten Handler prüfen.
  Zentral umsetzen über eine kleine Helfermethode `_authOk(request)`, aufgerufen
  am Anfang jedes Handlers (oder via Filter), statt 20× dupliziert.
- **WebSocket:** Token im Handshake mitgeben. Da FHEM den Handshake selbst baut
  (`98_CasambiGW.pm:282-288`), zwei gangbare Wege:
  1. zusätzlicher Header `X-API-Key: <token>` in der `GET /ws`-Anfrage, im
     `WS_EVT_CONNECT`/Handshake-Pfad geprüft, sonst Verbindung verwerfen, **oder**
  2. Token als Query-Param `GET /ws?k=<token>`.
  Variante 1 ist sauberer (kein Token in Logs/URLs); beide sind FHEM-seitig eine
  Einzeiler-Ergänzung.
- **CORS:** Wildcard entfernen. Entweder gar keinen `Access-Control-Allow-Origin`
  setzen (FHEM/HttpUtils braucht kein CORS — das ist nur für Browser relevant)
  oder auf eine konfigurierbare Origin einschränken. Damit entfällt der
  Browser-JS-Zugriffsweg aus #1.

### 3.4 Grenzen (ehrlich benannt)
Ohne TLS ist das Token ein **statisches Geheimnis im Klartext-LAN** und damit
durch einen **passiven Sniffer im selben L2-Segment** mitlesbar/replaybar
(genau wie die Steuerbefehle selbst). Das ist eine bewusste Abwägung:

- Das Token **stoppt** unauthentifizierte LAN-Geräte und Browser-JS (die
  benannte Hauptangriffsfläche „DMZ → Heimnetz").
- Es **schützt nicht** gegen einen Angreifer, der den LAN-Verkehr bereits
  mitlesen kann — dagegen hülfe nur TLS.
- **Stärkere Alternative (optional):** HTTP **Digest Auth**
  (Challenge-Response, Passwort/Token nie im Klartext, kein simpler Replay).
  ESPAsyncWebServer unterstützt Digest grundsätzlich. Nachteil: FHEM nutzt für
  den WebSocket einen **handgebauten** Handshake (DevIo) und für REST
  `HttpUtils_NonblockingGet` — Digest müsste dort nachgezogen werden (Challenge
  holen, Response berechnen). Empfehlung: **mit dem X-API-Key-Token starten**
  (klein, FHEM-kompatibel, deckt die Hauptbedrohung); Digest/TLS als spätere,
  separate Ausbaustufe bewerten.

### 3.5 Folgen für FHEM (`98_CasambiGW.pm`)
- Neues Attribut/Define-Parameter für das Casambi-Passwort (oder direkt das
  vorberechnete Token), z. B. `attr <gw> casambiPassword <pw>`.
- Token einmal aus dem PW ableiten (`Digest::SHA::sha256_hex`).
- Token-Header in **drei** Aufrufstellen ergänzen:
  - `CasambiGW_SendCommand` (`:733` `header => ...`),
  - `refreshCasambi`-POST (`:245`),
  - WebSocket-Handshake (`:282`).
- `/api/info` ggf. **auth-frei** belassen (Discovery: FHEM unterscheidet damit
  „configured" vs. „setup", `:126`), aber den Informationsgehalt reduzieren
  (z. B. nur `{configured, build}` ohne MAC/IP/Netzname). Das adressiert #12
  teilweise auch für den Discovery-Pfad.

### 3.6 Reboot-Endpoint (#2)
Unabhängig von der Auth zwei Korrekturen am `/api/reboot`-Handler
(`webserver.cpp:319`):
1. Hinter die Auth aus 3.3 stellen (kein unauth. DoS mehr).
2. `delay(1000)` aus dem Async-Callback entfernen: stattdessen ein
   `_rebootRequested`-Flag setzen und den Neustart in `loop()` ausführen — genau
   das Muster, das `refreshCasambi` über `consumeRefreshRequest()` bereits nutzt
   (`webserver.cpp:329-340`, `:93`).

## 4. Daten at-rest (#4)

WLAN-PW, AES-Keys und Casambi-PW liegen im Klartext im LittleFS. Optionen:

| Option | Aufwand | Schutz | Bemerkung |
|---|---|---|---|
| **A. ESP32 Flash Encryption** (`CONFIG_FLASH_ENCRYPTION_ENABLED`) | mittel | hoch (gesamtes Flash) | transparent für den Code; einmalige eFuse-Aktivierung, **irreversibel**; Partition/Boot-Aspekte beachten |
| **B. NVS mit Encryption** statt LittleFS-JSON für die Secrets | höher | hoch | Code-Umbau von `config_store` für die sensiblen Felder |
| **C. Status quo + Dokumentation** | gering | keine | nur ehrlich dokumentieren, dass physischer Zugriff = Kompromittierung |

**Empfehlung:** Option **A** als *empfohlene Produktionskonfiguration*
**dokumentieren** (README + `platformio.ini`-Hinweis) statt sie hart zu
erzwingen — Flash Encryption ist irreversibel und für viele Bastel-Setups
unerwünscht. Wer Sicherheit braucht, aktiviert sie; Default bleibt unverändert,
damit bestehende Flash-Workflows nicht brechen. Option B nur, falls eine
selektive Verschlüsselung ohne globale Flash-Encryption gewünscht ist.

## 5. BLE-/Krypto-Härtung

| Finding | Maßnahme | Aufwand | Risiko |
|---|---|---|---|
| **#6** constant-time CMAC-Vergleich | OR-Akkumulator statt Early-Exit (`diff |= a[i]^b[i]`) in `decryptAndVerify` (`encryption.cpp:76`) | trivial | keins |
| **#13** Secret-Zeroization | `memset(secret_bytes,0,…)` + `memset(hash,0,…)` vor `return` in `deriveTransportKey` (`key_exchange.cpp`) | trivial | keins |
| **#14** RFC-4493-Testvektoren | Selbsttest für CMAC-Subkeys/MAC gegen RFC-4493 App. D (Serial-Diagnose oder Build-Check) | gering | keins (nur Verifikation) |
| **#10** Nonce-Reuse bei Reconnect | Vor `Authenticated` prüfen, dass der Device-Nonce sich von der Vorsession unterscheidet (`casambi_client.cpp`) | gering–mittel | hardwareabhängig testen |
| **#5** XOR-Fold Transport-Key | **Zuerst klären, ob Protokollzwang** (sehr wahrscheinlich ja). Falls ja: **nicht ändern**, nur kommentieren. Nur falls nein: HKDF/erste 16 Byte. | n/a | **hoch bei Änderung** (Interop) |

**#6, #13, #14** sind „billige" echte Verbesserungen und sollten zuerst kommen.
**#5** ist mit hoher Wahrscheinlichkeit ein **False Positive** (Protokolltreue)
und darf nicht ohne Hardware-Verifikation angefasst werden.

## 6. Logging / Eingabevalidierung

- **#7** `_getClientIP()` (`webserver.cpp:1409`): `X-Forwarded-For` nur
  akzeptieren, wenn die Peer-IP einer konfigurierten Proxy-IP entspricht; sonst
  `request->client()->remoteIP()` verwenden. Für das typische Setup (kein Proxy)
  am einfachsten den Header **ganz ignorieren**.
- **#8** `98_CasambiGW.pm:690`: `$mac` vor dem `fhem("define …")` gegen
  `/^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/i` validieren; bei Nichtübereinstimmung
  Define ablehnen und loggen.
- **#11** `main.cpp` Serial-Command-Echo: sensible Befehle vor dem Logging
  erkennen und Argumente maskieren, z. B. Echo unterdrücken/kürzen, wenn das
  Kommando mit `wifi set` (oder anderen PW-tragenden Befehlen) beginnt.

## 7. Priorisierung / Umsetzungsreihenfolge

Gestaffelt, größter Nutzen zuerst, jede Stufe einzeln testbar:

1. **Web-Auth-Fundament** (#1/#9/#12, Kern):
   Token-Ableitung + `_authOk()` + `X-API-Key` an allen REST-Handlern +
   WS-Handshake-Prüfung; CORS-Wildcard entfernen. FHEM-Modul parallel anpassen.
2. **Reboot fix** (#2): Auth + Flag-in-`loop()` statt `delay()`.
3. **Billige Krypto-Härtung** (#6, #13, #14): constant-time, Zeroization,
   Testvektoren.
4. **Logging/Validierung** (#7, #8, #11).
5. **Daten at-rest** (#4): Flash-Encryption **dokumentieren** (Option A).
6. **Nonce-Reuse** (#10): mit Hardware verifizieren.
7. **#5** nur nach Hardware-Verifikation des Protokollzwangs (sonst belassen +
   kommentieren).

## 8. Aufwands- und Risiko-Einschätzung

| Bereich | Aufwand | Risiko | Anmerkung |
|---|---|---|---|
| Web-Auth (ESP + FHEM) | mittel | niedrig | Standard-Header-Prüfung; gut isoliert testbar (curl mit/ohne Token) |
| Reboot-Flag | gering | niedrig | bewährtes `consumeRefreshRequest`-Muster |
| CMAC/Zeroization/Testvektoren | gering | sehr niedrig | lokale Krypto, gegen Vektoren prüfbar |
| Logging/Validierung | gering | sehr niedrig | rein defensiv |
| Flash-Encryption-Doku | gering | niedrig | nur Doku; keine Code-Funktionsänderung |
| Nonce-Reuse | mittel | mittel | nur am echten Casambi-Netz verifizierbar |
| XOR-Fold (#5) | — | **hoch** | nicht ohne Interop-Beweis ändern |

**Kompatibilität:** Auth wird nur bei vorhandenem Casambi-PW erzwungen →
Bestandsgeräte ohne gespeichertes PW bleiben funktionsfähig; das FHEM-Update
muss das Token-Attribut setzen, sonst antwortet der ESP nach dem Härten mit
`401`. Diese Migration ist in README + FHEM-Doku zu beschreiben.

## 9. Offene Punkte / zu verifizieren

1. **#5 Protokollzwang:** Ist der XOR-Fold zwingend für die Casambi-Auth? (Sehr
   wahrscheinlich ja — vor jeder Änderung an realer Hardware bestätigen.)
2. **ESPAsyncWebServer-Auth-API:** Verfügbarkeit/Verhalten von Header-Prüfung im
   WS-Handshake bzw. `request->authenticate()` (für die Digest-Option) auf der
   eingesetzten Lib-Version prüfen.
3. **FHEM Digest-Fähigkeit:** Nur relevant, falls Digest statt X-API-Key gewählt
   wird (HttpUtils + handgebauter WS-Handshake).
4. **`/api/info` Discovery vs. Leak:** minimaler auth-freier Umfang, der FHEM
   noch reicht, ohne Topologie preiszugeben.
5. **Flash-Encryption-Boot-Auswirkungen** (Partition `huge_app.csv`,
   `platformio.ini`) auf der Zielhardware testen.
