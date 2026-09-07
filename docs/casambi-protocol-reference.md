# Casambi protocol reference (cloud + Bluetooth)

Status: **reference document**, derived from the reverse-engineering library
[lkempf/casambi-bt](https://github.com/lkempf/casambi-bt) (as analysed in
2026-07). **Parts A–C** describe the Casambi protocol as casambi-bt implements
it (the independent reference). **Part D** puts this repository's implementation
(esp32-casambi) alongside it and marks every deviation.

> **A note on reliability.** The protocol is not publicly documented; everything
> here comes from reverse engineering and may be incomplete or wrong in edge
> cases. Where casambi-bt marks uncertainty itself (`TODO`, "unknown",
> "arbitrary"), that is noted here. The concrete deviations of the ESP32
> firmware from casambi-bt are collected in **part D**.

The Casambi system consists of **two separate protocols**:

1. **Cloud (HTTPS/REST)** — a one-off or occasional retrieval of the network
   definition: keys, units, groups, scenes and, above all, the **capability
   descriptors** (unit types with bit-exact controls).
2. **Bluetooth (BLE/GATT)** — the actual runtime channel: connection, key
   exchange, authentication, encrypted control commands and state broadcasts.

The two are connected: the cloud supplies the **keys** (for the BLE auth) and
the **bit layouts** (for decoding the BLE state bytes). The BLE channel itself
transmits **no** capability metadata — it only sends raw state bytes whose
meaning comes exclusively from the cloud definition.

---

## Part A — Cloud protocol (HTTPS/REST)

Base host: `https://api.casambi.com`. All responses are JSON.
Source: `_network.py`, `_keystore.py`, `_unit.py`.

### A.1 The flow at a glance

```
UUID (BLE advertisement)
   │
   ├─(1) GET  /network/uuid/{uuid}            → internal network ID
   │
   ├─(2) POST /network/{id}/session           → session token (with the password)
   │
   ├─(3) PUT  /network/{id}/                  → the complete network definition
   │            (header X-Casambi-Session)       (units, groups, scenes, keyStore)
   │
   └─(4) GET  /fixture/{typeId}   per unit type → capability descriptor (controls)
```

### A.2 Step 1 — resolve the network ID

```
GET https://api.casambi.com/network/uuid/{uuid}
```

- `{uuid}` is the globally unique network UUID (it comes from the BLE scan of
  the gateway, see B.1).
- Response: `{ "id": "<internal-id>" }`. This `id` is used for all following
  calls and cached locally (file `networkid`).
- Status codes: `404` → network unknown (`NetworkNotFoundError`); anything other
  than `200` → error.

### A.3 Step 2 — login / session

```
POST https://api.casambi.com/network/{id}/session
Content-Type: application/json

{ "password": "<network-password>", "deviceName": "Casambi BT Python" }
```

- `deviceName` is a freely chosen client name (the constant `DEVICE_NAME`).
- The response (`200`) contains, among others:
  - `session` — the session token (string)
  - `network` — the network ID
  - `manager` — bool
  - `keyID` — int
  - `expires` — expiry timestamp in **milliseconds** since the epoch
  - `role` — the role (default 3)
- The token is persisted locally and reused as long as `expires` has not passed.
  Other status codes → `AuthenticationError`.

### A.4 Step 3 — fetch the network definition

```
PUT https://api.casambi.com/network/{id}/
X-Casambi-Session: <session-token>
Content-Type: application/json

{ "formatVersion": 1, "deviceName": "Casambi BT Python", "revision": <local-revision> }
```

- **Security note (from the source):** the session header must **not** be set as
  a global client header — otherwise the token leaks to other hosts. It belongs
  exclusively on this one request.
- `revision` is the locally known revision (0 if there is no cache). The server
  answers with `status: "UPTODATE"` if there is nothing new, otherwise with the
  complete definition and a new `network.revision`.
- `410 GONE` → the network no longer exists (e.g. a password change) → discard
  the cache.

**Structure of the response** (the `network` object, relevant fields):

| Path | Meaning |
|---|---|
| `network.name` | network name |
| `network.revision` | revision counter (monotonic) |
| `network.protocolVersion` | BLE protocol version (10 or 11) |
| `network.keyStore.keys[]` | auth keys (see A.6) |
| `network.units[]` | units (see A.5) |
| `network.grid.cells[]` | groups (cells, type 2) |
| `network.scenes[]` | scenes (`sceneID`, `name`) |

Units (`units[]`):

| Field | Meaning |
|---|---|
| `deviceID` | the unit's ID **within** the network (addressed over BLE) |
| `uuid` | globally unique device ID |
| `address` | the unit's MAC address |
| `name` | display name |
| `firmware` | firmware version |
| `type` | **unit type ID** → to be resolved separately via `/fixture/{type}` (A.7) |

Groups (`grid.cells[]`): only cells with `type == 2` are top-level groups. Their
sub-entries (`cells[]`) with `type == 1` reference the member `deviceID`s via
`unit`. Nested groups are not supported.

### A.5 Keys (`keyStore.keys[]`)

If `keyStore` is missing, this is presumably a **classic network** without keys
(the BLE auth is then skipped, see B.5).

A key entry (`Key`, from `_keystore.py`):

| Field | Range | Meaning |
|---|---|---|
| `id` | ≥ 0 | key ID (goes into the auth packet, B.6) |
| `type` | 0–255 | key type |
| `role` | 0–3 | permission level |
| `name` | string | display name |
| `key` | hex → bytes | the actual key material |

**Key selection:** the key with the **highest `role`** is used for
authentication (`KeyStore.getKey()`).

### A.6 Step 4 — unit type / capabilities (`/fixture/{id}`)

