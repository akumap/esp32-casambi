# Concept: ESP32 Casambi provisioning without serial

Status: implemented — SoftAP portal + cloud provisioning + mDNS + `/api/info`
are in place; **automatic gateway hopping (section 7) was dropped** after the
hardware test in section 7 showed it is unnecessary (Casambi moves the gateway
role internally), so reconnect stays pinned to the fixed MAC as before.
Branch: `claude/esp32-ap-rest-config-hejbf7`

## 1. Goal

After flashing, **no configuration over serial input** should be needed any
more. The **entire interactive first-time configuration** happens in **one** web
interface — the ESP32's **open SoftAP portal**:

- Wi-Fi selection + Wi-Fi password
- Casambi network password (BLE scan in the background; a **gateway choice** is
  only necessary if several Casambi networks are found — see 6.5)

The Casambi cloud fetch runs immediately afterwards with **live progress**. FHEM
then only connects to the **fully configured** device.

At runtime the ESP picks the gateway **automatically** and switches to the next
reachable unit of the same network if needed (see 7) — which removes the need
for the user to pick a fixed gateway.

The existing serial wizard is retained as a **fallback**.

## 2. Starting point (as-is)

- First-time configuration runs entirely over serial (`runSetupWizard`,
  `main.cpp:1297`): BLE scan → selection → Casambi password → Wi-Fi
  SSID/password → cloud download → save → reboot.
- Wi-Fi (STA) and the web server only start **once** a valid config exists.
- The FHEM module `98_CasambiGW.pm` connects by WebSocket to a hard-configured
  IP (`define <name> CasambiGW <ip>`) and controls through HTTP POSTs to
  `/api/units/...`.

The cloud steps are already encapsulated independently of serial
(`api_client.cpp`: `getNetworkId`, `createSession`, `fetchNetworkConfig`).

## 3. Constraints

### 3.1 Heap vs. BLE during cloud access
The TLS handshake to the Casambi cloud needs a large contiguous heap block that
is only free when the BLE stack is **not** initialised (see the comments at
`refresh`, `main.cpp:738`, and in the wizard). BLE therefore has to be
deinitialised during the cloud step.

### 3.2 Where does the `networkUuid` come from?
Cloud access needs **exactly one** piece of information from the BLE scan: the
`networkUuid`. Today that is the **BLE MAC** of the selected device
(`main.cpp:1353`, colons removed, lowercase). The first cloud call
`GET /networks/uuid/<networkUuid>` (`api_client.cpp:45`) takes this UUID as its
input. There is no source for the UUID other than the scan.
→ That mandates the order **scan before cloud**.

### 3.3 What works in AP mode and what does not

| Activity | pure AP mode | Reason |
|---|---|---|
| Wi-Fi list (`WiFi.scanNetworks`) | ✅ | no internet needed |
| **BLE scan** (Casambi gateway list) | ✅ | BLE + Wi-Fi AP coexist; **no TLS → no heap conflict** |
| entering/storing the Wi-Fi password + Casambi password | ✅ | pure input |
| **Casambi cloud fetch** | ❌ | needs internet (STA) **and** BLE off (heap) |

Consequence: **collecting the inputs** works entirely in the AP portal; the
**cloud fetch** additionally needs an STA connection and free heap. Solution:
**AP+STA mode** (see 6.3).

## 4. Order (principle)

```
1. WI-FI SELECTION  (AP portal)
2. BLE SCAN         (in AP mode → networkUuid candidates to choose from)   ← BLE on
   → BLEDevice::deinit()                                                   ← BLE off (heap free)
3. CASAMBI CLOUD    (uuid + password → keys/config; AP+STA)                ← BLE off, TLS needs heap
   → save + reboot
4. BLE CONNECTION   (authenticated, with the cloud keys)                   ← operating mode
```

An important distinction: **BLE scan ≠ BLE connection.**

| BLE activity | needs | yields | when |
|---|---|---|---|
| **Scan** (discovery, passive) | nothing | `networkUuid` (+ name/RSSI) | **before** the cloud step (in AP mode) |
| **Connection** (authenticated) | cloud **keys** | control of the lights | **after** the cloud step (operation) |

## 5. Boot state machine (2 states)

`setup()` decides based on `ConfigStore::hasValidConfig()`:

| State | Condition | Mode | BLE |
|---|---|---|---|
| **A. Setup** | `!hasValidConfig()` | open SoftAP portal (AP, AP+STA when needed) | on for the scan, off for the cloud step |
| **C. Operation** | valid config | operating mode (as today) + mDNS | on |

