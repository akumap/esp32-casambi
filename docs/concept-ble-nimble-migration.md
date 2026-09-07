# Concept: Migrating the BLE stack to NimBLE

Status: implemented — `h2zero/NimBLE-Arduino @ 2.1.0` is pinned in
`platformio.ini` and `src/ble/` runs on NimBLE. The text below is the original
migration plan, kept as the record of why the move was made and how it was
sequenced.
Branch: `claude/ble-nimbble-migration-uuwzu4`

## 1. Goal and motivation

The ESP32 Casambi controller used the **Bluedroid** BLE stack shipped with
Arduino-ESP32 (`BLEDevice.h`, `BLEClient.h`, `BLEScan.h`). Bluedroid occupies
considerably more RAM than the alternative host **NimBLE**. That RAM is exactly
the bottleneck here: the recent project history is dominated by heap pressure
(`heap-underrun-reboots`, "tight heap", `BLEDevice::deinit(true)` before every
TLS handshake, because otherwise no contiguous heap block is free for the cloud
connection).

**NimBLE saves roughly 40–100 KB of RAM** compared to Bluedroid (depending on
configuration) and reduces the flash footprint as well. For this project that
means concretely:

- More free heap at runtime → less risk of heap-underrun reboots.
- The contiguous block for the TLS handshake becomes more likely to be
  available — possibly even **without** a full `deinit(true)`, which could
  defuse an entire workaround path (see 6.4, not part of the mandatory scope).

The **goal of this concept** is not the migration itself but a
**risk-bounded migration plan**: identical outward behaviour, small verifiable
steps, revertible at any time.

## 2. Starting point (as-is)

The ESP32 is exclusively a **BLE central/client** (it connects to Casambi
devices) and performs **passive/active scans**. There is **no GATT server**, no
BLE advertising role, no pairing/bonding and no use of the BLE stack's
security/encryption features (the Casambi encryption lives entirely in
`src/crypto/` and is independent of the BLE stack).

That is the **most important risk-reducing fact**: we only use the small,
well-covered central part of the API.

### 2.1 Touch points with the BLE stack

| File | Lines (approx.) | Usage |
|---|---|---|
| `src/ble/casambi_client.h` | 12–13, 162–163, 250 | includes, `BLEClient*`, `BLERemoteCharacteristic*`, notify callback signature |
| `src/ble/casambi_client.cpp` | 60–80 | `createClient`, `connect`, `getService`, `getCharacteristic` |
| | 379, 635 | `registerForNotify`, static notify callback |
| | 180, 345, 432, 617 | `readValue`, `writeValue` |
| `src/ble/casambi_scan.cpp` | 7–9, 25–57 | `BLEScan`, `BLEAdvertisedDeviceCallbacks::onResult`, `BLEUUID` comparison, adv data |
| `src/ble/casambi_scan.h` | 8, 32 | API contract (init/deinit done by the caller) |
| `src/main.cpp` | 9–10, 177, 603–608, 806, 1357–1372, 1415, 1525–1540 | `init`/`deinit`, two scan sites, adv callback in the wizard |
| `src/web/setup_portal.cpp` | 9, 305, 327 | `init`/`deinit(true)` around the portal scan |

→ The BLE dependency is **well localised**: essentially the `src/ble/`
directory plus three lifecycle sites (`main.cpp`, `setup_portal.cpp`). No BLE
types leak into `web/`, `cloud/`, `crypto/`, `storage/`.

### 2.2 Sensitive spots (highest regression risk)

1. **Notify callback threading.** `_notifyCallback` runs in the BLE host task,
   not in the loop task. The synchronisation via `_mutex`/`_encMutex` and the
   comment at `_sendEncryptedPacket` (the response notification can fire
   **synchronously** during `writeValue`, in the same tick) are tuned exactly to
   the Bluedroid timing. NimBLE delivers notifications from its **own task** (by
   default) — ordering and reentrancy may change.
