# Concept: Apple Home / HomeKit — footprint estimate

Status: **closed — not pursued.** A pure feasibility and footprint analysis;
nothing was implemented and nothing is planned. The Apple Home integration
already runs via the Raspberry Pi (variant A, section 3); native HAP on the
ESP32 (variant B) is **rejected**, given the heap requirement determined here
and the web-server blocker from 4.4. The document is kept as the basis for that
decision — should the question come back later with different hardware (an
ESP32-S3 with PSRAM), the numbers and the measurement instructions (section 6)
are ready.

Answers the question "How large would the footprint (flash and heap) be for a
Homebridge that I can add to Apple Home?"
Branch: `claude/homebridge-apple-home-footprint-gnouc8`

## 0. Short answer

The question has two very different answers depending on **where** the bridge
runs:

| Variant | Flash on the ESP32 | Heap on the ESP32 | Feasibility on the ATOM Lite |
|---|---|---|---|
| **A — Homebridge on the Raspberry Pi**, a plugin talks to the existing REST/WS interface | **0 B** | **0 B** | ✅ immediately, without a firmware change |
| **B — native HAP on the ESP32** (HomeSpan), the device itself appears as a bridge in Apple Home | **+150–250 KB** (uncritical) | **+40–60 KB permanently**, peaks up to ~75 KB | ❌ does not fit the heap budget; plus an architectural blocker (section 4.4) |

**Flash is not a problem in either variant.** The bottleneck in variant B is the
heap — and not marginally so, but roughly a factor of 2 too small if the web UI,
WebSocket push and BLE stack are to be retained.

> Origin of the numbers: the as-is state (section 1) is measured in this repo.
> The HomeSpan numbers are partly measured by others (flash) and partly derived
> from the HomeSpan source (heap, section 4.2) — **not** re-measured on this
> hardware. Section 6 describes how to verify them in about an hour.

## 1. Starting point (measured)

### 1.1 Flash

| Segment | used | capacity | free |
|---|---|---|---|
| App (`huge_app`, `app0` = 0x300000) | 1,596,065 B (1.52 MiB) | 3,145,728 B (3 MiB) | **1.48 MiB** |
| static RAM (linker) | 59,572 B | 532,480 B | — |

(README, section "Tested Hardware", environment `devkit-v4`.)

### 1.2 Heap at runtime

From `docs/concept-asynctcp-churn-stability.md` (stress-test runs on real
hardware, values from `/api/status`):

| Situation | `free_heap` | `largest_block` |
|---|---|---|
| Idle / after cooldown | 90–96 KB | 37–83 KB |
| Under mixed load (HTTP + WS) | 37–58 KB | 7–27 KB |
| `min_free_heap` during the stress test | **7–16 KB** | — |

Plus the hard limits from `src/config.h`:

- `HEAP_CRITICAL_THRESHOLD` = 20,000 B → undercut 3× in a row ⇒ **reboot**
- `WS_SEND_HEAP_MARGIN` = 4,096 B, `HTTP_EXPENSIVE_GET_HEAP_FLOOR` = 8,192 B
  (admission control, so expensive responses are never even started)
- no PSRAM on the ATOM Lite (ESP32-PICO-D4)

The **operationally available margin** is therefore not "90 KB" but roughly
**30–40 KB above the reboot threshold under load** — and HAP would have to fit
into exactly that margin.

## 2. What "Homebridge" can mean here

1. **Homebridge** (Node.js, github.com/homebridge/homebridge) — runs on a
   machine on the LAN, presents itself to Apple Home as a HAP bridge and
   translates to arbitrary APIs. The ESP32 stays unchanged as a REST/WS device.
2. **A native HomeKit bridge on the ESP32** — the firmware implements HAP itself
   (HomeSpan, esp-homekit-sdk), the device appears directly in Apple Home, no
   intermediate machine.

Both produce the same picture in the Home app (one bridge with N lights). The
footprint differs fundamentally.

## 3. Variant A — Homebridge on the Pi

**ESP32 footprint: 0 flash, 0 heap.** The firmware needs no line of change;
everything a HomeKit bridge requires is already there:

| HomeKit needs | existing interface |
|---|---|
| device list + capabilities | `GET /api/units` (`controls[]`, `hasCCT`, `hasVertical`, `numChannels`, `cctMin/Max`) |
| stable identity (a UUID per accessory) | `uuid` or `address` in the `hello` snapshot |
| state without polling | WebSocket `/ws`: `hello` + `unit_state` on every change, < 100 ms |
| switching/dimming | `POST /api/units/:id/on\|off\|level\|temperature\|vertical` |
| atomic multi-channel set | `POST /api/units/:id/state` (API ≥ 1.1) |
| reachability | `online` per unit, `connection_state` (BLE link) |
| auth | `X-API-Key` = SHA-256(`casambi-api:` + password), for WS also `?k=<token>` |

### 3.1 Mapping onto HomeKit characteristics

| Casambi | HomeKit | Conversion |
|---|---|---|
| `on` | `On` (Lightbulb) | 1:1 |
| `level` 0–255 | `Brightness` 0–100 % | `round(level / 255 * 100)`, back `round(pct / 100 * 255)` |
| `temperature` (`colorTemp` 0–255 + `cctMin/cctMax`) | `ColorTemperature` in **mired** 140–500 | `kelvin = cctMin + colorTemp/255*(cctMax-cctMin)`; `mired = 1e6/kelvin`; derive the HomeKit range from `cctMin/cctMax` (`setProps({minValue, maxValue})`) — otherwise the Home app shows a control range the luminaire cannot do |
| `vertical` | no HomeKit counterpart | pragmatically: a second `Lightbulb` ("… indirect") with `Brightness`, or `Fan` `RotationSpeed`; the Home app does not display custom characteristics |
| several `dimmer` channels (Oligo Grace) | two `Lightbulb` services in one accessory | write them bundled via `/state`, otherwise the luminaire resets the respective other channel |
| `online: false` | `SERVICE_COMMUNICATION_FAILURE` | report an error state instead of "off" |

### 3.2 Effort and footprint on the Pi

- Plugin: ~300–500 lines of TypeScript (`homebridge` + `ws`), one WS connection
  for all accessories, `updateValue()` on `unit_state` → no polling load.
  Note: the ESP32 allows **3 simultaneous WS clients** (`WS_MAX_CLIENTS`); FHEM
  already occupies one.
- Debouncing while dimming belongs in the plugin (the Home app fires many
  `Brightness` writes while dragging the slider) — coalesce them and send one
  BLE telegram via `/state`.
- Pi resources: Homebridge itself ~80–150 MB RSS (Node.js), the plugin ~1 MB.
  Irrelevant on a Pi, which already hosts the build/FHEM environment anyway.
- It also works without writing anything: generic HTTP plugins or Home
  Assistant's HomeKit bridge integration — though then without the push path,
  i.e. with polling and a sluggish status display.

## 4. Variant B — native HAP on the ESP32 (HomeSpan)

The reference implementation is
[HomeSpan](https://github.com/HomeSpan/HomeSpan) (Arduino-ESP32, MIT).
Espressif's `esp-homekit-sdk` is pure ESP-IDF and does not fit this project's
Arduino framework build; `Arduino-HomeKit-ESP32` is explicitly unmaintained.

### 4.1 Flash: +150–250 KB — uncritical

The most solid external number: in
[HomeSpan issue #591](https://github.com/HomeSpan/HomeSpan/issues/591) a user
reports **1,244,957 B without** and **1,430,061 B with** `homeSpan.poll()` for
the same sketch — the difference of **185,104 B** is practically exactly the
incremental HAP code (HAP state machine, TLV8, SRP-6A, HKDF, the characteristic
tables plus libsodium for Ed25519/Curve25519/ChaCha20-Poly1305), since
everything else is optimised away without the poll call.

| Item | rough |
|---|---|
| HomeSpan core + libsodium + mbedTLS additions (SHA-512, bignum) | ~150–200 KB |
| Casambi→HAP adapter (accessory construction, update callbacks, mapping) | ~10–20 KB |
| possibly porting the web server to the synchronous `WebServer` library (4.4) | ±0, more likely −20 KB |
| **Total** | **~150–250 KB** |

Result: **1.52 MiB → ~1.75 MiB of 3 MiB**, still ~1.2 MiB free. With `huge_app`,
flash is simply a non-issue. (The usual HomeSpan pitfall "does not fit in
1.3 MB" concerns the default partition scheme — this project already uses
`huge_app.csv`.)

### 4.2 Heap: +40–60 KB permanently — the actual bottleneck

Derived from the HomeSpan source (`src/HAP.h`, `src/HomeSpan.h`, `src/SRP.h`, as
of master):

**a) Base load, independent of network size**

| Item | Evidence | rough |
|---|---|---|
| `HapOut` stream buffers (plaintext + encrypted, 1 KB each) | `HAP.h:174 bufSize=1024` | ~2 KB |
| controller list (up to `MAX_CONTROLLERS=16`, LTPK + ID per entry) | `HAP.h:91` | ~1–2 KB |
| mDNS advertising with HAP TXT records | ESPmDNS | ~2–4 KB |
| HomeSpan internals (config, SHA-384 hash, NVS caches, lists) | — | ~2–4 KB |
| **Base load total** | | **~8–12 KB** |

**b) Accessory database — scales with the number of luminaires**