```
GET https://api.casambi.com/fixture/{typeId}
```

The core of **bit-exact state decoding**. The response (`UnitType`):

| Field | Meaning |
|---|---|
| `id` | type ID |
| `model` | model name |
| `vendor` | manufacturer |
| `mode` | mode string |
| `stateLength` | length of the state blob in bytes |
| `controls[]` | list of **controls** (capabilities) |

A `controls[]` entry → `UnitControl`:

| Field | Meaning |
|---|---|
| `type` | control type (string, case normalised) |
| `offset` | **bit offset** in the state blob |
| `length` | **bit length** of the field |
| `default` | default value |
| `readonly` | bool |
| `min` / `max` | optional bounds (mainly for temperature) |

Unit types are cached locally (entries with an expiry: 28 days on success,
7 days on failure).

**Control types** (`UnitControlType`, from `_unit.py` — the numeric values are
"totally arbitrary" per the source, i.e. internal to the library):

| Name | Meaning |
|---|---|
| `DIMMER` | brightness |
| `WHITE` | white component |
| `RGB` | colour (hue/saturation encoded) |
| `ONOFF` | on/off |
| `TEMPERATURE` | colour temperature (linear between `min`/`max`) |
| `VERTICAL` | vertical distribution (up/down light) |
| `COLORSOURCE` | switch the colour source (TW/RGB/XY) |
| `XY` | colour in CIE colour space |
| `SLIDER` | generic slider |
| `SENSOR` | sensor value |
| `UNKOWN` | not implemented (stored for debugging only) |

