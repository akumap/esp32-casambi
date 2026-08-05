# ESP32 Casambi Controller

An offline BLE controller for Casambi lighting systems, running on ESP32. Control your Casambi lights without cloud dependency after initial setup. Receives real-time state updates from the Casambi mesh network and provides current values via HTTP REST API for integration with home automation systems.

[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-00979D.svg)](https://www.arduino.cc/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

-----

## Features

- ✅ **Hybrid WiFi/BLE Operation** — One-time WiFi setup, then fully offline BLE control
- ✅ **Browser-Based Setup (no serial needed)** — On first boot the device opens an open Wi-Fi access point with a captive-portal page for Wi-Fi + Casambi provisioning; the serial wizard remains as a fallback
- ✅ **HTTP REST API** — Control and monitor lights from any home automation system
- ✅ **WebSocket Push** — Real-time state push to connected clients; no polling required
- ✅ **Built-in Web Interface** — `http://<esp32-ip>/` shows both link states (Casambi/Bluetooth and Wi-Fi/API) plus every unit with its generic control names and live values, and controls them: on/off, a slider per control, and a warm→cold Kelvin slider for colour temperature; responsive for phone and tablet, portrait and landscape
- ✅ **Network Console** — The serial console is also reachable over Telnet (port 23), token-authenticated, for debugging and `refresh` without a cable — see [Network Console (Telnet)](#network-console-telnet)
- ✅ **Real-Time State Tracking** — Receives status broadcasts from the Casambi mesh; current brightness, color temperature, and vertical distribution always up to date, even when lights are controlled via the Casambi app or other controllers
- ✅ **Complete Protocol Support** — Full Casambi Evolution protocol implementation (ECDH, AES-CTR, CMAC)
- ✅ **Generic Capability Detection** — Unit capabilities (dimmer, CCT, vertical) automatically derived from cloud API; no hardcoding of fixture types needed
- ✅ **Scene, Unit & Group Control** — On/off, brightness, color, temperature, vertical, slider
- ✅ **Auto-Connect & Auto-Reconnect** — Reconnects automatically on BLE link loss with exponential backoff
- ✅ **WiFi Auto-Reconnect** — Recovers WiFi connection silently in background
- ✅ **Hardware Watchdog** — Prevents permanent hangs (45s timeout)
- ✅ **Heap Monitoring** — Automatic restart on critical memory levels
- ✅ **Connection Health Checks** — Detects silent BLE disconnects
- 💾 **Persistent Storage** — Configuration and capabilities stored in LittleFS (survives reboots and reflashing)

-----

## Tested Hardware & Lights

### ESP32 Boards

**M5Stack ATOM Lite** — the only board this firmware is tested on.

|Property |Value                                                            |
|---------|-----------------------------------------------------------------|
|Chip     |ESP32-PICO-D4, revision v1.1                                      |
|Cores    |Dual core @ 240 MHz                                               |
|Radio    |WiFi + BT                                                         |
|Flash    |4 MB, embedded in package (Coding Scheme None)                    |
|Crystal  |40 MHz                                                            |
|PSRAM    |None — and none is required                                       |
|Other    |VRef calibration in efuse                                         |

Build footprint of the `devkit-v4` environment on this chip:

|Segment|Used                       |Capacity                    |     |
|-------|---------------------------|----------------------------|-----|
|RAM    |59 572 B (≈58 KB)          |532 480 B (520 KB)          |11.2 %|
|Flash  |1 596 065 B (≈1.52 MiB)    |3 145 728 B (3 MiB)         |50.7 %|

The flash capacity is the **app partition** of the `huge_app` layout
(`app0` = 0x300000), not the 4 MB chip total; the rest of the chip holds the
LittleFS data partition, NVS, the coredump area and the bootloader. `huge_app`
buys that 3 MB precisely by dropping the second OTA slot, so there is no
over-the-air rollback partition — updates go over serial. The RAM figure is
what the **linker** places statically;
it says nothing about free heap at runtime, which is what the stability notes
below (`free_heap`, `largest_block`) are about. Roughly half the app partition
is still free, so there is headroom for the firmware to grow.

The dual-core part matters beyond the spec sheet: the BLE host task and the
async_tcp task (pinned to core 1) genuinely run in parallel here, which is the
premise behind the allocation policy in `src/crypto/encryption.h`.

Other ESP32 boards are expected to work — nothing in the firmware is
board-specific — but they are untested. The `devkit-v4` build environment is
what the ATOM Lite is flashed with: both are plain ESP32 targets with 4 MB
flash, so the `huge_app` partition layout applies unchanged.

### Build Host

- Raspberry Pi (used for PlatformIO builds and serial monitoring)

### Tested Casambi Lights

The following luminaires have been tested with this controller:

|Luminaire                |Capabilities                             |Verified          |
|-------------------------|-----------------------------------------|------------------|
|Occhio Mito sospeso (air)|Brightness + Vertical + Color Temperature|✅ All controls    |
|Occhio Sento (air)       |Brightness + Vertical                    |✅ Brightness + Vertical|
|Occhio Luna sospeso (air)|Brightness + Color Temperature           |✅ Brightness + CCT|
|Occhio air module        |Brightness only                          |✅ Brightness      |
|Oligo Grace              |2 independent dimmers (Up-/Downlight) + Color Temperature|⚠️ Partially tested — SetState write verified in PR #39; per-channel dimming, and the web interface's per-channel sliders and atomic on/off, untested on hardware|

The controller should work with any Casambi-enabled luminaire. Capabilities are detected generically from the Casambi cloud configuration — no fixture-specific code is required.

-----

## Quick Start

### Requirements

- Any ESP32 board with BLE support
- USB cable for programming and serial communication
- Casambi network with at least one BLE-enabled device
- [PlatformIO](https://platformio.org/) (recommended) or Arduino IDE

### Installation

1. **Clone the repository:**
   
   ```bash
   git clone https://github.com/akumap/esp32-casambi.git
   cd esp32-casambi
   ```
1. **Build and upload:**
   
   ```bash
   # Default environment — used for the ATOM Lite and other plain ESP32 boards
   pio run -e devkit-v4 -t upload
   ```
1. **Open serial monitor:**
   
   ```bash
   pio device monitor --filter esp32_exception_decoder --filter time
   ```

### Initial Setup (Wi-Fi access point + web page)

On first boot (no stored configuration) the device opens an **open Wi-Fi access point** with a captive-portal web page — **no serial input required**.

1. Connect a phone/laptop to the open Wi-Fi network **`Casambi-Setup-XXXX`** (XXXX = last 4 hex digits of the chip MAC).
1. A setup page should open automatically (captive portal). If not, browse to **`http://192.168.4.1/`**.
1. **Scan Wi-Fi**, select your home network and enter its password.
1. **Scan Casambi gateways**, select your network and enter the Casambi network password. With several gateways you can just enter the password — only the matching network authenticates.
1. Press **Set up**. The device joins your Wi-Fi, downloads the network configuration from the Casambi Cloud (unit capabilities included), saves it and reboots into operation mode.

The page shows live progress and any error (wrong password, cloud unreachable), so you can correct it and retry.

After provisioning the controller auto-connects to the Casambi gateway, serves the REST API + WebSocket once Wi-Fi is up, and advertises itself via mDNS as **`casambi-XXXX.local`**. Browsing to **`http://<esp32-ip>/`** then opens the [web interface](#web-interface-status--control-page) with both link states, all device values and direct control.

> **Tip:** The Casambi mesh rotates which unit advertises as connectable, so repeating **Scan Casambi gateways** may reveal different devices. Pick one that stays powered. (The ESP connects to a stable network-level gateway endpoint; Casambi handles the mesh internally, so no gateway selection is critical for normal operation.)

A factory reset (`clearconfig` over serial) wipes the configuration and returns the device to this setup portal on the next boot.

#### Serial setup (fallback)

The serial wizard is still available. Open the serial monitor and run:

```
> setup
```

The wizard will:

1. Scan for nearby Casambi networks via BLE
1. Auto-discover your network UUID from the device MAC
1. Connect to WiFi (you provide credentials)
1. Download network configuration from Casambi Cloud (including unit capabilities)
1. Save everything to flash
1. Reboot into operation mode

On subsequent boots the controller reconnects automatically using the saved gateway address.

-----

## Usage

### Serial Commands

#### Connection

```bash
scan               # Scan for Casambi devices (shows RSSI and BLE address type)
connect 0          # Connect to first device found (auto-saves MAC)
disconnect         # Disconnect from device
status             # Show detailed system status (BLE, WiFi, heap, uptime)
blediag            # BLE troubleshooting report: config, keys, the phase the last
                   # connect attempt died in, plus a live scan of what is actually
                   # advertising nearby (paste this into a bug report)
restart            # Restart ESP32
```

#### Scene Control

```bash
son 10             # Turn scene 10 ON
soff 10            # Turn scene 10 OFF
slevel 10 128      # Set scene 10 to 50% intensity
```

#### Unit Control

```bash
uon 5              # Turn unit 5 ON
uoff 5             # Turn unit 5 OFF
ulevel 5 200       # Set unit 5 brightness to 200/255
utemp 5 3000       # Set unit 5 to warm white (3000K)
ucolor 5 255 0 0   # Set unit 5 to red
uvertical 1 200    # Set light balance (0=top only, 127=both, 255=bottom only)
uslider 2 200      # Set motor position (0=up, 255=down)
```

#### Group Control

```bash
glevel 4 128       # Set group 4 brightness to 50%
gvertical 4 200    # Set group light balance
gslider 4 200      # Set group motor position
```

**Note on motor and light distribution controls:** The `vertical` command controls light distribution between top and bottom emitters (0=top only, 127=both equally, 255=bottom only). The `slider` command controls physical motor position. The actual behavior depends on your specific fixtures. Motor control (`slider`) has not been extensively tested and may not work on all devices — use scenes for reliable motor control.

#### Configuration

```bash
autoconnect on/off/status    # Auto-connect control
autoconnect set <mac>        # Set MAC for auto-connect
reconnect on/off             # Enable/disable auto-reconnect on link loss
wifi set <ssid> <password>   # Update WiFi credentials (saves, then restarts —
                             # the device comes back up on the new network)
wifi status                  # Show WiFi connection status
debug on/off/status          # Toggle all debug output (restores/saves per-category settings)
debug ble on/off             # BLE/crypto layer verbose logging
debug casambi on/off         # Casambi network events (unit states, echo, callbacks)
debug web on/off             # HTTP API request logging
debug parse on/off           # Protocol parse output with raw bytes (for analysis)
debug heap on/off            # Heap monitoring output
debug cloud on/off           # Dump raw cloud config on refresh (AES keys redacted)
refresh [password]           # Re-download config from Casambi cloud. Uses the
                             # saved password if none is given; a given password
                             # overrides it without an interactive prompt (the
                             # command never blocks waiting for typed input, so
                             # it also works over the telnet console below).
                             # (with 'debug cloud on', prints the raw JSON for analysis)
clearconfig                  # Factory reset
```

#### Information

```bash
list units         # Show all units with ON/OFF state
list groups        # Show all groups with IDs
list scenes        # Show all scenes with IDs
```

#### Network Console (Telnet)

The serial console above is also reachable over the network with a standard
Telnet client — useful when the device is mounted somewhere for good BLE
reception rather than sitting next to the machine you're debugging from
(design rationale: `docs/konzept-tcp-konsole.md`). It mirrors the exact same
commands and output as the serial console; **`setup` and `wifi set` are
serial-only** (they would leak WiFi/Casambi credentials over an unencrypted
connection and drive the same blocking BLE scan the setup wizard uses).
**Flashing still requires a USB/serial connection** — this does not replace
it.

The console starts automatically once a Casambi network password is stored
(so a login token exists — see below) and Wi-Fi is connected, listening on
**port 23**, the Telnet default. In PuTTY: *Session* → enter the device's
hostname/IP → *Connection type: Telnet* → *Open*.

**Login uses the same derived token as the REST API** (see
[Authentication](#authentication) above), not the raw Casambi network
password — so the Casambi cloud credential itself never goes out over the
unencrypted Telnet connection:

```
Password: <apiToken>   # SHA-256("casambi-api:" + <casambi-network-password>), lowercase hex
```

Save the computed token in the PuTTY session profile so you only type it
once. Three failed attempts close the connection. Only one session is
allowed at a time — a second connection attempt is refused while one is
active.

```
telnet status              - Show whether the console is listening, whether a
                             session is active, and dropped-byte count for a
                             client that couldn't keep up with the output
telnet timeout <seconds>   - Idle timeout before an inactive session is closed
                             (default 900s / 15 min); 0 disables it, e.g. for
                             an overnight capture. Measured from the last
                             complete command line, not the last byte — a
                             client's own Telnet keepalive does not reset it.
```

-----

## Web Interface (status & control page)

Open **`http://<esp32-ip>/`** (or `http://casambi-XXXX.local/`) in any browser —
the gateway serves a page for humans: it shows what the gateway currently sees
and lets you switch and dim the lights right there, no app and no FHEM needed.

The page needs no internet access and no build step: it is a single
self-contained HTML page in the firmware (`src/web/dashboard.h`), and it uses
the very same interface every other client uses (`GET /api/status`,
`GET /api/units`, the `POST /api/units/…` control routes, and the `/ws` push
channel).

**What it shows**

The **status line** at the top always shows the two link states as pills
(`Bluetooth: connected`, `API: live`). The three detail cards behind it are
**collapsed by default** so the devices are on screen right away — tap the
status line (the chevron on the right shows the state) to fold them out; the
choice is remembered on that device.

| Section | Content |
|---------|---------|
| **Bluetooth side** | Link state (connected / disconnected, incl. the BLE state machine), Casambi network name, connected gateway unit (MAC + resolved name), link RSSI, link uptime, packets received, last disconnect reason, Casambi protocol version vs. the tested range, unit count |
| **API side** | Push/polling state of this browser session, Wi-Fi SSID + RSSI, IP address, whether the API requires a token, interface version (`api_version_major.minor`), firmware build |
| **System** | Device uptime, free heap (+ largest block), reboot count, UTC time + NTP server, dropped pushes / parse counters |
| **Devices** | One card per Casambi unit: name, unit ID, BLE MAC, reachability (`on` / `off` / `offline`) and **every fixture control with its generic name and current value** — exactly the cloud-derived control names `POST /api/units/:id/state` accepts (`dimmer`, `dimmer0`, `dimmer1`, `vertical`, `temperature`, `white`, …). Dimmers are shown in percent, `temperature` in Kelvin, everything else raw; new control types appear automatically |

**What you can control**

- **On/off** — the `on` / `off` pill on each card is a button. On single-dimmer
  fixtures it posts to `/on` and `/off` (level 255 / 0). On **multi-dimmer**
  fixtures it writes *all* channels in one atomic `/state` telegram, and `off`
  remembers the current split so the next `on` restores it (same behaviour as
  the FHEM module).
- **Sliders with a knob** — one per writable control, draggable with mouse,
  finger or the arrow keys. A vertical swipe that starts on a slider still
  scrolls the page.
- **Colour temperature** — its own slider running from the fixture's own
  minimum to its maximum Kelvin (`min`/`max` of the `temperature` control),
  in 50 K steps, which is the resolution of the Casambi temperature operation.
  The track carries a **warm → cold gradient** (light orange to light blue), so
  the knob position reads as a colour.
- Every widget writes through the documented endpoint for its control type —
  `/level`, `/vertical`, `/slider`, `/temperature`, or the generic `/state` for
  multi-dimmer channels and types like `white`. **Unknown control types stay
  display-only**: their value range is not part of the interface, so a slider
  would be guesswork.

While the Bluetooth link is down every widget is disabled (the control
endpoints answer `503` anyway) and the section note says so. A rejected write —
`503` because the command queue is full, `409` for a unit without a fixture
layout — is shown as a short message at the bottom of the screen.

Because the gateway only *queues* a command (`202 Accepted`) and reports the
result later as a `unit_state` push, the page keeps the value you just set on
screen until the device echoes it back (or for ~2.5 s), so the knobs never jump
while a value is in flight. Dragging is throttled to one write per 250 ms — the
newest value wins and the value you release on is always sent — which keeps the
`BLE_CMD_QUEUE_DEPTH`-deep command queue from overflowing.

State changes arrive live over the WebSocket (`hello`, `unit_state`,
`connection_state`). If the WebSocket is unavailable — a proxy in between, or
the client limit `WS_MAX_CLIENTS` is exhausted — the page falls back to polling
the REST endpoints every 5 s and says so ("API: polling"). While the gateway
does not answer at all, the last known values stay on screen with a banner, and
the page reconnects on its own.

An open page counts as **one WebSocket client** against `WS_MAX_CLIENTS`
(3 by default; beyond that the gateway drops the *oldest* connection, which may
be FHEM's). To keep that budget free the page closes its WebSocket whenever it
is not visible — switching tabs or apps releases the slot immediately, and
returning to the page reconnects and refreshes it.

**Authentication.** When a Casambi password is stored, the page asks for it
once. The API token is derived **in the browser** —
`SHA-256("casambi-api:" + password)`, the same derivation the FHEM module uses
(see [Authentication](#authentication)) — and kept in `localStorage`; the
password itself never goes on the wire. *Forget password* at the bottom of the
page clears it again. Gateways without a stored password (open API) show the
page right away.

> The page is delivered over plain HTTP like the rest of the API — treat the
> controller as a trusted-LAN device (see [Security](#security)).

**Layout.** The page adapts to phones and tablets in both portrait and
landscape (single column up to a 320 px-wide screen, multi-column from tablet
width on) and follows the system light/dark preference.

-----

## HTTP REST API

The ESP32 provides a REST API for integration with home automation systems (FHEM, Loxone, Home Assistant, Node-RED, etc.). The API serves both control commands and real-time state information from the Casambi mesh.

### Base URL

```
http://<esp32-ip>/api
```

The IP address is displayed in the serial console on boot.

### Authentication

Once a Casambi network password is stored on the device (after provisioning),
**every endpoint except `GET /api/info` requires an API token** in the
`X-API-Key` request header; the WebSocket upgrade requires it too (as the
`X-API-Key` header or a `?k=<token>` query parameter). Unauthenticated requests
get `401 Unauthorized` — including a rejected WebSocket upgrade, so clients can
tell an auth problem from a wrong path.

The token is **not** the raw Casambi password — it is derived from it, so the
cloud password never travels on the wire:

```
apiToken = SHA-256( "casambi-api:" + <casambi-network-password> )   # lowercase hex
```

Compute it once and send it with each request, e.g.:

```bash
TOKEN=$(printf 'casambi-api:%s' "$CASAMBI_PASSWORD" | sha256sum | cut -d' ' -f1)
curl -H "X-API-Key: $TOKEN" http://<esp32-ip>/api/status
```

The **FHEM module derives the token automatically** — just set the password
(stored in FHEM's obfuscated key-value store, not in fhem.cfg):

```
set <gw> password <casambi-network-password>
```

(The legacy plaintext attribute `attr <gw> casambiPassword <pw>` still works;
the key store takes precedence when both are set.)

Notes:
- A device with **no** stored Casambi password (e.g. a config predating this
  feature) keeps the API **open** for backward compatibility. Re-provisioning or
  `refreshCasambi` stores the password and switches auth on.
- This protects against unauthorized LAN devices and cross-origin browser
  scripts. It is **not** a substitute for transport encryption: there is no TLS,
  so a party that can already sniff your LAN can capture the token. Treat the
  controller as a trusted-LAN device.

### Status & Discovery

**GET /api/info** — Lightweight discovery endpoint (used by the FHEM module)

```json
{ "configured": true, "build": 42, "api_version_major": 1, "api_version_minor": 1 }
```

Served in both modes: while the device is still in the setup portal it returns
`"configured": false`; once provisioned it returns `"configured": true`. A client
can poll this to tell a ready gateway apart from one still in setup, and connect
automatically once it becomes ready. This is the **only** endpoint that stays
unauthenticated (so discovery works before the client knows the token), and it
deliberately exposes nothing beyond `configured`/`build` and the interface
version — no network name, MAC or IP.

`api_version_major`/`api_version_minor` carry the **ESP↔FHEM interface
version** (one shared version for the REST API and the WebSocket protocol; see
[Interface versioning](#interface-versioning-esp--fhem) below). Firmware
predating interface versioning omits the fields; clients must treat that as
version `1.0`.

**GET /api/status**

```json
{
  "ble_connected": true,
  "ble_state": 3,
  "network_name": "My Home",
  "wifi_ssid": "MyNetwork",
  "wifi_ip": "192.168.1.100",
  "wifi_rssi": -32,
  "uptime_ms": 123456,
  "free_heap": 56000,
  "largest_block": 42000,
  "min_free_heap": 31000,
  "boot_count": 12,
  "ws_drops": 0,
  "ws_send_fails": 0,
  "http_busy": 0,
  "parse_partial": 0,
  "parse_malformed": 0,
  "ntp_server": "pool.ntp.org",
  "time_synced": true,
  "time_utc": "2026-06-20T09:15:42Z",
  "time_utc_ms": 1781946942000,
  "gateway_mac": "aa:bb:cc:dd:ee:01",
  "connection_uptime_ms": 98765,
  "packets_received": 42,
  "gateway_rssi": -67
}
```

`boot_count` is a power-loss-surviving counter (stored in NVS). `time_synced`
is `false` until NTP has set the clock; `time_utc*` fields appear only once synced.

Diagnostics for load/stability analysis (all cumulative since boot;
`scripts/stress_test.py` samples them and reports their deltas over a run):
`largest_block` is the largest contiguous free heap block — it exposes
fragmentation that `free_heap` alone hides — and `min_free_heap` is the
all-time low-water mark, catching transient dips between status polls.
`ws_drops` counts WebSocket broadcast events dropped on a full queue (each
drop triggers a fresh `hello` snapshot, so clients never stay stale).

`ws_send_fails` and `http_busy` count the two heap-admission guards, and are
**not** failures in the sense the other counters are — they are the device
refusing work it cannot currently afford, instead of attempting it and
aborting. `ws_send_fails` counts WebSocket payloads not sent because the heap
could not serve the copy the framework makes (a `hello` then degrades to a
unit-less snapshot; a dropped broadcast sets the resync flag). `http_busy`
counts expensive GETs answered `503` + `Retry-After` before building a
response. Both rising under load means the guards are working; both staying at
zero through a stress run means there was headroom to spare.
`parse_partial` / `parse_malformed` count BLE packets that were only
partially decoded (understood prefix applied, undecoded tail dropped —
likely a protocol element the reverse-engineering does not cover yet) or
rejected entirely (nothing usable). Both fields are the **sum across packet
types** 0x06 / 0x07 / 0x08. The **per-type breakdown** is available via the
serial `status` command, which prints these lines only when a counter is
non-zero:

```
  Partially decoded packets: 0x06=0 0x07=0 0x08=0
  Malformed packets dropped: 0x06=0 0x07=0 0x08=0
```

In normal operation both stay at 0; a rising counter points to a protocol
element worth capturing (get the raw hex with `debug parse on`).
`gateway_rssi` is the BLE link strength to the gateway in dBm (refreshed every
~10 s; `0` = not measured yet). After a disconnect the response additionally
carries `last_disconnect_reason` (numeric `DisconnectReason`),
`last_disconnect_reason_name` (its readable form, e.g. `auth-failed`) and
`last_disconnect_source` — which detector saw the loss: `silent` (health check
found the link dead), `keepalive` (no response to the periodic read), `send`
(link dead when sending a command), `connect` (failure during setup) or `user`.
While the link is **down**, `last_connect_phase` and `last_connect_rc` report
how far the last connect attempt got (`link`, `service`, `characteristic`,
`devinfo`, `keyexchange`, `auth`, `ready`) and the NimBLE return code that
ended it — the same information the serial `blediag` command prints in full.
The same reason/source pair and the last known RSSI are written to the event
log on every loss, and each successful auto-reconnect logs the attempt count
and offline duration.

All units of a Casambi network advertise the **same virtual BLE address**, so
each connect lands on a random physical unit ("gateway lottery"). To avoid
getting stuck on a distant luminaire for a whole session, an **RSSI quality
gate** re-rolls the connection: if the settled link RSSI after connect is below
`BLE_MIN_CONNECT_RSSI` (default −85 dBm, `config.h`), the link is dropped and
re-connected (up to `BLE_RSSI_REROLL_MAX` times, then the last roll is accepted
regardless — connectivity beats quality). With an always-powered unit near the
ESP32 the re-roll almost always lands there. Re-rolls appear in the event log
(`BLE gateway re-roll 1/2: rssi=-91 < -85`).

**GET /api/units** — List all units with current state

Streamed in HTTP chunks, one unit at a time, so the response never needs a
single contiguous allocation proportional to the network size. May answer
`503` with a `Retry-After` header when the heap is momentarily too fragmented
to start — the request was fine, retry after the given number of seconds. A
unit whose JSON would exceed the per-entry buffer is emitted as
`{"id":N,"truncated":true}` rather than breaking the array.

```json
{
  "units": [
    {
      "id": 7,
      "name": "Mito sospeso",
      "type": 19425,
      "address": "aa:bb:cc:dd:ee:02",
      "online": true,
      "on": true,
      "level": 200,
      "numChannels": 3,
      "vertical": 127,
      "colorTemp": 58,
      "cctMin": 2700,
      "cctMax": 4000,
      "controls": [
        { "type": "dimmer",      "name": "dimmer",      "value": 200 },
        { "type": "vertical",    "name": "vertical",    "value": 127 },
        { "type": "temperature", "name": "temperature", "value": 58, "kelvin": 2996, "min": 2700, "max": 4000 }
      ]
    },
    {
      "id": 2,
      "name": "air module",
      "type": 1422,
      "address": "aa:bb:cc:dd:ee:03",
      "online": true,
      "on": true,
      "level": 255,
      "numChannels": 1,
      "controls": [
        { "type": "dimmer", "name": "dimmer", "value": 255 }
      ]
    }
  ]
}
```

`on` and `online` (since API 1.3) are the Casambi unit's own flags bits from the
0x06 status broadcast, passed through verbatim — `on` is no longer derived from
`level > 0`. In every real capture gathered so far (5 fixture types, full
dimmer/vertical/temperature sweeps, genuine mains power-cycle transitions —
see `docs/captures/2026-08-04-0x06-framing/`), the device's `on` bit was
indistinguishable from `online`, so this firmware makes no claim about
whether a unit is "currently glowing" — a consumer wanting that should derive
it from `level`/`controls` (the FHEM module's multi-dimmer handling already
does this).

The `controls` array is the canonical, cloud-derived per-channel state: one entry
per fixture control, named by its control type, with the raw `value` (0–255) and,
for `temperature`, the resolved `kelvin` plus its `min`/`max` bounds. Consumers
(e.g. the FHEM integration) should read `controls` to name readings generically.

Each entry also carries a `name` (since API 1.1): the unique per-unit control
name used for addressing in `POST /api/units/:id/state`. It equals the control
type, except when a type occurs more than once on a unit — then the controls
get a 0-based index in fixture order. A dual-dimmer fixture (e.g. Oligo Grace
Uplight/Downlight) therefore reports
`{ "type": "dimmer", "name": "dimmer0", ... }` and
`{ "type": "dimmer", "name": "dimmer1", ... }`.

The legacy `level`, `vertical`, `colorTemp`, `cctMin`, and `cctMax` fields are
kept for backward compatibility and only appear for units that support them;
`colorTemp` is the same normalized 0–255 value (`kelvin = cctMin + (colorTemp /
255) * (cctMax - cctMin)`). All state fields are updated in real-time from status
broadcasts — changes via the Casambi app, timers, sensors, or other controllers
are reflected here.

**GET /api/groups** — List all groups

**GET /api/scenes** — List all scenes

### Control Endpoints

> All endpoints below (and the status/log/NTP endpoints above) require the
> `X-API-Key` header when the device has auth enabled — see
> [Authentication](#authentication). It is omitted from the examples for brevity;
> add `-H "X-API-Key: $TOKEN"` to each call.

#### Scenes

```bash
curl -X POST http://<ip>/api/scenes/10/on
curl -X POST http://<ip>/api/scenes/10/off
curl -X POST http://<ip>/api/scenes/10/level \
  -H "Content-Type: application/json" -d '{"level": 128}'
```

#### Units

```bash
curl -X POST http://<ip>/api/units/5/on
curl -X POST http://<ip>/api/units/5/off
curl -X POST http://<ip>/api/units/5/level \
  -H "Content-Type: application/json" -d '{"level": 200}'
curl -X POST http://<ip>/api/units/5/temperature \
  -H "Content-Type: application/json" -d '{"kelvin": 3000}'
curl -X POST http://<ip>/api/units/5/color \
  -H "Content-Type: application/json" -d '{"r": 255, "g": 100, "b": 0}'
curl -X POST http://<ip>/api/units/1/vertical \
  -H "Content-Type: application/json" -d '{"value": 127}'
curl -X POST http://<ip>/api/units/2/slider \
  -H "Content-Type: application/json" -d '{"value": 200}'

# Generic atomic full-state write (API >= 1.1): set one or more controls BY
# NAME (see the "name" field in GET /api/units) in a single BLE telegram.
# Controls not named keep their current value. This is the only way to change
# several channels atomically — e.g. both dimmers of a dual-dimmer fixture:
curl -X POST http://<ip>/api/units/9/state \
  -H "Content-Type: application/json" -d '{"dimmer0": 255, "dimmer1": 128}'
```

`/api/units/:id/state` requires the unit's fixture control layout (fetched
from the Casambi cloud on setup/refresh and persisted); it answers `409` when
no layout is known (run `set <gw> refreshCasambi` / serial `refresh` once) and
`400` for unknown control names or out-of-range values. Values are raw
(`0 … 2^length-1`, i.e. 0–255 for the usual 8-bit controls). The write is
atomic: the ESP32 merges the named controls into the unit's current state
vector and sends one `SetState` telegram — required because the fixture
resets any control whose byte is missing/zeroed (observed for the colour
temperature on Oligo Grace).

#### Groups

```bash
curl -X POST http://<ip>/api/groups/4/level \
  -H "Content-Type: application/json" -d '{"level": 128}'
curl -X POST http://<ip>/api/groups/4/vertical \
  -H "Content-Type: application/json" -d '{"value": 127}'
curl -X POST http://<ip>/api/groups/4/slider \
  -H "Content-Type: application/json" -d '{"value": 200}'
```

#### Maintenance

```bash
# Re-read the configuration from the Casambi cloud using the stored password.
# Returns {"status":"refreshing"} and reboots; the download runs at the next
# boot and the device comes back up with the fresh config (see "refresh" below).
curl -X POST http://<ip>/api/refreshCasambi

# Reboot the device
curl -X POST http://<ip>/api/reboot
```

`/api/refreshCasambi` responds with HTTP 409 if no configuration or no stored
Casambi password is present yet (run `setup`/`refresh` once via serial first).

### Response Format

**Control commands** (scene/unit/group on/off/level/color/temperature/
vertical/slider/state) answer `202 Accepted` with `{"success": true, "queued": true}`:
the command is validated, queued and then executed on the ESP32's loop task so
BLE operations never stall the HTTP/WebSocket server. The resulting state
change arrives as a WebSocket `unit_state` event; a command that later fails
on the BLE link is recorded in the event log (`GET /api/log`) and produces no
`unit_state` event. `503` means the command queue is full — retry shortly.

**Other endpoints:** `{"success": true}` with HTTP 200.

**Error:** `{"success": false, "error": "Unit not found"}`

-----

## Event Log & Time Synchronization

The controller keeps a **non-volatile, time-stamped event log** so that fatal
system events — crashes, watchdog resets, brownouts, BLE auth failures, WiFi
loss, low-heap restarts — can be diagnosed after the fact.

### Storage layers

1. **RTC NOINIT RAM** (`LOG_RTC_CAPACITY` entries): every event is written here
   first. RTC RAM survives watchdog, panic, and software resets (but **not**
   power-off or brownout), so the "last words" right before a crash are
   captured even if they never reach flash. On the next boot these entries are
   flushed into LittleFS.
2. **LittleFS ping-pong ring buffer** (`2 × 16 KB`, `/log/log_a.bin` and
   `/log/log_b.bin`): the active file is filled, then the other file is cleared
   and becomes active, bounding flash usage and wear while retaining the most
   recent history.

### Timestamps

Each entry carries a `tsUtc` timestamp: an **ISO 8601 UTC** string
(`YYYY-MM-DDTHH:MM:SS.mmmZ`), set via NTP after WiFi connects. Before the clock
is synced, the timestamp is derived from the device **uptime** instead, which
renders as a **1970** date — so any entry whose year is 1970 was logged before
NTP sync, with the time-of-day portion encoding the uptime (e.g.
`1970-01-01T00:00:45.000Z` = 45 s after boot). Combined with the `boot` counter
this keeps pre-sync entries ordered and attributable to a specific boot. Clients
(e.g. FHEM) can convert UTC to local time for display.

### Endpoints

```bash
# Newest entries first (full log)
curl http://<ip>/api/log

# Limit to the newest 50 entries
curl "http://<ip>/api/log?n=50"

# Clear the log
curl -X DELETE http://<ip>/api/log

# Show / change the NTP server (UTC)
curl http://<ip>/api/ntp
curl -X POST http://<ip>/api/ntp \
  -H "Content-Type: application/json" -d '{"server": "192.168.1.1"}'
```

While the NTP server is the untouched default (`pool.ntp.org`), the device
tries the **local router first**: the DHCP-provided DNS server and the gateway
(when they are private RFC 1918 addresses) are queried before the public pool.
Most home routers serve NTP, so time sync then works even without internet
access; if the router does not answer, SNTP falls through to the pool after a
few seconds. Setting a server explicitly (`ntp set` or `POST /api/ntp`)
disables this auto-detection — the configured server is then used exclusively.
The effective candidate order is shown by the serial `ntp status` command.

Each log entry:

```json
{
  "tsUtc": "2026-06-20T09:15:42.000Z",
  "boot": 12,
  "level": 4,
  "levelName": "CRITICAL",
  "msg": "Boot #12, reset reason: task watchdog (6)"
}
```

Levels: `0=DEBUG 1=INFO 2=WARN 3=ERROR 4=CRITICAL`. A `tsUtc` with year **1970**
marks an entry logged before NTP sync (its time-of-day portion is the uptime).

### Serial commands

```
log [n]            - Show newest n event-log entries (default 30)
log clear          - Erase the event log
ntp status         - Show NTP server and sync state
ntp set <host|ip>  - Set the NTP server hostname or IP, e.g.
                     pool.ntp.org or 192.168.1.1 (time is always UTC)
```

-----

## WebSocket Push

In addition to the REST API, the ESP32 pushes state changes in real time to all connected WebSocket clients. This eliminates polling and delivers updates with sub-100 ms latency.

### Endpoint

```
ws://<esp32-ip>/ws
```

A maximum of **3 simultaneous WebSocket clients** is enforced (`WS_MAX_CLIENTS`
in `src/config.h`); the server trims the client list to this many and closes
excess/stale connections, bounding memory use under connection churn. The
realistic need is small — typically the FHEM gateway plus an optional browser.

### Messages (server → client, JSON)

#### `hello` — sent immediately on connect

Full state snapshot of all units, including stable BLE MAC address and
capability flags used by the FHEM integration for device identification:

```json
{
  "type": "hello",
  "build": 1,
  "api_version_major": 1,
  "api_version_minor": 1,
  "casambi_protocol_version": 11,
  "casambi_protocol_min": 10,
  "casambi_protocol_max": 11,
  "network": "My Home",
  "ble_connected": true,
  "units": [
    {
      "id": 7,
      "name": "Mito sospeso",
      "address": "aa:bb:cc:dd:ee:02",
      "uuid": "1234abcd-...",
      "online": true,
      "on": true,
      "level": 200,
      "hasCCT": true,
      "hasVertical": true,
      "numChannels": 3,
      "vertical": 127,
      "colorTemp": 58,
      "cctMin": 2700,
      "cctMax": 4000,
      "controls": [
        { "type": "dimmer",      "name": "dimmer",      "value": 200 },
        { "type": "vertical",    "name": "vertical",    "value": 127 },
        { "type": "temperature", "name": "temperature", "value": 58, "kelvin": 2996, "min": 2700, "max": 4000 }
      ]
    },
    {
      "id": 2,
      "name": "air module",
      "address": "aa:bb:cc:dd:ee:03",
      "uuid": "5678efgh-...",
      "online": true,
      "on": true,
      "level": 255,
      "hasCCT": false,
      "hasVertical": false,
      "numChannels": 1,
      "controls": [
        { "type": "dimmer", "name": "dimmer", "value": 255 }
      ]
    }
  ]
}
```

The `controls` array (same shape as in `GET /api/units`, including the `name`
used to address controls in `POST /api/units/:id/state`) is the canonical,
cloud-derived per-channel state that the FHEM integration uses to name readings
generically. The legacy `vertical`, `colorTemp`, `cctMin`, and `cctMax` fields
remain for compatibility and appear only for units that support them.

`on`/`online` here and in `unit_state` below are the device's own flags bits
(since API 1.3) — see the `on`/`online` note under `GET /api/units` above,
which applies identically to both WebSocket messages.

`api_version_major`/`api_version_minor` are the ESP↔FHEM interface version
(see [Interface versioning](#interface-versioning-esp--fhem)).
`casambi_protocol_version` is the protocol version of the connected Casambi
network, `casambi_protocol_min`/`casambi_protocol_max` the range this firmware
is tested with (`MIN_PROTOCOL_VERSION`/`MAX_PROTOCOL_VERSION` in
`src/config.h`) — FHEM computes its `casambiVersionWarning` reading from these
three numbers. They travel only in the authenticated `hello`, not in the open
`/api/info`.

The snapshot carries at most `WS_HELLO_MAX_UNITS` (50, `src/config.h`) units so
the proactively pushed message can never grow into an allocation a fragmented
heap cannot serve. Networks beyond the cap — far above realistic home
installations — get the first 50 units plus `"units_truncated": true`; clients
needing the rest fetch `GET /api/units` themselves.

#### `unit_state` — sent on every state change

```json
{
  "type": "unit_state",
  "id": 7,
  "level": 200,
  "online": true,
  "on": true,
  "vertical": 127,
  "colorTemp": 58,
  "cctMin": 2700,
  "cctMax": 4000,
  "controls": [
    { "type": "dimmer",      "name": "dimmer",      "value": 200 },
    { "type": "vertical",    "name": "vertical",    "value": 127 },
    { "type": "temperature", "name": "temperature", "value": 58, "kelvin": 2996, "min": 2700, "max": 4000 }
  ]
}
```

Triggered by any change originating from the Casambi mesh — whether sent by this controller, the Casambi app, a scene timer, a sensor, or another controller. The `controls` array carries the generic, cloud-named per-channel state (see `GET /api/units`).

#### `connection_state` — sent on BLE connect/disconnect

```json
{
  "type": "connection_state",
  "connected": true,
  "reason": 0
}
```

### colorTemp conversion

`colorTemp` is a normalized value (0–255). Convert to Kelvin:

```
kelvin = cctMin + (colorTemp / 255) * (cctMax - cctMin)
```

This is the same formula used by the REST `/api/units` endpoint.

-----

## Security

The controller is intended for use on a **trusted home LAN**, not on an exposed
or DMZ network.

### Web API authentication
Control endpoints are protected with an API token derived from the Casambi
network password — see [Authentication](#authentication) above. This keeps
unauthorized LAN devices and cross-origin browser scripts out. Because the LAN
HTTP/WebSocket traffic is **not** TLS-encrypted, it does not defend against an
attacker who can already passively sniff your network.

### Cloud API transport (TLS)
The connection to `api.casambi.com` — used during setup and on `refresh`, and
carrying the Casambi network password and session token — is HTTPS with the
**server certificate validated against the Mozilla root-CA bundle** embedded in
the arduino-esp32 core (`api_client.cpp`). This authenticates the cloud endpoint
and prevents a man-in-the-middle from capturing the network password. Validation
uses the full root bundle rather than a pinned certificate, so it keeps working
when Casambi rotates its CA.

> If a particular core build does not export the bundle symbol and the cloud
> calls fail to compile/link, you can build with `-DCASAMBI_TLS_INSECURE` to fall
> back to the previous, unauthenticated behaviour. This re-opens the cloud
> channel to MITM and is logged loudly at runtime — avoid it for production.

### Sensitive data at rest
Wi-Fi credentials (`/wifi_config.json`) and the Casambi AES keys plus network
password (`/casambi_config.json`) are stored in the LittleFS partition as
**plaintext JSON**. Anyone with physical access to the board can read the flash
and recover them.

For deployments where physical access is a concern, enable **ESP32 flash
encryption** so the entire flash (including LittleFS) is encrypted with a
per-device key fused into the chip:

- Build with the IDF option `CONFIG_FLASH_ENCRYPTION_ENABLED` (Release mode for
  production). The application code needs no changes — encryption is transparent
  to LittleFS.
- ⚠️ Flash encryption is **irreversible** once the eFuses are burned, can
  complicate re-flashing/debugging, and is therefore **not enabled by default**
  to keep the standard flashing workflow intact. Enable it deliberately for
  hardened/production units.

### BLE crypto self-check
On boot the firmware validates its AES-CMAC implementation against the RFC 4493
test vectors (`CMAC self-test: PASS/FAIL` on serial), so a broken crypto build
is caught immediately rather than silently failing BLE authentication.

-----

## Known Limitations & Stability Notes

### Stability

- **Web server / WebSocket load:** The HTTP + WebSocket stack runs on the
  maintained [ESP32Async](https://github.com/ESP32Async) libraries and is
  stress-tested (see `scripts/stress_test.py` and `scripts/verify_tcp_stack.py`).
  It stays stable under heavy concurrent HTTP/WebSocket load with full heap
  recovery, so HTTP polling at normal rates is fine and no polling-rate limit is
  required. When the device has auth enabled, pass the Casambi password to the
  scripts so they can reach the protected endpoints, e.g.
  `python3 scripts/stress_test.py --host <ip> --password <casambi-pw>`
  (both scripts also accept a pre-derived `--token`).
- **Long-term BLE stability** has not been exhaustively tested. The auto-reconnect mechanism mitigates most connection drops, but edge cases may exist.
- The **hardware watchdog** (45s) and **heap monitoring** provide safety nets against hangs and memory leaks.

### Untested or Partially Tested Features

- **Motor control** (`uslider`, `gslider`): Not extensively tested. May not work on all devices. Use scenes for reliable motor control.
- **RGB color control** (`ucolor`): Implemented but not tested with RGB-capable Casambi fixtures.
- **0x09 Mesh Topology Parser**: Experimental — reverse-engineered from two captures. The structure is interpreted as `[0x80+nodeId][metric][quality]` triplets. Node IDs map to units, groups, or scenes; metric/quality bytes are not fully understood. May misclassify nodes under different network configurations.
- **Multi-dimmer fixtures** (two or more `dimmer` controls, e.g. **Oligo Grace** Up-/Downlight): the per-channel path is implemented and verified against a mock gateway — `POST /api/units/:id/state`, the FHEM per-channel readings/commands, and the web interface's per-channel sliders plus the atomic on/off that remembers the channel split — but **not yet tested on hardware**. Single-channel control (`brightness`, `temperature`) works on these fixtures as on any other.
- **Classic networks** (non-Evolution): The code has a fallback path for networks without encryption keys, but this has not been tested.

### Protocol Notes

- **Group IDs** are assigned internally by Casambi and do not start at 0. After setup, verify with `list groups`.
- **Color temperature** values in status broadcasts are device-normalized (0–255), not absolute Kelvin. The `cctMin`/`cctMax` fields from the cloud API are needed for conversion.
- **Fixture type codes** (the `type` field) are manufacturer-specific IDs registered with Casambi. There is no public lookup table.

-----

## Architecture

### Stability Features

The controller is designed for 24/7 unattended operation:

- **BLE Auto-Reconnect:** On link loss, reconnects with exponential backoff (5s → 60s). A merely absent peer (e.g. lights cut by a wall switch) is retried at max backoff indefinitely — rebooting cannot help there; only 10 consecutive *internal* failures (auth, key exchange, GATT structure) restart the ESP32.
- **WiFi Auto-Reconnect:** Checks every 30s, reconnects silently. Web server is restarted automatically.
- **Hardware Watchdog:** 45-second WDT timeout prevents permanent hangs (sized above NimBLE's 30 s ATT procedure timeout, the longest blocking BLE call on the loop task). Fed in every `loop()` iteration.
- **Heap Monitoring:** Logged every 60s. If free heap drops below 20KB, the ESP32 restarts to prevent corruption.
- **Connection Health:** Periodic `isBLEConnected()` check detects silent disconnects where the BLE stack hasn’t noticed a link loss.

### BLE Protocol

**BLE stack:** The firmware uses **NimBLE** (NimBLE-Arduino) as its BLE host, not the default Bluedroid stack. NimBLE's substantially smaller RAM footprint leaves more contiguous heap for the TLS cloud handshake and reduces heap-pressure reboots. See [`docs/konzept-ble-nimble-migration.md`](docs/konzept-ble-nimble-migration.md) for the migration rationale.

**Authentication Flow:**

1. BLE connection to Casambi gateway
1. ECDH key exchange (SECP256R1)
1. Derive transport key (SHA256 + XOR fold)
1. Authenticate with session key (SHA256 digest)
1. Encrypted communication (AES-128-CTR + CMAC)

**Incoming Data Packets (after decryption):**

|Type|Description                                     |Status                                                |
|----|------------------------------------------------|------------------------------------------------------|
|0x06|Status Broadcast (unit states)                  |Fully decoded — state bytes mapped generically to the unit's fixture controls (dimmer/vertical/temperature/…); event-driven, one record per changed unit|
|0x07|Operation Echo / Switch Event (from other controllers)|**Diagnostic only** — decoded and logged, but no state applied. The operation-echo reading is an unverified inference (conflicts with casambi-bt's switch-event reading) and never observed on the wire; real changes always arrive as 0x06|
|0x08|Unit State Update                               |Parsed via `parseUnitStateUpdate()`                                                              |
|0x09|Mesh Topology                                   |Experimental parser — `[0x80+nodeId][metric][quality]` triplets; IDs map to units/groups/scenes  |
|0x0A|Time Sync                                       |Recognized, payload **not decoded** — the bytes are dumped with `debug ble on`/`debug parse on` so a capture can be taken; `debug parse on` additionally prints both endian readings of a leading 32-bit field against the "Unix epoch" hypothesis. Nothing is applied (the clock comes from NTP)|
|0x0C|Keepalive                                       |Recognized, payload **not decoded** — bytes dumped for capture           |

**Outgoing Operation Packets:**

- Opcode `0x01` SetLevel, `0x03` SetTemperature, `0x04` SetVertical, `0x07` SetColor, `0x0C` SetSlider
- Target encoding: `(id << 8) | type` where type is 0x01=Unit, 0x02=Group, 0x04=Scene

### Generic Capability Detection

For each distinct unit type, the controller fetches the fixture definition
(`GET https://api.casambi.com/fixture/{type}`) and reads its `controls` list —
the authoritative description of every channel: control `type`
(dimmer/vertical/temperature/white/slider/…), bit `offset`/`length`, and the
Kelvin `min`/`max` for temperature — plus `stateLength`. These controls drive:

- **Decoding:** each incoming state byte is mapped to its control by bit offset
  (`state byte n → control at offset n·8`), so a channel's meaning comes from the
  cloud, not a fixed slot (e.g. the 2nd byte is *temperature* on a Dim+CCT unit
  but *vertical* on a Dim+Vertical unit).
- **Naming:** field and FHEM reading names are derived from the control types.

Controls are persisted to LittleFS, so decoding works after a reboot without a
cloud round-trip. If a fixture cannot be fetched, the controller falls back to a
heuristic (channel count from `modes[0].state` length; CCT from
`settings.cct.minKelvins`), logged alongside for verification.

No fixture-type-specific hardcoding is needed; new device types and control
types are supported automatically.

### Directory Structure

```
esp32-casambi/
├── src/
│   ├── main.cpp              # Main application, reconnect logic, monitoring
│   ├── config.h              # Configuration constants, timeouts, debug flags
│   ├── ble/
│   │   ├── casambi_client.*  # BLE connection, encryption, state tracking
│   │   ├── casambi_scan.*    # Shared BLE discovery (used by the setup portal)
│   │   └── packet.*          # Packet building, 0x06/0x07/0x08/0x09 parsing
│   ├── cloud/
│   │   ├── api_client.*      # Casambi Cloud API client
│   │   └── network_config.h  # Data structures (units, groups, capabilities)
│   ├── crypto/
│   │   ├── encryption.*      # AES-CTR + CMAC
│   │   └── key_exchange.*    # ECDH (SECP256R1)
│   ├── storage/
│   │   └── config_store.*    # LittleFS persistence (config + debug flags)
│   └── web/
│       ├── webserver.*       # HTTP REST API + WebSocket push (ESPAsyncWebServer)
│       ├── dashboard.h       # Status & control page at GET / (self-contained HTML)
│       └── setup_portal.*    # First-boot SoftAP + captive portal provisioning
├── FHEM/
│   ├── 98_CasambiGW.pm       # FHEM gateway module (WebSocket connection, unit sync)
│   └── 98_CasambiUnit.pm     # FHEM unit + companion vertical dimmer modules
├── platformio.ini
└── README.md
```

-----

## Configuration

### Auto-Connect & Auto-Reconnect

Auto-connect saves the gateway MAC on first manual connection:

```bash
connect 0          # MAC address auto-saved
```

On subsequent boots, the controller reconnects automatically. If the BLE link drops during operation, auto-reconnect attempts to restore the connection with exponential backoff.

```bash
autoconnect on/off     # Enable/disable auto-connect on boot
reconnect on/off       # Enable/disable auto-reconnect on link loss
```

### Debug Categories

Debug output is split into independently controllable categories, all persisted to flash:

|Category  |Default|Description                                                              |
|----------|-------|-------------------------------------------------------------------------|
|`ble`     |off    |BLE transport layer: connections, crypto, raw packet hex dumps           |
|`casambi` |on     |Casambi protocol events: unit state changes, operation echo, callbacks   |
|`web`     |on     |HTTP API: incoming requests and response codes                           |
|`parse`   |off    |Protocol analysis: compact parse output with raw bytes for all packets   |
|`heap`    |off    |Heap size logged every 60 seconds                                        |

`debug off` suppresses all output without changing the saved per-category settings. `debug on` restores them.

### Syncing Changes

If you modify your Casambi network (add lights, rename devices, change scenes) in the official app:

```bash
refresh
```

This re-downloads the full configuration from the Casambi cloud while preserving local settings (auto-connect address, per-category debug flags).

The network password entered during `setup` is stored and reused automatically, so `refresh` no longer prompts for it — just press Enter at the password prompt to keep the saved one, or type a new password if it changed in the Casambi app. The password is stored in flash in plaintext, alongside the WiFi password and the BLE keys.

The actual cloud download runs early on the **next boot**, before the BLE stack and the web server are started: `refresh` only schedules the refresh and reboots. Running it on a clean boot (large contiguous heap for the TLS handshake, no concurrent BLE/network tasks) avoids the use-after-free that tearing those subsystems down at runtime would cause. The same mechanism backs the FHEM `set refreshCasambi` command and `POST /api/refreshCasambi`, so a refresh can be triggered entirely from home automation without a serial console. After the download the device reboots once more into normal operation with the fresh config.

-----

## Troubleshooting

### Setup Issues

- **WiFi won’t connect:** Ensure 2.4 GHz network (ESP32 doesn’t support 5 GHz)
- **`HTTP -1` during setup/refresh:** TLS handshake failure, almost always caused by insufficient contiguous heap for mbedTLS while the BLE stack is active — `setup` runs before BLE is initialised, and `refresh` performs its download early on the next boot for the same reason (BLE/web not yet started). If it still occurs, it usually means too little free heap; reboot and retry
- **Network not found during scan:** Ensure Casambi devices are powered on and in BLE range

### Connection Issues

- **Authentication fails:** Run `clearconfig` and redo setup with correct password
- **Auto-connect doesn’t work:** Run `scan` + `connect` once to save the MAC address
- **Silent disconnects:** Enable `debug ble on` to see BLE packet activity

#### BLE stays disconnected (setup worked, the link never comes up)

Start with `blediag`. It prints the configuration, the **phase the last connect
attempt died in**, the NimBLE return code, and a live scan — that combination
identifies the fault without guessing:

|Last phase      |What already worked            |Where to look                                                                       |
|----------------|-------------------------------|------------------------------------------------------------------------------------|
|`link`          |nothing — the peer never answered|Light powered off, out of range, a phone/gateway already holding the unit's single central slot, or the stored MAC belongs to a unit that is currently off|
|`service`/`characteristic`|link established     |Connected to something that is not a Casambi unit — the trace lists the GATT services actually found|
|`devinfo`       |GATT discovered                |Read rejected or the link dropped mid-handshake (rc and raw bytes are printed)        |
|`keyexchange`   |device info read               |No notification from the device — the trace reports notification counts and whether the subscription succeeded|
|`auth`          |ECDH completed                 |Wrong/outdated keys: run `refresh`, or `clearconfig` + `setup` with the correct password|
|`ready`         |everything                     |The link is up; look at disconnects instead                                          |

Additional pointers:

- `blediag` flags an **address-type mismatch** (peer advertises `random`, the
  reconnect path connects as `public`) — a device that is found by `scan` on
  every attempt but never connects
- `debug ble on` adds per-phase traces and re-runs the advertisement probe after
  every failed connect attempt
- If nothing is attempted at all, the log now says why once a minute
  (`auto-connect is disabled`, `no gateway MAC stored`, …)
- A Casambi unit accepts only **one** central at a time: close the Casambi phone
  app (and disable other gateways) before testing

### Spontaneous Reboots

- Ensure PSRAM flags are **not** set in `platformio.ini` for boards without PSRAM
- Check `status` for heap values; a steadily decreasing free heap indicates a memory leak
- Watch `min_free_heap` and `largest_block` in `/api/status`: the low-water
  mark catches transient dips between polls, and a shrinking largest block
  reveals fragmentation while `free_heap` still looks fine. `ws_drops` and the
  `parse_partial`/`parse_malformed` counters should stay at 0 in normal
  operation. `scripts/stress_test.py` reports all of them as deltas over a
  load run, plus an end-to-end check that queued control commands actually
  come back as `unit_state` events
- Check the persistent event log (`GET /api/log` or serial `log`) — reset
  reasons and the "last words" before a crash survive the reboot
- Build with `pio run -e debug` for full stack traces on crash

### Control Issues

- **Commands don’t work:** Check `status` — must show “Authenticated”
- **Wrong group affected:** Verify group IDs with `list groups`; Casambi assigns IDs internally (not sequential from 0)
- **Temperature command ignored:** Not all fixtures support CCT. Check `numChannels` in `/api/units`

-----

## Development

### Build Environments

|Environment|Purpose                                          |
|-----------|-------------------------------------------------|
|`devkit-v4`|Plain ESP32 (default) — the environment the tested ATOM Lite is flashed with|
|`esp32-c3` |ESP32-C3 DevKit M1 — build-only, untested on hardware|
|`debug`    |Verbose logging, debug symbols, exception decoder|
|`release`  |Size-optimized production build                  |
|`native`   |Host-side unit tests, no hardware required       |

```bash
pio run -e devkit-v4 -t upload    # Default build
pio run -e debug -t upload        # Debug build with verbose BLE logging
pio test -e native                # Host-side unit tests
```

The native tests cover the pure logic extracted into Arduino-free headers:
config validation (`config_validation.h`), the BLE packet parsers incl.
deterministic fuzzing (`packet_parse.h`), the cloud-config structural
invariants (`config_invariants.h`) and the serial argument parsing
(`serial_args.h`). CI runs them on every push alongside the firmware builds.

**Note:** After structural changes to the config format, run `clearconfig` + `setup` on the ESP32 to repopulate the configuration with new fields.

### Firmware build number

The `esp32Build` reading reported by the gateway is a monotonically increasing
integer injected at compile time by `scripts/build_number.py` (a PlatformIO
pre-build script).  It counts the total number of commits reachable from
`origin/main` via `git rev-list --count origin/main`.

To get a non-zero build number after pulling updates:

```bash
git pull
pio run -e devkit-v4 -t upload
```

The pre-build script writes the number into the generated header
`src/firmware_build.h` (gitignored), which `src/config.h` includes with a
fallback of `0`. The file is rewritten **only when the number changes**, so
the build system's dependency tracking recompiles exactly the affected
translation units on the next `pio run` after a `git pull` — no clean build
or manual source touching required. (The number was previously injected as a
`-DFIRMWARE_BUILD` compiler flag; SCons does not recompile unchanged sources
when such a flag changes, so incremental builds silently kept reporting a
stale build number.)

The count is taken from the **last fetched** state of `origin/main`, so the
script runs a `git fetch origin main` itself before counting (short timeout;
offline it silently falls back to the last fetched state and prints a hint).
Without this, a checkout that only ever pulls a feature branch never advances
`origin/main` and the build number freezes at that old count.

**Verify the injection ran:** the build output must contain a line like
`*** FIRMWARE_BUILD = 103 (git commit count on main) ***`. If it is missing
(and the device reports `build: 0`), the script is not being executed — each
firmware environment must reference it via
`extra_scripts = ${common.extra_scripts}` in `platformio.ini`; PlatformIO does
NOT apply options from the `[common]` section on its own. (This was broken
until build 103: every earlier firmware reported 0.)

### Interface versioning (ESP ↔ FHEM)

The REST API and the WebSocket protocol share one explicit **interface
version** (`major.minor`), independent of the build number: the build number
answers *"which firmware state is running?"*, the interface version answers
*"do ESP and FHEM understand each other?"*.

- **Minor** is incremented for compatible extensions (new endpoints, message
  types, or optional fields — both sides ignore what they don't know), in
  either direction: a newer or older minor on one side is never a problem.
- **Major** is incremented (minor reset to 0) for incompatible changes; one
  side then needs an update. FHEM warns via the `apiVersionWarning` reading
  but keeps operating (fail-operational).

The version is defined in `src/config.h` (`FHEM_API_VERSION_MAJOR/MINOR`) and
mirrored in `FHEM/98_CasambiGW.pm` (`API_VERSION_MAJOR/MINOR`); both files
carry the full **versioning contract** as a comment at those constants —
**anyone (human or AI assistant) changing the ESP↔FHEM interface must follow
the checklist there** (bump both sides in the same commit, document the change
here). The version travels in `GET /api/info` and the WebSocket `hello`; a
missing field means firmware at version `1.0` (predating the contract). Design
rationale: `docs/konzept-versionierung.md` (issue #29).

**Version history:**

| Version | Change |
|---|---|
| 1.0 | Initial versioned interface |
| 1.1 | `POST /api/units/:id/state` (generic atomic full-state write) and the optional `name` field in every `controls` entry (hello, `unit_state`, `GET /api/units`) — FHEM uses both to drive multi-dimmer fixtures (e.g. Oligo Grace Uplight/Downlight) per channel |
| 1.2 | Optional BLE connect diagnostics in `GET /api/status`: `last_disconnect_reason_name` alongside the numeric reason, plus `last_connect_phase`/`last_connect_rc` while the link is down |
| 1.3 | `on` (`GET /api/units`, `hello`, `unit_state`) is now the Casambi unit's own flags bit, read verbatim from the 0x06 status broadcast, instead of a firmware-side `level > 0` heuristic — field name/type/shape unchanged, only its source; the old heuristic misreported both `on` and `online` for a unit that had just gone offline (stale/stuck-on state), see `docs/captures/2026-08-04-0x06-framing/` |

-----

## FHEM Integration

Two FHEM modules provide a fully generic integration without any hardcoding of
device names, IDs, or IP addresses.  Casambi unit devices are created
automatically and identified by their stable BLE MAC address, so HomeKit
associations (FUUID) survive network reconfigurations.

### Architecture

```
ESP32                             FHEM
─────                             ────
WebSocket /ws  ──push──►  CasambiGW  (DevIo gateway device)
                               │  hello → sync / pending detection
                               │  unit_state → route to child device
                               └──► CasambiUnit_UpdateFromState(Casambi_*)
                                         │  vertical → CasambiVertical_UpdateFromParent
                                         └──► Casambi_*_vertical  (companion light)

REST /api/units/*  ◄──POST──  CasambiGW_SendCommand
                               called from CasambiUnit SetFn
                               and CasambiVertical SetFn
```

State updates flow from the ESP32 to FHEM via WebSocket push.
Commands flow from FHEM to the ESP32 via HTTP POST.

### Installation

Copy both module files to your FHEM modules directory and reload:

```bash
cp FHEM/98_CasambiGW.pm   /opt/fhem/FHEM/
cp FHEM/98_CasambiUnit.pm /opt/fhem/FHEM/
```

```
reload 98_CasambiGW
reload 98_CasambiUnit
```

`98_CasambiUnit.pm` contains two module definitions: `CasambiUnit` (one per
luminaire) and `CasambiVertical` (auto-created companion light for units with
vertical light distribution).  No separate file is needed for `CasambiVertical`.

### Gateway device

Define one gateway device per ESP32, passing its IP address or mDNS hostname
(and optionally a port):

```
define MyCasambi CasambiGW 192.168.1.100
define MyCasambi CasambiGW casambi-24f0.local
```

A fixed IP (DHCP reservation on the router) or the `casambi-XXXX.local` hostname
keeps the definition stable; the hostname suffix also distinguishes multiple
gateways.

Before opening the WebSocket the module queries `GET /api/info` and only
connects once the ESP reports `configured: true`. While the device is still in
the setup portal (`configured: false`) or unreachable, it polls periodically and
connects automatically once provisioning finishes — no manual reconnect needed.
The gateway then opens a persistent WebSocket connection and reconnects
automatically on link loss or FHEM startup.

**Readings on the gateway device:**

| Reading | Values | Description |
|---------|--------|-------------|
| `state` | `connected` / `disconnected` / `setup_required` / `unreachable` / `initializing` | Connection / setup state |
| `configured` | `true` / `false` | Whether the ESP32 has a valid configuration (from `/api/info`) |
| `network` | text | Casambi network name, from the authenticated WebSocket `hello` (not exposed by the unauthenticated `/api/info`) |
| `ble_state` | `ble_connected` / `ble_disconnected` | BLE link of the ESP32 |
| `gatewayState` | `connected` / `disconnected` | BLE gateway link state |
| `gatewayMac` | MAC | BLE address of the current gateway endpoint (empty while disconnected) |
| `gatewayName` | text | Advertised name of the current gateway, if known (often empty: the endpoint is a network-level address) |
| `syncState` | `ok` / `changes_pending` | Whether structural changes are waiting |
| `pendingSync` | text summary | e.g. `"2 new (Mito sospeso, Sento); 1 removed (Casambi_OldLight)"` |
| `lastSync` | timestamp | Time of last successful hello sync |
| `esp32Build` | integer | Build number reported by the ESP32 firmware |
| `esp32BuildWarning` | `ok` / warning text | Set when ESP32 build is below `MIN_FIRMWARE_BUILD` |
| `espApiVersion` | `major.minor` | ESP↔FHEM interface version reported by the firmware (`1.0` for firmware predating interface versioning) |
| `fhemApiVersion` | `major.minor` | Interface version implemented by the FHEM module |
| `apiVersionWarning` | `ok` / warning text | Warns (naming the side to update) when the major versions differ; minor differences are compatible. Operation continues either way. |
| `casambiProtocolVersion` | integer | Protocol version of the Casambi network (from `hello`) |
| `casambiProtocolVersionRange` | e.g. `10-11` | Casambi protocol versions the ESP32 firmware is tested with |
| `casambiVersionWarning` | `ok` / warning text | Set when the network protocol version lies outside the tested range (informational — the network version cannot be influenced) |

**Gateway attributes:**

| Attribute | Default | Description |
|-----------|---------|-------------|
| `autocreate` | `1` | Create new `CasambiUnit` devices when `applyChanges` is called |
| `deleteRemovedUnits` | `1` | Delete the FHEM device (`1`) or only set `online false` (`0`) for units removed from the Casambi network |
| `casambiPassword` | *(none)* | Legacy plaintext alternative to `set <gw> password` (see below), which takes precedence and stores the password in FHEM's obfuscated key-value store. Required once the ESP32 has auth enabled: the module derives the `X-API-Key` token from it for REST calls and the WebSocket handshake. |

### Automatic unit sync

On each (re-)connect the ESP32 sends a `hello` snapshot.  The gateway
immediately applies state and capability updates to all already-known units.
Structural changes — new or removed units — are held as **pending changes**
and require explicit confirmation:

```
set MyCasambi applyChanges     # create new / remove deleted unit devices
set MyCasambi discardChanges   # ignore the pending changes
set MyCasambi reconnect        # close and reopen the WebSocket connection
set MyCasambi refreshCasambi   # re-read the config from the Casambi cloud
set MyCasambi password <pw>    # store the Casambi network password (key store)
```

New or deleted unit devices exist only in memory until the FHEM config is
saved — run `save` after `applyChanges` (the module logs a reminder).

This prevents unintended device creation or deletion caused by a transient
network reconfiguration or gateway restart.

`set refreshCasambi` tells the ESP32 to re-read its configuration from the
Casambi cloud using the stored network password (equivalent to the serial
`refresh`). The ESP downloads the fresh config and reboots; the gateway then
reconnects automatically and any new or changed units show up as **pending
changes** — adopt them with `applyChanges`. It requires that the ESP already
has a stored Casambi password (set once via the serial `refresh`/`setup`).

### Unit devices

Each Casambi unit is represented as a `CasambiUnit` device, auto-created by
the gateway (name prefix `Casambi_`, spaces replaced with underscores).
Units are identified by their BLE MAC address (`casambiMac` attribute),
so the FHEM device name and HomeKit associations remain stable even if
Casambi reassigns unit IDs.

**Readings:**

| Reading | Description |
|---------|-------------|
| `state` | `on` / `off` |
| `brightness` | 0–100 (%) |
| `online` | `true` / `false` |
| *(per control type)* | One reading per fixture control, **named by the cloud control type** — e.g. `vertical` (0–255), `temperature` (**Kelvin**), and future types (`white`, `slider`, …) appear automatically |
| `temperature` | Color temperature in **Kelvin** (units with a `temperature` control) |
| `colorTemp` | Compatibility alias of `temperature` (Kelvin), kept for existing dashboards / the HomeKit mapping |
| `vertical` | 0–255 light distribution (units with a `vertical` control) |
| `casambiId` | Current Casambi unit ID (may change after network reconfiguration) |
| `casambiName` | Unit name as configured in the Casambi network |
| `channels` | Comma-separated control names on **multi-dimmer units** (e.g. `dimmer0,dimmer1`); absent on single-dimmer units |
| *(per dimmer channel)* | 0–100 (%) — one reading per dimmer channel of a multi-dimmer unit, named after the control (`dimmer0`, `dimmer1`, …) or its `channelNames` alias. On these units they replace the single `brightness` reading. |

The channel readings are derived generically from the `controls` array the
gateway sends (see the WebSocket `hello`/`unit_state` messages), so no per-fixture
code is needed and new control types surface as readings automatically. If the
gateway sends no `controls` (older firmware), the module falls back to the legacy
`vertical`/`colorTemp` fields.

Units with a `vertical` control automatically get a companion **`CasambiVertical`**
device named `<unitName>_vertical`.  See [CasambiVertical](#casambivertical-companion-device) below.

**Set commands** (capability-dependent, generated automatically):

`on` restores the last non-zero brightness (like the Casambi app) and falls
back to 100 % when no previous level is known.

```
set Casambi_Mito_sospeso on
set Casambi_Mito_sospeso off
set Casambi_Mito_sospeso brightness 75
set Casambi_Mito_sospeso colorTemp 3000      # Kelvin or Mired (<500) accepted
set Casambi_Mito_sospeso vertical 200
```

**Multi-dimmer units** (fixtures with two or more independent dimmer controls,
e.g. Oligo Grace Uplight/Downlight) are driven per channel instead of via the
single `brightness`: one 0–100 % command per channel, named after the cloud
control or its `channelNames` alias. All channel writes go through the
gateway's atomic `POST /api/units/:id/state`, so several channels change in
one BLE telegram and untouched controls (e.g. the colour temperature) keep
their values. `on`/`off` act on all channels together — `off` remembers the
current split, `on` restores it (100 % everywhere when no split is known,
e.g. after a FHEM restart):

```
attr Casambi_Grace channelNames dimmer0:up dimmer1:down   # optional aliases

set Casambi_Grace up 80        # Uplight to 80%, Downlight unchanged
set Casambi_Grace down 30
set Casambi_Grace off          # both channels off (split remembered)
set Casambi_Grace on           # restores 80%/30%
```

Analog values (`brightness`, `colorTemp`, `vertical`, channel dimmers) are
debounced by 300 ms to avoid flooding the BLE mesh during slider movement.

A feedback-loop guard suppresses outgoing commands while readings are being
updated from incoming push data, preventing re-triggering when Homebridge or
other systems react to reading changes.

### User attributes on unit devices

| Attribute | Description |
|-----------|-------------|
| `channelNames` | Display aliases for the dimmer channels of a multi-dimmer unit, e.g. `dimmer0:up dimmer1:down`. Renames readings, set commands and WebUI sliders; the ESP32 communication always uses the original control names. Set it before the next gateway reconnect (or trigger one) so `setList`/`webCmd` are regenerated with the aliases. |

### Managed attributes on unit devices

The gateway sets these attributes automatically on each (re-)connect;
they are only written when the value actually changes:

| Attribute | Description |
|-----------|-------------|
| `casambiMac` | BLE MAC — stable identifier, set once on creation |
| `cctMin` / `cctMax` | CCT range in Kelvin from the Casambi network |
| `setList` | Capability-dependent set command list |
| `webCmd` | Quick-access buttons in the FHEM WebUI |
| `genericDeviceType` | Always `light` |
| `homebridgeMapping` | HomeKit mapping in classic homebridge-platform-fhem format: `On=state,valueOff=off,cmdOff=off,cmdOn=on Brightness=brightness,...`; CCT units additionally include `ColorTemperature=colorTemp,...,minValue=140,maxValue=500,expr=int(1000000/$val)`. These attributes are refreshed on every (re-)connect so they stay in sync after firmware or module updates. |

### HomeKit / Homebridge integration

`colorTemp` is stored in Kelvin (human-readable in the FHEM WebUI).
The generated `homebridgeMapping` uses the classic
[homebridge-platform-fhem](https://github.com/domos4/homebridge-platform-fhem)
format with `cmdOn`/`cmdOff`/`valueOff` fields for reliable on/off control.
It includes an `expr=int(1000000/$val)` conversion so HomeKit
receives the expected Mired value.  Incoming Mired commands from Homebridge
(values < 500) are automatically converted back to Kelvin by the SetFn.

The ColorTemperature slider is mapped to the full standard HomeKit range
(**140–500 Mired**, i.e. ~2000–7143 K) regardless of the unit's actual CCT
range.  The SetFn clamps commands to the unit's `cctMin`/`cctMax` limits, so
the slider always fills the full width in the Home app.

Vertical light distribution is exposed via a separate `CasambiVertical`
companion device (not as a second service on the main unit).

Multi-dimmer units map only `On` by default — HomeKit's single Brightness
characteristic cannot represent independent channels. The per-channel percent
readings and set commands are ready to be mapped manually via the
`homebridgeMapping` attribute if desired.

### CasambiVertical companion device

For units with vertical capability, a companion device `<unitName>_vertical`
of type `CasambiVertical` is auto-created.  It exposes vertical light
distribution as a standard dimmer so that Homebridge can map it to a
Lightbulb Brightness characteristic:

| Reading | Description |
|---------|-------------|
| `state` | `on` / `off` |
| `pct` | 0–100 % — used by Homebridge (maps to raw vertical 0–255) |
| `vertical` | 0–255 raw Casambi value |

Direction convention:
- `pct 0%` → vertical 0 — light directed fully **upward**
- `pct 100%` → vertical 255 — light directed fully **downward**

Set commands: `on`, `off`, `pct 0-100`.

`genericDeviceType` is set to `light`; `homebridgeMapping` is set to
`On=state,valueOff=off,cmdOff=off,cmdOn=on Brightness=pct,homekit=Brightness,cmd=pct,minValue=0,maxValue=100`.

Both attributes are refreshed on every (re-)connect alongside the parent unit
so they stay current after module updates.

The companion device is deleted automatically alongside the parent unit
when `set <gw> applyChanges` removes a unit.

-----

## Protocol Documentation

Based on reverse-engineering the [Casambi Bluetooth library](https://github.com/lkempf/casambi-bt) and extensive packet analysis.

### Outgoing Operations

|Opcode|Name          |Payload                |
|------|--------------|-----------------------|
|`0x01`|SetLevel      |`[level]` (0-255)      |
|`0x03`|SetTemperature|`[kelvin/50]`          |
|`0x04`|SetVertical   |`[value]` (0-255)      |
|`0x07`|SetColor      |`[hue_lo, hue_hi, sat]`|
|`0x0C`|SetSlider     |`[value]` (0-255)      |

### Incoming 0x06 Status Broadcast

Variable-length records, one per unit, concatenated in a single packet. Record structure:

```
[unit_id] [flags] [cap] [const?] [prev_level?] [brightness] [aux1?] [aux2?]
```

- `flags` bit 4: previous level byte present; lower nibble: change source (0=physical, 3/7=software)
- `cap`: upper nibble = aux count (0-2); lower nibble 0x03 = has constant byte, 0x00 = no constant
- Record length: `3 + (cap==0x03?1:0) + (flags&0x10?1:0) + 1 + (cap>>4)`

Group commands produce multi-unit packets with one record per affected unit.

-----

## License

MIT License — see <LICENSE> file for details.

-----

## Acknowledgments

- **Casambi Protocol:** Reverse-engineered from [casambi-bt](https://github.com/lkempf/casambi-bt) Python library
- **Implementation:** based on [esp32-casambi](https://github.com/lian/esp32-casambi) project by lian
- **0x06 Packet Format:** Reverse-engineered through systematic testing with Occhio luminaires
- **ESP32 Community:** For excellent BLE and crypto libraries

-----

## Disclaimer

This project is not affiliated with or endorsed by Casambi or Occhio. It is an independent implementation for personal use.

**Security Note:** This controller stores Casambi network keys locally. Protect your ESP32 device physically to maintain security.
