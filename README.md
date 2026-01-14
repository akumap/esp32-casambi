# ESP32 Casambi Controller

An offline BLE controller for Casambi lighting systems, running on ESP32. Control your Casambi lights without cloud dependency after initial setup.

[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-00979D.svg)](https://www.arduino.cc/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

## Features

- ✅ **Hybrid WiFi/BLE Operation** - One-time WiFi setup, then fully offline BLE control
- ✅ **HTTP REST API** - Control lights from home automation systems (Loxone, Home Assistant, etc.)
- ✅ **Complete Protocol Support** - Full Casambi Evolution protocol implementation
- ✅ **Encrypted Communication** - ECDH key exchange, AES-CTR encryption, CMAC authentication
- ✅ **Scene Control** - Activate and control scenes with full state restoration
- ✅ **Unit Control** - Individual light control (on/off, brightness, color, temperature)
- ✅ **Group Control** - Control multiple lights simultaneously
- 🚀 **Auto-Connect** - Automatically connects to your gateway on boot
- 🔧 **Debug Toggle** - Clean operation by default, verbose debugging when needed
- 💾 **Persistent Storage** - Configuration stored in LittleFS (survives reboots)
- 🔍 **Auto-Discovery** - Setup wizard scans and finds your Casambi network

---

## Quick Start

### Hardware Requirements

**Supported ESP32 Boards:**
- [AZDelivery ESP32 Dev Kit C V4](https://www.amazon.de/dp/B07Z83MF5W) - Primary development board
- [ESP32-C3 Super Mini](https://www.amazon.de/dp/B0DMNBWTFD) - Compact alternative
- Any ESP32 board with BLE support

**Additional Requirements:**
- USB cable for programming and serial communication
- Casambi network with at least one BLE-enabled device

### Software Requirements
- [PlatformIO](https://platformio.org/) (recommended) or Arduino IDE

### Installation

1. **Clone the repository:**
   ```bash
   git clone https://github.com/lian/esp32-casambi.git
   cd esp32-casambi
   ```

2. **Build and upload:**
   ```bash
   # For ESP32 Dev Kit V4 (default)
   pio run -t upload

   # For ESP32-C3 Super Mini
   pio run -e esp32-c3 -t upload
   ```

3. **Open serial monitor:**
   ```bash
   pio device monitor --echo
   ```

### Initial Setup

On first boot, the device enters setup mode. Run the setup wizard:

```
> setup
```

The wizard will:
1. Scan for nearby Casambi networks
2. Auto-discover your network UUID from the device MAC
3. Connect to WiFi (you provide credentials)
4. Download network configuration from Casambi Cloud
5. Save everything to flash
6. Reboot into operation mode

**After setup:** WiFi stays connected to provide the HTTP REST API for home automation systems, while BLE handles all Casambi protocol communication.

---

## Usage

### Connection

```bash
scan               # Scan for Casambi devices
connect 0          # Connect to first device found (auto-saves for next boot)
disconnect         # Disconnect from device
```

### Scene Control

```bash
son 1              # Turn scene 1 ON
soff 1             # Turn scene 1 OFF
slevel 1 255       # Set scene 1 to full intensity
slevel 1 128       # Set scene 1 to 50% intensity
```

### Unit Control

```bash
uon 2              # Turn unit 2 ON
uoff 2             # Turn unit 2 OFF
ulevel 2 200       # Set unit 2 brightness to 200/255
ucolor 2 255 0 0   # Set unit 2 to red
utemp 2 3000       # Set unit 2 to warm white (3000K)
uvertical 2 200    # Set light balance (0=top only, 127=both, 255=bottom only)
uslider 2 200      # Set motor position (0=up, 255=down)
```

### Group Control

```bash
glevel 1 128       # Set group 1 brightness to 50%
gvertical 1 200    # Set light balance (0=top only, 127=both, 255=bottom only)
gslider 1 200      # Set motor position (0=up, 255=down)
```

### Information

```bash
status             # Show connection status
refresh            # Sync config from Casambi cloud
list units         # Show all units
list groups        # Show all groups
list scenes        # Show all scenes
debug on/off       # Toggle verbose debug output
```

### Full Command Reference

| Command | Description |
|---------|-------------|
| `help` | Show all commands |
| `status` | Show connection status |
| `refresh` | Refresh config from Casambi cloud |
| `clearconfig` | Factory reset |
| **BLE Commands** | |
| `scan` | Scan for BLE devices |
| `connect <n>` | Connect to device number n |
| `disconnect` | Disconnect from device |
| `autoconnect on/off/status` | Auto-connect control |
| `autoconnect set <mac>` | Set MAC for auto-connect |
| `wifi status` | Show WiFi connection status |
| `wifi set <ssid> <password>` | Update WiFi credentials |
| `debug on/off/status` | Debug output control |
| **Scene Commands** | |
| `son <id>` | Activate scene |
| `soff <id>` | Deactivate scene |
| `slevel <id> <0-255>` | Set scene intensity |
| **Unit Commands** | |
| `uon <id>` | Turn unit on |
| `uoff <id>` | Turn unit off |
| `ulevel <id> <0-255>` | Set unit brightness |
| `ucolor <id> <r> <g> <b>` | Set unit RGB color (0-255 each) |
| `utemp <id> <kelvin>` | Set unit color temperature (e.g., 2700-6500K) |
| `uvertical <id> <0-255>` | Set unit motor position |
| `uslider <id> <0-255>` | Set unit top/bottom light balance (0=bottom, 127=middle, 255=top) |
| **Group Commands** | |
| `glevel <id> <0-255>` | Set group brightness |
| `gvertical <id> <0-255>` | Set group motor position |
| `gslider <id> <0-255>` | Set group top/bottom light balance |
| **Info Commands** | |
| `list units` | List all units |
| `list groups` | List all groups |
| `list scenes` | List all scenes |

**Motor and Light Distribution Controls:**
- **Vertical** (light balance): Controls light distribution between top and bottom emitters (0=top only, 127=both equally, 255=bottom only)
- **Slider** (motor position): Controls physical motor position (0=up, 255=down)

**Note:** The actual behavior of vertical/slider controls depends on your specific light fixtures. Different manufacturers map these Casambi protocol controls differently. The descriptions above reflect typical motorized lights with dual emitters. Test with your fixtures to verify behavior.

---

## Configuration

### Auto-Connect

Auto-connect is enabled by default. Simply connect once and the MAC address is saved:

```bash
connect 0          # Connect to device (MAC address auto-saved)
```

On next boot, the controller automatically reconnects to your Casambi gateway.

**Disable auto-connect:**
```bash
autoconnect off    # Disable auto-connect
autoconnect on     # Re-enable auto-connect
```

### Debug Mode

Control output verbosity:

```bash
debug off     # Clean operation (default) - only shows important messages
debug on      # Verbose mode - shows packet details, encryption, nonces
```

### WiFi Configuration

Update WiFi credentials without losing your Casambi configuration:

```bash
wifi status                        # Show WiFi connection status
wifi set MyNetwork MyPassword123   # Update WiFi credentials
```

The `wifi set` command will:
- Save new credentials to flash
- Disconnect from current WiFi
- Connect to new network
- Restart the web server
- Display the new IP address

This is useful when:
- Your WiFi password changes
- You move the ESP32 to a different network
- You need to troubleshoot WiFi issues

### Syncing Changes from Official App

If you add new lights, rename devices, or modify scenes in the official Casambi app, use the `refresh` command to sync:

```bash
refresh
```

The command will:
- Download fresh configuration from Casambi cloud
- Update all units, groups, and scenes
- Preserve your local settings (auto-connect, debug mode)
- Require your network password for authentication

This is useful when:
- Adding new lights to your Casambi network
- Renaming devices, groups, or scenes
- Modifying scene configurations
- Changes made by other users need to be synced

**Note:** You need WiFi connected. The command will auto-connect if WiFi credentials are saved.

---

## HTTP REST API

The ESP32 provides a REST API for integration with home automation systems like Loxone, Home Assistant, Node-RED, and others.

### Base URL

After setup, the API is available at:
```
http://<esp32-ip>/api
```

The ESP32's IP address is displayed in the serial console on boot.

### Endpoints

#### Status & Discovery

**GET /api/status**
```bash
curl http://192.168.1.100/api/status
```
```json
{
  "ble_connected": true,
  "network_name": "Home Lights",
  "wifi_ssid": "MyNetwork",
  "wifi_ip": "192.168.1.100",
  "uptime_ms": 123456,
  "gateway_mac": "connected"
}
```

**GET /api/units** - List all light units
```bash
curl http://192.168.1.100/api/units
```
```json
{
  "units": [
    {"id": 1, "name": "Kitchen Ceiling", "address": "...", "online": true},
    {"id": 2, "name": "Living Room", "address": "...", "online": true}
  ]
}
```

**GET /api/groups** - List all groups
```bash
curl http://192.168.1.100/api/groups
```

**GET /api/scenes** - List all scenes
```bash
curl http://192.168.1.100/api/scenes
```

#### Scene Control

**POST /api/scenes/:id/on** - Activate scene
```bash
curl -X POST http://192.168.1.100/api/scenes/1/on
```

**POST /api/scenes/:id/off** - Deactivate scene
```bash
curl -X POST http://192.168.1.100/api/scenes/1/off
```

**POST /api/scenes/:id/level** - Set scene intensity (0-255)
```bash
curl -X POST http://192.168.1.100/api/scenes/1/level \
  -H "Content-Type: application/json" \
  -d '{"level": 128}'
```

#### Unit Control

**POST /api/units/:id/on** - Turn unit on
```bash
curl -X POST http://192.168.1.100/api/units/2/on
```

**POST /api/units/:id/off** - Turn unit off
```bash
curl -X POST http://192.168.1.100/api/units/2/off
```

**POST /api/units/:id/level** - Set brightness (0-255)
```bash
curl -X POST http://192.168.1.100/api/units/2/level \
  -H "Content-Type: application/json" \
  -d '{"level": 200}'
```

**POST /api/units/:id/color** - Set RGB color (0-255 each)
```bash
curl -X POST http://192.168.1.100/api/units/2/color \
  -H "Content-Type: application/json" \
  -d '{"r": 255, "g": 100, "b": 0}'
```

**POST /api/units/:id/temperature** - Set color temperature (Kelvin)
```bash
curl -X POST http://192.168.1.100/api/units/2/temperature \
  -H "Content-Type: application/json" \
  -d '{"kelvin": 2700}'
```

**POST /api/units/:id/vertical** - Set light balance (0-255)
```bash
curl -X POST http://192.168.1.100/api/units/2/vertical \
  -H "Content-Type: application/json" \
  -d '{"value": 127}'
```

**POST /api/units/:id/slider** - Set motor position (0-255)
```bash
curl -X POST http://192.168.1.100/api/units/2/slider \
  -H "Content-Type: application/json" \
  -d '{"value": 200}'
```

#### Group Control

**POST /api/groups/:id/level** - Set group brightness (0-255)
```bash
curl -X POST http://192.168.1.100/api/groups/1/level \
  -H "Content-Type: application/json" \
  -d '{"level": 128}'
```

**POST /api/groups/:id/vertical** - Set group light balance (0-255)
```bash
curl -X POST http://192.168.1.100/api/groups/1/vertical \
  -H "Content-Type: application/json" \
  -d '{"value": 127}'
```

**POST /api/groups/:id/slider** - Set group motor position (0-255)
```bash
curl -X POST http://192.168.1.100/api/groups/1/slider \
  -H "Content-Type: application/json" \
  -d '{"value": 200}'
```

### Response Format

**Success:**
```json
{"success": true}
```

**Error:**
```json
{
  "success": false,
  "error": "Unit not found"
}
```

### Integration Examples

#### Loxone Miniserver

Use Virtual HTTP Output commands:

```
Scene ON: http://192.168.1.100/api/scenes/1/on (POST)
Unit Dimmer: http://192.168.1.100/api/units/2/level (POST, body: {"level":<v>})
```

#### Home Assistant

```yaml
rest_command:
  casambi_scene_on:
    url: "http://192.168.1.100/api/scenes/{{ scene_id }}/on"
    method: POST

  casambi_unit_level:
    url: "http://192.168.1.100/api/units/{{ unit_id }}/level"
    method: POST
    content_type: "application/json"
    payload: '{"level": {{ level }}}'
```

#### Node-RED

Use HTTP Request nodes with method POST and appropriate payloads.

---

## Architecture

### Protocol Implementation

**Authentication Flow:**
1. BLE connection to Casambi gateway
2. ECDH key exchange (SECP256R1)
3. Derive transport key (SHA256 + XOR fold)
4. Authenticate with session key
5. Encrypted communication (AES-CTR + CMAC)

**Encryption Details:**
- Key Exchange: ECDH with SECP256R1 elliptic curve
- Encryption: AES-128-CTR mode
- Authentication: Manual CMAC-AES (RFC 4493)
- Packet Format: Big-endian operation packets

**Cloud API:**
- Initial setup downloads network configuration
- Stored locally in LittleFS flash filesystem
- Includes keys, units, groups, scenes
- No cloud dependency after setup

### Directory Structure
```
esp32-casambi/
├── src/
│   ├── main.cpp              # Main application
│   ├── config.h              # Configuration constants
│   ├── ble/                  # BLE protocol implementation
│   ├── cloud/                # Casambi Cloud API client
│   ├── crypto/               # AES-CTR + CMAC + ECDH
│   └── storage/              # LittleFS configuration
├── platformio.ini
└── README.md
```

---

## Troubleshooting

### Setup Issues

**WiFi won't connect:**
- Ensure 2.4 GHz network (ESP32 doesn't support 5 GHz)
- Check SSID and password
- Try moving closer to access point

**Network not found during scan:**
- Ensure Casambi device is powered on and in range
- Only gateway devices advertise via BLE

### Connection Issues

**Authentication fails:**
- Run `clearconfig` and redo setup
- Ensure correct Casambi network password

**Auto-connect doesn't work:**
- Check `autoconnect status`
- Try manual `connect` first to save MAC address

### Control Issues

**Commands don't work:**
- Check connection: `status`
- Verify device IDs: `list units`, `list scenes`
- Try reconnecting

---

## Development

### Build Environments

The project includes optimized PlatformIO configurations:

**Production Environments:**
- `devkit-v4` - ESP32 Dev Kit V4 (default)
- `esp32-c3` - ESP32-C3 Super Mini

**Development Environments:**
- `debug` - Verbose logging with full debug symbols
- `release` - Size-optimized production build

**Build Features:**
- Automatic exception decoding in serial monitor
- Fast uploads (921600 baud vs default 115200)
- Pinned library versions for reproducible builds
- AsyncTCP optimizations for better WiFi/BLE coexistence
- PSRAM support for Dev Kit V4

**Usage:**
```bash
pio run              # Build default environment
pio run -e esp32-c3  # Build for ESP32-C3
pio run -e debug     # Build with debug symbols
pio run -e release   # Build optimized release
```

---

## Protocol Documentation

Based on reverse-engineering the [Casambi Bluetooth library](https://github.com/lkempf/casambi-bt).

**Key Opcodes:**
- `0x01` - SetLevel (brightness, scenes)
- `0x03` - SetTemperature (color temperature)
- `0x04` - SetVertical (position)
- `0x07` - SetColor (RGB/HSV)
- `0x0C` - SetSlider
- `0x30` - SetState

**Target Encoding:**
- Unit: `(deviceId << 8) | 0x01`
- Group: `(groupId << 8) | 0x02`
- Scene: `(sceneId << 8) | 0x04`

---

## License

MIT License - see [LICENSE](LICENSE) file for details.

---

## Acknowledgments

- **Casambi Protocol:** Reverse-engineered from [casambi-bt](https://github.com/lkempf/casambi-bt) Python library
- **ESP32 Community:** For excellent BLE and crypto libraries
- **Screaming GPUs in a datacenter somewhere**

---

## Disclaimer

This project is not affiliated with or endorsed by Casambi. It is an independent implementation for educational and personal use.

**Security Note:** This controller stores Casambi network keys locally. Protect your ESP32 device physically to maintain security.

---

## Project Status

**Status:** Complete and hardware-tested

**Tested Hardware:**
- [AZDelivery ESP32 Dev Kit C V4](https://www.amazon.de/dp/B07Z83MF5W)
- [ESP32-C3 Super Mini](https://www.amazon.de/dp/B0DMNBWTFD)
- Casambi network (protocol v43)

---

## Support

- **Issues:** [GitHub Issues](https://github.com/lian/esp32-casambi/issues)
