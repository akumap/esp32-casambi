# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (C++/Arduino, PlatformIO) for an ESP32 that speaks the Casambi Evolution BLE
protocol directly to a Casambi lighting mesh, serves a REST API + WebSocket + web
dashboard on the local network, and ships with a companion FHEM integration
(`FHEM/`). No cloud dependency after initial setup — the Casambi cloud is only
used during provisioning/`refresh` to fetch network config and fixture
capabilities. `README.md` is the full user/protocol/API reference (>1500
lines) — read the relevant section there before touching REST/WebSocket
behavior, the BLE protocol, or FHEM interop; this file only covers what a
coding agent needs to build, test, and navigate the source.

## Build, flash, test

Uses PlatformIO. Common commands:

```bash
pio run -e devkit-v4               # build (default target: plain ESP32 / ATOM Lite)
pio run -e devkit-v4 -t upload     # build + flash
pio device monitor --filter esp32_exception_decoder --filter time   # serial monitor
pio run -e esp32-c3                # build-only, untested-on-hardware alt board
pio run -e debug -t upload         # verbose logging + debug symbols + -O0, for crash traces
pio run -e release                 # size-optimized build

pio test -e native                 # host-side unit tests (no hardware, fast)
pio test -e native -f test_packet_parse   # run a single test suite (-f = filter by dir name)
```

CI (`.github/workflows/ci.yml`) runs on push to `main`/`claude/**` and on PRs:
firmware builds for both `devkit-v4` and `esp32-c3`, `pio test -e native`, a
Python syntax check on `scripts/*.py`, and Perl syntax + unit tests for the
FHEM modules (`perl FHEM/t/CasambiGW_helpers.t`). Match that locally before
pushing if you touched `scripts/` or `FHEM/`.

There is no hardware-in-the-loop CI. `scripts/stress_test.py` and
`scripts/verify_tcp_stack.py` are manual load/stability tools you run against
a real device (`python3 scripts/stress_test.py --host <ip> --password
<casambi-pw>`) — use them after changes to the web server, WebSocket, or heap
handling.

### Native tests (`test/`, env `native`)

Only pure, Arduino-free logic is host-tested — each test dir compiles a
single header with no BLE/WiFi/LittleFS dependency:

- `test_config_validation` → `src/storage/config_validation.h`
- `test_packet_parse` → `src/ble/packet_parse.h` (includes deterministic fuzzing)
- `test_cloud_invariants` → `src/cloud/config_invariants.h`
- `test_serial_args` → `src/serial_args.h`
- `test_state_codec` → `src/cloud/state_codec.h`

When adding logic that doesn't need Arduino/BLE APIs, prefer putting it in a
plain header like these so it's host-testable; anything touching NimBLE,
LittleFS, WiFi, or ESPAsyncWebServer can only be exercised on real hardware.

### Firmware build number

`FIRMWARE_BUILD` (reported as `esp32Build`) is injected by
`scripts/build_number.py`, a PlatformIO `pre:` script that counts commits on
`origin/main` and writes `src/firmware_build.h` (gitignored). It fetches
`origin/main` itself before counting. Every firmware environment must set
`extra_scripts = ${common.extra_scripts}` — PlatformIO does **not** apply
`[common]` automatically per-environment. Verify it ran by checking the build
log for `*** FIRMWARE_BUILD = N (git commit count on main) ***`.

## Local hardware setup (this Raspberry Pi)

This machine is the dev host with the actual hardware attached — not just a
build box.

- Repo: `/home/pi/esp32-casambi`
- PlatformIO: `/home/pi/platformio-venv/bin/pio` (6.1.19) — **not on PATH**,
  always invoke by full path.
- Board: M5Stack ATOM Lite (ESP32-PICO-D4, dual-core, 4 MB flash, no PSRAM).
- Device on the network: `192.168.178.111` / `casambi-4a80.local`.

### Serial ports — critical, do not mix up

```
/dev/ttyUSB0  = M5Stack ATOM Lite     <- the target board
/dev/ttyACM0  = busware CUL868 (FHEM RF module)   <- NEVER write to this one
```

Don't rely on PlatformIO's port auto-detect — always pass the port explicitly:
`--upload-port /dev/ttyUSB0`.

### Compiling on this host

```bash
cd /home/pi/esp32-casambi
/home/pi/platformio-venv/bin/pio run -e devkit-v4
/home/pi/platformio-venv/bin/pio run -e esp32-c3     # CI parity check only — no C3 hardware here
/home/pi/platformio-venv/bin/pio test -e native      # 86 tests
```

Roughly 2-3 min per firmware target, ~20s for the native tests, on this Pi.