There is **no** separate "Wi-Fi ok but no Casambi config" state any more: the
config only counts as valid once the entire portal flow including the cloud
fetch has succeeded. If the procedure is aborted (e.g. a power cut mid-setup),
the device starts in the portal again; already stored Wi-Fi credentials are
pre-filled in the form.

## 6. Setup portal (state A)

### 6.1 Start
1. `WiFi.softAP("Casambi-Setup-XXXX")` **without a password** (open AP).
   XXXX = 4 hex digits from `ESP.getEfuseMac()`.
2. `DNSServer` on port 53 → captive-portal redirect to `192.168.4.1`.
3. AsyncWebServer serves the single page (HTML in `PROGMEM`, no LittleFS upload
   needed).

### 6.2 Portal endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | single page with both sections (Wi-Fi, Casambi) |
| `GET` | `/api/wifi-scan` | Wi-Fi list (`WiFi.scanNetworks`) as JSON |
| `POST` | `/api/ble-scan` | trigger a BLE scan (response `202`) |
| `GET` | `/api/ble-scan` | `{state:"scanning\|done", devices:[…]}` |
| `POST` | `/api/provision` | `{ssid, wifiPassword, networkUuid, casambiPassword}` (response `202`) |
| `GET` | `/api/provision/status` | `{state, msg, networkName?}` for live progress |

Long operations (BLE scan ~10 s, STA connect, cloud fetch) run **not** in the
AsyncTCP handler but in a **state machine in `loop()`** (with
`esp_task_wdt_reset()`); the handlers only record the job + parameters, and the
portal page polls the status.

### 6.3 Flow after "submit" (AP+STA with live progress)
1. Store the Wi-Fi credentials.
2. **`BLEDevice::deinit(true)`** (BLE scan finished → heap free).
3. `WiFi.mode(WIFI_AP_STA)`, establish the STA connection to the home Wi-Fi —
   the **AP stays up**, so the progress page in the browser remains reachable.
4. Cloud: `getNetworkId(uuid)` → `createSession(pw)` → `fetchNetworkConfig()`
   (choosing the `uuid`, see 6.5 — automatic when there is only one network).
5. Set `networkUuid/networkId/casambiPassword`;
   **`autoConnectAddress` = the MAC from `uuid`** (colons inserted) as the
   *preferred* first attempt, `autoConnectEnabled = true`; `saveNetworkConfig()`
   (which via `unit.address` contains the MAC list of **all** units → the basis
   for automatic gateway hopping at runtime, see 7).
6. Status → **done**, the portal shows the cloud-confirmed `networkName` →
   reboot into state C.
7. **Errors** at any point (wrong Casambi password, cloud unreachable, wrong
   Wi-Fi password) → status **error** with a message; back to pure AP mode, the
   page stays reachable, the user corrects and submits again.

### 6.4 Scan result (per device)
```json
{
  "uuid": "a1b2c3d4e5f6",      // MAC without colons = networkUuid candidate
  "mac":  "a1:b2:c3:d4:e5:f6",
  "name": "Living room",       // advertised name (often = the network name)
  "rssi": -62,
  "mfgData": "...",            // manufacturer data (hex), if present
  "svcData": "..."             // service data (hex), if present
}
```
`ScanCallbacks::onResult` is extended for this (today address/name/rssi only).

### 6.5 Choosing the right network (disambiguation during setup)
At first-time setup the ESP does **not** yet have the unit MAC list (that only
arrives with the cloud config), so the network filter from section 7 does not
apply here yet. Instead:

- **One** Casambi advertiser found → no selection, straight to the cloud
  attempt.
- **Several** advertisers → the **password is the unambiguous probe**: the ESP
  tries the candidates in turn (`getNetworkId` → `createSession`).
  `createSession` only returns `200` for the matching network, otherwise
  `401/403` ("Invalid password", `api_client.cpp:117`). The candidate that
  authenticates wins; its `networkName`, confirmed via `fetchNetworkConfig`, is
  shown in the portal.
- In addition the advertised **name** (BLE "complete local name",
  `main.cpp:557`) is displayed in the list, so the user can pre-select the
  probably-correct one and save attempts.

Caveats (hardware check points, see 9):
- **Two networks with an identical password** → both authenticate → genuine
  ambiguity; then show both `networkName`s and let the user choose (rare).
- **Two units of the same network** as candidates → whether both unit MACs
  resolve in the `getNetworkId` cloud lookup is open; a MAC that does not
  resolve simply drops out and the working candidate remains.

