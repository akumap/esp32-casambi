# Concept: A Matter bridge alongside the REST API (issue #44)

Status: **DEFERRED — not being pursued for now** (as of 2026-08-03, the
maintainer's decision). No implementation, no open tasks. The document is kept
as the **analysis and decision basis**; should the topic be picked up again, the
next steps are the two measurements in 14.4.

**Constraint at the time of deferral: the hardware stays the M5Stack ATOM Lite
(ESP32-PICO-D4, 4 MB flash, no PSRAM), and a second device is out of the
question.** Whether a Matter bridge is possible on that is **open and tight**:
the calculation with the corrected heap baseline (~90 KB rather than the 56 KB
taken from the README) and the two missing measurements are in section 14.
Section 15 assesses the question about IFTTT and names the interface extension
that does work without new hardware.
Issue: [#44](https://github.com/akumap/esp32-casambi/issues/44)
Branch: `claude/matter-bridge-no-config-zmie1w`

## 1. Goal

The discovered Casambi devices should appear as Matter devices **in addition to
the existing REST/WebSocket API**, so that ecosystems such as Google Home, Apple
Home or SmartThings can integrate them directly — **with as little configuration
as possible**:

- no additional setup step in the setup portal,
- no file, no mapping, no IDs the user has to maintain,
- the user scans a QR code in the ecosystem app (or types the numeric code) that
  the device generates itself and shows on its web interface.

The REST/WebSocket API and FHEM remain usable unchanged; Matter is a **second,
equal consumer** of the same internal interfaces — not a replacement and not a
rework of the existing path.

**The result of this concept, up front:**

1. With the **standard stack** (`esp_matter`/CHIP in its default configuration,
   Arduino as the framework) Matter does **not** fit alongside the existing
   firmware — not on the ESP32-WROOM-32. Evidenced by Espressif's own
   measurements in section 4.1.
2. There are, however, **substantial memory levers** (section 5): Espressif
   documents some ~85–100 KB of additional free heap from configuration alone,
   plus moving the Matter BSS segments into **PSRAM**. That makes the on-device
   route realistic — at the price of a **PSRAM board** and a build as *ESP-IDF
   with Arduino as a component*.
3. There is a **considerably lighter Matter implementation** than CHIP: Tasmota's
   Berry implementation (~209 KB flash, minimal RAM requirement, IP
   commissioning, bridge mode). It cannot be extracted as a library, but it works
   as a **companion device** — the fastest route to Google Home with zero risk to
   this firmware.

## 2. Starting point (as-is)

### 2.1 What already exists and suits a bridge

| Building block | File | Why it suits Matter |
|---|---|---|
| Generic device model | `cloud/network_config.h` (`CasambiUnit`, `UnitControl`, `controlName()`) | Capabilities come from the cloud fixture, not from hardcoded types — exactly what a "generic" bridge mapper needs |
| Command queue | `web/webserver.h:72` (`BleCommand`), `web/webserver.cpp:61`/`:149` | A single serialisation point: the REST handler enqueues, the loop task executes. A second producer (Matter) slots in without new concurrency |
| State callback | `casambiClient->setUnitStateCallback(...)` (`main.cpp:452`) | A push source for attribute updates; already decoupled from the BLE task by a queue today |
| Atomic multi-channel write | `ble/casambi_client.h:208` (`setUnitState`) | Matter commands affecting several channels (level + CCT) go out as **one** telegram |
| mDNS already runs | `main.cpp:140` (`startMDNS`) | Matter needs DNS-SD — but beware, there is conflict potential (4.5) |

The bridge therefore does **not** have to rebuild any Casambi logic. It is a
pure protocol adapter: Matter clusters ⇄ the existing unit model + command
queue.

### 2.2 Memory situation today

In normal operation **~90 KB of heap are free** (the maintainer's measurement).
The 56 KB free / 31 KB low water mark shown as an example in the README are
**not an operational figure** but illustrative JSON, or values from the overload
tests — see the correction in 14.1. Below 20 KB the firmware restarts itself
(`HEAP_CRITICAL_THRESHOLD`, `config.h:204`). This baseline is the core of the
feasibility question in sections 4 and 14.2.

### 2.3 Toolchain

`platformio.ini` uses `platform = espressif32` without a pin, and the code uses
`esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true)` (`main.cpp:341`) — the
**two-argument** form, which only exists up to ESP-IDF 4.4 / **Arduino core
2.x**. So the firmware builds against core 2.x today. Matter (`esp_matter`) is
only available for Arduino from **core 3.x** on.

## 3. What a "Matter bridge" means technically

In the Matter data model, a bridge is a single node with:

- **Endpoint 0** — the root node (basic information, network and fabric
  management),
- **Endpoint 1** — the **aggregator** (device type `0x000E`), whose
  `Descriptor.PartsList` lists all bridged devices,
- **one further endpoint per bridged device** with the appropriate device type
  (e.g. dimmable light) **plus** the cluster *Bridged Device Basic Information*
  (`NodeLabel` = the Casambi name, `Reachable` = the online flag).

On top of that comes **commissioning**: discriminator (12 bit), setup passcode
(27 bit), VID/PID, from which the onboarding payload is built (`MT:…` as a QR
code or an 11-digit numeric code), discoverability via DNS-SD (`_matterc._udp`
before, `_matter._tcp` after commissioning) and **device attestation** (DAC).
IPv6 (at least link-local) is mandatory.

Relevant for this firmware: **dynamic endpoint management**. The Casambi unit
list is only settled after the cloud refresh and changes again later — so the
endpoints have to be built from `NetworkConfig` at runtime. That is exactly the
mechanism of the CHIP bridge example (`NUM_DYNAMIC_ENDPOINTS`), costing per
Espressif **≈ 550 bytes of DRAM per endpoint slot**, with a default of 16 slots.

## 4. Constraints — the hard limits

### 4.1 RAM: why the standard stack does not fit on the WROOM-32

Espressif's own measurements for the **light example** (Matter only, nothing
else) on an ESP32-C3, in the **default configuration**:

| Quantity | Value |
|---|---|
| Flash (bin) | 1,476,960 bytes |
| static D/IRAM | 195,080 bytes |
| free heap at boot | 35,976 bytes |
| free heap **after** commissioning | 101,580 bytes |

The jump from 36 KB to 101 KB comes from
`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING`: **after commissioning, the BLE memory
is released.**

That is exactly what is **not possible** here — BLE *is* the Casambi connection
and runs permanently. So for this firmware it is rather the "boot" row that
applies: ~36 KB free for an application that does *nothing but* Matter. Our
firmware additionally needs NimBLE as a central, Wi-Fi STA, AsyncTCP/
AsyncWebServer with WebSocket clients, LittleFS and the event log at the same
time — and already has only ~90 KB free today (2.2).

The dominant part here is **not** the runtime heap requirement but the **static
share**: ~195 KB of D/IRAM out of 320 KB before the first line of our own code
runs. That is exactly where the levers in section 5 apply — which is why "does
not fit" is a statement about the *default configuration*, not about Matter as
such.

### 4.2 Flash and partitions

Matter alone brings ~1.5 MB of code. Together with the existing firmware
(NimBLE + AsyncWebServer + TLS cloud client + PROGMEM UI), `huge_app.csv` (3 MB
app, no OTA) is tight at best. Recommendation: a Matter target with **8 MB or
16 MB of flash** and a partition table of its own (a larger NVS for
fabrics/attributes). For attestation, the **test DAC** shipped with `esp_matter`
is used (no `fctry` partition, no manufacturing process) — consequences in 8.5.

### 4.3 BLE coexistence: no BLE commissioning

Matter devices normally advertise **over BLE** during commissioning. Here NimBLE
occupies the controller permanently as a **central** to the Casambi gateway, and
Matter's BLE manager initialises/deinitialises the stack itself — a takeover
conflict that could, in the worst case, drop the Casambi connection.

**Design decision: IP-only commissioning.** The device is already on the Wi-Fi
anyway (Casambi provisioning happens first, see `concept-provisioning.md`) and
needs no Wi-Fi credentials from the commissioner. So it advertises exclusively
via DNS-SD (`_matterc._udp`) on the LAN.

This decision is **proven in practice**: Tasmota commissions its Matter devices
exclusively over IP/DNS-SD, with the same rationale ("BLE mainly serves the
Wi-Fi handover — which is already done in our case"). That lowers the risk of
this point considerably, but it does not replace testing against the actual
ecosystem apps in stage 1.

### 4.4 Toolchain migration to Arduino core 3.x

A precondition, not a side effect (see 2.3). Affected:

| Point | Assessment |
|---|---|
| Platform package | Switch to a core 3.x package (pioarduino), since the official `espressif32` stopped at core 2.x |
| `esp_task_wdt_init` | Signature change (`esp_task_wdt_config_t`) — `main.cpp:341` |
| NimBLE-Arduino 2.1.0 | supports core 3.x, presumably uncritical |
| ESP32Async AsyncTCP/ESPAsyncWebServer (pinned) | support core 3.x, the pins may need raising |
| Wi-Fi/LittleFS/HTTPClient API | selective adaptations, plus re-verification of the TLS heap and the `#18` stability |

That is effort and regression risk **for the entire existing firmware** and
should be tracked as **its own issue** — not hidden inside the Matter work.
Until then Matter stays confined to its own environment.

### 4.5 mDNS double occupancy

The firmware already registers an mDNS responder (`main.cpp:140`). CHIP ships a
"minimal mDNS" implementation of its own by default, which also serves port
5353. Mitigation: configure CHIP to use the platform mDNS
(`CONFIG_USE_MINIMAL_MDNS=n`) or verify in the spike that the two coexist.
Symptom of a misconfiguration: the device is undiscoverable in the ecosystem
even though it is running.

### 4.6 Sizing

`CLOUD_MAX_UNITS` is 250 (`config.h:298`), and CHIP's bridge example ships with
16 dynamic endpoint slots by default (≈550 B of DRAM per slot). A limit is
therefore unavoidable — Tasmota even recommends only ~8 bridged endpoints for
its bridge mode, for performance reasons. Proposal:
`MATTER_MAX_BRIDGED_UNITS` (default 16, calibrated in the spike against the
measured heap), the selection **deterministic in ascending `deviceId` order**,
with the excess reported visibly in the dashboard and the event log — not
silently swallowed.

## 5. Memory: levers and lighter foundations

The question "can't it be done more frugally?" has four solid answers, in
increasing order of effect.

### 5.1 Save on our own stack (≈ 10–25 KB, helpful, not decisive)

Limiting concurrent WebSocket clients, smaller AsyncTCP queues and stacks,
smaller event-log buffers, NimBLE fine-tuning (central role only, one
connection), lowering `WS_HELLO_MAX_UNITS`. All feasible, but it does not shift
the order of magnitude: against ~195 KB of static Matter BSS, 20 KB is rounding
error.

### 5.2 Configure esp-matter (Espressif's own numbers)

Espressif documents the following savings (measured on C3/H2, the light
example):

| Option | Flash | D/IRAM | free heap (boot) | usable for us? |
|---|---|---|---|---|
| `CONFIG_ENABLE_CHIP_SHELL=n` | −55…−67 KB | −0.8…−2.2 KB | +9.9…+10.5 KB | yes |
| BLE controller code in flash (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`) | — | −19.4…−19.9 KB | +19.8…+23.1 KB | yes, but at a latency/throughput cost on BLE — **measure against Casambi stability in the spike** |
| FreeRTOS functions in flash | — | −9.1…−10.3 KB | +9.1…+9.5 KB | yes (performance trade-off) |
| SPI flash ROM implementation | — | −9.5…−12.7 KB | +8.3…+12.6 KB | yes |
| Log event buffer down to 256 B | — | −5.4 KB | +6.5…+7.0 KB | yes |
| Lower the endpoint/device-type count | — | −6.6 KB | +6.0…+6.8 KB | partly — we *need* endpoints (≈550 B per slot) |
| Ringbuf functions in flash | — | −4.7 KB | +4.0…+4.6 KB | yes |
| Exclude unused clusters | −37 KB | −3.7 KB | +3.9 KB | yes |
| Shrink task stacks | — | — | +3.3…+3.9 KB | carefully, we have tasks of our own |
| Newlib nano formatting | −47 KB | — | +1.9…+2.4 KB | yes |
| BLE optimisations (max connections, disabling roles) | −23 KB | −1.7…−2.2 KB | +9.8…+19.1 KB | **only partly** — the central role *is* our Casambi connection and must not go |

In total, realistically some **~70–90 KB** of additional free heap are
achievable — turning the 36 KB row from 4.1 into ~110 KB. That is the order of
magnitude in which our firmware can exist alongside it.

**The catch:** these are **IDF Kconfig options**. In the PlatformIO Arduino
framework the core arrives as a *precompiled* library with a fixed `sdkconfig` —
those switches are simply unreachable there. Anyone wanting to use them has to
run the Matter build as **ESP-IDF with Arduino as a component**. That is a
well-documented but considerably larger toolchain step than the plain core 3.x
migration from 4.4 — and it would only affect the Matter environment, not the
`devkit-v4` build.

### 5.3 PSRAM — the biggest single lever

Espressif explicitly describes **moving the BSS segments of `libCHIP.a` and
`libesp_matter.a` into external RAM via a linker fragment**
(`CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). That removes exactly the item
which dominates in 4.1 — the static share — from internal RAM.

Candidates:

| Module | RAM | Assessment |
|---|---|---|
| **ESP32-WROVER** (classic ESP32 + 4/8 MB PSRAM) | 320 KB internal + PSRAM | The closest relative of today's hardware; `platformio.ini` already has the PSRAM flags prepared (commented out). But: the cache workaround (`-mfix-esp32-psram-cache-issue`) costs performance, PSRAM is not DMA-capable, and Wi-Fi/BLE buffers have to stay internal |
| **ESP32-S3 N16R8** | 512 KB internal + 8 MB PSRAM | Cleaner (octal PSRAM, no cache bug), more internal RAM, plenty of flash — the **recommended Matter target** |
| ESP32-C6 (without PSRAM) | 512 KB internal | Conceivable, but without the lever from 5.3 tighter than S3+PSRAM |

With 5.2 + 5.3 together, the on-device route is **comfortably feasible** as far
as we know today; the stage-1 gate (section 11) nevertheless stands, because our
combination of a permanent BLE central, AsyncTCP and Matter appears in none of
Espressif's measurements.

### 5.4 A lighter implementation: Tasmota's Berry Matter

Tasmota implemented Matter **completely afresh in Berry** rather than using
CHIP:

- **~209 KB of flash** instead of ~1.5 MB — "very little compared to
  connectedhomeip", per the author;
- **very low RAM requirement**, because the Berry code sits as bytecode *in
  flash* ("solidified") and is not loaded into RAM;
- **IP commissioning** via DNS-SD, no BLE (see 4.3);
- crypto via BearSSL instead of mbedTLS;
- runs on **all ESP32 variants**, included in the standard `tasmota32` builds;
- has a **bridge mode** with "virtual endpoints" driven by Berry code — so it
  can be coupled freely to a foreign HTTP interface. Recommended limit: ~8
  bridged endpoints.

That is the proof that Matter itself is not hard — what is hard is *CHIP*.

Two ways of using it, one sensible, one not:

- **Technically viable but ruled out here — a companion device (option B in
  section 6; the requester does not want to set up a second device, see
  section 14):** a second, cheap ESP32 with **stock Tasmota** plus a Berry
  script that maps our REST endpoints and the WebSocket push onto virtual Matter
  endpoints. No firmware change here, no core 3.x requirement, no PC/server
  needed, hardware cost in single-digit euros. The price: a second device, a
  second update path, and the Berry script has to be maintained.
- **Not sensible — extraction:** Tasmota's Matter depends on the Berry VM and on
  Tasmota infrastructure. "Just take the Matter classes" would mean pulling
  Berry and half of Tasmota into this firmware. Anyone doing that is really
  running Tasmota.

**Effort of adopting it (as asked):** both conceivable routes take months, and
one of them already fails on the licence.

- *Embedding the Berry VM and running Tasmota's Matter modules on it:* the
  implementation comprises **~140 Berry files** written against Tasmota's object
  model (`tasmota.*`), its timers/tasker, persistence, mDNS, UDP sockets and
  crypto bindings **to BearSSL**. On top comes the "solidify" build step that
  translates Berry code into bytecode in flash. So one would drag in the Berry VM
  (~10 KB of RAM base load) plus a considerable part of Tasmota's substructure —
  or write a compatibility layer that reproduces exactly that substructure.
- *Porting it to C++:* that would not be a port but a reimplementation with a
  template — including converting the crypto from BearSSL to mbedTLS. After that
  there are **no upstream updates** any more: every fix for an ecosystem quirk
  would have to be carried over by hand.
- **Licence:** Tasmota is under **GPLv3**, this project under **MIT**. Adopting
  the code — even in translated form, as a derivative work — would force the
  entire firmware under GPLv3. That is a project decision, not a technical one.

### 5.5 A minimal implementation of our own — explicitly not recommended

Tempting, but the effort does not lie in the data model; it lies in PASE/SPAKE2+,
CASE, the sigma handshake, TLV, the interaction model, subscriptions, DNS-SD and
the ecosystems' quirks. Those are person-months, and the bugs sit exactly where
they are hardest to debug. Tasmota did it — and needs a language environment of
its own and years of maintenance for it.

### 5.6 Does it help that Casambi only knows luminaires?

An obvious thought: we need no thermostat, no door locks, no cameras — only
dimmers, colour temperature and the occasional RGB. Shouldn't a Matter node cut
down that far be much smaller?

**With CHIP: no, only marginally.** Espressif measured exactly this optimisation
("exclude unused clusters"): **−37 KB flash, −3.7 KB D/IRAM, +3.9 KB free
heap**. It saves noticeable flash, but practically no RAM.

The reason: the memory goes not into the *data model* but into the **protocol
machinery**, and that is identical for a single lamp and for a smart home full
of devices. Every Matter node has to bring along:

- **MRP** (message reliability protocol) over UDP/IPv6 — retransmits, acks,
  backoff;
- **TLV** encoding and the message layer;
- **PASE** commissioning with **SPAKE2+** on P-256;
- **CASE** session establishment (sigma1/2/3) including X.509 operational
  certificates, NOC/ICAC/RCAC chain validation, ECDH and signatures;
- **attestation** (DAC/PAI/CD, CSR request) — even with test certificates;
- the **interaction model** with read/write/invoke **and subscriptions**,
  including the report engine with min/max interval timers (ecosystems
  subscribe, they do not poll);
- **access control** (ACL enforcement per fabric), the fabric table, the session
  store;
- **DNS-SD** advertising in two modes;
- the mandatory clusters Basic Information, Descriptor, General/Network
  Commissioning, Operational Credentials, Administrator Commissioning, General
  Diagnostics.

Only after that come On/Off, Level Control and Color Control — the part our
narrow device spectrum touches at all.

**What does scale with the narrowing** (and should therefore be taken along
anyway):

| Adjustment | Saving | Note |
|---|---|---|
| Exclude unused clusters | ~3.9 KB heap, 37 KB flash | measured (see above) |
| Size the endpoint slots exactly | ~550 B per slot | 8 instead of 16 slots ≈ 4.4 KB |
| Drop Color Control (phase 1 dimmer only) | a few hundred bytes per endpoint | the fattest light cluster |
| Lower the max fabrics (default 5 → 2–3) | a few KB | certificates + ACL entries per fabric |
| Bound subscriptions/sessions/exchanges | a few KB | buffers per open subscription |

In total **~10–20 KB** — welcome, but not the order of magnitude that decides
the WROOM-32 question. The floor is set by CHIP, not by the device catalogue.

**Where the narrowing really counts:** when CHIP is *replaced*. Tasmota's 209 KB
versus ~1.5 MB (5.4) show the potential. But be careful with the conclusion:
that saving comes from the leaner *implementation of the machinery*, not from
supporting lamps only. Of a bespoke implementation, roughly 15 % of the work
would fall away because "it's only luminaires" — the remaining 85 % (SPAKE2+,
CASE, MRP, TLV, IM with subscriptions, ACL) has to be done regardless of the
device spectrum. The assessment stands as in 5.5: not proportionate.

### 5.7 Conclusion of the memory analysis

| Route | additional memory demand here | Hardware | Effort |
|---|---|---|---|
| CHIP/esp-matter, default config | does not fit | — | — |
| CHIP/esp-matter + 5.2 (IDF build) | borderline without PSRAM | S3/C6 | high |
| CHIP/esp-matter + 5.2 + 5.3 + 5.6 | comfortable | S3 with PSRAM | high |
| A bespoke subset implementation (5.5) | presumably sufficient | existing hardware | very high, months, riskiest class of bugs |
| Tasmota companion (5.4) | **zero** in this firmware | a second ESP32 | low — **but a second device, ruled out by the requester** |

## 6. Options at a glance

| # | Approach | Effort | Hardware | "without configuration" | Assessment |
|---|---|---|---|---|---|
| **A** | A Matter bridge **in this firmware**, its own environment, IDF+Arduino-as-component, PSRAM target | high (core 3 migration + IDF build + bridge) | **one** device — an ESP32-S3 with PSRAM instead of the current board | yes (QR in the dashboard) | **Recommended** — the only route that genuinely delivers "one device, without configuration" |
| **B** | **Tasmota companion**: a second ESP32 with stock Tasmota + a Berry script against our REST/WS API | low (a script, no firmware risk) | two devices | no (flashing, Wi-Fi, token, script) | **ruled out** — the requester does not want to set up a second device. Documented only as a fallback should A fail at the gate |
| **C** | External bridge software (Matterbridge / Home Assistant Matter Hub) on an existing server | low | an always-on host | no | A footnote for HA users who run the host anyway |
| **D** | A bespoke subset implementation of Matter in this firmware, without CHIP (5.5, 5.6) | very high (months) | the existing board | yes | **not proportionate** — narrowing to luminaires saves only ~15 % of the work |

**Recommendation:** A. Important for context: A means **not a second device**
but **a different board for the same device** — the complete firmware including
Casambi BLE, the REST API and Matter runs on a single ESP32-S3 with PSRAM. The
setup effort for the user stays what it is today, plus scanning a QR code.

## 7. Target architecture (option A)

### 7.1 Module boundaries

```
src/control/command_queue.h     (new)     BleCommand + queue + executor, extracted from webserver.*
src/matter/matter_bridge.h/.cpp (new)     Matter node, endpoint construction, attribute sync, command mapping
src/matter/endpoint_map.h/.cpp  (new)     persistent mapping unitId <-> endpointId
src/web/webserver.*             (changed) uses command_queue instead of its own queue; + /api/matter
src/web/dashboard.h             (changed) Matter tile with QR + numeric code + fabric status
```

The core principle: **one** command path. REST handlers and Matter handlers are
both merely producers on `command_queue`; execution still happens exclusively in
the loop task (`webserver.cpp:149` moves along with it). That keeps the BLE
serialisation unchanged, and Matter automatically inherits backpressure, error
logging and the existing security/timeout properties.

The split is worthwhile independently of Matter — it is also the precondition
for a **companion (option B)** to see the same semantics as the dashboard.

### 7.2 Mapping Casambi → Matter

Driven by `controls[]` / `controlName()` (`cloud/network_config.h`), i.e.
**without** fixture special cases:

| The unit's Casambi controls | Matter device type | Clusters |
|---|---|---|
| `dimmer` only | Dimmable Light (`0x0101`) | On/Off, Level Control |
| `dimmer` + `temperature` | Color Temperature Light (`0x010C`) | + Color Control (CT, mireds) |
| `dimmer` + `rgb`/`xy` | Extended Color Light (`0x010D`) | + Color Control (HS/XY) |
| additional `dimmer1`, `vertical`, `slider` | one **further** dimmable-light endpoint each, `NodeLabel` = `"<name> <controlName>"` | On/Off, Level Control |
| Groups | optionally one dimmable-light endpoint each (phase 4) | — |
| Scenes | phase 4, as an On/Off endpoint (Matter has no "scene" device type for bridges) | — |

Conversions (pure functions, host-testable):
Matter level `1..254` ⇄ Casambi `0..255`; `OnOff` ⇄ level 0/the last value;
mireds ⇄ kelvin clamped to `cctMinKelvin`/`cctMaxKelvin`; kelvin ⇄ the
normalised Casambi value already exists (`cloud/state_codec.h`).

`Reachable` = `unit.online`; if the Casambi connection drops, **all** bridged
endpoints are set to `Reachable=false` (a signal to the ecosystems, rather than
silently showing stale values).

This table applies **to option B as well** — it is the functional mapping, not
the technical implementation.

### 7.3 Stable endpoint IDs

Ecosystems hang room assignment, names and automations off the **endpoint ID**.
If IDs wander after a cloud refresh, the user's lamps get swapped around.
Therefore: `endpoint_map` as a small LittleFS file (`unitId` + `controlName` →
`endpointId`), with these rules:

- an existing assignment always wins,
- new units get the next free ID (never a reused one),
- units that have disappeared keep their slot for now and become
  `Reachable=false`; release only via an explicit Matter reset.

### 7.4 State flow

```
BLE notify task → (existing UnitStateCallback) → event queue
                → loop task: WebSocket broadcast   (unchanged)
                           + Matter attribute update (new, only when the value changes)
```

No `esp_matter` call from the BLE task. Attribute updates are debounced (only on
an actual change), so that a dimming-ramp broadcast from the mesh does not
trigger a flood of updates in the fabrics.

### 7.5 Command flow

```
Matter task (CHIP) → command_queue (BleCommand) → loop task → CasambiClient
```

Important: Matter level commands (`MoveToLevel` with a transition, dragging a
slider in the app) arrive in series. The BLE path is orders of magnitude slower.
Hence **coalescing per unit** ("last value wins", ~150 ms), implemented in
`command_queue` so that **the REST API benefits too**. Multi-channel changes go
out as a single `setUnitState` (`casambi_client.h:208`).

### 7.6 Relationship to FHEM/WebSocket

Since all routes go through the same queue, FHEM sees every change triggered via
Matter as a perfectly ordinary `unit_state` push — and vice versa. There is no
second "source of truth" and no special-casing in the FHEM module.

## 8. "Without configuration" — concretely

### 8.1 The device generates the pairing data itself

The discriminator and the setup passcode are **generated randomly once at the
first Matter start** (`esp_random()`), stored in NVS and reused unchanged
afterwards.

Deliberately **not** derived from the eFuse MAC: the MAC is publicly visible
over Wi-Fi and BLE; a passcode computable from it would be guessable by anyone
within radio range and would devalue the commissioning secret. The standard's
forbidden values (`00000000`, `11111111`, …, `12345678`, `87654321`) are
excluded during generation.

### 8.2 Display

- **Dashboard** (`/`): a "Matter" tile with a QR code, the 11-digit manual-entry
  code and the fabric status. The QR is rendered client-side from the payload
  string (a small embedded encoder — **no CDN**, the device has to work
  offline); the numeric code is the fallback should the QR not render.
- **`GET /api/matter`** (authenticated): `{enabled, commissioned, fabrics,
  qr_payload, manual_code, endpoints:[{unit_id, control, endpoint_id,
  reachable}], units_over_limit}`.
- **Serial**: `matter` shows the same, as a fallback without a browser.

The pairing code is a **secret** and is treated like the API token: only through
authenticated endpoints, never in `/api/info` (which is deliberately open).

### 8.3 The commissioning window

As long as no fabric exists, the window is open automatically (basic
commissioning mode) — the user has to press nothing. It closes after the first
join; for a second ecosystem (multi-admin) there is a button in the dashboard,
or `POST /api/matter/commission-window`, which reopens it for a limited time.

### 8.4 Reset

`POST /api/matter/reset` (and `matter reset` over serial) removes all fabrics
and generates new pairing data. `clearconfig` additionally deletes the endpoint
map, so a freshly provisioned device does not inherit old assignments.

### 8.5 Where "without configuration" ends — stated honestly

Without Matter certification (CSA membership, test houses, a VID) the device
runs with a **test VID/PID** (`0xFFF1`/…). Consequences:

- **Google Home** only admits uncertified devices if a project with **exactly
  this test VID/PID** has been created in the *Google Home Developer Console*
  and the account is registered as a tester. That is a one-off action **in the
  user's Google account** — outside what the firmware can automate away. The
  goal "entirely without configuration" is therefore **not fully achievable**
  for Google Home; the instructions for it belong in the README.
- **Apple Home / SmartThings** generally pair with a "not certified" warning,
  without a developer account.
- **Home Assistant** pairs without restriction.

This applies **regardless of the chosen route** — a Tasmota companion (option B)
is an uncertified Matter device too.

## 9. Impact on the versioned interface

New endpoints (`GET /api/matter`, `POST /api/matter/reset`,
`POST /api/matter/commission-window`) are purely additive →
`FHEM_API_VERSION_MINOR` +1, `MAJOR` stays (`config.h:92`, rules in
`concept-versioning.md`). Optionally a `matter` field in the WebSocket hello and
a reading in the FHEM module later; neither is a precondition for stages 1–3.

## 10. Security considerations

| Point | Assessment |
|---|---|
| Pairing passcode | random, persistent, not derivable from public values (8.1) |
| Visibility of the code | only through an authenticated API/dashboard session |
| Attack surface | the Matter node listens permanently on the LAN — a new network surface that did not exist before; test in the spike with a limited number of fabrics |
| Fabric management | every paired fabric may switch all bridged lamps — the same trust level as an API token; name it in the README |
| Test attestation | signals "not certified" to the ecosystems — deliberately accepted (8.5) |
| Companion (option B) | needs an API token of this firmware; the token then sits in plaintext on the Tasmota device — note that with the recommendation |

## 11. Implementation in stages (with abort criteria)

| Stage | Content | Completion/abort criterion |
|---|---|---|
| **0** | Procure/select an S3 board with PSRAM; start the core 3.x migration as its own issue (4.4) | The firmware builds unchanged on core 3.x, `devkit-v4` stays green |
| **1 — spike (time-boxed)** | **IDF with Arduino as a component**, the optimisation set from 5.2, the PSRAM BSS relocation per 5.3, the narrowing per 5.6; minimal Matter (2 endpoints) **simultaneously** with NimBLE Casambi and the web server | **Gate:** ≥ 60 KB of free heap in operation, `min_free_heap` ≥ 30 KB over 24 h, the Casambi link stable (in particular with the BLE code in flash, 5.2), the mDNS conflict resolved. **Otherwise:** fall back to option B/C, decision by the requester |
| **2** | Bridge core: extract `command_queue`, aggregator + bridged nodes from `NetworkConfig`, the state/command path, the endpoint map | All units up to `MATTER_MAX_BRIDGED_UNITS` switchable/dimmable, the state follows changes from the Casambi app |
| **3** | Zero-config UX: QR + numeric code in the dashboard, `/api/matter`, reset, the serial command | A new device can be paired without reading the documentation (apart from the Google Console step) |
| **4** | CCT/RGB/vertical/multi-channel fixtures, optionally groups/scenes | Mapping table 7.2 complete |
| **5** | README, FHEM notes, a CI environment for the Matter build | CI builds the Matter build too |

The core 3.x migration (4.4) is formally a precondition for stage 1 and is
tracked as **its own issue** — it affects the entire existing firmware and must
not be hidden in the Matter branch.

Stage 1 is deliberately the first substantive step: it costs little but answers
the one question the whole undertaking hangs on — does Matter fit alongside a
permanent BLE central and AsyncTCP on **one** board?

## 12. Tests and acceptance

**Host tests** (`pio test -e native`, patterned on `test/test_state_codec`):
level conversion `1..254 ⇄ 0..255` including boundary values, mireds⇄kelvin with
clamping, selection of the device type from `controls[]`, the endpoint map
(persistence, never reusing IDs, behaviour on a refresh with new/removed units),
the passcode generator (the forbidden list).

**Hardware acceptance:** commissioning in at least two ecosystems; switching
from the ecosystem, the Casambi app and the REST API in a mixed fashion, without
state divergence; behaviour on a BLE drop (`Reachable=false`, recovery); a 24 h
heap run; a cloud refresh with a changed unit list without the assignments
getting swapped.

## 13. Risks and open questions

| Risk / question | Impact | Handling |
|---|---|---|
| RAM is not enough even with 5.2 + PSRAM | Option A is dead, and the one-device requirement cannot be met | The gate in stage 1 — early and cheap; afterwards a decision between the fallback (B/C) and dropping the idea |
| BLE controller code in flash (5.2) degrades the Casambi connection | The core function suffers | Track it as a measurement criterion of its own in the spike; if necessary deselect the option and find the heap elsewhere |
| IDF with Arduino as a component | A second build path next to PlatformIO Arduino | Strictly confine it to the Matter environment; `devkit-v4` stays as it is |
| PSRAM on the classic ESP32 (WROVER) | The cache workaround costs performance, no DMA | Prefer the S3 |
| On-network commissioning is poorly supported by Google Home | The main target ecosystem drops out | Check it against the real apps in the stage 1 spike before building the bridge core; Tasmota's practice is encouraging (4.3) |
| CHIP mDNS collides with the existing responder | The device is undiscoverable | Configure the platform mDNS (4.5) |
| The core 3.x migration introduces regressions in the existing stack (#18 stability, TLS heap) | Affects non-Matter users too | Its own issue, its own merge, Matter behind a feature flag |
| Endpoint limit < network size | Large networks only partially bridged | A documented, deterministic limit + a visible message (4.6) |
| Two build targets (with/without Matter) | Maintenance effort, CI time | Encapsulate Matter strictly in `src/matter/`, leave the rest untouched via a flag |
| Test VID/PID vs. certification | "not certified" hurdles per ecosystem | Name it in the README (8.5); certification explicitly out of scope |

## 14. What has to be decided now

Both open hardware questions have since been answered:

| Question | Decision |
|---|---|
| A second device (option B, companion)? | **no** |
| Switch the board to an ESP32-S3 with PSRAM (option A)? | **no**, it stays the ATOM Lite (ESP32-PICO-D4, 4 MB flash, no PSRAM) |

### 14.1 Correcting the starting figure

The 56 KB of free heap / 31 KB low water mark quoted in 2.2 come from the
**example JSON in the README**, not from a running device. The maintainer
measures **~90 KB free** in normal operation; the low values arose under the
load tests (`scripts/stress_test.py`, profiles `medium`/`heavy`).

That is a relevant correction, and it points in the right direction:

- For a feasibility statement what counts is **continuous operation** under the
  realistic profile (one FHEM WebSocket connection), not the low water mark of
  an overload test. The test is built to find the breaking point.
- The **footprint of the load tests cannot be "reduced"**, because it does not
  sit on the ESP: `stress_test.py` runs on the PC, and there is no test code in
  the firmware image. What the test consumed on the device were **real
  connections** (AsyncTCP buffers, WebSocket state). "Less test footprint" would
  therefore mean allowing fewer simultaneous clients — a genuine functional
  restriction, not a cleanup.
- There is barely any slack in our own buffers anyway: `WS_MAX_CLIENTS` is
  already **3**, the broadcast queue costs ~0.5 KB (64 × 8 B) and the command
  queue ~0.25 KB. The only notable items would be the AsyncTCP task stack (16 KB
  by default) and NimBLE fine-tuning — together roughly **10–20 KB**.

### 14.2 What is missing — the calculation with the corrected figure

| Item | Order of magnitude |
|---|---|
| **Available** today (continuous operation) | ~90 KB heap |
| + our own trimming (AsyncTCP stack, NimBLE) | +10…20 KB |
| + esp-matter optimisations (5.2), **discounted for the ESP32** | +30…50 KB |
| **Total available (best case)** | **~130–160 KB** |
| **Demand:** the static Matter share (DRAM) | ~100…150 KB (estimated) |
| **Demand:** CHIP at runtime (sessions, subscriptions, mDNS, CASE crypto) | ~40…60 KB |
| **Total demand** | **~140–210 KB** |

Two caveats about the honesty of this table:

- Espressif's measurements (195 KB "used D/IRAM", 36 KB free heap at boot) come
  from the **ESP32-C3** with unified SRAM. The classic ESP32 separates DRAM and
  IRAM — the figures are **not directly transferable**, and the estimate of the
  static DRAM share is exactly that: an estimate.
- Several of the most effective optimisations from 5.2 are **IRAM→flash
  relocations** (BLE controller, FreeRTOS, ringbuf, SPI flash ROM). On the C3
  that lands directly in usable heap; on the ESP32 it frees **IRAM**, which is
  only usable as heap in a restricted way (32-bit aligned). Hence the discount
  above from ~70–90 KB to ~30–50 KB.

**Result:** demand and supply are roughly level in the best case; in the more
likely case **~30–70 KB are missing** — plus a reserve against the
fragmentation a device with permanent BLE and a web server accumulates over
weeks. On top of that comes a hard architectural limit: on the classic ESP32 the
**statically allocatable amount of DRAM is capped at 160 KB** (a documented IDF
limitation); Matter's static share would have to share that ceiling with what
the firmware already occupies statically.

### 14.3 Flash — the second, independent limit

It is independent of how good the heap looks: the ATOM Lite has **4 MB**, and
`huge_app.csv` gives the app **3 MB**. CHIP's light example alone weighs
1.48 MB; add the existing firmware (NimBLE, AsyncWebServer, mbedTLS, LittleFS)
and the bridge code. That lands at roughly **2.6–2.9 MB in a 3 MB partition** —
possible on paper, without any reserve, without OTA, and esp-matter additionally
wants a larger NVS.

### 14.4 An honest appraisal and the two missing measurements

My earlier "not realisable on this hardware" was formulated with the 56 KB
starting figure and is therefore **too categorical**. The correct statement is:

> Feasibility **not refuted, but not demonstrated either**. Both binding limits
> — DRAM and flash — are in "tight" territory, and neither has been **measured**
> so far.

Two measurements settle the question; the first takes minutes:

1. **Flash and static RAM today** — `pio run -e devkit-v4` prints RAM and flash
   usage at the end. If the app is already close to 1.5 MB, the matter is
   settled by CHIP's 1.48 MB in 3 MB, without any need to talk about heap.
2. **A realistic heap baseline** — `/api/status` after ≥ 24 h of continuous
   operation under the `realistic` profile: `free_heap`, `min_free_heap` and
   above all **`largest_block`**. For Matter's initialisation what counts is the
   largest contiguous block, not the sum.

Only then is the actual test worthwhile: a core 3.x build with *Arduino as an
IDF component* that merely **links and boots** `esp_matter` — it delivers the
static DRAM share and the free heap on **this** hardware and settles the
question definitively. That is the expensive step (days to weeks, 4.4), and it
should only be started after measurements 1 and 2.

**Consequence for issue #44:** the maintainer **deferred** the topic on
**2026-08-03** — it is not being pursued for now, and neither measurement has
been commissioned. The issue stays open with no work in progress; this document
is the state a later resumption can pick up from. Section 15 describes the route
that would provide value without new hardware (MQTT) — likewise not
commissioned.

## 15. As asked: IFTTT — and what really works on this hardware

### 15.1 Why IFTTT does not reach the goal

IFTTT is a **cloud service**. For our purpose it fails on four points, any one
of which would suffice:

| Point | State of affairs |
|---|---|
| **Reachability** | For control *towards* the device, IFTTT would have to call our ESP — which sits on the LAN. That would only work through a port forward (unacceptable: unencrypted HTTP, a token derived from the Casambi password, no rate limit) or through a relay on the network — at which point you again need the very device you were trying to avoid |
| **Google Assistant** | Precisely the function we would need is gone: since 31/08/2022 IFTTT's Google Assistant service no longer supports **triggers with variable input** ("say a phrase with a number") or custom responses. Dimming to a percentage by voice is therefore ruled out; Google's "Conversational Actions" were discontinued entirely on 12/06/2023 |
| **Plan** | Webhooks — the only technically usable building block — are only available from **IFTTT Pro**; the free plan allows 2 simple applets with up to **60 minutes of delay**. Pro covers 20 applets. At one applet per lamp and action, that is a good handful of luminaires |
| **Architecture** | A cloud service in the control path contradicts the basic idea of this firmware: **local and offline** after provisioning (BLE + LAN, no cloud). Latency, outages and a third-party dependency would be added with no functional return |

And even if all of that were different: IFTTT does not turn the lamps into
**devices in Google Home**. They do not appear in the device list, have no room
assignment and no state — what remains are voice triggers on fixed phrases. That
is no substitute for Matter.

### 15.2 What would make sense as an interface extension

The question is nevertheless a good one — it just points in a different
direction.

**a) Outgoing event webhooks (small, immediately feasible).**
On a state change, a BLE connection loss or a heap warning, send an HTTP POST
with JSON to a configurable URL. Benefit: notifications and automations in FHEM,
Node-RED, n8n or Home Assistant — and, for those who want it, IFTTT webhooks
(Pro). Low effort, minimal memory requirement.
**Caveat:** outgoing **HTTPS** is the delicate variant on this device — the TLS
handshake needs a large contiguous heap block, which is why the cloud refresh
deliberately runs before the BLE start today (README, "Troubleshooting").
Recommendation therefore: **HTTP to receivers on the LAN**, with TLS only
optional, gated on a heap check and with the risk documented.

**b) An MQTT client with Home Assistant auto-discovery (the actual
recommendation).**
The ESP connects **outbound** to a broker on the LAN, publishes the discovery
message per unit and publishes states; commands come back over `.../set` topics.
Advantages:

- **both directions**, without a port forward, without a cloud, without a second
  device;
- Home Assistant creates the luminaires **automatically** — the same "without
  configuration" principle as the QR code, just one level up;
- Google Home, Apple Home and Alexa are reachable through HA — exactly the goal
  from #44, only with HA as the intermediary instead of Matter in the device;
- memory requirement: one TCP connection and a small client — on the order of a
  few KB, uncritical on the ATOM Lite;
- the command path would be the same as for REST and Matter (7.1), making MQTT
  another producer on `command_queue`.

**Honest about the limit:** (b) also needs a broker and an HA instance on the
network. If the household has **no** always-on machine and neither an additional
device nor a board change is an option, there is no route into Google Home —
that is a hardware limit, not a question of interface design.

### 15.3 Recommendation

1. Do **not** pursue IFTTT (15.1).
2. If a Home Assistant instance exists or can be set up: **MQTT with
   auto-discovery** as its own issue — that is the viable extension on unchanged
   hardware and reaches the original goal by a detour.
3. Independently of that: **outgoing event webhooks** as a small, useful feature
   for local automation.
4. Defer issue #44 until the two measurements from 14.4 are available — do not
   close it.

## Sources

- Espressif, *Configuration options to optimize RAM and Flash* (esp-matter) —
  memory figures for the light example and all optimisation switches including
  the BSS relocation to PSRAM:
  <https://github.com/espressif/esp-matter/blob/main/docs/en/optimizations.rst>
- The Matter bridge example (aggregator, dynamic endpoints):
  <https://project-chip.github.io/connectedhomeip-doc/examples/bridge-app/esp32/README.html>
- Espressif Developer Portal, *Matter: Bridge for Non-Matter Devices*:
  <https://developer.espressif.com/blog/matter-bridge-for-non-matter-devices/>
- ESP-IDF, *Support for External RAM* (PSRAM use, restrictions):
  <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html>
- Tasmota, *Adventures with Matter protocol on Tasmota* (the Berry
  implementation, ~209 KB flash, IP commissioning):
  <https://github.com/arendst/Tasmota/discussions/17872>
- Tasmota, *Matter* / *Matter Internals* (bridge mode, virtual endpoints, the
  endpoint recommendation): <https://tasmota.github.io/docs/Matter/> ·
  <https://tasmota.github.io/docs/Matter-Internals/>
- Google Home Developers, testing a Matter integration (test VID/PID, the
  developer console project):
  <https://developers.home.google.com/matter/test>
- IFTTT, *Google Assistant changes* (removal of triggers with variable input on
  31/08/2022): <https://ifttt.com/explore/google-assistant-changes>
- Google, discontinuation of Conversational Actions on 12/06/2023:
  <https://en.wikipedia.org/wiki/Actions_on_Google>
- IFTTT plans (webhooks only from Pro; free allows 2 applets with up to 60 min
  delay): <https://ifttt.com/plans>
- Arduino Matter examples (no aggregator/bridge example available):
  <https://github.com/espressif/arduino-esp32/tree/master/libraries/Matter/examples>
