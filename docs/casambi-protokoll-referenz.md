# Casambi-Protokollreferenz (Cloud + Bluetooth)

Status: **Referenzdokument**, abgeleitet aus der Reverse-Engineering-Bibliothek
[lkempf/casambi-bt](https://github.com/lkempf/casambi-bt) (Stand: Analyse
2026-07). **Teil A–C** beschreiben das Casambi-Protokoll so, wie casambi-bt es
implementiert (die unabhängige Referenz). **Teil D** stellt die Umsetzung in
diesem Repository (esp32-casambi) daneben und macht alle Abweichungen kenntlich.

> **Hinweis zur Verlässlichkeit.** Das Protokoll ist nicht öffentlich
> dokumentiert; alle Angaben stammen aus Reverse Engineering und können lückenhaft
> oder in Randfällen falsch sein. Wo casambi-bt selbst Unsicherheit markiert
> (`TODO`, „unknown", „arbitrary"), ist das hier vermerkt. Die konkreten
> Abweichungen der ESP32-Firmware gegenüber casambi-bt sind in **Teil D**
> gesammelt.

Das Casambi-System besteht aus **zwei getrennten Protokollen**:

1. **Cloud (HTTPS/REST)** — einmalige bzw. gelegentliche Beschaffung der
   Netzwerkdefinition: Schlüssel, Einheiten, Gruppen, Szenen und vor allem die
   **Fähigkeits-Deskriptoren** (Unit-Typen mit bit-genauen Controls).
2. **Bluetooth (BLE/GATT)** — der eigentliche Laufzeit-Kanal: Verbindung,
   Schlüsselaustausch, Authentisierung, verschlüsselte Steuerbefehle und
   Zustands-Broadcasts.

Beide hängen zusammen: Die Cloud liefert die **Schlüssel** (für die BLE-Auth) und
die **Bit-Layouts** (zum Dekodieren der BLE-Zustandsbytes). Der BLE-Kanal
überträgt selbst **keine** Fähigkeits-Metadaten — er sendet nur rohe
Zustandsbytes, deren Bedeutung ausschließlich aus der Cloud-Definition kommt.

---

## Teil A — Cloud-Protokoll (HTTPS/REST)

Basis-Host: `https://api.casambi.com`. Alle Antworten sind JSON.
Quelle: `_network.py`, `_keystore.py`, `_unit.py`.

### A.1 Ablauf im Überblick

```
UUID (BLE-Advertisement)
   │
   ├─(1) GET  /network/uuid/{uuid}            → interne Netzwerk-ID
   │
   ├─(2) POST /network/{id}/session           → Session-Token (mit Passwort)
   │
   ├─(3) PUT  /network/{id}/                  → komplette Netzwerkdefinition
   │            (Header X-Casambi-Session)       (units, groups, scenes, keyStore)
   │
   └─(4) GET  /fixture/{typeId}   je Unit-Typ → Fähigkeits-Deskriptor (controls)
```

### A.2 Schritt 1 — Netzwerk-ID auflösen

```
GET https://api.casambi.com/network/uuid/{uuid}
```

- `{uuid}` ist die global eindeutige Netzwerk-UUID (kommt aus dem BLE-Scan des
  Gateways, siehe B.1).
- Antwort: `{ "id": "<interne-id>" }`. Diese `id` wird für alle folgenden Aufrufe
  benutzt und lokal zwischengespeichert (Datei `networkid`).
- Statuscodes: `404` → Netz unbekannt (`NetworkNotFoundError`); alles außer `200`
  → Fehler.

### A.3 Schritt 2 — Login / Session

```
POST https://api.casambi.com/network/{id}/session
Content-Type: application/json

{ "password": "<netzwerk-passwort>", "deviceName": "Casambi BT Python" }
```

- `deviceName` ist ein frei wählbarer Client-Name (Konstante `DEVICE_NAME`).
- Antwort (`200`) enthält u. a.:
  - `session` — Session-Token (String)
  - `network` — Netzwerk-ID
  - `manager` — bool
  - `keyID` — int
  - `expires` — Ablaufzeitpunkt in **Millisekunden** seit Epoch
  - `role` — Rolle (Default 3)
- Das Token wird lokal persistiert und wiederverwendet, solange `expires` nicht
  überschritten ist. Andere Statuscodes → `AuthenticationError`.

### A.4 Schritt 3 — Netzwerkdefinition abrufen

```
PUT https://api.casambi.com/network/{id}/
X-Casambi-Session: <session-token>
Content-Type: application/json

{ "formatVersion": 1, "deviceName": "Casambi BT Python", "revision": <lokale-revision> }
```

- **Sicherheitshinweis (aus der Quelle):** Der Session-Header darf **nicht** als
  globaler Client-Header gesetzt werden — sonst leckt das Token an andere Hosts.
  Er gehört ausschließlich an diesen einen Request.
- `revision` ist die lokal bekannte Revision (0, falls kein Cache). Der Server
  antwortet mit `status: "UPTODATE"`, wenn nichts Neues vorliegt, sonst mit der
  vollständigen Definition und einer neuen `network.revision`.
- `410 GONE` → Netzwerk existiert nicht mehr (z. B. Passwortänderung) → Cache
  verwerfen.

**Struktur der Antwort** (`network`-Objekt, relevante Felder):

| Pfad | Bedeutung |
|---|---|
| `network.name` | Netzwerkname |
| `network.revision` | Revisionszähler (monoton) |
| `network.protocolVersion` | BLE-Protokollversion (10 oder 11) |
| `network.keyStore.keys[]` | Auth-Schlüssel (siehe A.6) |
| `network.units[]` | Einheiten (siehe A.5) |
| `network.grid.cells[]` | Gruppen (Zellen, Typ 2) |
| `network.scenes[]` | Szenen (`sceneID`, `name`) |

Einheiten (`units[]`):

| Feld | Bedeutung |
|---|---|
| `deviceID` | ID der Einheit **innerhalb** des Netzwerks (adressiert per BLE) |
| `uuid` | global eindeutige Geräte-ID |
| `address` | MAC-Adresse der Einheit |
| `name` | Anzeigename |
| `firmware` | Firmware-Version |
| `type` | **Unit-Typ-ID** → separat via `/fixture/{type}` aufzulösen (A.7) |