The **existence** of a control (e.g. `VERTICAL`) is the authoritative statement
about whether a unit has that capability — **not** the number of state bytes.
This separation is the central difference from the firmware's earlier
channel-count heuristic (see issue #34).

---

## Part B — Bluetooth protocol (BLE/GATT)

Source: `_client.py`, `_encryption.py`, `_operation.py`, `_constants.py`,
`_unit.py`.

### B.1 Transport (GATT)

| Element | Value |
|---|---|
| Service UUID | `0000fe4d-0000-1000-8000-00805f9b34fb` |
| Auth characteristic | `c9ffde48-ca5a-0001-ab83-8f519b482f77` |
| Protocol versions | 10 (min) – 11 (max) |

All communication after the connection is established runs over **this single
characteristic**: read, write and notifications. All units of a network
advertise the same virtual service UUID, so a connect lands on whichever unit
happens to be physically reachable (the "gateway").

### B.2 Connection state machine

```
NONE ─connect→ CONNECTED ─exchangeKey→ KEY_EXCHANGED ─authenticate→ AUTHENTICATED
                                   └────(no key)──────────────────────┘
ERROR (99): protocol/auth error at any step
```

`ConnectionState`: `NONE=0, CONNECTED=1, KEY_EXCHANGED=2, AUTHENTICATED=3,
ERROR=99`.

### B.3 Packet counters

Two counters are set after the connect:

- `outPacketCount = 2` (outgoing data packets, +1 per packet afterwards)
- `inPacketCount = 1` (incoming packets)

These counters go into both the **packet header** and the **nonce** (B.7).
*Note:* casambi-bt does **not** verify incoming counters
(`TODO: Check incoming counter and direction flag`) — so there is no replay
protection on the receiving side.

### B.4 Reading the device info (start of the handshake)

The first `read` on the auth characteristic returns (big-endian, from byte 0):

```
Byte 0:      type        = 0x01
Byte 1:      version     = protocolVersion (special case: 0x2B on v11, see below)
Byte 2:      MTU         (1 byte)
Byte 3–4:    unit ID     (uint16, BE)
Byte 5–6:    flags       (uint16, BE)
Byte 7–22:   nonce       (16 bytes)  ← the basis for all subsequent nonces
```

Structure in the source: `struct.unpack_from(">BHH16s", firstResp, 2)`.

**Version 11 quirk:** with protocol version 11, byte 1 may carry the value
`0x2B` (43); casambi-bt then skips the error message and carries on
(`TODO: proper handling`).

### B.5 ECDH key exchange

Curve: **SECP256R1 (NIST P-256)**. The goal is a shared **transport key**
(16 bytes) that secures the rest of the channel.

The flow:

1. After the device-info read, the client subscribes to notifications. The
   **device** initiates the exchange and sends a notification `0x02 ‖ X ‖ Y`.
2. Parse the device public key: `struct.unpack_from("<32s32s", data, 1)` — X and
   Y of **32 bytes little-endian** each, as a point on SECP256R1.
3. The client generates its own key pair (SECP256R1).
4. Compute the shared secret (`ECDH`), **reverse** the bytes
   (`secret.reverse()`), then `SHA256`.
5. Transport key by **XOR fold** of the 32-byte digest:
   `transportKey[i] = digest[i] XOR digest[i+16]` for `i = 0..15`.
6. The client answers with its own public key:
   ```
   0x02 ‖ X(32, little-endian) ‖ Y(32, little-endian) ‖ 0x01
   ```
   (`struct.pack(">B32s32sB", 0x2, x_le, y_le, 0x1)`).
7. The device confirms with a 1-byte notification `0x03` (`len == 1`). A
   different length or type → `ERROR`.

After a successful exchange the `Encryptor` is initialised with the transport
key. If the network has **no** key (classic), the state moves directly to
`AUTHENTICATED`, otherwise to `KEY_EXCHANGED`.

### B.6 Authentication

Only if a key is present. The key with the highest `role` is used (A.5).

**Auth digest:**
```
authDig = SHA256( key.key ‖ nonce ‖ transportKey )
```
(`nonce` = the 16 bytes from B.4; the order is exactly this.)

**Auth packet (plaintext, before encryption):**
```
counter(4, little-endian = 1) ‖ 0x04 ‖ key.id(1) ‖ authDig(32)
```
It is sent encrypted with `_writeEncPacket` (B.8) under nonce ID 1.

**Response:** an encrypted notification; it is decrypted with the nonce
`data[:4] ‖ nonce[4:]` and verified by CMAC. An invalid signature → `ERROR`.
(The additional digest-2 check of the device response is left as a `TODO` in the
source.) Success → `AUTHENTICATED`.

### B.7 Nonce construction

From the 16-byte base nonce and a packet ID (4 bytes):
```
nonce(id) = basenonce[0:4] ‖ id(4, little-endian) ‖ basenonce[8:16]
```
For **incoming** packets the packet ID is taken from the first 4
plaintext/header bytes of the packet: `nonce = data[0:4] ‖ basenonce[4:16]`.

### B.8 Encryption (AES-CTR + AES-CMAC)

The scheme: **encrypt-then-MAC**. The header stays in plaintext, only the
payload behind it is encrypted; a CMAC runs over the whole thing.

`encryptThenMac(packet, nonce, headerLen=4)`:
1. `packet[0:headerLen]` stays plaintext.
2. `packet[headerLen:]` is encrypted with **AES-128-CTR**.
3. An **AES-CMAC** (16 bytes) is computed over `header ‖ ciphertext` and
   appended.

**AES-CTR in detail** (`_encryptInternal`): the keystream is produced block by
block by **AES-ECB** over the counter block; the last 4 nonce bytes (bytes
12–15) are a **little-endian block counter** that counts up from 0 per 16-byte
block, overwriting the bytes 12–15 set by `nonce(id)`. `keystream XOR data`.

**AES-CMAC** (RFC 4493): subkey derivation by left-shifting the AES-encrypted
zero block with Rb = `0x87`; encryption and decryption are symmetric.

`decryptAndVerify(packet, nonce, headerLen=4)`: splits off the last 16 bytes as
the MAC, **always decrypts** (even on a MAC failure — deliberately, against
timing attacks) and verifies the CMAC; on failure the packet is discarded.

### B.9 Outgoing control commands

**Operation codes** (`OpCode`, from `_operation.py`):

| Value | Name |
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

**Operation packet** (`prepareOperation`):
```
struct.pack(">HBHHH", flags, op, origin, target, 0) ‖ payload

flags  = (lifetime & 0xF) << 11 | len(payload)     (2 bytes, BE; lifetime=5)
op     = OpCode                                     (1 byte)
origin = running counter, starts at 1               (2 bytes, BE, wraps at 2^16)
target = target encoding                            (2 bytes, BE)
0x0000                                              (2 bytes, reserved)
payload: max. 63 bytes
```

**Target encoding (`target`):** `(id << 8) | type`. The type values
(unit/group/scene) are not defined as an enum in casambi-bt's BLE layer; this
firmware uses `Unit=0x01`, `Group=0x02`, `Scene=0x04`.

**Send frame** (`send`): the operation packet is wrapped once more:
```
counter(4, little-endian = outPacketCount) ‖ 0x07 ‖ operation packet
```
This frame goes through `encryptThenMac` (nonce ID = `outPacketCount`), then
`outPacketCount += 1`.

### B.10 Incoming packets

casambi-bt decrypts incoming notifications in the `AUTHENTICATED` state (nonce
`data[:4] ‖ nonce[4:]`), silently discards those with an invalid CMAC (no error
state) and dispatches on the **first plaintext byte** (`packetType`):

| Type | `IncomingPacketType` | Handling in casambi-bt |
|---|---|---|
| 6 | `UnitState` | state records → `_parseUnitStates` (B.11) |
| 7 | `SwitchEvent` | switch/sensor events → `parseSwitchEvents` (B.12) |
| 9 | `NetworkConfig` | **deliberately ignored** (the cloud config is taken as authoritative) |
| other | — | ignored ("not implemented") |

> **Important for the comparison with this firmware:** the firmware interprets
> type 7 as an *operation echo* and additionally knows 0x08/0x0A/0x0C —
> casambi-bt does not. Conversely, casambi-bt parses type 7 as a *switch event*.
> That is the biggest substantive divergence between the two implementations.

### B.11 Unit state (type 6) — records and bit decoding

**Record splitting** (`_parseUnitStates`) — pure framing detection, **without**
any meaning for the state bytes:
```
Byte 0: id
Byte 1: flags       online = flags & 2 ; on = flags & 1
Byte 2: stateLen = ((byte2 >> 4) & 15) + 1 ; prio = byte2 & 15
then:   if flags & 4:  +1 byte   (unknown, "con?")
        if flags & 8:  +1 byte   (unknown, "sid?")
        if flags & 16: +1 byte   (unknown)
state = the next stateLen bytes   ← the raw state blob
then:   + ((flags >> 6) & 3) bytes (padding?)
```
The raw `state` blob is passed upwards. Several records per packet are possible.

**Bit-exact decoding** (`Unit.setStateFromBytes`) — the actual meaning, driven
by the cloud controls (A.6). For each control:
```
byteLen = (length + offset % 8 - 1) // 8 + 1
cBytes  = state[offset//8 : offset//8 + byteLen]
cInt    = int.from_bytes(cBytes, "little") >> (offset % 8)
cInt   &= (2**length - 1)
```
and then, per control type:

| Control | Conversion |
|---|---|
| `DIMMER` | `cInt << (DIMMER_RESOLUTION - length)` |
| `VERTICAL` | `cInt << (VERTICAL_RESOLUTION - length)` |
| `WHITE` | `cInt << (WHITE_RESOLUTION - length)` |
| `SLIDER` | `cInt << (SLIDER_RESOLUTION - length)` |
| `TEMPERATURE` | `((cInt / (2**length-1)) * (max-min)) + min` (needs `min`/`max`) |
| `RGB` | `hueLen = length*10//18`; the upper bits → hue, the lower → saturation; stored as HS |
| `XY` | split into two halves: the upper → x, the lower → y (each normalised to 0..1) |
| `COLORSOURCE` | `ColorSource(cInt)` (0=temperature, 1=RGB, 2=XY) |
| `ONOFF` | `cInt != 0` |

The state is therefore fully determined by (offset, length) per control — fields
can be **sub-byte** (e.g. packed RGB). That is precisely why a fixed "one byte
per aux channel" assumption is inadequate.

### B.12 Switch event (type 7) — structure

`parseSwitchEvents` interprets the payload as a sequence of messages (several
per packet are possible). Special case: if the payload begins with `0x29`, it is
**not** a switch event and is ignored.

**Message header** (per message):
```
Byte 0: message_type
Byte 1: flags
Byte 2: length = ((byte2 >> 4) & 15) + 1   (param = the whole of byte 2)
payload = the next length bytes
```
Message types `> 0x80` count as invalid → resync (search one byte further on).

**Relevant types:**
- `0x08`, `0x10` → **button/switch event** (`_processSwitchMessage`).
- `0x00, 0x06, 0x09, 0x1F, 0x2A` → known, but not switch events (log only).
- `0x29` → ignored.

**Button extraction** (reverse-engineered, and correspondingly uncertain):
- Type `0x08`: `button = ((param & 0x0F) + 2) % 4 + 1`; press/release from bit 1
  of `action` (`payload[1]`).
- Type `0x10`: `unit_id = payload[2]`; the state byte at the fixed position 9 of
  the message → `ButtonEventType`.

**Event types** (`ButtonEventType`): `PRESS=0x01`, `RELEASE=0x02`, `HOLD=0x09`,
`RELEASE_AFTER_HOLD=0x0C`, `UNKNOWN=0xFFFF`.

The result per event (`SwitchEvent`): `message_type`, `button`, `unit_id`,
`action`, `event`, `flags`, `extra_data`.

### B.12a Firmware model: INVOCATION frame stream (type 7, untested)

**Status: experimental, UNVERIFIED.** 0x07 has never been observed on the
reference network — there is not a single capture, neither for this model nor
for B.12. The firmware nevertheless implements, as of this change set
(`src/ble/packet_parse.h::parseInvocationStream`), a model that structurally
**contradicts the B.12 theory above**: instead of a 3-byte message header with
resync on an invalid `message_type`, it reads the 0x07 payload as a stream of
INVOCATION frames with a **9-byte header**:

```
Byte 0-1: flags, BIG-endian
           bits 0-5 (0x003F): payloadLen (0-63)
           bit 9    (0x0200): hasOriginHandle — 1 additional byte follows
Byte 2:   opcode
Byte 3-4: origin, BIG-endian
Byte 5-6: target, BIG-endian — (id<<8)|type, as encodeTarget() in the send path
Byte 7-8: age, BIG-endian
[originHandle]        — only if hasOriginHandle
payload[payloadLen]   — 0-63 bytes
```

Frame length = 9 + hasOriginHandle + payloadLen; several frames follow directly
one after another until the payload is exhausted.

**Why this model was implemented despite the lack of verification:**

- The 6-bit payload length (max. 63 bytes) agrees with the note already
  documented in B.9 about the OUTGOING operation packet ("payload: max. 63
  bytes") — independently of the source, an indication for this mask.
- Observation on the reference network: when switching luminaires from the
  Casambi/Occhio app, only 0x06 telegrams appear, never 0x07. The ESP is
  connected as a BLE central to only one mesh node; app operations would only
  show up there as an "echo" if 0x07 generically mirrored every operation of
  every controller. That this never happens argues against the old
  "operation echo" reading and **for** a binding of 0x07 (incoming) to physical
  input hardware (buttons/sensors) — on which B.12 and this model agree, even
  though their byte layouts are incompatible.

**Button stream** (`targetType() == 0x06`, `opcode` 29-36 = ButtonEvent0..7):
`buttonIndex = opcode - 29`; `payload[0]`: bit 7 = pressed, bits 3-6 = `p`,
bits 0-2 = `s`. A label has only been observed/documented for indices 0-3:
`BUTTON_LABELS = {4, 1, 2, 3}`; indices 4-7 stay unlabelled (no guessing).

**NotifyInput stream** (`targetType() == 0x12`, `opcode` 64-71 =
NotifyInput0..7): `inputIndex = opcode - 64`; `payload[0]` = `inputCode`,
`payload[1] & 0x07` = `channel`; from 4 payload bytes on there is additionally a
little-endian `value16` from `payload[2..3]`. Code table:

| Code | Meaning |
|---|---|
| 0x01 | Press |
| 0x02 | Release |
| 0x09 | Hold |
| 0x0C | Release after hold |
| other | unknown (raw) |

**Conflict with B.12 (casambi-bt):** the two theories cannot both be true for
the same bytes (3-byte vs. 9-byte header). Since 0x07 has never been captured,
neither is confirmed. The firmware therefore continues to treat 0x07
**diagnostically/via callbacks only**
(`CasambiClient::setInputEventCallback`/`setRawInvocationCallback`,
`_applyInvocationFrames` in `casambi_client.cpp`) — `_applyUnitStates` is
**never** invoked from 0x07 data, exactly as before under the operation-echo
reading. A real button/sensor capture is needed before either model (or a third)
can count as confirmed.

### B.13 Network config (type 9)

casambi-bt **does not parse type 9** and ignores it deliberately: the assumption
is that the cloud and local configurations agree; on a divergence the user is
expected to use the app. (This firmware, by contrast, decodes 0x09 as a
revision/scene tracker — see the P09 notes in the firmware code.)

---

## Part C — Summary of the cryptography

| Aspect | Value |
|---|---|
| Key exchange | ECDH over SECP256R1 (P-256) |
| Public key encoding | X/Y, 32 bytes little-endian each |
| Shared-secret post-processing | reverse the bytes → SHA-256 → XOR fold (32→16 bytes) |
| Transport key | 16 bytes (AES-128) |
| Data encryption | AES-128-CTR (ECB keystream), block counter LE in bytes 12–15 |
| Integrity | AES-CMAC (RFC 4493), 16 bytes, encrypt-then-MAC |
| Nonce | `base[0:4] ‖ id(4,LE) ‖ base[8:16]`, 16 bytes |
| Auth digest | `SHA-256(key ‖ nonce ‖ transportKey)` |
| Counters | out starts at 2, in at 1; no replay protection on the receiving side |

---

## Part D — The implementation in esp32-casambi (deviations)

The ESP32 firmware implements **the same protocol** as the reference above. The
transport and crypto layers are byte-compatible; the differences lie almost
exclusively in the **acquisition of capabilities** (cloud) and the
**interpretation of incoming packets** (BLE). Sources:
`src/cloud/api_client.cpp`, `src/ble/casambi_client.cpp`,
`src/ble/packet_parse.h`, `src/crypto/*`, `src/config.h`.

**Markers:** `[=]` identical · `[Δ]` different · `[+]` firmware only.

### D.0 Overview of the deviations

| Area | casambi-bt | esp32-casambi |
|---|---|---|
| Capability source | `GET /fixture/{id}` → controls | `GET /fixture/{id}` → controls; the mode-string heuristic only as a fallback `[≈]` |
| Cloud revision | incremental (`revision`, `UPTODATE`) | `revision=0`, always a full fetch `[Δ]` |
| State decoding | bit-exact (offset/length) | control-driven by byte offset (offset/8); sub-byte still open `[≈]` |
| Packet type 7 | switch/sensor event (B.12) | INVOCATION frame stream (B.12a), diagnostics/callbacks only (no state) `[Δ]` |
| Packet type 8/0A/0C | — | UnitState update / TimeSync / keepalive `[+]` |
| Packet type 9 | ignored | P09 revision tracker decoded `[+]` |
| 0x06 online | `flags & 2` | `(flags & 0x0F) == 0` `[Δ]` |
| Device info unit/flags | big-endian | big-endian `[=]` (until #49: little-endian) |
| Device info version | special case for `0x2B` | lower 4 bits masked `[≈]` |
| CMAC | library (`cryptography`) | own RFC 4493 implementation + self-test `[+]` |

### D.1 Cloud (HTTPS/REST)

- `[=]` The same three endpoints, request bodies and the `X-Casambi-Session`
  header (`getNetworkId` / `createSession` / `fetchNetworkConfig`,
  `api_client.cpp`). TLS is **validated** against the embedded Mozilla CA bundle
  (not `setInsecure`, except with `-DCASAMBI_TLS_INSECURE`).
- `[=]` Key selection by the highest `role` (`getBestKey`).
- `[Δ]` **`revision` is fixed at `0`** — the ESP32 pulls the **complete**
  definition on every refresh; casambi-bt's incremental `UPTODATE` logic (A.4)
  does not exist.
- `[≈]` **`GET /fixture/{typeId}` (implemented, `_fetchFixtures`).** After
  parsing the network, the firmware fetches the fixture definition (A.6) for
  each **distinct** unit type, parses its `controls` and derives the
  capabilities from them: `hasVertical` = a `VERTICAL` control is present,
  `hasCCT` = a `TEMPERATURE` control (kelvin bounds from its `min`/`max`). That
  is the **reliable signal** instead of the channel count — and it fixes the
  root cause of issue #34 (Oligo Grace = dimmer + CCT, but a 3-byte mode
  string).
- `[Δ]` **Fallback heuristic** (`_parseUnits`, only when a fixture fetch fails
  or returns no `controls`): `numChannels = len(modes[0].state)/2` (1–3);
  `hasCCT` from `settings["cct.minKelvins"]`; `hasVertical` with 3 channels, or
  with 2 channels without CCT. On every fixture hit the heuristic value is
  logged alongside for comparison (verification).
- `[≈]` **Decoding:** the state bytes are assigned **control-driven** by byte
  offset (state byte `n` → the control with `offset = n·8`), and every control
  value is stored (D.5.2). **Still open** is only the bit-exact unpacking of
  **sub-byte controls** (RGB/XY) — not present in the current inventory.
- `[Δ]` Runtime: the ESP32 works from the configuration **stored** in LittleFS;
  the cloud is only contacted during provisioning/refresh. Plus hard structural
  invariants (duplicate IDs, limits) during parsing.
- `[+]` **Analysis aid:** `debug cloud on` prints, on the next `refresh`, the
  raw network response (`_dumpRedactedConfig`, with AES keys replaced by `***`)
  **and** every raw fixture response (`/fixture/{type}`, unredacted — it
  contains no secrets) to serial, so `modes`/`settings`/`controls` can be
  evaluated per unit.

### D.2 BLE transport & handshake

- `[=]` UUIDs, the state machine, the packet counters (`out=2`, `in=1`), ECDH
  over SECP256R1, transport key derivation (reverse → SHA-256 → XOR fold), the
  public key exchange (`0x02 ‖ X ‖ Y ‖ 0x01`), the `0x03` ack, the auth digest
  and the auth packet (`counter ‖ 0x04 ‖ key.id ‖ digest`) — all identical.
- `[=]` **Device-info endianness:** `unitId`/`flags` are read **big-endian**,
  like casambi-bt (B.4). Until issue #49 the firmware read both fields
  little-endian, which produced 2816 for unit 11 (`00 0b` on the wire). Only the
  diagnostics were affected (both fields are used nowhere else, and the 16 nonce
  bytes were always identical) — which is why it went unnoticed for years.
  Layout and endianness now live as a pure parser in `packet_parse.h`
  (`parseDeviceInfo`, host-tested in `test_packet_parse`).
- `[≈]` **Version 11 special case (`0x2B`):** byte 1 carries the protocol
  version only in the **lower 4 bits**; above that sit undecoded flags
  (`0x2B` = flags `0x2` + version 11). The firmware masks with `0x0F` instead of
  casambi-bt's special case for the specific value `0x2B` — the same fix, more
  general. Previously it compared the whole byte and reported a version mismatch
  on **every** connect: "device reports 43, config has 11". The raw byte is
  still logged (`versionRaw`) so the undecoded upper bits stay visible. The mask
  is the narrowest assumption fitting the observation; a network with version
  ≥ 16 would break it.
- `[+]` After sending its own public key it actively waits for the `0x03` ack
  notification before authenticating.
- `[+]` **Gateway selection by RSSI re-roll** and a **keepalive via GATT read**
  on the auth characteristic — operational logic with no protocol counterpart in
  casambi-bt.

### D.3 Cryptography

- `[=]` AES-128-CTR (ECB keystream, block counter LE in bytes 12–15), AES-CMAC
  (RFC 4493), encrypt-then-MAC, `headerLen=4`, nonce construction (B.7/B.8).
- `[+]` **Its own CMAC implementation** (the mbedTLS CMAC is not enabled in the
  core) with an **RFC 4493 self-test**, a **constant-time MAC comparison** and
  **wiping** of the intermediate secret/hash. casambi-bt uses the `cryptography`
  library.
- Both: **no** replay/counter check on the receiving side.

### D.4 Outgoing control commands

- `[=]` The opcodes are identical (B.9). The operation packet is byte-identical
  (`flags(2,BE)=lifetime<<11|len ‖ op ‖ origin(2,BE) ‖ target(2,BE) ‖ 0x0000 ‖
  payload`), `lifetime=5`, send frame `counter(4,LE) ‖ 0x07 ‖ operation`
  (`_buildOperation` / `_sendOperation`).
- `[=]` Target encoding `(id<<8)|type`; the firmware defines the type values
  explicitly: `Unit=0x01`, `Group=0x02`, `Scene=0x04`.
- `[+]` **Rollback of `origin`/`outPacketCount`** on a failed GATT write, so the
  nonce sequence does not drift (casambi-bt increments unconditionally).
- `[Δ]` **The firmware's payload encoding:** `SetTemperature` sends `kelvin/50`
  (1 byte); `SetColor` converts RGB→hue/saturation (`rgbToHS`, hue 0–1023 as
  2 bytes LE + saturation). The outer operation frame is identical; casambi-bt
  builds these specific payload formats in its higher layer (not in
  `_operation.py`) and they have **not** been cross-checked here.
- `[+]` **`SetState` (48) as a write path — verified on hardware** (Oligo Grace,
  two independent 8-bit dimmers + a temperature byte; the insight came from
  PR #39). Semantics:
  - The payload is the unit's **complete state blob** — the same layout the
    fixture definition (`/fixture/{type}`, A.6) describes through its controls
    (offset/length) and that arrives inbound in 0x06 records (`stateLength`
    bytes).
  - A `SetState` write sets **all** controls to the transmitted values; there
    are no partial writes. **Bytes not written along (zeroed) reset the
    respective control** — observed on the temperature byte, which fell to 0
    when the current value was not preserved. Senders must therefore always
    build the blob from the current control values and overwrite only the
    desired controls.
  - Only this way can several channels be changed **atomically** in one telegram
    (e.g. both dimmers of an uplight/downlight luminaire) — that is not possible
    with `SetLevel`/`SetVertical`, and two-dimmer fixtures do not implement
    `SetVertical` at all.
  - Implementation: `CasambiClient::setUnitState()` + the pure encoder
    `src/cloud/state_codec.h` (host-tested, `test/test_state_codec`); on the
    REST side `POST /api/units/:id/state`. Byte-aligned 8-bit controls are
    verified; 16-bit is encoded little-endian (by analogy with hue in
    `SetColor`), and the encoder explicitly rejects sub-byte layouts rather than
    guessing an untested bit order.

### D.5 Incoming packets — the biggest divergence

Dispatch on the first plaintext byte (`_handleDataNotification`):

| Type | casambi-bt (B.10) | esp32-casambi |
|---|---|---|
| 0x06 | UnitState (bit-exact) | status broadcast, `[=]` same framing (D.5.1), control-driven by byte offset `[≈]` |
| 0x07 | **switch event** (3-byte header, B.12) | INVOCATION frame stream (9-byte header, B.12a), **diagnostics/callbacks only** `[Δ]` |
| 0x08 | — | UnitState update (pair or 0x06 format) `[+]` |
| 0x09 | ignored | P09 revision/scene tracker (debug) `[Δ]` |
| 0x0A | — | TimeSync — undecoded, the payload is dumped `[+]` |
| 0x0C | — | keepalive — undecoded, the payload is dumped `[+]` |

- `[Δ]` **Type 7 is the weightiest difference:** casambi-bt reads it as a
  switch/sensor event with a 3-byte message header (B.12). The firmware
  originally decoded it as an **echo of an operation** (the same structure as an
  outgoing operation, an 11-bit payload length) — an unsupported symmetry
  assumption that was never verified against a real incoming 0x07 (never
  observed on the network). As of this change set the firmware instead
  implements a more detailed, likewise unverified model (B.12a: a 9-byte header,
  button/NotifyInput frames, a 6-bit payload length) that collides structurally
  with B.12 but agrees with it substantively that 0x07 is tied to physical input
  hardware, not to arbitrary operations. The firmware **still deliberately
  applies no state from it** (only classifying/logging/callbacks + a
  `malformed07` counter) — every real change arrives as a 0x06 anyway, and a
  misinterpreted event must not be able to inject a false level. A real
  button/sensor capture is needed before either model can count as confirmed.

**D.5.1 Record format for type 0x06 — by now aligned with casambi-bt.**
Originally the two implementations diverged at byte 2: casambi-bt reads
`stateLen=(b2>>4)+1`/`prio=b2&15`, while esp32-casambi treated the same byte as
an ad-hoc "capability" (upper nibble = the aux count, lower nibble `0x00`/`0x03`
with a special case for a fixed `0x80` constant byte when `cap==0x03` exactly).
For every byte pattern observed up to that point, both readings happened to
compute the same record length (for `cap=0x23`, for instance, both gave 6 bytes
— the "malformed/one byte too short" assumption from issue #34 held in
**neither**) — the **semantics** nevertheless differed, particularly for
`online`.

Two targeted captures (build 2026-08-04, `debug parse on` + `debug ble on`): a
manual sweep across 9 units/5 fixture types over the full dimmer/vertical/
temperature value range, plus a second capture with a real mains power cycle —
the one situation that appeared in no previous capture. Result across 56 real
records: **0 differing record lengths** between the two readings. The firmware
was then switched to the documented framing (`src/ble/packet_parse.h`,
`UnitStateRecord`); the old capability heuristic has been removed. Raw data and
analysis: `docs/captures/2026-08-04-0x06-framing/`.

| | casambi-bt | esp32-casambi (now) |
|---|---|---|
| Byte 2 | `stateLen=(b2>>4)+1`, `prio=b2&15` | `[=]` identical |
| Optional bytes | `flags & 4/8/16` (con/sid/extra, +1 each) | `[=]` identical |
| `on` | `flags & 1` | `[=]` identical — passed through verbatim, no `level` derivation any more |
| `online` | `flags & 2` | `[=]` identical |
| Padding | `(flags>>6)&3` | `[=]` identical |

**What is hardware-verified and what is not** (across all captures, historical
and new):

- `priority` (byte 2, lower nibble) was **always 3** — regardless of fixture
  type, source of the change or online/offline transition. Whether the value
  ever varies cannot be observed on this network; the full 0–15 range is
  implemented nonetheless, not just the observed value.
- `con` (flag bit 2) only occurred on single-dimmer fixtures (type 1422), always
  with the value `0x80` — at exactly the position and with exactly the value the
  old heuristic already skipped as a "constant byte". `sid` (bit 3) was never
  observed set. Whether the two can occur independently is open.
- Padding (bits 6–7) was 0 in all captures.
- `on`/`online` were identical in **every** one of the 56 records — including
  the two real offline transitions in the second capture. On this network bit 0
  therefore carries no information beyond bit 1; in particular it does NOT
  reflect "the luminaire is currently dark" (when dimming explicitly to 0 the
  `on` bit stayed set as long as the unit was online).
- **But:** the old `online` heuristic (lower nibble of `flags` ≠ 0) is
  demonstrably wrong on real offline transitions — two cases in the second
  capture (`flags=0x04`, `0x14`) had a non-zero lower nibble even though the
  unit was losing power; the bit-exact reading correctly reports `online=false`
  there. The old `on` (`level>0`) carried the last known, no longer valid
  brightness value forward in the same cases. The change therefore fixes a real,
  reproducible bug, not just a documentation divergence.

**A deliberate decision: no semantic interpretation in the firmware.**
`on`/`online` are passed through unchanged — whether, say, "on but level=0"
counts as "on" or "off" for a dashboard is decided by the application layer
(FHEM or similar), not by this firmware. FHEM's `CasambiUnit_UpdateFromState`
already has a derivation of its own for multi-channel dimmers ("any channel
> 0"); that stays unchanged.

**D.5.2 State semantics.** `[=]` Since the change, the protocol layer
(`parseStatusBroadcast`) delivers the full, uninterpreted state blob
(`UnitStateRecord::state`, 1–16 bytes depending on `state_len`) instead of the
former 3 position-based fields (`level`/`aux1`/`aux2`); the **meaning** is still
assigned **generically from the fixture controls** in the unit model
(`_applyUnitStates`), but now by bit offset across the complete blob
(`state_codec::decodeControl`, no longer only the first 3 bytes): state byte `n`
→ the control with `offset = n·8`, and that control's `typeName`
(dimmer/vertical/temperature/…) determines the target and the naming. That lifts
both the fixed "aux1→vertical" assumption (aux1 is temperature on unit 5 and
vertical on unit 7, for instance) and the old 3-byte limit — a fixture with more
than 3 controls no longer loses bytes. Every control value is stored for the
generic API; in addition the complete raw blob is kept per unit
(`CasambiUnit::rawState`/`rawStateLen`), so unknown/future controls can be
inspected without a parser change. Sub-byte fields (RGB/XY) are not yet
bit-unpacked — they do not occur in the current inventory.

> **State of the alignment.** Implemented: the `/fixture/{id}` fetch (D.1) with
> storage + persistence of the controls (offset/length/min/max/stateLength),
> **control-driven, cloud-derived decoding** (`_applyUnitStates`) now over the
> full state blob instead of only 3 bytes, a **generic API** (a `controls` array
> per unit) and **FHEM** readings whose names come from the cloud control types.
> Golden-vector tests (`test/test_packet_parse`) freeze both the historical
> Occhio captures and the new multi-fixture/offline captures byte for byte. Also
> implemented: the **generic write path** via `SetState` (D.4) —
> `POST /api/units/:id/state` addresses controls by name and writes the complete
> state blob atomically; FHEM drives multi-channel dimmers (e.g. Oligo Grace
> uplight/downlight) through it without fixture-specific code. Still open: only
> the bit-exact unpacking/encoding of sub-byte controls (RGB/XY) and Homebridge
> mappings for new control types.

### D.6 Endianness — the complete picture

Prompted by issue #49. Since the protocol uses **both** byte orders and a
mistake here stays silent (the affected fields were purely diagnostic), here are
all of the firmware's multi-byte fields at a glance. The underlying rule:
**transport layer little-endian, handshake and operation layer big-endian.**

| Field | Location | Order | Evidence |
|---|---|---|---|
| Packet counter (4 B, header + nonce) | `_sendOperation`, `_getNonce`, `_handleAuth-/DataNotification` | LE | B.7/B.8; auth works |
| AES-CTR block counter (bytes 12–15) | `encryption.cpp` | LE | B.8 |
| ECDH public key X/Y (32 B each) | `key_exchange.cpp` | LE (reversed) | B.5; the handshake works |
| Device info `unitId`, `flags` | `parseDeviceInfo` | **BE** | casambi-bt `>BHH16s` (B.4); #49 |
| Operation `flags`, `origin`, `target` (out) | `_buildOperation` | BE | D.4, byte-identical to casambi-bt |
| INVOCATION `flags`, `opcode`, `origin`, `target`, `age` (in, 0x07) | `parseInvocationStream` | BE | B.12a — **unsupported**, see below |
| `SetColor` hue (2 B) | `setUnitColor` | LE | D.4, not cross-checked |
| `SetState` 16-bit controls | `state_codec.h` | LE | by analogy with hue; **no** 16-bit control observed so far |

**Result of the search for further instances.** Apart from the device info, the
review turned up no more plausible reading anywhere:

- **Transport and crypto** are proven by the working handshake — a wrong order
  there would never have permitted an authentication.
- **Outgoing operations** are byte-identical to casambi-bt (D.4) and verified on
  hardware.
- **0x06/0x08/0x09** contain no multi-byte field at all: 0x06/0x08 are byte
  streams, and 0x09 consists of 3-byte records with individually interpreted
  bytes. There is nothing to flip here.
- **0x07 incoming** does have BE fields, but the reading as an INVOCATION frame
  (B.12a, as previously as an "operation echo") is itself unsupported (D.5) and
  no state is applied. An endianness change would be guessing on an unverified
  base structure — worth doing only with a real capture.
- **`SetColor` / `state_codec`** are LE by analogy. Neither has been
  cross-checked on hardware, but there is no counter-evidence either: no fixture
  seen so far has a 16-bit control, and RGB is untested (README, "Untested
  Features"). Without an RGB luminaire that stays open; a change here would not
  be a correction but a second guess.

**The most promising place to look is the keepalive response.**
`sendKeepalive()` reads over GATT from the auth characteristic and checks only
the **length** — typically 25 bytes, i.e. a complete encrypted packet (4-byte
counter header + plaintext + 16-byte CMAC), hence **1 type byte + 4 payload
bytes**. That is the only place in the firmware where protocol bytes arrive and
are discarded unread — and a 4-byte field is exactly the shape in which the
endianness question arises. The bytes are now dumped under `debug ble on`/`debug
parse on`. **Decrypting** them would need the same path as a notification
(`data[0:4] ‖ basenonce[4:16]`); whether the counter/direction convention of a
read response is the same is unverified — a CMAC failure would be
inconsequential, but it is a change of its own with its own risk and needs a
capture first.

**0x0A/0x0C as a notification were not testable** — both types were recognised,
but theirs were the only payloads **never printed** (the `default` branch dumps
every genuinely unknown type; these two did not). That made it impossible to
take a capture with which the question could even be answered. Both now dump
their bytes; for 0x0A, `debug parse on` additionally prints the two readings of
a leading 32-bit field against the obvious "Unix epoch" hypothesis and marks the
plausible one. A single capture then settles the question. **Nothing is
applied** — the firmware's clock comes from NTP.

In a 100 s capture on the real network (build 2026-08-04, `debug parse on` +
`debug ble on`), **not a single 0x0A or 0x0C appeared as a notification**; only
0x06 broadcasts and the keepalive **read** were seen. The 0x0C branch in
`_handleDataNotification` may therefore be dead code — the keepalive response
does not arrive over the notification path.

---

## Known gaps / open points (from casambi-bt itself)

- Incoming packet counters and the direction flag are not checked (replay).
- Digest 2 of the auth response is not cross-checked.
- The version 11 handshake (`0x2B`) is merely "waved through", not understood.
- Switch-event parsing (type 7) is heavily reverse-engineered (button formulas,
  a fixed state byte position) and carries many `TODO`s.
- Optional bytes in the unit state record (`flags & 4/8/16`, con/sid/extra):
  position and length are now implemented and hardware-observed for `con`/
  `extra` (see D.5.1), but their **content** remains uninterpreted; `sid` was
  never observed set. `priority` (byte 2, lower nibble) was 3 in every capture —
  whether the value ever varies is open. Padding was never observed.
- Network config (type 9), `managerKey`/`visitorKey` for classic networks and
  further network fields are not parsed.