Cost: each attempt = one cloud round trip (BLE is off here anyway → heap free),
which is uncritical with a handful of advertisers.

**Repeated scanning for a targeted gateway choice (proven in practice):**
Because the Casambi mesh negotiates which unit currently advertises as
"connectable", every fresh BLE scan may show a **different** unit of the same
network. You can therefore repeat the scan in the portal **until the desired
gateway appears** and deliberately pick a unit that is **permanently powered and
not disconnected**. That matters today because reconnect still pins to the
stored MAC (automatic hopping, section 7, was dropped): choosing a stable,
fixed-location unit as the gateway increases operational reliability.

## 7. Automatic gateway hopping at runtime

> 🔎 **Hardware finding — hopping may be unnecessary (Casambi does it itself).**
> Observation: several advertisers are found (e.g. a named `air_module` and an
> unnamed participant `9ed82b331544`), but the **actual connection MAC is the
> same in both cases** (`9e:d8:2b:33:15:44`). That strongly suggests the Casambi
> network presents **one stable, network-wide gateway endpoint** (random static
> address) and handles forwarding into the mesh as well as any role change
> **internally**. Consequence: hopping on the ESP side would be **redundant** —
> the existing reconnect to the fixed `autoConnectAddress` would suffice as long
> as that endpoint stays stable.
>
> **Decisive test — carried out, hopping NOT needed.** The unit contacted
> initially ("AZ floor lamp", id=1) was switched off and on again: it went
> `OFFLINE` and back `ONLINE`, but the **BLE connection to the network persisted
> throughout** (no "connection lost", no reconnect, the WebSocket kept sending).
> Casambi therefore shifts the gateway role internally without the ESP losing
> the connection. **Result:** hopping on the ESP side will **not** be
> implemented; the existing reconnect to the fixed `autoConnectAddress`
> suffices. Section 7 is therefore **closed** (the subsections below remain only
> as background/history).

### 7.1 Basis: network-wide keys + a known MAC list
Two properties would make automatic switching possible:

1. **Network-wide authentication.** `connect()` uses the **network key** loaded
   via `_config->getBestKey()` (`casambi_client.cpp:112`), not device-specific
   keys; the ECDH exchange is fresh per connection. The same key can
   authenticate against **any** unit of the network — the MAC is not
   cryptographically special.
2. **A known MAC list.** After setup, the ESP knows the MACs of **all** units of
   its network via `unit.address` (`api_client.cpp:299`, persisted at
   `config_store.cpp:87`).

> ⚠ **Hardware finding (partially refutes assumption 2):** the gateway address
> actually connected to is a **random static** BLE address (observed e.g.
> `9e:d8:2b:33:15:44` — top two bits `10`) and does **not** match `unit.address`
> from the cloud. The planned **network filter via the known unit MAC list
> therefore does NOT work** (point 9.1 is thus answered in the negative).
> Moreover the address can change after a device restart — which can even break
> today's reconnect via the fixed `autoConnectAddress`. Consequence for hopping:
> network association would have to go through the **advertisement (service
> data / advertised name)** or through an **auth attempt with the network key**,
> not through unit MACs.

### 7.2 Procedure
Instead of pinning to a fixed MAC, the ESP would hop to the next reachable unit:

```
connection lost
   → BLE scan (service UUID 0xFE4D)
   → intersect with the known unit MAC list        ← network filter, no foreign network
   → connect to the best available candidate
   → ECDH + auth with the network key
```

- **Prioritisation:** the last-used gateway (`autoConnectAddress`) first, then
  last-online + strongest RSSI.
- **Network filter without service data:** intersecting with the known MAC list
  automatically excludes foreign Casambi networks — point 9.1 thus becomes
  **moot** for runtime (it stays relevant only for setup disambiguation,
  see 6.5).
- **Proactive scan:** online/offline events arrive through the unit state
  callback (`unit.online`, `main.cpp:182`). They help prioritise candidates and
  scan earlier on topology changes (faster detection). Limitation: those events
  travel over the **current** connection — if the current gateway drops, that is
  the transport itself; it works for *other* units, and otherwise the full MAC
  list serves as the fallback.

### 7.3 Why often only one device appears / switching duration
This is a **Casambi mesh property**: the participants negotiate **which unit
currently advertises as "connectable"** — usually only one at a time. The ESP
can only connect to a unit that is **currently advertising as connectable**; the
MAC list only tells it *which* ones belong to the network and are online, but
cannot force a unit to become the gateway.