Gruppen (`grid.cells[]`): nur Zellen mit `type == 2` sind Gruppen der obersten
Ebene. Deren Untereinträge (`cells[]`) mit `type == 1` referenzieren über `unit`
die Mitglieds-`deviceID`s. Verschachtelte Gruppen werden nicht unterstützt.

### A.5 Schlüssel (`keyStore.keys[]`)

Fehlt `keyStore`, handelt es sich vermutlich um ein **Classic-Netzwerk** ohne
Schlüssel (BLE-Auth wird dann übersprungen, siehe B.5).

Ein Schlüssel-Eintrag (`Key`, aus `_keystore.py`):

| Feld | Wertebereich | Bedeutung |
|---|---|---|
| `id` | ≥ 0 | Schlüssel-ID (geht in das Auth-Paket, B.6) |
| `type` | 0–255 | Schlüsseltyp |
| `role` | 0–3 | Berechtigungsstufe |
| `name` | String | Anzeigename |
| `key` | Hex → Bytes | eigentliches Schlüsselmaterial |

**Schlüsselwahl:** Für die Authentisierung wird der Schlüssel mit der **höchsten
`role`** verwendet (`KeyStore.getKey()`).

### A.6 Schritt 4 — Unit-Typ / Fähigkeiten (`/fixture/{id}`)

```
GET https://api.casambi.com/fixture/{typeId}
```

Der Kern für die **bit-genaue Zustands-Dekodierung**. Antwort (`UnitType`):

| Feld | Bedeutung |
|---|---|
| `id` | Typ-ID |
| `model` | Modellname |
| `vendor` | Hersteller |
| `mode` | Modus-String |
| `stateLength` | Länge des Zustands-Blobs in Bytes |
| `controls[]` | Liste der **Controls** (Fähigkeiten) |

Ein `controls[]`-Eintrag → `UnitControl`:

| Feld | Bedeutung |
|---|---|
| `type` | Control-Typ (String, Groß-/Kleinschreibung normalisiert) |
| `offset` | **Bit-Offset** im Zustands-Blob |
| `length` | **Bit-Länge** des Feldes |
| `default` | Default-Wert |
| `readonly` | bool |
| `min` / `max` | optionale Grenzen (v. a. für Temperatur) |

Unit-Typen werden lokal gecacht (Einträge mit Ablaufdatum: 28 Tage bei Erfolg,
7 Tage bei Fehler).

