# Casambi-Protokollreferenz (Cloud + Bluetooth)

Status: **Referenzdokument**, abgeleitet aus der Reverse-Engineering-Bibliothek
[lkempf/casambi-bt](https://github.com/lkempf/casambi-bt) (Stand: Analyse
2026-07). Beschreibt das Casambi-Protokoll so, wie casambi-bt es implementiert —
als unabhängige Referenz für den in diesem Repository umgesetzten ESP32-Client.

> **Hinweis zur Verlässlichkeit.** Das Protokoll ist nicht öffentlich
> dokumentiert; alle Angaben stammen aus Reverse Engineering und können lückenhaft
> oder in Randfällen falsch sein. Wo casambi-bt selbst Unsicherheit markiert
> (`TODO`, „unknown", „arbitrary"), ist das hier vermerkt. Abweichungen zwischen
> casambi-bt und dieser Firmware sind in `docs/` an anderer Stelle bzw. in den
> Issues diskutiert.

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

## Bekannte Lücken / offene Punkte (aus casambi-bt selbst)

- Eingehende Paketzähler und Richtungsflag werden nicht geprüft (Replay).
- Digest-2 der Auth-Antwort wird nicht gegengeprüft.
- Version-11-Handshake (`0x2B`) ist nur „durchgewinkt", nicht verstanden.
- Switch-Event-Parsing (Typ 7) ist stark reverse-engineert (Button-Formeln,
  feste Zustandsbyte-Position) und mit vielen `TODO` versehen.
- Optionale Bytes im Unit-State-Record (`flags & 4/8/16`) sind unbekannt.
- Network-Config (Typ 9), `managerKey`/`visitorKey` für Classic-Netze und
  weitere Netzwerkfelder werden nicht geparst.