Switching duration (estimated from the code):

| Phase | Time | Source |
|---|---|---|
| Detect the loss | cleanly ~0–10 s (callback / `CONNECTION_CHECK_INTERVAL_MS`), silent hang up to ~30 s (keepalive) | `config.h:111`, `main.cpp:285` |
| Find a new advertiser | seconds of scanning **+ mesh re-election (variable, Casambi side)** | unknown |
| Connect + key exchange + auth | typically ~1–3 s | `config.h:94` |

- **Several units connectable at once:** switching is virtually immediate
  → **~3–8 s**.
- **Only one at a time:** waiting for the mesh re-election → variable, the
  bottleneck.
- Worst case (silent hang + slow re-election): up to ~30–60 s; on top of that
  the backoff applies (`BLE_RECONNECT_INTERVAL_MS`=5 s → max 60 s) and after
  `MAX_RECONNECT_FAILURES`=10 a restart.

Whether several units advertise as connectable at the same time is the one point
that can only be measured on real hardware (see 9) — it decides between "almost
seamless" and "a noticeable pause".

## 8. mDNS / FHEM integration

- **Operating mode (state C):** mDNS hostname `casambi-XXXX` (XXXX = 4 hex
  digits from `ESP.getEfuseMac()`) → `casambi-a1b2.local`, service `_http._tcp`
  (port 80) with the TXT records `configured=1`, `build=<n>`, `network=<name>`.
  That makes **several gateways** on the Wi-Fi unambiguously distinguishable.
- **The FHEM define** accepts a hostname **or** an IP:
  - `define gw1 CasambiGW casambi-a1b2.local`
  - `define gw1 CasambiGW 192.168.178.111` (recommended together with a DHCP
    reservation on the router for the ESP MAC)
- **FHEM module changes are minimal:** no `scanNetworks`/`casambiSetup`
  commands, no status polling — the entire setup happens in the portal.
  Optional: `GET /api/info` (`{configured, build, hostname, mac, ip}`), so FHEM
  detects and reports when it points at a not-yet-configured device.

## 9. Open verification points

1. **Advertising MAC == `unit.address`? → NO (settled on hardware).** The
   gateway address connected to is a **random static** BLE address (e.g.
   `9e:d8:2b:33:15:44`) and matches **no** `unit.address`. The planned network
   filter via the unit MAC list (7.1/7.2) is therefore void; resolving the
   gateway MAC to a unit name does not work either (workaround: remember the
   advertised name at provisioning time + use the network name as a fallback,
   implemented). **Open/new:** does this address change on a device restart? If
   so, the reconnect via the fixed `autoConnectAddress` breaks too → hopping
   would have to go via the advertisement/auth probe.
2. **Several units connectable at once?** Determines the switching duration
   (7.3): almost seamless vs. waiting for the mesh re-election. Only measurable
   on real hardware.
3. **Setup disambiguation (6.5):** whether, with several units of the same
   network, both unit MACs resolve in the cloud `getNetworkId`; and the
   behaviour with two networks sharing a password.
4. **AP+STA + TLS, heap-wise:** with BLE deinitialised the TLS heap should be
   available; AP+STA itself needs little heap. Confirm on hardware.
5. **Scan duration:** a fixed 10 s BLE scan in the portal — proposed.

> Note: the earlier point "networkUuid = MAC or service data?" is **moot for
> runtime**, since the network filter goes through the known MAC list (7.2), not
> through service data.

## 10. Resource requirements (rough)

**No new external libraries:** `DNSServer` and `ESPmDNS` are part of the
ESP32 Arduino core; `ESPAsyncWebServer`/`AsyncTCP` are already linked in.

### 10.1 Flash (program)

| Component | rough |
|---|---|
| DNSServer (captive portal) | ~2–5 KB |
| ESPmDNS | ~5–10 KB |
| portal HTML in `PROGMEM` | ~3–8 KB |
| setup state machine, refactor, automatic hopping | a few KB |
| **Total** | **~15–30 KB** |

Uncritical: the partition is `huge_app.csv` (`platformio.ini`), and flash space
is plentiful.

### 10.2 RAM/heap
The sensitive item (no PSRAM, `HEAP_CRITICAL_THRESHOLD` = 20 KB):

| Situation | Additional demand | Assessment |
|---|---|---|
| Operation (state C): mDNS permanently | ~2–4 KB | uncritical |
| Automatic hopping: BLE scan only on connection loss | transient, existing scan infrastructure | uncritical |
| Setup: AP + portal + BLE scan | moderate, **no TLS at the same time** | uncritical |
| **Setup peak: AP+STA + TLS handshake (BLE off)** | AP_STA ~10–30 KB **+** TLS ~30–50 KB | **check critically** |