**Control-Typen** (`UnitControlType`, aus `_unit.py` — die Zahlenwerte sind laut
Quelle „totally arbitrary", d. h. nur bibliotheksintern):

| Name | Bedeutung |
|---|---|
| `DIMMER` | Helligkeit |
| `WHITE` | Weißanteil |
| `RGB` | Farbe (Hue/Saturation-kodiert) |
| `ONOFF` | An/Aus |
| `TEMPERATURE` | Farbtemperatur (linear zwischen `min`/`max`) |
| `VERTICAL` | vertikale Verteilung (Up/Down-Light) |
| `COLORSOURCE` | Farbquelle umschalten (TW/RGB/XY) |
| `XY` | Farbe im CIE-Farbraum |
| `SLIDER` | generischer Regler |
| `SENSOR` | Sensorwert |
| `UNKOWN` | nicht implementiert (nur zu Debugzwecken gespeichert) |

Die **Existenz** eines Controls (z. B. `VERTICAL`) ist die maßgebliche Aussage
darüber, ob eine Einheit diese Fähigkeit besitzt — **nicht** die Anzahl der
Zustandsbytes. Diese Trennung ist der zentrale Unterschied zur bisherigen
Kanalzahl-Heuristik der Firmware (siehe Issue #34).

---

## Teil B — Bluetooth-Protokoll (BLE/GATT)

Quelle: `_client.py`, `_encryption.py`, `_operation.py`, `_constants.py`,
`_unit.py`.

### B.1 Transport (GATT)

| Element | Wert |
|---|---|
| Service-UUID | `0000fe4d-0000-1000-8000-00805f9b34fb` |
| Auth-Characteristic | `c9ffde48-ca5a-0001-ab83-8f519b482f77` |
| Protokollversionen | 10 (min) – 11 (max) |

Die gesamte Kommunikation nach dem Verbindungsaufbau läuft über **diese eine
Characteristic**: Lesen (`read`), Schreiben (`write`) und Notifications
(`notify`). Alle Einheiten eines Netzes werben mit derselben virtuellen Service-
UUID; ein Connect landet also auf einer beliebigen physisch erreichbaren Einheit
(„Gateway").

### B.2 Verbindungs-Zustandsmaschine

```
NONE ─connect→ CONNECTED ─exchangeKey→ KEY_EXCHANGED ─authenticate→ AUTHENTICATED
                                   └────(kein Schlüssel)────────────────┘
ERROR (99): Protokoll-/Auth-Fehler in beliebigem Schritt
```

`ConnectionState`: `NONE=0, CONNECTED=1, KEY_EXCHANGED=2, AUTHENTICATED=3,
ERROR=99`.

### B.3 Paketzähler

Nach dem Connect werden zwei Zähler gesetzt:

- `outPacketCount = 2` (ausgehende Datenpakete, danach +1 je Paket)
- `inPacketCount = 1` (eingehende Pakete)

Diese Zähler gehen sowohl in den **Paket-Header** als auch in die **Nonce**
(B.7) ein. *Hinweis:* casambi-bt verifiziert eingehende Zähler **nicht**
(`TODO: Check incoming counter and direction flag`) — es gibt also keinen
Replay-Schutz auf Empfangsseite.

### B.4 Geräteinfo lesen (Handshake-Start)

Erster `read` auf die Auth-Characteristic liefert (Big-Endian, ab Byte 0):

```
Byte 0:      Typ         = 0x01
Byte 1:      Version     = protocolVersion (Sonderfall: 0x2B bei v11, s. u.)
Byte 2:      MTU         (1 Byte)
Byte 3–4:    Unit-ID     (uint16, BE)
Byte 5–6:    Flags       (uint16, BE)
Byte 7–22:   Nonce       (16 Byte)  ← Basis für alle folgenden Nonces
```

Struktur in der Quelle: `struct.unpack_from(">BHH16s", firstResp, 2)`.

**Version-11-Eigenheit:** Bei Protokollversion 11 kann Byte 1 den Wert `0x2B`
(43) tragen; casambi-bt überspringt dann die Fehlermeldung und macht weiter
(`TODO: proper handling`).

### B.5 ECDH-Schlüsselaustausch

Kurve: **SECP256R1 (NIST P-256)**. Ziel ist ein gemeinsamer **Transportschlüssel**
(16 Byte), der den weiteren Kanal absichert.

Ablauf:

1. Nach dem Geräteinfo-Read abonniert der Client Notifications. Das **Gerät**
   initiiert den Austausch und schickt eine Notification `0x02 ‖ X ‖ Y`.
2. Gerät-Public-Key parsen: `struct.unpack_from("<32s32s", data, 1)` — X und Y
   je **32 Byte little-endian**, als Punkt auf SECP256R1.
3. Client erzeugt eigenes Schlüsselpaar (SECP256R1).
4. Shared Secret berechnen (`ECDH`), Bytes **umdrehen** (`secret.reverse()`),
   dann `SHA256`.
5. Transportschlüssel per **XOR-Faltung** aus dem 32-Byte-Digest:
   `transportKey[i] = digest[i] XOR digest[i+16]` für `i = 0..15`.
6. Client antwortet mit dem eigenen Public Key:
   ```
   0x02 ‖ X(32, little-endian) ‖ Y(32, little-endian) ‖ 0x01
   ```
   (`struct.pack(">B32s32sB", 0x2, x_le, y_le, 0x1)`).
7. Gerät bestätigt mit einer 1-Byte-Notification `0x03` (`len == 1`). Bei
   abweichender Länge oder anderem Typ → `ERROR`.

Nach erfolgreichem Austausch wird der `Encryptor` mit dem Transportschlüssel
initialisiert. Hat das Netz **keinen** Schlüssel (Classic), wechselt der Zustand
direkt auf `AUTHENTICATED`, sonst auf `KEY_EXCHANGED`.

### B.6 Authentisierung

Nur wenn ein Schlüssel vorhanden ist. Verwendet wird der Schlüssel mit höchster
`role` (A.5).

**Auth-Digest:**
```
authDig = SHA256( key.key ‖ nonce ‖ transportKey )
```
(`nonce` = die 16 Byte aus B.4; Reihenfolge exakt so.)

**Auth-Paket (Klartext, vor Verschlüsselung):**
```
counter(4, little-endian = 1) ‖ 0x04 ‖ key.id(1) ‖ authDig(32)
```
Es wird mit `_writeEncPacket` (B.8) unter Nonce-ID 1 verschlüsselt gesendet.

**Antwort:** verschlüsselte Notification; wird mit Nonce `data[:4] ‖ nonce[4:]`
entschlüsselt und per CMAC verifiziert. Ungültige Signatur → `ERROR`. (Die
zusätzliche Digest-2-Prüfung der Geräteantwort ist in der Quelle als `TODO`
offen.) Erfolg → `AUTHENTICATED`.

### B.7 Nonce-Konstruktion

Aus der 16-Byte-Basisnonce und einer Paket-ID (4 Byte):
```
nonce(id) = basisnonce[0:4] ‖ id(4, little-endian) ‖ basisnonce[8:16]
```
Für **eingehende** Pakete wird die Paket-ID aus den ersten 4 Klartext-/Header-
Bytes des Pakets genommen: `nonce = data[0:4] ‖ basisnonce[4:16]`.

### B.8 Verschlüsselung (AES-CTR + AES-CMAC)

Verfahren: **Encrypt-then-MAC**. Header bleibt im Klartext, nur die Nutzlast
dahinter wird verschlüsselt; über das Ganze läuft ein CMAC.

`encryptThenMac(packet, nonce, headerLen=4)`:
1. `packet[0:headerLen]` bleibt Klartext.
2. `packet[headerLen:]` wird mit **AES-128-CTR** verschlüsselt.
3. Über `header ‖ ciphertext` wird ein **AES-CMAC** (16 Byte) gebildet und
   angehängt.

**AES-CTR im Detail** (`_encryptInternal`): Keystream blockweise per **AES-ECB**
über den Zählerblock; die letzten 4 Nonce-Bytes (Byte 12–15) sind ein
**little-endian-Blockzähler**, der pro 16-Byte-Block ab 0 hochzählt und die per
`nonce(id)` gesetzten Bytes 12–15 überschreibt. `keystream XOR daten`.

**AES-CMAC** (RFC 4493): Subkey-Ableitung per Linksschieben des
AES-verschlüsselten Nullblocks mit Rb = `0x87`; Verschlüsselung/Entschlüsselung
sind symmetrisch.

`decryptAndVerify(packet, nonce, headerLen=4)`: trennt die letzten 16 Byte als
MAC ab, **entschlüsselt immer** (auch bei MAC-Fehler — bewusst gegen Timing-
Angriffe) und verifiziert den CMAC; bei Fehlschlag wird das Paket verworfen.

### B.9 Ausgehende Steuerbefehle

**Operation-Codes** (`OpCode`, aus `_operation.py`):

| Wert | Name |
|---|---|
| 0 | `Response` |
| 1 | `SetLevel` |
| 3 | `SetTemperature` |
| 4 | `SetVertical` |
| 5 | `SetWhite` |
| 7 | `SetColor` |
| 12 | `SetSlider` |
| 48 | `SetState` |
| 54 | `SetColorXY` |

**Operation-Paket** (`prepareOperation`):
```
struct.pack(">HBHHH", flags, op, origin, target, 0) ‖ payload

flags  = (lifetime & 0xF) << 11 | len(payload)     (2 Byte, BE; lifetime=5)
op     = OpCode                                     (1 Byte)
origin = fortlaufender Zähler, startet bei 1        (2 Byte, BE, wraparound 2^16)
target = Ziel-Kodierung                             (2 Byte, BE)
0x0000                                              (2 Byte, reserviert)
payload: max. 63 Byte
```

**Ziel-Kodierung (`target`):** `(id << 8) | typ`. Die Typ-Werte
(Unit/Group/Scene) sind im BLE-Layer von casambi-bt nicht als Enum hinterlegt;
diese Firmware verwendet `Unit=0x01`, `Group=0x02`, `Scene=0x04`.

**Sende-Rahmen** (`send`): Das Operation-Paket wird noch einmal umhüllt:
```
counter(4, little-endian = outPacketCount) ‖ 0x07 ‖ operation-paket
```
Dieser Rahmen geht durch `encryptThenMac` (Nonce-ID = `outPacketCount`), danach
`outPacketCount += 1`.

### B.10 Eingehende Pakete

casambi-bt entschlüsselt eingehende Notifications im Zustand `AUTHENTICATED`
(Nonce `data[:4] ‖ nonce[4:]`), verwirft bei ungültigem CMAC still (kein
Error-State) und dispatcht nach dem **ersten Klartextbyte** (`packetType`):

| Typ | `IncomingPacketType` | Behandlung in casambi-bt |
|---|---|---|
| 6 | `UnitState` | Zustands-Records → `_parseUnitStates` (B.11) |
| 7 | `SwitchEvent` | Schalter-/Sensor-Ereignisse → `parseSwitchEvents` (B.12) |
| 9 | `NetworkConfig` | **bewusst ignoriert** (Cloud-Config gilt als maßgeblich) |
| sonst | — | ignoriert („not implemented") |

> **Wichtig für den Vergleich mit dieser Firmware:** Die Firmware deutet Typ 7 als
> *Operation-Echo* und kennt zusätzlich 0x08/0x0A/0x0C — casambi-bt nicht. Umgekehrt
> parst casambi-bt Typ 7 als *Switch-Event*. Das ist die größte inhaltliche
> Divergenz zwischen beiden Implementierungen.

### B.11 Unit-State (Typ 6) — Records und Bit-Dekodierung

**Record-Zerlegung** (`_parseUnitStates`) — reine Rahmenerkennung, **ohne**
Bedeutung der Zustandsbytes:
```
Byte 0: id
Byte 1: flags       online = flags & 2 ; on = flags & 1
Byte 2: stateLen = ((byte2 >> 4) & 15) + 1 ; prio = byte2 & 15
weiter: if flags & 4:  +1 Byte   (unbekannt, „con?")
        if flags & 8:  +1 Byte   (unbekannt, „sid?")
        if flags & 16: +1 Byte   (unbekannt)
state = nächste stateLen Bytes    ← roher Zustands-Blob
weiter: + ((flags >> 6) & 3) Bytes (Padding?)
```
Der rohe `state`-Blob wird nach oben gereicht. Mehrere Records pro Paket sind
möglich.

**Bit-genaue Dekodierung** (`Unit.setStateFromBytes`) — die eigentliche
Bedeutung, gesteuert durch die Cloud-Controls (A.6). Für jedes Control:
```
byteLen = (length + offset % 8 - 1) // 8 + 1
cBytes  = state[offset//8 : offset//8 + byteLen]
cInt    = int.from_bytes(cBytes, "little") >> (offset % 8)
cInt   &= (2**length - 1)
```
und dann je Control-Typ:

| Control | Umrechnung |
|---|---|
| `DIMMER` | `cInt << (DIMMER_RESOLUTION - length)` |
| `VERTICAL` | `cInt << (VERTICAL_RESOLUTION - length)` |
| `WHITE` | `cInt << (WHITE_RESOLUTION - length)` |
| `SLIDER` | `cInt << (SLIDER_RESOLUTION - length)` |
| `TEMPERATURE` | `((cInt / (2**length-1)) * (max-min)) + min` (braucht `min`/`max`) |
| `RGB` | `hueLen = length*10//18`; obere Bits → Hue, untere → Sättigung; als HS gespeichert |
| `XY` | in zwei Hälften geteilt: obere → x, untere → y (je auf 0..1 normiert) |
| `COLORSOURCE` | `ColorSource(cInt)` (0=Temperature, 1=RGB, 2=XY) |
| `ONOFF` | `cInt != 0` |

Der Zustand ist damit vollständig durch (offset, length) je Control bestimmt —
Felder können **sub-byte** liegen (z. B. RGB gepackt). Genau deshalb ist eine
feste „ein Byte pro Aux-Kanal"-Annahme unzureichend.

### B.12 Switch-Event (Typ 7) — Struktur

`parseSwitchEvents` interpretiert die Nutzlast als Folge von Nachrichten
(mehrere pro Paket möglich). Sonderfall: beginnt die Nutzlast mit `0x29`, ist es
**kein** Switch-Event und wird ignoriert.

**Nachrichten-Header** (je Nachricht):
```
Byte 0: message_type
Byte 1: flags
Byte 2: length = ((byte2 >> 4) & 15) + 1   (param = ganzes Byte 2)
payload = nächste length Bytes
```
Nachrichtentypen `> 0x80` gelten als ungültig → Resync (ein Byte weiter suchen).

**Relevante Typen:**
- `0x08`, `0x10` → **Button-/Schalter-Ereignis** (`_processSwitchMessage`).
- `0x00, 0x06, 0x09, 0x1F, 0x2A` → bekannt, aber Nicht-Switch (nur Log).
- `0x29` → ignoriert.

**Button-Extraktion** (reverse-engineert, entsprechend unsicher):
- Typ `0x08`: `button = ((param & 0x0F) + 2) % 4 + 1`; Press/Release aus Bit 1
  von `action` (`payload[1]`).
- Typ `0x10`: `unit_id = payload[2]`; Zustandsbyte an fester Position 9 der
  Nachricht → `ButtonEventType`.

**Ereignistypen** (`ButtonEventType`): `PRESS=0x01`, `RELEASE=0x02`, `HOLD=0x09`,
`RELEASE_AFTER_HOLD=0x0C`, `UNKNOWN=0xFFFF`.

Ergebnis je Ereignis (`SwitchEvent`): `message_type`, `button`, `unit_id`,
`action`, `event`, `flags`, `extra_data`.

### B.13 Network-Config (Typ 9)

casambi-bt **parst Typ 9 nicht** und ignoriert ihn bewusst: Es wird angenommen,
dass Cloud- und lokale Konfiguration übereinstimmen; bei Abweichung soll der
Nutzer die App verwenden. (Diese Firmware dekodiert 0x09 dagegen als
Revisions-/Szenen-Tracker — siehe die P09-Notizen im Firmware-Code.)

---

## Teil C — Zusammenfassung der Kryptografie

| Aspekt | Wert |
|---|---|
| Schlüsselaustausch | ECDH über SECP256R1 (P-256) |
| Public-Key-Kodierung | X/Y je 32 Byte little-endian |
| Shared-Secret-Nachbearbeitung | Bytes umdrehen → SHA-256 → XOR-Faltung (32→16 Byte) |
| Transportschlüssel | 16 Byte (AES-128) |
| Datenverschlüsselung | AES-128-CTR (ECB-Keystream), Blockzähler LE in Byte 12–15 |
| Integrität | AES-CMAC (RFC 4493), 16 Byte, Encrypt-then-MAC |
| Nonce | `basis[0:4] ‖ id(4,LE) ‖ basis[8:16]`, 16 Byte |
| Auth-Digest | `SHA-256(key ‖ nonce ‖ transportKey)` |
| Zähler | out startet bei 2, in bei 1; kein Replay-Schutz eingangsseitig |

---

## Teil D — Umsetzung in esp32-casambi (Abweichungen)

Die ESP32-Firmware implementiert **dasselbe Protokoll** wie die Referenz oben.
Transport- und Kryptoschicht sind byte-kompatibel; die Unterschiede liegen fast
ausschließlich in der **Beschaffung der Fähigkeiten** (Cloud) und der
**Interpretation eingehender Pakete** (BLE). Quellen: `src/cloud/api_client.cpp`,
`src/ble/casambi_client.cpp`, `src/ble/packet_parse.h`, `src/crypto/*`,
`src/config.h`.

**Markierungen:** `[=]` identisch · `[Δ]` abweichend · `[+]` nur in der Firmware.

### D.0 Kurzüberblick der Abweichungen

| Bereich | casambi-bt | esp32-casambi |
|---|---|---|
| Fähigkeits-Quelle | `GET /fixture/{id}` → Controls | `GET /fixture/{id}` → Controls; Mode-String-Heuristik nur als Fallback `[≈]` |
| Cloud-Revision | inkrementell (`revision`, `UPTODATE`) | `revision=0`, immer Vollabruf `[Δ]` |
| Zustands-Dekodierung | bit-genau (offset/length) | control-gesteuert per Byte-Offset (offset/8); Sub-Byte noch offen `[≈]` |
| Paket-Typ 7 | Switch-/Sensor-Event | Operation-Echo, nur Diagnose (kein State) `[Δ]` |
| Paket-Typ 8/0A/0C | — | UnitState-Update / TimeSync / Keepalive `[+]` |
| Paket-Typ 9 | ignoriert | P09-Revisions-Tracker dekodiert `[+]` |
| 0x06-online | `flags & 2` | `(flags & 0x0F) == 0` `[Δ]` |
| Geräteinfo unit/flags | Big-Endian | Big-Endian `[=]` (bis #49: Little-Endian) |
| Geräteinfo-Version | Sonderfall für `0x2B` | untere 4 Bit maskiert `[≈]` |
| CMAC | Bibliothek (`cryptography`) | eigene RFC-4493-Impl + Selbsttest `[+]` |

### D.1 Cloud (HTTPS/REST)

- `[=]` Dieselben drei Endpunkte, Request-Bodies und der `X-Casambi-Session`-
  Header (`getNetworkId` / `createSession` / `fetchNetworkConfig`,
  `api_client.cpp`). TLS wird gegen das eingebettete Mozilla-CA-Bundle
  **validiert** (nicht `setInsecure`, außer per `-DCASAMBI_TLS_INSECURE`).
- `[=]` Schlüsselwahl nach höchster `role` (`getBestKey`).
- `[Δ]` **`revision` ist fest `0`** — der ESP32 zieht bei jedem Refresh die
  **komplette** Definition; die inkrementelle `UPTODATE`-Logik von casambi-bt
  (A.4) existiert nicht.
- `[≈]` **`GET /fixture/{typeId}` (implementiert, `_fetchFixtures`).** Nach dem
  Netzwerk-Parse holt die Firmware je **distinktem** Unit-Typ die
  Fixture-Definition (A.6), parst deren `controls` und leitet daraus die
  Fähigkeiten ab: `hasVertical` = ein `VERTICAL`-Control vorhanden, `hasCCT` =
  ein `TEMPERATURE`-Control (Kelvin-Grenzen aus dessen `min`/`max`). Das ist das
  **verlässliche Signal** statt der Kanalzahl — und behebt die Wurzel von
  Issue #34 (Oligo Grace = Dimmer + CCT, aber 3-Byte-Mode-String).
- `[Δ]` **Fallback-Heuristik** (`_parseUnits`, nur wenn ein Fixture-Abruf
  scheitert oder keine `controls` liefert): `numChannels = len(modes[0].state)/2`
  (1–3); `hasCCT` aus `settings["cct.minKelvins"]`; `hasVertical` bei 3 Kanälen,
  oder bei 2 Kanälen ohne CCT. Bei jedem Fixture-Treffer wird der
  Heuristik-Wert zum Vergleich mitgeloggt (Verifikation).
- `[≈]` **Dekodierung:** Die Zustandsbytes werden **control-gesteuert** per Byte-
  Offset zugeordnet (State-Byte `n` → Control mit `offset = n·8`), und jeder
  Control-Wert wird gespeichert (D.5.2). **Noch offen** ist nur das bit-genaue
  Entpacken von **Sub-Byte-Controls** (RGB/XY) — im aktuellen Bestand nicht
  vorhanden.
- `[Δ]` Laufzeit: Der ESP32 arbeitet aus der in LittleFS **gespeicherten**
  Konfiguration; die Cloud wird nur bei Provisionierung/Refresh kontaktiert.
  Zusätzlich harte Struktur-Invarianten (Duplikat-IDs, Limits) beim Parsen.
- `[+]` **Analyse-Hilfe:** `debug cloud on` gibt beim nächsten `refresh` die rohe
  Netzwerk-Antwort (`_dumpRedactedConfig`, AES-Schlüssel durch `***` ersetzt)
  **und** jede rohe Fixture-Antwort (`/fixture/{type}`, ohne Redaktion — enthält
  keine Geheimnisse) auf Serial aus, um `modes`/`settings`/`controls` je Unit
  auszuwerten.

### D.2 BLE-Transport & Handshake

- `[=]` UUIDs, Zustandsmaschine, Paketzähler (`out=2`, `in=1`), ECDH über
  SECP256R1, Transportschlüssel-Ableitung (Reverse → SHA-256 → XOR-Faltung),
  Public-Key-Austausch (`0x02 ‖ X ‖ Y ‖ 0x01`), `0x03`-Ack, Auth-Digest und
  Auth-Paket (`counter ‖ 0x04 ‖ key.id ‖ digest`) — alles identisch.
- `[=]` **Geräteinfo-Endianness:** `unitId`/`flags` werden **big-endian**
  gelesen, wie casambi-bt (B.4). Die Firmware las beide Felder bis Issue #49
  little-endian; das lieferte für Unit 11 (auf dem Draht `00 0b`) den Wert
  2816. Betroffen war nur die Diagnose (beide Felder werden sonst nirgends
  verwendet, die 16 Nonce-Bytes waren immer identisch) — deshalb fiel es
  jahrelang nicht auf. Layout und Endianness liegen jetzt als reiner Parser in
  `packet_parse.h` (`parseDeviceInfo`, host-getestet in `test_packet_parse`).
- `[≈]` **Version-11-Sonderfall (`0x2B`):** Byte 1 trägt die Protokollversion
  nur in den **unteren 4 Bit**, darüber stehen undekodierte Flags
  (`0x2B` = Flags `0x2` + Version 11). Die Firmware maskiert mit `0x0F` statt
  casambi-bts Sonderfall für den konkreten Wert `0x2B` — dieselbe Korrektur,
  allgemeiner. Vorher verglich sie das ganze Byte und meldete bei **jedem**
  Connect einen Versions-Mismatch „device reports 43, config has 11". Das rohe
  Byte wird weiter mitgeloggt (`versionRaw`), damit die undekodierten oberen
  Bits sichtbar bleiben. Die Maske ist die engste zur Beobachtung passende
  Annahme; ein Netz mit Version ≥ 16 würde sie sprengen.
- `[+]` Wartet nach dem eigenen Public Key aktiv auf die `0x03`-Ack-Notification,
  bevor authentisiert wird.
- `[+]` **Gateway-Auswahl per RSSI-Re-Roll** und **Keepalive per GATT-Read** auf
  der Auth-Characteristic — Betriebslogik ohne Protokoll-Entsprechung in
  casambi-bt.

### D.3 Kryptografie

- `[=]` AES-128-CTR (ECB-Keystream, Blockzähler LE in Byte 12–15), AES-CMAC
  (RFC 4493), Encrypt-then-MAC, `headerLen=4`, Nonce-Konstruktion (B.7/B.8).
- `[+]` **Eigene CMAC-Implementierung** (mbedTLS-CMAC ist im Core nicht
  aktiviert) mit **RFC-4493-Selbsttest**, **konstantzeit-MAC-Vergleich** und
  **Wiping** des Zwischen-Geheimnisses/Hashes. casambi-bt nutzt die
  `cryptography`-Bibliothek.
- Beide: **kein** Replay-/Zähler-Check auf Empfangsseite.

### D.4 Ausgehende Steuerbefehle

- `[=]` OpCodes identisch (B.9). Operation-Paket byte-identisch
  (`flags(2,BE)=lifetime<<11|len ‖ op ‖ origin(2,BE) ‖ target(2,BE) ‖ 0x0000 ‖
  payload`), `lifetime=5`, Sende-Rahmen `counter(4,LE) ‖ 0x07 ‖ operation`
  (`_buildOperation` / `_sendOperation`).
- `[=]` Ziel-Kodierung `(id<<8)|typ`; die Firmware definiert die Typ-Werte
  explizit: `Unit=0x01`, `Group=0x02`, `Scene=0x04`.
- `[+]` **Rollback von `origin`/`outPacketCount`** bei fehlgeschlagenem
  GATT-Write, damit die Nonce-Sequenz nicht driftet (casambi-bt inkrementiert
  bedingungslos).
- `[Δ]` **Payload-Kodierung der Firmware:** `SetTemperature` sendet `kelvin/50`
  (1 Byte); `SetColor` rechnet RGB→Hue/Sättigung (`rgbToHS`, Hue 0–1023 als
  2 Byte LE + Sättigung). Der äußere Operations-Rahmen ist identisch; diese
  konkreten Nutzlast-Formate baut casambi-bt in seiner höheren Schicht (nicht in
  `_operation.py`) und wurden hier **nicht** gegengeprüft.
- `[+]` **`SetState` (48) als Schreibpfad — auf Hardware verifiziert** (Oligo
  Grace, zwei unabhängige 8-Bit-Dimmer + Temperatur-Byte; Erkenntnis aus
  PR #39). Semantik:
  - Die Payload ist der **komplette State-Blob** der Unit — dasselbe Layout,
    das die Fixture-Definition (`/fixture/{type}`, A.6) über die Controls
    (offset/length) beschreibt und das eingehend in 0x06-Records steht
    (`stateLength` Bytes).
  - Ein `SetState`-Write setzt **alle** Controls auf die übertragenen Werte;
    es gibt keine Teil-Writes. **Nicht mitgeschriebene (genullte) Bytes setzen
    das jeweilige Control zurück** — beobachtet am Temperatur-Byte, das ohne
    Erhalt des aktuellen Werts auf 0 fiel. Sender müssen den Blob daher immer
    aus den aktuellen Control-Werten aufbauen und nur die gewünschten Controls
    überschreiben.
  - Nur so lassen sich mehrere Kanäle **atomar** in einem Telegramm ändern
    (z. B. beide Dimmer einer Uplight/Downlight-Leuchte) — mit `SetLevel`/
    `SetVertical` ist das nicht möglich, und `SetVertical` implementieren
    Zwei-Dimmer-Fixtures gar nicht.
  - Umsetzung: `CasambiClient::setUnitState()` + purer Encoder
    `src/cloud/state_codec.h` (host-getestet, `test/test_state_codec`);
    REST-seitig `POST /api/units/:id/state`. Verifiziert sind byte-alignierte
    8-Bit-Controls; 16-Bit wird little-endian kodiert (analog Hue in
    `SetColor`), Sub-Byte-Layouts lehnt der Encoder explizit ab statt eine
    ungetestete Bit-Reihenfolge zu raten.

### D.5 Eingehende Pakete — die größte Divergenz

Dispatch nach dem ersten Klartextbyte (`_handleDataNotification`):

| Typ | casambi-bt (B.10) | esp32-casambi |
|---|---|---|
| 0x06 | UnitState (bit-genau) | Status-Broadcast, control-gesteuert per Byte-Offset `[≈]` |
| 0x07 | **Switch-Event** | Operation-Echo, **nur Diagnose** `[Δ]` |
| 0x08 | — | UnitState-Update (Paar- oder 0x06-Format) `[+]` |
| 0x09 | ignoriert | P09-Revisions-/Szenen-Tracker (Debug) `[Δ]` |
| 0x0A | — | TimeSync — undekodiert, Payload wird gedumpt `[+]` |
| 0x0C | — | Keepalive — undekodiert, Payload wird gedumpt `[+]` |

- `[Δ]` **Typ 7 ist der gewichtigste Unterschied:** casambi-bt deutet ihn als
  Schalter-/Sensor-Ereignis (B.12), die Firmware dekodiert ihn als **Echo einer
  Operation** (gleiche Struktur wie eine ausgehende Operation). Diese Deutung ist
  eine unbelegte Symmetrie-Annahme aus dem Sende-Format und wurde nie gegen ein
  echtes eingehendes 0x07 verifiziert (im Netz nie beobachtet). Die Firmware
  **wendet daraus bewusst keinen Zustand mehr an** (nur Dekodieren/Loggen +
  `malformed07`-Zähler) — jede reale Änderung kommt ohnehin als 0x06, und ein
  fehlgedeutetes Switch-Event soll keinen falschen Level injizieren können. Ein
  echter Switch-/Sensor-Parser (casambi-bt-Port) folgt erst mit einem Mitschnitt.

**D.5.1 Record-Format bei Typ 0x06.** Beide lesen Byte 0 = ID, Byte 1 = Flags,
aber Byte 2 verschieden:

| | casambi-bt | esp32-casambi |
|---|---|---|
| Byte 2 | `stateLen=(b2>>4)+1`, `prio=b2&15` | **Capability**: oberes Nibble = Aux-Zahl, unteres `0x00`/`0x03` |
| Optionalbytes | `flags & 4/8/16` (je +1, unbekannt) | `0x80`-Byte nur bei `cap==0x03`; Prev-Level bei `flags & 0x10` |
| online | `flags & 2` | `(flags & 0x0F) == 0` → offline `[Δ]` |
| Padding | `(flags>>6)&3` | — |

Für `cap=0x23` errechnen **beide** dieselbe Satzlänge von 6 Byte — die
„malformed/ein-Byte-zu-kurz"-Annahme aus Issue #34 trifft in **keiner** der
beiden Implementierungen zu.

**D.5.2 Zustands-Semantik.** `[≈]` Die Protokollschicht (`parseStatusBroadcast`)
extrahiert die State-Bytes weiter positionsbasiert (`level`, `aux1`, `aux2`);
die **Bedeutung** wird im Unit-Modell (`_applyUnitStates`) **generisch aus den
Fixture-Controls** zugeordnet: State-Byte `n` → Control mit `offset = n·8`, und
dessen `typeName` (dimmer/vertical/temperature/…) bestimmt Ziel und Benennung.
Damit ist die feste „aux1→vertical"-Annahme aufgehoben (aux1 ist z. B.
temperature auf Unit 5, vertical auf Unit 7). Jeder Control-Wert wird für die
generische API gespeichert. Sub-Byte-Felder (RGB/XY) werden noch nicht
bit-entpackt — im aktuellen Bestand kommen sie nicht vor.

> **Stand der Angleichung.** Umgesetzt: `/fixture/{id}`-Abruf (D.1) mit
> Speicherung + Persistenz der Controls (offset/length/min/max/stateLength),
> **control-gesteuerte, cloud-abgeleitete Dekodierung** (`_applyUnitStates`),
> **generische API** (`controls`-Array je Unit) und **FHEM**-Readings, deren
> Namen aus den Cloud-Control-Typen stammen. Golden-Vector-Tests
> (`test/test_packet_parse`) frieren die Occhio-Captures byte-für-byte ein.
> Ebenfalls umgesetzt: der **generische Schreibpfad** über `SetState` (D.4) —
> `POST /api/units/:id/state` adressiert Controls per Name und schreibt den
> kompletten State-Blob atomar; FHEM bedient Mehrkanal-Dimmer (z. B. Oligo
> Grace Uplight/Downlight) darüber ohne fixture-spezifischen Code. Offen nur
> noch: bit-genaues Entpacken/Kodieren von Sub-Byte-Controls (RGB/XY) und
> Homebridge-Mappings für neue Control-Typen.

### D.6 Endianness — Gesamtübersicht

Anlass: Issue #49. Da das Protokoll **beide** Byte-Reihenfolgen verwendet und
ein Fehlgriff hier lautlos bleibt (die betroffenen Felder waren reine
Diagnose), hier alle Mehrbyte-Felder der Firmware auf einen Blick. Die Regel
dahinter: **Transportschicht little-endian, Handshake- und Operationsschicht
big-endian.**

| Feld | Stelle | Reihenfolge | Beleg |
|---|---|---|---|
| Paketzähler (4 B, Header + Nonce) | `_sendOperation`, `_getNonce`, `_handleAuth-/DataNotification` | LE | B.7/B.8; Auth funktioniert |
| AES-CTR-Blockzähler (Byte 12–15) | `encryption.cpp` | LE | B.8 |
| ECDH-Public-Key X/Y (je 32 B) | `key_exchange.cpp` | LE (reversed) | B.5; Handshake funktioniert |
| Geräteinfo `unitId`, `flags` | `parseDeviceInfo` | **BE** | casambi-bt `>BHH16s` (B.4); #49 |
| Operation `flags`, `origin`, `target` (aus) | `_buildOperation` | BE | D.4, byte-identisch zu casambi-bt |
| Operation `flags`, `target` (ein, 0x07) | `parseOperationEcho` | BE | Symmetrie zum Sendeformat — **unbelegt**, s. u. |
| `SetColor` Hue (2 B) | `setUnitColor` | LE | D.4, nicht gegengeprüft |
| `SetState` 16-Bit-Controls | `state_codec.h` | LE | analog Hue; **kein** 16-Bit-Control bisher beobachtet |

**Ergebnis der Prüfung auf weitere Fundstellen.** Außer der Geräteinfo hat die
Nachziehung nirgends ein plausibleres Ergebnis geliefert:

- **Transport und Krypto** sind durch den funktionierenden Handshake bewiesen —
  eine falsche Reihenfolge hätte dort nie eine Authentisierung zugelassen.
- **Ausgehende Operationen** sind byte-identisch zu casambi-bt (D.4) und auf
  Hardware verifiziert.
- **0x06/0x08/0x09** enthalten überhaupt kein Mehrbyte-Feld: 0x06/0x08 sind
  Byte-Ströme, 0x09 besteht aus 3-Byte-Records mit einzeln gedeuteten Bytes.
  Hier gibt es nichts zu drehen.
- **0x07 eingehend** hat zwar BE-Felder, aber die Deutung als „Operation-Echo"
  ist selbst unbelegt (D.5) und es wird kein Zustand angewendet. Eine
  Endianness-Änderung wäre Raten auf einer ungeprüften Grundstruktur — erst mit
  einem echten Mitschnitt sinnvoll.
- **`SetColor` / `state_codec`** sind LE per Analogieschluss. Beide sind auf
  Hardware nicht gegengeprüft, aber es existiert auch kein Gegenbeleg: kein
  bisher gesehenes Fixture hat ein 16-Bit-Control, und RGB ist ungetestet
  (README, „Untested Features"). Ohne RGB-Leuchte bleibt das offen; die
  Änderung wäre hier nicht „nachgezogen", sondern eine zweite Vermutung.

**Die aussichtsreichste Fundstelle ist die Keepalive-Antwort.** `sendKeepalive()`
liest per GATT von der Auth-Characteristic und prüft nur die **Länge** —
typisch 25 Byte, also ein vollständiges verschlüsseltes Paket (4 Byte
Zähler-Header + Klartext + 16 Byte CMAC), mithin **1 Byte Typ + 4 Byte
Nutzlast**. Das ist die einzige Stelle der Firmware, an der Protokollbytes
ankommen und ungelesen verworfen werden — und ein 4-Byte-Feld ist genau die
Form, in der sich die Endianness-Frage stellt. Die Bytes werden jetzt unter
`debug ble on`/`debug parse on` gedumpt. Sie zu **entschlüsseln** würde denselben
Pfad wie eine Notification brauchen (`data[0:4] ‖ basisnonce[4:16]`); ob die
Zähler-/Richtungskonvention einer Read-Antwort dieselbe ist, ist ungeprüft —
schlägt der CMAC fehl, wäre das folgenlos, aber es ist eine eigene Änderung mit
eigenem Risiko und braucht zuerst einen Mitschnitt.

**0x0A/0x0C als Notification waren nicht prüfbar** — beide
Typen wurden zwar erkannt, ihre Payload aber als einzige **nie ausgegeben**
(der `default`-Zweig dumpt jeden wirklich unbekannten Typ, diese beiden nicht).
Damit ließ sich kein Mitschnitt nehmen, mit dem die Frage überhaupt zu
beantworten wäre. Beide dumpen jetzt ihre Bytes; für 0x0A gibt `debug parse on`
zusätzlich die beiden Lesarten eines führenden 32-Bit-Felds gegen die
naheliegende Hypothese „Unix-Epoch" aus und markiert die plausible. Ein
einziger Mitschnitt entscheidet die Frage dann. **Angewendet wird nichts** —
die Uhrzeit der Firmware kommt aus NTP.

In einem 100-s-Mitschnitt am realen Netz (Build 2026-08-04, `debug parse on` +
`debug ble on`) trat **kein einziges 0x0A oder 0x0C als Notification** auf;
gesehen wurden nur 0x06-Broadcasts und der Keepalive-**Read**. Der 0x0C-Zweig
in `_handleDataNotification` ist damit möglicherweise toter Code — die
Keepalive-Antwort kommt nicht über den Notification-Pfad.

---

## Bekannte Lücken / offene Punkte (aus casambi-bt selbst)

- Eingehende Paketzähler und Richtungsflag werden nicht geprüft (Replay).
- Digest-2 der Auth-Antwort wird nicht gegengeprüft.
- Version-11-Handshake (`0x2B`) ist nur „durchgewinkt", nicht verstanden.
- Switch-Event-Parsing (Typ 7) ist stark reverse-engineert (Button-Formeln,
  feste Zustandsbyte-Position) und mit vielen `TODO` versehen.
- Optionale Bytes im Unit-State-Record (`flags & 4/8/16`) sind unbekannt.
- Network-Config (Typ 9), `managerKey`/`visitorKey` für Classic-Netze und
  weitere Netzwerkfelder werden nicht geparst.