The build number (`scripts/build_number.py`) counts commits on `origin/main`,
so it stays constant while working on a feature branch — it is **not**
evidence that a fresh flash actually happened. To confirm a flash took, use
the binary size/hash from the `esptool` upload output and the boot counter
instead.

### Flashing

Check the port is free first (kill any monitor holding it):

```bash
fuser -v /dev/ttyUSB0
```

Then:

```bash
/home/pi/platformio-venv/bin/pio run -e devkit-v4 -t upload \
    --upload-port /dev/ttyUSB0
```

A normal `-t upload` writes only bootloader/partition-table/app and preserves
LittleFS (Casambi config + Wi-Fi credentials). Only `-t uploadfs` touches the
data partition — don't run that unless you specifically intend to wipe stored
config. There is **no OTA rollback** (the `huge_app` partition layout has no
second OTA slot by design) — reverting means reflashing over serial from
`main`.

### Waiting for long-running commands

Builds take 2-3 min per target; a serial flash (1.6 MB at 1.5M baud,
`upload_speed`/`monitor_speed` in platformio.ini) takes well under a minute,
but is still async — don't poll for it. Start the command with
`run_in_background` and wait for the harness completion notification — that
is the only reliable signal here.

Do **not** wait with `while pgrep -f "<command>"; do sleep …; done`. It fails
in both directions:

- **Self-match:** the polling shell's own command line contains the pattern,
  so `pgrep` always finds it and the loop never exits.
- **`pgrep` uses ERE:** writing `\|` for alternation (BRE habit) matches a
  literal `|`, so the pattern silently matches nothing and the loop exits
  *immediately* — looking exactly like "the command finished."

Also note that piping a background command through `tail`/`head` buffers its
output until it exits, so an empty output file does not mean "nothing has
happened yet."

### Serial monitor (non-interactive shell)

`pio device monitor` needs an interactive TTY and fails here with
`termios.error: Inappropriate ioctl for device`. Use `pyserial` directly
instead.

Capture the boot log (toggling DTR/RTS triggers a clean reset):

```bash
/home/pi/platformio-venv/bin/python3 - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyUSB0', 1500000, timeout=1)   # matches monitor_speed/Serial.begin()
s.dtr = False; s.rts = True; time.sleep(0.1)   # EN low  = reset
s.rts = False                                  # EN high = run
t0 = time.time()
while time.time() - t0 < 32:
    l = s.readline()
    if l: print(l.decode('utf-8','replace').rstrip())
s.close()
PY
```

Send commands and read the response:

```bash
/home/pi/platformio-venv/bin/python3 - <<'PY'
import serial, time
CMDS = ["debug status","ntp status","autoconnect status","wifi status",
        "reconnect status","list units","log 2",
        "uon 199","glevel 199 5","son 199","ulevel 5 999",
        "connect foo","quatsch"]
s = serial.Serial('/dev/ttyUSB0', 1500000, timeout=1)   # matches monitor_speed/Serial.begin()
time.sleep(0.5); s.reset_input_buffer()
t0 = time.time()                    # wait for boot (opening the port resets it)
while time.time()-t0 < 45:
    if 'Ready. Type' in s.readline().decode('utf-8','replace'): break
time.sleep(1); s.reset_input_buffer()
for c in CMDS:
    s.write((c+"\n").encode()); s.flush(); time.sleep(1.5)
    out = s.read(s.in_waiting or 1).decode('utf-8','replace')
    print(f"----- $ {c}")
    for line in out.splitlines():
        if line.strip(): print("   ", line.rstrip())
s.close()
PY
```

### Testing safely without switching real lights

This network has units `1,2,3,5,7,8,9,10,11`, 1 group, 4 scenes. Use
IDs that don't exist — entity lookup is validated before anything is sent
over BLE:

```
uon 199        -> "Unit 199 not found"
glevel 199 5   -> "Group 199 not found"
son 199        -> "Scene 199 not found"
ulevel 5 999   -> "Invalid value (must be 0-255)"
connect foo    -> "Usage: connect <index>"
quatsch        -> "Unknown command. Type 'help'"
```

**Do not run these while testing** — they act on the real device/network:
`restart`, `clearconfig`, `refresh`, `setup`, `scan`, `connect <n>`,
`disconnect`, and any control command with a *valid* ID (it switches a real
light).

FHEM runs on this same Pi (`MeinCasambi`, `/opt/fhem`) and holds a live
WebSocket to the gateway, so **the lights change on their own during a
capture**: outgoing `Sending operation` lines and incoming 0x06 broadcasts
appear without you causing them. Don't read them as an effect of your own
commands — cross-check `/opt/fhem/log/fhem-*.log` before attributing
anything. Flashing also shows up there as a disconnect/reappear pair.

### REST API smoke-check