The only real bottleneck is the **transient peak during the cloud fetch in AP+STA
mode**. Because BLE is deinitialised then (6.3) it should fit — that is
verification point 9.4 and has to be measured on hardware.

> Way out if the peak turns out too tight: close the AP **before** the TLS step
> (pure STA) and only show progress after the reboot — sacrificing the live
> feedback for ~10–30 KB of heap.

Overall: a moderate flash increase, a negligible RAM increase at runtime; the
only risk is the brief setup peak, mitigated by the BLE deinit.

## 11. ESP firmware changes at a glance

1. `main.cpp` — `setup()` as a 2-state machine (setup portal / operation);
   provisioning state machine (idle/wifi-scan/ble-scan/connecting/fetching/
   done/error) in `loop()`. The serial wizard stays as a fallback.
2. `src/web/setup_portal.{h,cpp}` (new) — open SoftAP, DNSServer, single-page
   HTML, portal endpoints, AP+STA switchover.
3. Extend `ScanCallbacks` (mfg/svc data); the scan result as a shared struct for
   the serial and portal paths.
4. Extract the cloud steps from `runSetupWizard()` into
   `provisionFromCloud(uuid, pw, &cfg)` (used by **both** the portal and
   serial); multi-network disambiguation by password probe (6.5).
5. ~~**Automatic hopping at runtime** (`main.cpp` `checkAndReconnectBLE` +
   `casambi_client`): switch the reconnect from "fixed MAC" to "scan → intersect
   with the known unit MAC list → connect to the best candidate";
   `autoConnectAddress` only as a preferred first attempt; online/RSSI
   prioritisation.~~ **Dropped** — see the hardware finding in section 7.
6. `webserver.{h,cpp}` — mDNS in operating mode, optionally `/api/info`.
7. `config.h` — prefixes/constants (AP SSID, mDNS hostname).
8. `98_CasambiGW.pm` — optionally evaluate `/api/info`; otherwise unchanged.
9. Update the README.

## 12. Risk assessment (debugging effort)

| Area | Risk | Where debugging arises |
|---|---|---|
| Extracting `provisionFromCloud()` (refactor) | Low | behaviour-neutral, testable against the serial wizard |
| SoftAP + DNSServer + portal HTML | Low | standard pattern, testable without Casambi |
| `wifi-scan`, mDNS, scan struct | Low | well isolated for testing |
| Long ops in `loop()` instead of the async handler | Medium | concurrency/WDT, but the pattern is clear |
| BLE scan → `deinit` → cloud fetch | Medium | known to be delicate; the wizard proves feasibility |
| **AP+STA + TLS heap peak** | Medium | heap measurement (9.4); fallback "close the AP before TLS" |
| **Automatic hopping at runtime** | Higher (hardware-dependent) | mesh/advertising behaviour only checkable against the real network (9.1/9.2) |
| Setup disambiguation (6.5) | Low–medium | edge cases (9.3) only with a real account; the error paths are forgiving |

**Overall picture:** the firmware mechanics are low to medium risk and no
architectural new ground — the existing wizard already demonstrates the hard
parts (BLE-before-cloud, TLS, auth) working. The real debugging effort
concentrates on two on-hardware topics: the heap peak at the AP+STA+TLS moment
(measurable, with a fallback) and the Casambi mesh/advertising behaviour for
automatic hopping (iterative testing).

**Risk mitigation:**
- The serial wizard is retained as a fallback.
- Staged implementation (section 13): refactor first, then the portal, then
  automatic hopping **separately** and **afterwards** — so the largest single
  risk is not in the critical path of first-time commissioning.
- Use the existing tools: `heapDebug`, `bleDebug`, the non-volatile event log.

## 13. Implementation order

1. Extract `provisionFromCloud()` from the wizard (refactor,
   behaviour-neutral).
2. Extend `ScanCallbacks` + a shared scan struct.
3. SoftAP/captive-portal module with the single page + portal endpoints.
4. Provisioning state machine in `loop()` (incl. AP+STA + cloud fetch +
   disambiguation 6.5).
5. 2-state `setup()`; mDNS in operating mode.
6. ~~Implement automatic hopping in the reconnect (7).~~ **Dropped** — see the
   hardware finding in section 7.
7. Optionally `/api/info` + FHEM evaluation.
8. Update the README.