Each luminaire becomes one accessory made of `SpanAccessory` +
`AccessoryInformation` (6 characteristics) + `LightBulb` (`On`, `Brightness`,
`ColorTemperature`). `SpanCharacteristic` is ~112 B (`HomeSpan.h:627 ff.`: 3×
`UVal` for min/max/step, `UVal` value + newValue, 6 pointers, `EVLIST` vector) —
with the malloc header, service vectors and the heap-copied name strings you end
up at:

| | per luminaire |
|---|---|
| SpanAccessory + 2 services incl. `req`/`opt` vectors | ~0.4 KB |
| 9–10 characteristics at ~124 B | ~1.2 KB |
| strings (name, manufacturer, model, serial number, firmware) | ~0.15 KB |
| **Total** | **~1.5–2 KB** |

A luminaire with `vertical` (a second Lightbulb service) or two dimmers costs
~0.5 KB more.

| Network size | Accessory DB |
|---|---|
| 5 luminaires | ~8–10 KB |
| 10 luminaires | ~15–20 KB |
| 20 luminaires | ~30–40 KB |
| 30 luminaires | ~45–60 KB |

**c) Per HAP connection**

By default HomeSpan keeps 8 connection slots (`setMaxConnections()`).
Realistically 1–3 are permanently open (home hub, HomePod/Apple TV) plus
transient ones from iPhones/iPads.

| Item | Evidence | rough |
|---|---|---|
| `HAPClient` + session keys (a2c/c2a, Curve25519) | `HAP.h:107–116` | ~0.3 KB |
| lwIP socket (PCB, pbufs, send window) | ESP-IDF defaults | ~1–2 KB idle, 5–6 KB during transfer |
| HTTP request buffer, `TempBuffer<uint8_t> httpBuf` | `HAP.h:90 MAX_HTTP=8096`, `HAP.cpp:123` | up to 8 KB **transient** |
| **per active controller** | | **~2 KB idle, 6–12 KB while in use** |

**d) One-off peaks**

- **Pairing (SRP-6A, 3072 bit, HAP §5.5):** `SRP6A` holds 14 `mbedtls_mpi` of up
  to 384 B each plus the `_rr` helper and `Verification` (400 B) — with the
  mbedTLS temporaries that is **~10–15 KB transient**, part of it as a
  contiguous block. The ESP32's hardware MPI unit softens this compared to pure
  software exponentiation; a comparison value from the Arduino-HomeKit world:
  pairing succeeds from ~14 KB of free heap. Only incurred at initial pairing.