Unauthenticated (reachability only):

```bash
curl -s http://192.168.178.111/api/info
curl -s http://casambi-4a80.local/api/info
# -> {"configured":true,"build":N,"api_version_major":1,"api_version_minor":2}
```

Every other endpoint needs `X-API-Key: <token>`, where
`token = hex(SHA-256("casambi-api:" + <Casambi network password>))` (constants:
`API_KEY_HEADER` / `API_TOKEN_PREFIX` in `src/config.h`).

### Git / GitHub on this host

- Remote: `https://github.com/akumap/esp32-casambi` (not `esp-casambi`).
- `gh` CLI is not installed here, and the GitHub MCP connector is not
  authorized — PRs must be created via the GitHub web UI, not from this host.
- CI (`.github/workflows/ci.yml`) only triggers on push for `main` and
  `claude/**`; other branch names only build once a PR is opened.

## Architecture

### Runtime shape

`src/main.cpp` is a thin Arduino `setup()`/`loop()` shell — most logic has
been extracted into focused modules (see the recent `refactor/*` commit
history: `net/`, `ble/reconnect_supervisor`, `diagnostics`, `serial_console`,
`cloud_refresh` were all pulled out of `main.cpp` this way). `setup()` branches
on whether valid config exists in LittleFS:

- **No/invalid config** → `SetupPortal` opens an open SoftAP + captive-portal
  web UI (`src/web/setup_portal.*`) for Wi-Fi + Casambi provisioning. The
  serial wizard (`setup` command) is a fallback for the same flow.
- **Valid config** → BLE (`CasambiClient`) and, once Wi-Fi connects, the REST/
  WebSocket server (`CasambiWebServer`) are started; auto-connect dials the
  saved gateway MAC.

`loop()` feeds the hardware watchdog every iteration (45s timeout —
NimBLE's ATT procedure timeout, ~30s, is the longest single blocking BLE call
on this task, so WDT must exceed it), then drives: serial command dispatch,
the setup portal (if active), BLE health-check + auto-reconnect
(`ble/reconnect_supervisor`), a 30s BLE keepalive, Wi-Fi reconnect
(`net/wifi_manager`), first-NTP-sync detection, heap monitoring
(`diagnostics`), event-log flush from RTC RAM to LittleFS, and web-server
housekeeping (NTP-change/refresh/clear-log/reboot requests queued by
`CasambiWebServer` are drained here, never handled inline in the async_tcp
callback).

**Concurrency rule, load-bearing:** `NetworkConfig`'s mutable `String` fields
are written only from the loop task and read (under `g_configMutex`, via
`configLock()`/`configUnlock()` in `main.cpp`) from the async_tcp task. Any
new code that lets the web server mutate shared config state must go through
this lock, or be deferred to the loop task via a request flag consumed there
(follow the existing `consumeNtpRequest`/`consumeRefreshRequest`/
`consumeClearLogRequest`/`consumeRebootRequest` pattern in `webserver.*`).

### Directory map

```
src/
├── main.cpp                  # setup()/loop(), global state, task wiring
├── app_state.h                # Shared runtime structs (e.g. ScannedDevice)
├── config.h                   # Protocol constants, timeouts, debug flags, FHEM_API_VERSION_*
├── ble/
│   ├── casambi_client.*       # BLE connection lifecycle, auth, state tracking
│   ├── casambi_scan.*         # Shared BLE discovery (setup portal + serial `scan`)
│   ├── packet.* / packet_parse.h   # Packet building + 0x06/0x07/0x08/0x09 parsing (host-testable)
│   └── reconnect_supervisor.*  # Auto-reconnect state machine, backoff
├── cloud/
│   ├── api_client.*           # Casambi Cloud API (HTTPS, TLS-validated)
│   ├── network_config.h       # NetworkConfig and friends (units/groups/scenes/capabilities)
│   ├── config_invariants.h    # Structural validation of downloaded cloud config (host-testable)
│   └── state_codec.h          # State-vector <-> per-control encode/decode (host-testable)
├── cloud_refresh.*            # Scheduled cloud-config refresh (runs at next boot, not inline)
├── crypto/
│   ├── encryption.*           # AES-128-CTR + CMAC (RFC 4493 self-tested at boot)
│   └── key_exchange.*         # ECDH (SECP256R1)
├── diagnostics.*              # Heap monitoring, status reports
├── log/event_log.*            # RTC-RAM + LittleFS ping-pong persistent event log
├── net/
│   ├── time_sync.*            # NTP (local router first, then pool.ntp.org)
│   └── wifi_manager.*         # Wi-Fi connect/auto-reconnect
├── serial_args.h               # Serial command argument parsing (host-testable)
├── serial_console.*           # Serial command dispatch (`handleCommand`, split by topic)
├── storage/
│   ├── config_store.*         # LittleFS persistence (config + debug flags)
│   └── config_validation.h    # Config validation (host-testable)
└── web/
    ├── webserver.*             # REST API + WebSocket push (ESPAsyncWebServer)
    ├── dashboard.h              # Self-contained HTML control page served at GET /
    └── setup_portal.*          # First-boot SoftAP + captive-portal provisioning

FHEM/
├── 98_CasambiGW.pm            # Gateway module: WebSocket client, unit sync, command routing
├── 98_CasambiUnit.pm          # Per-luminaire device + CasambiVertical companion module
└── t/CasambiGW_helpers.t      # Perl unit tests (run in CI)

test/test_*/test_main.cpp      # Unity host tests, one dir per header under test (env: native)
scripts/                       # build_number.py (pre-build), stress_test.py, verify_tcp_stack.py
docs/                          # Design docs ("concept-*"), protocol reference, code-analysis notes
```

