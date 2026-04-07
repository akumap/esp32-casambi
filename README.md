# ESP32 Casambi Controller

An offline BLE controller for Casambi lighting systems, running on ESP32. Control your Casambi lights without cloud dependency after initial setup. Receives real-time state updates from the Casambi mesh network and provides current values via HTTP REST API for integration with home automation systems.

[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-00979D.svg)](https://www.arduino.cc/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

-----

## Features

- ✅ **Hybrid WiFi/BLE Operation** — One-time WiFi setup, then fully offline BLE control
- ✅ **HTTP REST API** — Control and monitor lights from any home automation system
- ✅ **WebSocket Push** — Real-time state push to connected clients; no polling required
- ✅ **Real-Time State Tracking** — Receives status broadcasts from the Casambi mesh; current brightness, color temperature, and vertical distribution always up to date, even when lights are controlled via the Casambi app or other controllers
- ✅ **Complete Protocol Support** — Full Casambi Evolution protocol implementation (ECDH, AES-CTR, CMAC)
- ✅ **Generic Capability Detection** — Unit capabilities (dimmer, CCT, vertical) automatically derived from cloud API; no hardcoding of fixture types needed
- ✅ **Scene, Unit & Group Control** — On/off, brightness, color, temperature, vertical, slider
- ✅ **Auto-Connect & Auto-Reconnect** — Reconnects automatically on BLE link loss with exponential backoff
- ✅ **WiFi Auto-Reconnect** — Recovers WiFi connection silently in background
- ✅ **Hardware Watchdog** — Prevents permanent hangs (30s timeout)
- ✅ **Heap Monitoring** — Automatic restart on critical memory levels
- ✅ **Connection Health Checks** — Detects silent BLE disconnects
- 💾 **Persistent Storage** — Configuration and capabilities stored in LittleFS (survives reboots and reflashing)

-----

## Tested Hardware & Lights

### ESP32 Boards

- [AZDelivery ESP32 Dev Kit C V4](https://www.amazon.de/dp/B07Z83MF5W) — Primary development and test board (PSRAM not required)
- [ESP32-C3 Super Mini](https://www.amazon.de/dp/B0DMNBWTFD) — Compact alternative

### Build Host

- Raspberry Pi (used for PlatformIO builds and serial monitoring)

### Tested Casambi Lights

The following Occhio luminaires have been tested with this controller:

|Luminaire                |Capabilities                             |Verified          |
|-------------------------|-----------------------------------------|------------------|
|Occhio Mito sospeso (air)|Brightness + Vertical + Color Temperature|✅ All controls    |
|Occhio Sento (air)       |Brightness + Vertical                    |✅ Brightness + Vertical|
|Occhio Luna sospeso (air)|Brightness + Color Temperature           |✅ Brightness + CCT|
|Occhio air module        |Brightness only                          |✅ Brightness      |

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
   # For ESP32 Dev Kit V4 (default)
   pio run -e devkit-v4 -t upload
   
   # For ESP32-C3 Super Mini
   pio run -e esp32-c3 -t upload
   ```
1. **Open serial monitor:**
   
   ```bash
   pio device monitor --filter esp32_exception_decoder --filter time
   ```

### Initial Setup

On first boot, the device enters setup mode. Run the setup wizard:

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

Then connect to a BLE gateway:

```
> scan
> connect 0
```

The MAC address is saved automatically. On subsequent boots, the controller reconnects automatically.

-----

## Usage

### Serial Commands

#### Connection

```bash
scan               # Scan for Casambi devices
connect 0          # Connect to first device found (auto-saves MAC)
disconnect         # Disconnect from device
status             # Show detailed system status (BLE, WiFi, heap, uptime)
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
wifi set <ssid> <password>   # Update WiFi credentials
wifi status                  # Show WiFi connection status
debug on/off/status          # Toggle all debug output (restores/saves per-category settings)
debug ble on/off             # BLE/crypto layer verbose logging
debug casambi on/off         # Casambi network events (unit states, echo, callbacks)
debug web on/off             # HTTP API request logging
debug parse on/off           # Protocol parse output with raw bytes (for analysis)
debug heap on/off            # Heap monitoring output
refresh                      # Re-download config from Casambi cloud
clearconfig                  # Factory reset
```

#### Information

```bash
list units         # Show all units with ON/OFF state
list groups        # Show all groups with IDs
list scenes        # Show all scenes with IDs
```

-----

## HTTP REST API

The ESP32 provides a REST API for integration with home automation systems (FHEM, Loxone, Home Assistant, Node-RED, etc.). The API serves both control commands and real-time state information from the Casambi mesh.

### Base URL

```
http://<esp32-ip>/api
```

The IP address is displayed in the serial console on boot.

### Status & Discovery

**GET /api/status**

```json
{
  "ble_connected": true,
  "ble_state": 3,
  "network_name": "Wohnung",
  "wifi_ssid": "MyNetwork",
  "wifi_ip": "192.168.178.111",
  "wifi_rssi": -32,
  "uptime_ms": 123456,
  "free_heap": 56000,
  "gateway_mac": "9e:d8:2b:33:15:44",
  "connection_uptime_ms": 98765,
  "packets_received": 42
}
```

**GET /api/units** — List all units with current state

```json
{
  "units": [
    {
      "id": 7,
      "name": "Mito sospeso",
      "type": 19425,
      "address": "b0e0780c693a",
      "online": true,
      "on": true,
      "level": 200,
      "numChannels": 3,
      "vertical": 127,
      "colorTemp": 58,
      "cctMin": 2700,
      "cctMax": 4000
    },
    {
      "id": 2,
      "name": "Più Spüle",
      "type": 1422,
      "address": "c755032f4f9c",
      "online": true,
      "on": true,
      "level": 255,
      "numChannels": 1
    }
  ]
}
```

The `level`, `on`, `vertical`, and `colorTemp` fields are updated in real-time from status broadcasts received over the BLE mesh. Changes made via the Casambi app, timers, sensors, or other controllers are reflected here.

`colorTemp` is a device-normalized value (0–255). To convert to Kelvin: `kelvin = cctMin + (colorTemp / 255) * (cctMax - cctMin)`.

Fields like `vertical` and `colorTemp` only appear for units that support these controls.

**GET /api/groups** — List all groups

**GET /api/scenes** — List all scenes

### Control Endpoints

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
```

#### Groups

```bash
curl -X POST http://<ip>/api/groups/4/level \
  -H "Content-Type: application/json" -d '{"level": 128}'
curl -X POST http://<ip>/api/groups/4/vertical \
  -H "Content-Type: application/json" -d '{"value": 127}'
curl -X POST http://<ip>/api/groups/4/slider \
  -H "Content-Type: application/json" -d '{"value": 200}'
```

### Response Format

**Success:** `{"success": true}`

**Error:** `{"success": false, "error": "Unit not found"}`

-----

## WebSocket Push

In addition to the REST API, the ESP32 pushes state changes in real time to all connected WebSocket clients. This eliminates polling and delivers updates with sub-100 ms latency.

### Endpoint

```
ws://<esp32-ip>/ws
```

### Messages (server → client, JSON)

#### `hello` — sent immediately on connect

Full state snapshot of all units:

```json
{
  "type": "hello",
  "ble_connected": true,
  "units": [
    {
      "id": 7,
      "name": "Mito sospeso",
      "online": true,
      "on": true,
      "level": 200,
      "vertical": 127,
      "colorTemp": 58,
      "cctMin": 2700,
      "cctMax": 4000
    },
    {
      "id": 2,
      "name": "Più Spüle",
      "online": true,
      "on": true,
      "level": 255
    }
  ]
}
```

`vertical`, `colorTemp`, `cctMin`, and `cctMax` are only present for units that support these controls.

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
  "cctMax": 4000
}
```

Triggered by any change originating from the Casambi mesh — whether sent by this controller, the Casambi app, a scene timer, a sensor, or another controller.

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

## Known Limitations & Stability Notes

### Stability

- **Spontaneous reboots** have been observed under certain conditions. The most likely cause is the ESPAsyncWebServer library crashing under regular HTTP polling. Avoid polling more frequently than once per minute. A BLE-level keepalive (`readValue()`) is a more robust alternative to HTTP polling for connection monitoring.
- **Long-term BLE stability** has not been exhaustively tested. The auto-reconnect mechanism mitigates most connection drops, but edge cases may exist.
- The **hardware watchdog** (30s) and **heap monitoring** provide safety nets against hangs and memory leaks, but do not address root causes.

### Untested or Partially Tested Features

- **Motor control** (`uslider`, `gslider`): Not extensively tested. May not work on all devices. Use scenes for reliable motor control.
- **RGB color control** (`ucolor`): Implemented but not tested with RGB-capable Casambi fixtures.
- **0x09 Mesh Topology Parser**: Experimental — reverse-engineered from two captures. The structure is interpreted as `[0x80+nodeId][metric][quality]` triplets. Node IDs map to units, groups, or scenes; metric/quality bytes are not fully understood. May misclassify nodes under different network configurations.
- **Classic networks** (non-Evolution): The code has a fallback path for networks without encryption keys, but this has not been tested.

### Protocol Notes

- **Group IDs** are assigned internally by Casambi and do not start at 0. After setup, verify with `list groups`.
- **Color temperature** values in status broadcasts are device-normalized (0–255), not absolute Kelvin. The `cctMin`/`cctMax` fields from the cloud API are needed for conversion.
- **Fixture type codes** (the `type` field) are manufacturer-specific IDs registered with Casambi. There is no public lookup table.

-----

## Architecture

### Stability Features

The controller is designed for 24/7 unattended operation:

- **BLE Auto-Reconnect:** On link loss, reconnects with exponential backoff (5s → 60s). After 10 consecutive failures, restarts the ESP32.
- **WiFi Auto-Reconnect:** Checks every 30s, reconnects silently. Web server is restarted automatically.
- **Hardware Watchdog:** 30-second WDT timeout prevents permanent hangs. Fed in every `loop()` iteration.
- **Heap Monitoring:** Logged every 60s. If free heap drops below 20KB, the ESP32 restarts to prevent corruption.
- **Connection Health:** Periodic `isBLEConnected()` check detects silent disconnects where the BLE stack hasn’t noticed a link loss.

### BLE Protocol

**Authentication Flow:**

1. BLE connection to Casambi gateway
1. ECDH key exchange (SECP256R1)
1. Derive transport key (SHA256 + XOR fold)
1. Authenticate with session key (SHA256 digest)
1. Encrypted communication (AES-128-CTR + CMAC)

**Incoming Data Packets (after decryption):**

|Type|Description                                     |Status                                                |
|----|------------------------------------------------|------------------------------------------------------|
|0x06|Status Broadcast (unit states)                  |Fully parsed — brightness, vertical, color temperature; event-driven, one record per changed unit|
|0x07|Operation Echo (commands from other controllers)|Parsed and applied to state                                                                      |
|0x08|Unit State Update                               |Parsed via `parseUnitStateUpdate()`                                                              |
|0x09|Mesh Topology                                   |Experimental parser — `[0x80+nodeId][metric][quality]` triplets; IDs map to units/groups/scenes  |
|0x0A|Time Sync                                       |Recognized                                            |
|0x0C|Keepalive                                       |Recognized                                            |

**Outgoing Operation Packets:**

- Opcode `0x01` SetLevel, `0x03` SetTemperature, `0x04` SetVertical, `0x07` SetColor, `0x0C` SetSlider
- Target encoding: `(id << 8) | type` where type is 0x01=Unit, 0x02=Group, 0x04=Scene

### Generic Capability Detection

Unit capabilities are derived from the Casambi Cloud API response during setup:

- **Number of channels:** Length of `modes[0].state` string / 2 (1=dimmer, 2=dimmer+aux, 3=dimmer+vertical+temp)
- **CCT support:** Presence of `settings.cct.minKelvins` and `settings.cct.maxKelvins`
- **Vertical support:** Inferred — 3 channels always has vertical; 2 channels without CCT has vertical

No fixture-type-specific hardcoding is needed. New Casambi device types are supported automatically.

### Directory Structure

```
esp32-casambi/
├── src/
│   ├── main.cpp              # Main application, reconnect logic, monitoring
│   ├── config.h              # Configuration constants, timeouts, debug flags
│   ├── ble/
│   │   ├── casambi_client.*  # BLE connection, encryption, state tracking
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
│       └── webserver.*       # HTTP REST API + WebSocket push (ESPAsyncWebServer)
├── FHEM/
│   └── 99_OcchioControl.pm   # FHEM integration module (WebSocket push + REST fallback)
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

-----

## Troubleshooting

### Setup Issues

- **WiFi won’t connect:** Ensure 2.4 GHz network (ESP32 doesn’t support 5 GHz)
- **`HTTP -1` during setup:** TLS handshake failure — usually transient, retry
- **Network not found during scan:** Ensure Casambi devices are powered on and in BLE range

### Connection Issues

- **Authentication fails:** Run `clearconfig` and redo setup with correct password
- **Auto-connect doesn’t work:** Run `scan` + `connect` once to save the MAC address
- **Silent disconnects:** Enable `debug ble on` to see BLE packet activity

### Spontaneous Reboots

- Ensure PSRAM flags are **not** set in `platformio.ini` for boards without PSRAM
- ESPAsyncWebServer can crash under frequent HTTP requests — avoid polling more than once per minute
- Check `status` for heap values; decreasing free heap indicates a memory leak
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
|`devkit-v4`|ESP32 Dev Kit V4 (default)                       |
|`esp32-c3` |ESP32-C3 Super Mini                              |
|`debug`    |Verbose logging, debug symbols, exception decoder|
|`release`  |Size-optimized production build                  |

```bash
pio run -e devkit-v4 -t upload    # Default build
pio run -e debug -t upload        # Debug build with verbose BLE logging
```

**Note:** After structural changes to the config format, run `clearconfig` + `setup` on the ESP32 to repopulate the configuration with new fields.

-----

## FHEM Integration

The FHEM module `FHEM/99_OcchioControl.pm` serves as a reference integration showing how to connect a home automation system to the ESP32. It uses the WebSocket push interface for real-time state updates and the REST API for sending commands.

### Architecture

```
ESP32                          FHEM
─────                          ────
WebSocket /ws  ──push──►  OcchioControl (DevIo device)
                               │ unit_state, hello
                               └──► readingsBulkUpdate(Occhio_*)

REST /api/units/*  ◄──POST──  OcchioControl_SendCommand
                               called from notify rules

REST /api/units    ◄──GET───  OcchioControl_UpdateStatus
                               (optional fallback, via at-timer)
```

State updates flow from the ESP32 to FHEM via WebSocket push. Commands flow from FHEM to the ESP32 via HTTP POST. A REST polling fallback is available for resilience but is not needed for normal operation.

### Installation

Copy `FHEM/99_OcchioControl.pm` to your FHEM `FHEM/` directory and reload:

```
reload 99_OcchioControl
```

### WebSocket device

Define one device per ESP32. This opens a persistent TCP connection and performs the WebSocket handshake automatically:

```
define OcchioWS OcchioControl
```

On successful connect, the ESP32 sends a `hello` message with the current state of all units. From that point on, every state change in the Casambi mesh is pushed as a `unit_state` message and applied to the corresponding FHEM device readings immediately.

The module reconnects automatically on link loss, including on FHEM startup.

**Readings on `OcchioWS`:**

| Reading | Values | Description |
|---------|--------|-------------|
| `state` | `connected` / `disconnected` | WebSocket connection state |
| `ble_state` | `ble_connected` / `ble_disconnected` | BLE connection of the ESP32 |

### Unit devices

Each Casambi unit is represented as a separate FHEM device. Set the `casambiUnitId` attribute to link it to the correct Casambi unit ID (visible in `/api/units`):

```
attr Occhio_Mito_Sospeso casambiUnitId 7
attr Occhio_Mito_Sospeso cctMin 2700
attr Occhio_Mito_Sospeso cctMax 4000
```

**Readings updated by push:**

| Reading | Description |
|---------|-------------|
| `state` | `on` / `off` |
| `brightness` | 0–100 (%) |
| `online` | `true` / `false` |
| `colorTemp` | Kelvin (only for CCT-capable units) |
| `vertical` | 0–255 raw value (only for units with vertical control) |

### Command forwarding

Commands are forwarded to the ESP32 via HTTP POST. The module is typically called from FHEM `notify` rules:

```
define n_occhio_mito notify Occhio_Mito_Sospeso {OcchioControl_Notify("Occhio_Mito_Sospeso", $EVENT)}
```

Supported events: `on`, `off`, `brightness <0-100>`, `colorTemp <kelvin>`, `vertical <0-255>`.

Analog values (`brightness`, `colorTemp`, `vertical`) are debounced with a 300 ms delay to avoid flooding the BLE mesh during slider movement.

The module includes a feedback-loop guard (`$_updatingStatus`) that suppresses outgoing commands while readings are being updated from incoming push or poll data. This prevents re-triggering when other systems (e.g. Homebridge) react to reading changes.

ColorTemp accepts both Kelvin (≥ 500) and Mired (< 500, converted automatically). Values are clamped to the device's `cctMin`/`cctMax` range.

### Polling fallback

`OcchioControl_UpdateStatus()` performs a full REST poll of `/api/units` and updates all unit readings. It can be called via an `at`-timer as a fallback, e.g. every 5 minutes:

```
define at_occhio_poll at +*00:05:00 {OcchioControl_UpdateStatus()}
```

With WebSocket push active, this interval can be set very low-frequency (5–10 minutes) or omitted entirely.

### Unit ID map

The module contains a hardcoded map of Casambi unit IDs to FHEM device names. Edit `%_deviceMap` in `99_OcchioControl.pm` to match your installation:

```perl
my %_deviceMap = (
    7  => "Occhio_Mito_Sospeso",
    8  => "Occhio_Mel_Luna",
    # ...
);
```

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
