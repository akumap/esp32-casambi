# Concept: Versioning of the interfaces (issue #29)

Status: implemented (ESP firmware + FHEM module + tests + docs, on this branch).
Issue: [#29](https://github.com/akumap/esp32-casambi/issues/29)
Branch: `claude/issue-29-solution-6su80s`

## 1. Goal

Both interfaces of the system are to be versioned and monitored:

1. **Casambi network ↔ ESP32**: The Casambi network version is outside our
   control. If it falls outside the range the ESP supports, a **warning** shall
   be shown (no blocking).
2. **ESP32 ↔ FHEM**: The REST/WebSocket interface gets an explicit
   **major/minor version**. Minor = backwards/forwards-compatible extension,
   major = incompatible change (one side has to be updated).
3. **Display**: All version levels (Casambi network version, the range supported
   by the ESP, the ESP's API version, the FHEM module's API version) are made
   visible as **FHEM readings**; on a mismatch there are warning readings.

The guiding principle in all cases: **fail-operational** — deviations are
reported prominently, but operation continues. This matches the firmware's
existing tolerant design (see `config.h:23-30`).

## 2. Starting point (as-is)

### 2.1 Casambi ↔ ESP32 — detection present, display missing

- `config.h:29-30` defines `MIN_PROTOCOL_VERSION 10` /
  `MAX_PROTOCOL_VERSION 11`.
- `checkCasambiVersions()` (`main.cpp:900`) warns at boot and after every cloud
  refresh if `protocolVersion` lies outside the range; in addition
  `config_store.cpp` (load/save) and `casambi_client.cpp:517` (on-air mismatch)
  warn.
- **Gap:** All warnings end up on the serial console only. Neither the network
  version nor the supported range nor the warning is exposed through the API —
  FHEM cannot display any of it.

### 2.2 ESP32 ↔ FHEM — only implicit versioning via the build number

- `FIRMWARE_BUILD` (`git rev-list --count`, injected by
  `scripts/build_number.py`) is sent along in `/api/info` and the WebSocket
  hello.
- FHEM checks it against `MIN_FIRMWARE_BUILD` (`98_CasambiGW.pm:69`) and sets
  the readings `esp32Build` / `esp32BuildWarning`.
- **Gap:** The build number is an *implementation* version, not an *interface*
  version — it increases with every commit and says nothing about
  compatibility.

## 3. Delimiting the version concepts

| Version | Question it answers | Source | stays/new |
|---|---|---|---|
| `FIRMWARE_BUILD` | "Which firmware revision is running?" | git commit counter | stays unchanged |
| Casambi protocol version | "Which version does the network speak?" | cloud config | stays (additionally exposed) |
| `MIN/MAX_PROTOCOL_VERSION` | "What has the ESP been tested against?" | `config.h` | stays (additionally exposed) |
| **ESP↔FHEM API version** | "Do the ESP and FHEM understand each other?" | new constant on both sides | **new** |

The build-number check remains in place alongside; it answers a different
question than the API version.

## 4. Part A: Casambi ↔ ESP32

Detection already exists in full (2.1); **only the visibility** changes. The
WebSocket hello (`_buildHelloMessage()`, `webserver.cpp:348`) is extended by
three fields:

```json
{
  "type": "hello",
  "casambi_protocol_version": 11,
  "casambi_protocol_min": 10,
  "casambi_protocol_max": 11,
  ...
}
```

Rationale for the transport: the hello reaches **authenticated** WebSocket
clients only. `/api/info` is deliberately unauthenticated and reduced to two
fields (`webserver.cpp:517-521`) — details of the Casambi network do not belong
there. That data-minimisation decision stays untouched.

The evaluation (inside/outside the range) is done by **FHEM** itself from the
three numbers (see 6). The ESP sends facts only — that way the warning logic
does not exist twice, and the firmware's serial warnings stay unchanged.

## 5. Part B: ESP↔FHEM API version (major/minor)

### 5.1 Definition and semantics

**One** shared version for REST **and** WebSocket — both live in the same
firmware revision and the same FHEM module; separate versions would be
maintenance effort without benefit.

- **Minor +1**: compatible extension (new field, new endpoint, new WS message
  type). Both sides must continue to ignore unknown fields/messages (which they
  already do today).
- **Major +1**, minor reset to 0: incompatible change (field removed/renamed,
  semantics changed, auth changed). One side has to be updated.

**Initial value: 1.0.** A **missing** version field (older firmware) is
**interpreted as 1.0** by FHEM — this resolves the chicken-and-egg problem (the
version field is itself an interface extension) without special-casing: firmware
predating this concept *is* interface level 1.0.

### 5.2 Where it lives and the maintenance rule

ESP side, in `config.h` next to the existing version constants:

```c
// ESP32 <-> FHEM interface version (REST + WebSocket, one shared version).
// Contract: bump MINOR for backwards/forwards-compatible extensions (new
// fields, endpoints, WS message types); bump MAJOR (and reset MINOR to 0)
// for incompatible changes. MUST be kept in sync with the constants in
// FHEM/98_CasambiGW.pm. A missing field on either side means 1.0.
#define FHEM_API_VERSION_MAJOR    1
#define FHEM_API_VERSION_MINOR    0
```

FHEM side, in `98_CasambiGW.pm` next to `MIN_FIRMWARE_BUILD` (the same comment
contract, mirrored):

```perl
use constant API_VERSION_MAJOR => 1;   # keep in sync with src/config.h
use constant API_VERSION_MINOR => 0;
```

Since the ESP firmware and the FHEM module live in the same repository, the
maintenance discipline ("count up on every interface change") is the actual risk
factor. The comment contract on both constants plus a note in the README is the
pragmatic route here; CI tooling for it is not (yet) worth it.

### 5.3 Transport

Two places send the version as **integer fields** (numerically comparable, no
string parsing):

1. **`/api/info`** (unauthenticated, `webserver.cpp:522`): additionally
   `"api_version_major": 1, "api_version_minor": 0`. Harmless (reveals nothing
   about the network) and useful, because it lets FHEM know about compatibility
   **before** the WebSocket is even established. The setup portal
   (`setup_portal.cpp:237`) sends the same fields so that `/api/info` has the
   same shape in both modes.
2. **WebSocket hello** (`webserver.cpp:348`): the same two fields — the hello is
   what the reading maintenance in FHEM builds on.

### 5.4 Check rules in FHEM

Evaluated in `CasambiGW_HandleHello` (`98_CasambiGW.pm:645`), analogous to the
existing build-number check:

| Situation | Behaviour |
|---|---|
| Field missing | interpret as 1.0, then run the normal check |
| ESP major ≠ FHEM major | `apiVersionWarning` with plain text ("incompatible, update …"), log level 2; **operation continues** (fail-operational) |
| Same major, different minor | no warning reading (`apiVersionWarning: ok`), log level 4 only — a minor difference is compatible by definition, no matter which side is newer |
| Versions equal | `apiVersionWarning: ok` |

Deliberately **no** connection refusal on a major mismatch: a degraded but
functioning system with a clear warning is easier for the user to diagnose than
a dead one.

## 6. FHEM readings (part C)

New and existing readings on the gateway device:

| Reading | Content | Source | new? |
|---|---|---|---|
| `casambiProtocolVersion` | e.g. `11` | hello `casambi_protocol_version` | new |
| `casambiProtocolVersionRange` | e.g. `10-11` | hello `casambi_protocol_min/max` | new |
| `casambiVersionWarning` | `ok` or plain text | computed by FHEM | new |
| `espApiVersion` | e.g. `1.0` | hello `api_version_major/minor` | new |
| `fhemApiVersion` | e.g. `1.0` | constant in the module | new |
| `apiVersionWarning` | `ok` or plain text | computed by FHEM (5.4) | new |
| `esp32Build` | commit counter | hello `build` | existing |
| `esp32BuildWarning` | `ok` or plain text | existing | existing |

`casambiVersionWarning` is set when `casambiProtocolVersion` lies outside
`casambiProtocolVersionRange` — with different text for "too old" (network below
the minimum) and "newer than tested" (above the maximum), analogous to the
serial warnings in `checkCasambiVersions()`.

That makes every revision level demanded by the issue visible: Casambi network
version, ESP Casambi min/max, ESP API version, FHEM interface version — plus
warnings on a mismatch.

## 7. Scope of changes

| File | Change |
|---|---|
| `src/config.h` | `FHEM_API_VERSION_MAJOR/MINOR` + comment contract |
| `src/web/webserver.cpp` | hello: 5 new fields (5.3, 4); `/api/info`: 2 new fields |
| `src/web/setup_portal.cpp` | `/api/info`: the same 2 fields |
| `FHEM/98_CasambiGW.pm` | constants, evaluation + readings in `HandleHello`, helper function for the version comparison, commandref section |
| `FHEM/t/CasambiGW_helpers.t` | tests for the comparison helper (see below) |
| `README.md` | document the version contract and the new fields (the REST/WS interface is specified in the README, not in the protocol reference — that one covers the BLE protocol) |

The version comparisons in FHEM are cut as **pure Perl helper functions**
without a FHEM runtime dependency (e.g.
`CasambiGW_ApiVersionWarning($espMajor,$espMinor)` and
`CasambiGW_CasambiVersionWarning($ver,$min,$max)`, returning `"ok"` or warning
text), so that they are directly testable in the existing test file
`CasambiGW_helpers.t` — the same pattern as the percent/byte helpers there.

This concept is itself a **minor extension**: it only adds fields. The version
stays at the initial value **1.0** (the new fields *are* the definition of 1.0;
firmware without the fields is likewise read as 1.0 and is compatible with it).

## 8. Test concept

1. **Unit tests (Perl):** comparison helpers with the cases from 5.4 and 6
   (missing→1.0, major mismatch in both directions, minor difference in both
   directions, Casambi version below/above/inside the range).
2. **Old→new compatibility:** new FHEM module against old firmware (without the
   version fields) → readings show `1.0`, `apiVersionWarning: ok`, no errors in
   the log.
3. **Manual:** new firmware + new module → all readings populated; as a one-off
   test, change `API_VERSION_MAJOR` in FHEM → the warning appears and control
   keeps working.

## 9. Non-goals

- **No blocking** on a version mismatch (neither on the Casambi nor the API
  side) — warnings only.
- **No separate versions** for REST and WebSocket.
- **No CI enforcement** of version maintenance — the comment contract suffices
  given the current repository layout (both sides in one repo).
- **No change** to the existing build-number check or to the data minimisation
  of `/api/info` (network details stay reserved for authenticated clients).