### Generic capability detection (no fixture hardcoding)

For each distinct fixture `type` the controller fetches
`GET https://api.casambi.com/fixture/{type}` and reads its `controls` list —
each control's `type` (dimmer/vertical/temperature/white/slider/…), bit
`offset`/`length`, and (for temperature) Kelvin `min`/`max`. Incoming state
bytes are mapped to controls by bit offset, not a fixed slot — the same byte
position means different things on different fixtures. This is why there is
no per-fixture-type branching anywhere in the BLE/state code; new fixture
types work automatically as long as the cloud fixture definition is
well-formed. Controls are persisted to LittleFS so decoding survives a reboot
without a cloud round-trip.

### Interface versioning (ESP ↔ FHEM) — read before touching the wire format

The REST API + WebSocket protocol share one `FHEM_API_VERSION_MAJOR/MINOR`
(defined in `src/config.h`, mirrored in `FHEM/98_CasambiGW.pm` as
`API_VERSION_MAJOR/MINOR` — **both files carry the full versioning contract as
a comment; read it there**). Any change to a request/response shape, endpoint,
or WebSocket message **must**, in the same commit:

1. Bump MINOR for a compatible extension, or MAJOR (reset MINOR to 0) for a
   breaking change — see the checklist comment in `src/config.h`.
2. Update the same constants in `FHEM/98_CasambiGW.pm`.
3. Document the change in `README.md` (REST API / WebSocket Push sections)
   including the version-history table there.

### BLE stack

NimBLE (`NimBLE-Arduino`), not Bluedroid — chosen for its smaller RAM
footprint, which matters because it competes with the TLS cloud handshake and
the async_tcp task for contiguous heap. `-DCONFIG_NIMBLE_CPP_ENABLE_RETURN_CODE_TEXT`
(in `platformio.ini`) is required for readable BLE error logs — don't remove it.

## Conventions worth knowing before editing

- **Extract Arduino-free logic into plain headers under `test/`-covered
  modules** (see the `native` env list above) rather than adding untestable
  logic straight into `.cpp` files that touch BLE/WiFi/LittleFS — this is the
  established pattern (`packet_parse.h`, `config_validation.h`, etc.) and lets
  CI actually catch regressions in that logic.
- **Long-running/blocking work happens on the loop task, not in async
  callbacks.** LittleFS writes, config mutation, and reboots triggered by HTTP
  requests are deferred via a `consume*Request()` flag drained in
  `main.cpp`'s `loop()` — never done directly in an `ESPAsyncWebServer`
  handler.
- **Cloud refresh never runs inline.** `refresh` (serial), `POST
  /api/refreshCasambi`, and FHEM's `refreshCasambi` all just set a "pending"
  flag and reboot; the actual HTTPS download happens early in the *next*
  boot's `setup()`, before BLE/web start, to guarantee a large contiguous
  heap for the TLS handshake and avoid tearing down live subsystems.
- **Comments in this codebase often explain a specific past bug or non-obvious
  constraint** (e.g. the `[common]`-section gotcha in `platformio.ini`, the
  build-number injection rationale in `config.h`). Read existing comments
  before "simplifying" code that looks over-engineered — the complexity is
  frequently load-bearing and tied to a numbered GitHub issue.
- **`docs/concept-*.md`** are design/decision documents for larger features or
  investigations (BLE migration, security hardening, HomeKit/Matter
  feasibility, provisioning, versioning, the Telnet console). Check there
  before re-investigating something that may have already been designed or
  deliberately shelved.
- Commit convention in this repo is Conventional Commits scoped to the module,
  e.g. `refactor(serial): extract the console and the cloud refresh from
  main.cpp`, `fix(web): calibrate the GET heap floor`.