- **`GET /accessories`:** current HomeSpan versions stream the response in 1 KB
  records (`HapOut`, two passes for the Content-Length), so there is **no** large
  block proportional to the network size any more. That used to be the limiting
  factor in older versions (cf.
  [issue #684](https://github.com/HomeSpan/HomeSpan/issues/684): users hit the
  wall at ~40 KB `largest_block`). With an update window that lacked streaming,
  the assessment would be considerably worse.

### 4.3 Balance against the available budget

Scenario: 15 luminaires, 2 permanent controller connections.

| | Heap |
|---|---|
| Base load | ~10 KB |
| Accessory DB (15 × ~1.7 KB) | ~26 KB |
| 2 permanent connections (idle) | ~4 KB |
| **permanent** | **~40 KB** |
| + a concurrent HAP request (8 KB buffer + socket) | **~50 KB peak** |
| + pairing | **~55 KB peak** |

Against the as-is state:

| Situation | free today | after HAP (−40 KB) |
|---|---|---|
| Idle | 90–96 KB | ~50–56 KB — would work |
| Under mixed load (HTTP + WS) | 37–58 KB | **−3 … +18 KB** |
| `min_free_heap` during the stress test | 7–16 KB | **far below 0** |

Under load the result is **below** `HEAP_CRITICAL_THRESHOLD` (20 KB) — i.e. in
boot-loop territory, and that is before holding the `largest_block` (7–27 KB
under load) against the 8 KB HTTP buffer and the SRP block. The conclusion is
robust against errors of detail in the estimate: even at half the requirement no
meaningful margin would remain.

It would only fit if something large disappeared in parallel — which is on the
cards for variant B anyway (4.4) — or on a board **with PSRAM**: HomeSpan
allocates `SpanAccessory`/`SpanService`/`SpanCharacteristic`/`SRP6A` via
`HS_MALLOC` (`HomeSpan.h`, `src/PSRAM.h`), explicitly preferring PSRAM. On an
ESP32-S3 with 2–8 MB of PSRAM, item (b) and most of (d) disappear from the
internal heap. The ATOM Lite (PICO-D4) has none.

### 4.4 The hard blocker beyond memory

In the
[ProgrammableHub example](https://github.com/HomeSpan/ProgrammableHub) HomeSpan
documents explicitly: *"ESPAsyncWebServer requires a different TCP stack and
cannot be used with HomeSpan"* — the route shown there is the **synchronous**
`WebServer` library, and HomeSpan's TCP slots have to be reduced from 8 to 5 for
it.

This project stands entirely on `AsyncTCP` + `ESPAsyncWebServer`:
`src/web/webserver.cpp` (1,956 lines), `src/web/setup_portal.cpp` (612 lines),
the WebSocket push layer and all the stability work from #18
(`docs/concept-asynctcp-churn-stability.md`, `scripts/stress_test.py`,
`scripts/verify_tcp_stack.py`). Variant B would mean:

- porting the REST API, dashboard and setup portal to the synchronous
  `WebServer` library — which runs blocking in `loop()`, where the BLE client
  and the watchdog (45 s) also live,
- **loss of the WebSocket push** (the synchronous library has none) — which
  breaks the FHEM integration in its current form,
- running the entire churn/stability verification again,
- plus the small stuff: a port conflict (HAP wants 80, `setPortNum()` needed),
  HomeSpan master requires Arduino-ESP32 core ≥ 3.3.0 (`src/version.h`) —
  `platform = espressif32` in `platformio.ini` is unpinned, so that has to be
  checked in advance; `huge_app` has no second OTA slot, but HAP devices are
  exactly the ones you want to update over the air.

Memory is therefore not even the most expensive argument.

## 5. Decision

**Variant A — and it is already in use.** The Apple Home integration runs via
the Raspberry Pi; nothing changes on the ESP32 as a result. **Variant B is
rejected** and will not be pursued: the heap requirement does not fit on the
ATOM Lite (4.3), and the web-server blocker (4.4) would not justify the price
even if it did. The paragraphs below record *why* — not what would still be left
to do.

Variant A costs exactly nothing on the ESP32, uses the WebSocket push for
precisely the purpose it was built for, and leaves the BLE/web architecture
untouched. The HomeKit logic (mired conversion, debouncing, error state) sits
where it is cheap to change — on the Pi, next to FHEM.

Two routes would remain should the question come up again — both explicitly
**not** planned and recorded here only so the analysis need not be repeated:

- **An ESP32-S3 with PSRAM** as the target board; PSRAM then carries the
  accessory DB and "only" the web-server port from 4.4 remains.
- **A second ESP32 as a pure HAP bridge** talking to the existing controller
  through its REST/WS API — functionally identical to variant A, just without
  the Pi. Heap is free there because no BLE stack and no Casambi protocol run on
  it.

## 6. Measurement instructions (not carried out)

The numbers in 4.1/4.2 are derived, not measured on this hardware — and, after
the decision in section 5, will not be. Anyone who does want to make them hard
needs about an hour:

1. **Flash:** add `homespan/HomeSpan` to a throwaway environment, build a
   minimal bridge with 15 Lightbulb accessories, compare `pio run -t size`
   against today's value (1,596,065 B). Expectation: +150–250 KB.
2. **Heap:** flash the same sketch and log `ESP.getFreeHeap()` /
   `heap_caps_get_largest_free_block()` at three points — after
   `homeSpan.begin()`, after building the accessory DB, and during
   `GET /accessories` from a paired controller. Expectation: ~10 KB / ~35 KB /
   ~50 KB delta.
3. Only if (2) comes out substantially better than estimated here is the
   question from 4.4 worth asking at all.