2. **`deinit(true)` is not reversible** within one boot (controller RAM is
   released). Both stacks share that behaviour — the existing "reboot
   afterwards" logic stays valid.
3. **Wi-Fi/BLE coexistence and heap timing.** The "BLE before Wi-Fi" ordering
   (`main.cpp:176`) and the TLS heap workaround depend on real memory
   consumption — which changes with NimBLE (intentionally), but has to be
   **measured**.

## 3. Target picture

- Library: **`h2zero/NimBLE-Arduino`**, version **pinned** in `platformio.ini`
  (see 4.1).
- `src/ble/` uses NimBLE types; the **public interface** of `CasambiClient` and
  `CasambiScan` stays **unchanged** (no NimBLE types in the headers as far as
  possible → see 5.1).
- Behaviour identical from the outside: scan results, connection setup, key
  exchange, auth, control commands, auto-reconnect, WebSocket push.

## 4. Risk containment: guard rails

### 4.1 Pin the version and choose it deliberately

NimBLE-Arduino **2.x** is the current line (2.1.0 at the time of writing) and
supports Arduino-ESP32 on both the 2.x and the 3.x core series. The **1.4.x**
line has an API somewhat closer to the old `BLEDevice`.

**Recommendation: NimBLE-Arduino 2.x, pinned exactly** (e.g.
`h2zero/NimBLE-Arduino @ 2.1.0`). Rationale: 2.x is maintained, is compatible
with current cores, and the one-off extra effort of the 2.x scan/subscribe API
is small compared to having to migrate again later. An official
[1.x→2.x migration guide](https://github.com/h2zero/NimBLE-Arduino) exists.

> Side risk (optional): `platform = espressif32` is **not** pinned in
> `platformio.ini`. Pinning the core and NimBLE versions together would be more
> reproducible — but that is a separate decision and not part of the mandatory
> migration scope.

### 4.2 In-place migration with branch rollback

The switch happens **directly** in the existing build environments — no parallel
Bluedroid build, no `#ifdef` bridge, no new PlatformIO environment. Bluedroid is
**replaced** by NimBLE. The only change needed in `platformio.ini` is adding the
pinned NimBLE library to `lib_deps` (see 4.1); the environments themselves
(`devkit-v4`, `esp32-c3`, `debug`, `release`) stay structurally unchanged.

Risk containment therefore sits at **branch level**: the entire migration lives
isolated on `claude/ble-nimbble-migration-uuwzu4`. If something goes wrong the
branch is **discarded** (and restarted if needed) — `main`, with the working
Bluedroid revision, stays untouched. The safeguard is thus not keeping both
stacks buildable at once, but committing in **small, individually tested steps**
(7), so that a failed attempt is spotted early and cheaply.

### 4.3 Small, verifiable steps (see 7)

Scan first (stateless, easy to isolate), then the client. After each step,
build + test on real hardware before starting the next.

### 4.4 Measure the heap before and after

Before the migration, log the free heap on the Bluedroid build at three defined
points (after `BLEDevice::init`, after auth, immediately before the TLS
handshake). After the migration, the same points. That documents the RAM gain
**and** checks whether the TLS heap workaround is still needed. The existing
heap logging (`heapDebugEnabled`) works as a basis.

### 4.5 Define the rollback criterion up front

The migration counts as failed (→ do not merge the branch) if any of these
holds: connection setup/auth fails reproducibly, unit state notifications are
lost, new reboots/watchdog resets appear, or the free heap is **not** better
than before. Rolling back = **discard** the branch and restart it if needed;
`main` with the Bluedroid revision remains the fallback point.

## 5. Architecture of the switch

### 5.1 Keep NimBLE types out of the headers

`casambi_client.h` currently exposes `BLEClient*` and
`BLERemoteCharacteristic*` as private members plus the Bluedroid notify
signature. To reduce risk and avoid include sprawl:

- Switch the members to the NimBLE counterparts, but pull the `#include`s into
  the `.cpp` only and **forward-declare** in the header
  (`class NimBLEClient; class NimBLERemoteCharacteristic;`). NimBLE then becomes
  a pure implementation detail of `src/ble/`.
- The public API (methods, enums, callback `std::function` types) stays
  **byte-identical** — `main.cpp`, `web/`, `cloud/` need not be touched, apart
  from the three lifecycle calls (init/deinit, 5.3).

### 5.2 API mapping (Bluedroid → NimBLE 2.x)

| Bluedroid (today) | NimBLE 2.x | Note |
|---|---|---|
| `BLEDevice::init(name)` | `NimBLEDevice::init(name)` | same |
| `BLEDevice::deinit(true)` | `NimBLEDevice::deinit(true)` | same; frees controller RAM |
| `BLEDevice::createClient()` | `NimBLEDevice::createClient()` | NimBLE can pool/reuse clients |
| `BLEClient::connect(BLEAddress)` | `NimBLEClient::connect(NimBLEAddress)` | address type (public/random) may matter |
| `getService(BLEUUID)` | `getService(NimBLEUUID)` | same |
| `getCharacteristic(BLEUUID)` | `getCharacteristic(NimBLEUUID)` | same |
| `chr->readValue()` → `std::string` | `chr->readValue()` → `NimBLEAttValue` | `NimBLEAttValue` converts to `std::string`; review the length checks |
| `chr->writeValue(data, len)` | `chr->writeValue(data, len, response)` | set the `response` flag explicitly (write **with/without** response — verify the behaviour!) |
| `chr->registerForNotify(cb)` | `chr->subscribe(true, cb)` | **callback signature changed** (5.2.1) |
| `BLEScan* = BLEDevice::getScan()` | `NimBLEScan* = NimBLEDevice::getScan()` | same |
| `setAdvertisedDeviceCallbacks(BLEAdvertisedDeviceCallbacks*)` | `setScanCallbacks(NimBLEScanCallbacks*)` | **class + method renamed** |
| `onResult(BLEAdvertisedDevice dev)` (by value) | `onResult(const NimBLEAdvertisedDevice* dev)` (pointer) | access via `->` instead of `.` |
| `scan->start(sec, false)` | `scan->getResults(sec*1000, false)` or `start(...)` | check signature/unit (ms) |
| `dev.getServiceUUID().equals(uuid)` | `dev->getServiceUUID() == uuid` | comparison operator |
| `dev.haveName()/getName()` etc. | identical methods on the pointer | same |

#### 5.2.1 Notify callback

Bluedroid:
`void cb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool isNotify)`.
NimBLE 2.x:
`void cb(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool isNotify)`
— same parameters, only the type of the first argument changes. The existing
static trampoline construction (`g_clientInstance`) remains usable.

### 5.3 Lifecycle sites

`main.cpp` (177, 806, 1357 ff., 1415, 1525 ff.) and `setup_portal.cpp`
(305, 327) only swap `BLEDevice::` → `NimBLEDevice::`. The semantics of
`init`/`deinit(true)` are equivalent; the "reboot afterwards" logic stays.

## 6. Function-specific risks and mitigations

### 6.1 Notify timing/threading (highest risk)
NimBLE invokes notify callbacks from a dedicated task. The comment at
`_sendEncryptedPacket` describes **synchronous** Bluedroid behaviour (the
response fires during `writeValue`). Under NimBLE it fires **asynchronously**
shortly afterwards instead.
→ Mitigation: the `_encMutex` logic is already designed for "callback from a
foreign task" and should hold. Even so: test the auth and key-exchange flow
(which wait on `_totalReceivedPackets` polling with timeouts) specifically on
real hardware. The polling timeouts (2 s/5 s) stay as a safety net.

### 6.2 Write with/without response
Casambi control writes via `writeValue`. Whether that is "write with response"
or "without" was implicitly determined by Bluedroid so far. Under NimBLE it is
explicit. → Test both variants; reproduce the old behaviour as the default so
timing/reliability stay identical.

### 6.3 Address type on connect
Casambi devices are connected to by fixed MAC. NimBLE distinguishes
public/random addresses more strictly. → If `connect` fails, take the address
type from the scan result instead of blindly using the string address.

### 6.4 Optional follow-on benefit: the TLS heap workaround
If NimBLE saves enough RAM, a full `deinit(true)` before cloud access may no
longer be necessary. **Deliberately out of the mandatory scope** — measure first
(4.4), separately, and only once the core migration is stable. Otherwise the
risk is changing two variables at once.

### 6.5 Size of NimBLE buffers / MTU
NimBLE has its own build-time defaults (max connections, MTU, ATT buffer sizes)
via `nimconfig`/build flags. The defaults suffice for 1 connection; verify the
MTU behaviour when reading the device info (21 bytes) and the packets.

## 7. Implementation order (each step individually testable)

1. **Preparation/measurement.** Log the heap at the three points (4.4) on the
   current Bluedroid revision (`main`) as the reference — before the first
   migration commit.
2. **Pull in the NimBLE library.** Add the pinned `h2zero/NimBLE-Arduino`
   version to `lib_deps` in `platformio.ini` (no new environment). The build
   must still succeed.
3. **Migrate the scan** (`casambi_scan.cpp`, plus the two scan sites in
   `main.cpp` and the portal scan). Stateless, the smallest unit. Test: the
   portal/wizard scan finds the same Casambi devices as before.
4. **Migrate the client** (`casambi_client.*`): connect → service/characteristic
   → notify subscribe → readValue/writeValue. Test: key exchange, auth, a
   control command (set level), incoming unit state notification, auto-reconnect
   after link loss.
5. **Switch the lifecycle/deinit** at all sites; test the cloud refresh and the
   wizard path (BLE released → TLS succeeds → reboot into operating mode).
6. **Heap comparison** against the reference from step 1; check the acceptance
   criteria (8).
7. **Clean up.** Remove dead Bluedroid includes/leftovers, update docs/README.

## 8. Acceptance criteria

- The scan finds the same Casambi networks (count/MACs/names) as the Bluedroid
  build.
- The full connection lifecycle works: connect → ECDH → auth → control commands
  → incoming state updates → clean disconnect → auto-reconnect.
- Cloud refresh and first-time provisioning (portal + wizard) run through,
  including BLE deinit/TLS/reboot.
- **Free heap after auth measurably higher** than on the Bluedroid build.
- **No** new watchdog/heap reboots over a multi-hour soak run.
- Public API of `CasambiClient`/`CasambiScan` unchanged (no changes outside
  `src/ble/` other than the lifecycle calls).

## 9. Rollback

The entire migration lives on `claude/ble-nimbble-migration-uuwzu4` and is only
merged into `main` after passing acceptance (8). If something goes wrong, the
**branch is discarded** (and set up again if needed) — `main` still carries the
working Bluedroid revision. There is deliberately **no** parallel Bluedroid
build and no `#ifdef` bridge; the safeguard is branch isolation plus the
small-step, individually tested commits (7).

## 10. Effort estimate (rough)

| Step | Effort |
|---|---|
| Pull in the pinned NimBLE library | small |
| Scan migration | small |
| Client migration (incl. notify timing tests) | medium — the core of the risk |
| Lifecycle/deinit + cloud/wizard tests | small–medium |
| Heap measurement + soak run | medium (mostly waiting time) |
| Cleanup/docs | small |

By far the most sensitive block is the **client migration with notify timing**
(6.1) — that is where testing is concentrated.

## Sources

- [h2zero/NimBLE-Arduino (GitHub, incl. the 1.x→2.x migration guide)](https://github.com/h2zero/NimBLE-Arduino)
- [NimBLE-Arduino 2.1.0 release notes](https://newreleases.io/project/github/h2zero/NimBLE-Arduino/release/2.1.0)
- [esp-nimble-cpp v2.0.0 changelog (ESP Component Registry)](https://components.espressif.com/components/h2zero/esp-nimble-cpp/versions/2.0.0/changelog?language=en)
