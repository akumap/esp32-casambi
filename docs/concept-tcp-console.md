# Concept: Serial console over TCP/IP (Telnet)

Status: **implemented and verified on hardware** (see section 8). The body of
this document is the original decision basis — the choice between the variants,
the security decision and the design; section 8 records the review after the
first implementation.
Branch: `claude/serial-console-tcp-ip-yxnmy8`

Answers the question: "How can the serial console additionally be operated over
the network, with a standard tool on the laptop?"

## 0. Short answer

**Target configuration:** the ESP32 on a USB power supply, placed freely for
good BLE conditions, within Wi-Fi range. A laptop on the same network operates
the console for `debug`, `refresh`, `status`, `log`. **Flashing stays
unchanged — serial over USB.**

**Decision:** a Telnet server in the firmware on port 23, login with the
**derived API token** that already exists (not with the Casambi password).
Client on the laptop: PuTTY, `telnet`, `nc`.

**SSH is rejected** — two independent showstoppers (heap, licence), section 3.

| Variant | Firmware effort | Heap at runtime | Host tool | Live logs | meets the target configuration |
|---|---|---|---|---|---|
| **A** `ser2net`/`socat` on a host at the USB port | none | 0 | `telnet`, `pio monitor rfc2217://` | ✅ | ❌ — requires a computer on the USB cable |
| **B** Telnet server in the firmware | medium–high | a few KB | **PuTTY**, `telnet`, `nc` | ✅ | ✅ **chosen** |
| **C** Console over the existing WebSocket `/ws` | medium | low (reuse) | `websocat`, browser | ✅ | ⚠️ no PuTTY |
| **D** `POST /api/console` + `curl` | low | minimal | `curl` | ❌ | ⚠️ no console, single commands only |
| **E** SSH server in the firmware | very high | ~40 KB+ contiguous | `ssh`, PuTTY | ✅ | ❌ **rejected**, section 3 |

Variant A remains the leanest solution for pure development work at the desk
(zero firmware, survives Wi-Fi outages, keeps the `esp32_exception_decoder`) —
it is ruled out only because the target configuration deliberately foresees *no*
computer at the device. It is therefore not a replacement but a complement for
the workbench.

## 1. Starting point in the code

Three properties of the existing code determine the entire design:

### 1.1 Output is scattered across the whole code base

662 `Serial.print*` calls in 16 files. There is no central output channel that
could be redirected:

| File | Calls | Task |
|---|---|---|
| `src/serial_console.cpp` | 223 | loop |
| `src/ble/casambi_client.cpp` | 143 | **NimBLE host task** (callbacks) |
| `src/diagnostics.cpp` | 67 | loop |
| `src/cloud/api_client.cpp` | 51 | loop (or boot) |
| `src/main.cpp` | 42 | loop |
| `src/storage/config_store.cpp` | 34 | loop |
| `src/cloud_refresh.cpp` | 26 | boot |
| `src/ble/packet.cpp` | 23 | NimBLE host task |
| `src/crypto/*`, `src/net/*`, `src/log/*`, `src/web/setup_portal.cpp` | 3–12 each | loop |
| `src/web/webserver.cpp` | 5 | **async_tcp task** |
| **Total** | **662** | |

The task column is the important part and was not obvious at first glance:
**output originates on at least three tasks.** `Serial.print` is uncritical
there, because the Arduino `HardwareSerial` serialises internally. A socket
write is not — writing to the same `WiFiClient` directly from the tee would
drive lwIP from three tasks in parallel. That rules out the naive "the tee
writes into the socket immediately" approach and leads straight to the ring
buffer in section 4.2.

### 1.2 Input comes exclusively from the loop task

`main.cpp:329-335` reads `Serial.readStringUntil('\n')` and calls
`handleCommand()` synchronously. `serial_console.h:8-14` explicitly records that
this is a load-bearing invariant: the wizard, `scan`, `connect` and the reboot
commands must not run in parallel with anything else; that is precisely why the
REST API bypasses these paths and instead enqueues `BleCommand` entries for the
loop task.

A Telnet server polled from `loop()` satisfies this invariant **at no extra
cost** — it calls `handleCommand()` on the same task. That is the main reason
for `WiFiServer` (synchronous) instead of AsyncTCP.

### 1.3 Five blocking input sites

`serial_console.cpp:242, 887, 916, 928, 938` wait hard on `Serial` with
`while (!Serial.available()) { esp_task_wdt_reset(); … }` — the setup wizard and
`connect`. Over Telnet the session would hang there. See decision E3.

## 2. Why Telnet at all and not C/D

The value of a network console stands or falls with the **asynchronous** output:
BLE reconnects, 0x06 broadcasts, the heap monitor, parse warnings. Anyone who
only wants to issue commands and see their immediate response does not need a
console — `curl` against the existing REST API can already do that (variant D).
That is exactly why the mechanical rename in section 4.1 is unavoidable: without
it the network console only sees what `handleCommand()` prints itself, making it
a cumbersome variant of `curl`.

Variant C (console over `/ws`) would be smaller on the firmware side, because
auth, heap guards and client cleanup already exist — but it fails on the
standard-tool criterion: `websocat` is not present on a fresh laptop, and a
browser panel in the dashboard would be a feature of its own. On top of that it
would be a change to the wire format and therefore subject to a
`FHEM_API_VERSION` bump (CLAUDE.md). Telnet is free of both.

## 3. SSH — rejected

The motivation is legitimate: with Telnet the login secret travels the LAN in
plaintext. Even so, SSH is not the way on this device.

### 3.1 Heap — the hard blocker

Measured values (from `docs/concept-asynctcp-churn-stability.md`, stress tests
on real hardware, via `/api/status`):

| Situation | `free_heap` | `largest_block` |
|---|---|---|
| Idle | 90–96 KB | 37–83 KB |
| Under mixed load (HTTP + WS) | 37–58 KB | **7–27 KB** |
| `min_free_heap` during the stress test | 7–16 KB | — |

Plus `HEAP_CRITICAL_THRESHOLD` = 20,000 B (`config.h:225`) → undercut three
times in a row ⇒ reboot (`diagnostics.cpp:54-67`). No PSRAM.

The decisive column is `largest_block`: under load only 7–27 KB are temporarily
available **contiguously**. That is exactly what runtime TLS handshakes already
fail on today — `README.md:1126` documents `HTTP -1` with the BLE stack active,
and `cloud_refresh.cpp:20-28` therefore defers the cloud download to the next
boot, *before* BLE and the web server start.

An SSH key exchange needs a comparable contiguous crypto working area — but **at
runtime, on every login, with BLE and async_tcp active**. That is precisely the
constellation the project has already documented as not working. A login that
happened to coincide with a BLE reconnect would fail or push the device below
the reboot threshold — on a device one was just trying to service remotely. The
trick from `cloud_refresh` (defer it to boot) is by definition inapplicable
here.

### 3.2 Licence — the second, independent blocker

The repository is under **MIT** (`LICENSE`).

| Library | Licence | Assessment |
|---|---|---|
| **wolfSSH** (+ wolfSSL) | GPLv3 or commercial | statically linked ⇒ the firmware binary becomes GPLv3, colliding with the MIT distribution |
| **libssh** (ESP32 ports exist) | LGPL | the relinking obligation is practically unsatisfiable for statically linked firmware |
| **Dropbear / OpenSSH** | — | presuppose POSIX processes, not sensibly portable |

mbedTLS is in the build (`webserver.cpp:16`), but it only provides primitives —
the SSH transport layer, KEX, userauth and channels are not included. There is
no "flip a flag" path.

### 3.3 Even the convenience gain is questionable

- A host key has to be generated and persisted in LittleFS; RSA keygen takes
  minutes on an ESP32, so only Ed25519/ECDSA is viable.
- Current OpenSSH/PuTTY versions reject outdated KEX/cipher sets. If the
  embedded library can only do a narrow set, the client ends up at
  `-oKexAlgorithms=+…` — at which point SSH is no longer a "just works" standard
  tool either.
- The blocking/backpressure problem from 4.2 would remain identical, only with a
  more expensive crypto layer in the write path.

### 3.4 Proportionality

`/api/*`, `/ws` and the dashboard already run unencrypted over HTTP in this
project, with the token in plaintext in every request header —
ESPAsyncWebServer cannot do TLS on the ESP32. SSH would make the debug console
of all things the only encrypted surface, while the full control API sits next
to it, open on the LAN. Anyone eavesdropping does not need the console at all.
That matches the protection-objective table in
`concept-security-hardening.md`, section 1: "protection against a passive L2
sniffer" is already classified there as **only partially in scope**.

### 3.5 If encryption is needed after all

Without a firmware change: if a Linux host runs somewhere on the network (e.g.
the FHEM Raspberry Pi, even when the ESP32 is no longer on its USB), then
`ssh pi` and from there `telnet casambi-xxxx.local`. Encrypted up to the Pi,
plaintext only on the last hop — and usable from outside the LAN.

## 4. Design

A new module `src/net/telnet_console.*`, polled from `loop()`.

### 4.1 Output: a `Console` tee + a mechanical rename

A global object `Console` derived from `Print` whose `write()` fans out to
`Serial` **and** to the Telnet ring buffer. Plus a `sed` replacement
`Serial.print` → `Console.print` at 662 sites.

**Only `print`/`println`/`printf`.** The six input and setup sites
(`Serial.begin`, `Serial.available`, `Serial.readStringUntil`) stay on the real
`Serial`. That makes the diff a pure, mechanically verifiable rename, and the
serial channel keeps working unchanged.

Deliberately *not* chosen: `#define Serial gConsole` in a central header. A
one-line trick, but a hidden redefinition of a global symbol — it does not match
this repository's style and would be extremely confusing on compiler errors.

### 4.2 The ring buffer solves three problems at once

`Console::write()` **never** touches the socket. It writes into a statically
allocated ring buffer; `loop()` drains it into the Telnet client.

That solves, simultaneously:

1. **Cross-task safety** (section 1.1) — output from the NimBLE host task and
   the async_tcp task lands in the buffer, not in the socket. The buffer is
   written under a short critical section, and lwIP is served exclusively by the
   loop task.
2. **Backpressure without watchdog risk** — if the client stops reading (laptop
   lid closed, half-dead session), the TCP window fills up. A blocking
   `client.write()` would stall the loop task; with a 45 s WDT that is a reboot.
   Before each chunk the drainer checks with a **`select()` with timeout 0** on
   the socket descriptor whether anything can be sent at all, and on overflow
   **discards** the oldest data instead of waiting. A drop counter is emitted on
   the next successful write as `[… N bytes dropped …]`, so silent gaps are
   recognisable.

   > **Do not use `availableForWrite()`.** `WiFiClient` does not override the
   > method in the ESP32 Arduino core, so it inherits the default
   > implementation from `Print`, which constantly returns **0**. A gate of the
   > form `if (availableForWrite() > 0)` therefore *never* sends — which in the
   > first version swallowed all command output (the banner, echo and prompt
   > remained visible because they are written directly and do not pass through
   > the ring buffer) and at the same time disabled the liveness probe from E6b.
   > In addition the **return value of `write()`** is evaluated and the cursor
   > is rewound over the unsent remainder, so a partial write loses nothing.
3. **Scrollback on login** — on connecting, the client gets a read index on the
   oldest still-valid entry instead of on the end of the buffer. That way you
   see the last ~4 KB of output from *before* the login. On a device you are
   visiting over the network precisely because something went wrong earlier,
   that is the difference between useful and useless.

One mechanism, three problems — which is why the ring buffer is not optional
trimming (decision E5).

### 4.3 Input: line and IAC parser

The bytes from the client run through a parser that
- discards Telnet `IAC` sequences starting at `0xFF` (3-byte `WILL/WONT/DO/DONT`
  and subnegotiation `IAC SB … IAC SE`) — PuTTY sends them on connecting;
  without the filter they land in the line buffer and the first command is
  garbage. `nc` sends nothing at all, so the filter has to be tolerant;
- treats CR, LF, CRLF and **CR NUL** alike as a line ending (Telnet clients send
  CR NUL; `readStringUntil('\n')` + `trim()` only covers that by accident
  today);
- handles backspace/DEL for the local echo;
- limits the line length hard (protection against a client that never sends a
  `\n`).

This logic is Arduino-free and, per the repository convention, belongs in a
header of its own, `src/net/telnet_line.h`, with host tests under
`test/test_telnet_line` — analogous to `serial_args.h` / `packet_parse.h`
(decision E10).

### 4.4 Command execution

A completed line goes straight to `handleCommand()` — on the loop task, so it
remains conformant with `serial_console.h:8-14`. No queue, no second task.

Blocking commands (`scan`, `connect`, `refresh` with a subsequent reboot) then
also block the Telnet session for their duration — exactly as on the serial
console. The WDT is already fed in those loops.

### 4.5 Login

```
token = hex( SHA-256( "casambi-api:" + <Casambi network password> ) )
```

The device derives the same value from the typed input and compares it in
constant time against the stored token. The building blocks exist in full:
`webserver.cpp:250-267` (`_deriveApiToken`) and `_constantTimeEquals`. Rationale
in decision E1.

Flow: connection → `IAC WILL ECHO` + `IAC WILL SGA` → prompt `Password:` (echo
deliberately suppressed) → 3 attempts with a delay after a failure → close the
connection afterwards. After a successful login: scrollback replay, a banner
with the build number, then the prompt.

### 4.6 Idle timeout and liveness

The timeout serves two purposes: ending the session of a forgotten login and —
more importantly — freeing the **single session slot** (E6) when the client
disappears without a FIN (laptop lid closed, Wi-Fi gone). Otherwise the device
never detects a half-open TCP connection and you lock yourself out.

The two purposes are solved **separately**, so that `timeout 0` is safe:

**Timeout** = the time since the last **fully received command line**. That
definition is not arbitrary:

- *Not* "since the last received byte": PuTTY can send keepalives of its own
  (setting *Connection → Seconds between keepalives*, which sends Telnet NOPs to
  keep NAT state alive). Those would reset the timer permanently — the mechanism
  would be ineffective without anyone noticing.
- *Not* "no traffic in either direction": that would keep an overnight capture
  alive by itself, but a 15-minute quiet phase of the device at 3 a.m. would
  abort the recording mid-way and unnoticed. Input-based, plus a deliberately
  set `0`, is predictable.

**Liveness** = independent of the timeout. lwIP keepalive on the socket
(`SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT` via `setsockopt`; whether it is
active in this build's IDF configuration has to be checked during
implementation), or alternatively an `IAC NOP` every 60 s from the device —
invisible to the client, but it runs into a TCP error with a dead peer and frees
the slot. That keeps the slot self-healing even with the timeout switched off.

Operation, in the style of the existing commands (`debug …`, `wifi …`,
`ntp status`):

```
telnet status              # timeout, active session, dropped bytes
telnet timeout <seconds>   # 0 = off, otherwise 60..86400
```

Persisted as a field in `NetworkConfig`, like the debug flags
(`network_config.h:181-186`, `config_store.cpp:101-104`). **Important:** the new
value has to be added to the merge list `network_config.h:266-271`, otherwise
the next cloud `refresh` silently resets it — the same trap that was already
accounted for there for the six debug flags.

Nothing extra is needed on the laptop side for long captures: PuTTY → *Session →
Logging → All session output* writes straight to a file.

## 5. Decisions (the previously open questions)

| # | Question | Decision | Rationale |
|---|---|---|---|
| **E1** | Login with the network password or the derived token? | **Token** (64 hex characters, stored once in the PuTTY session profile) | The Casambi password is a **cloud account credential** — it gets you into the app and the cloud API, far beyond this device. The token is purely device-local. `API_TOKEN_PREFIX` was introduced for exactly this (`config.h:187-195`). It costs no extra line of firmware compared to the password variant. To be honest about it: the token remains a bearer credential in plaintext and can be captured/replayed on the LAN — but an eavesdropper then only has what the already-open REST API would have given them, and **no access to the cloud account**. |
| **E2** | SSH instead of Telnet? | **rejected** | Heap (3.1) and licence (3.2) are two independent showstoppers, neither of them avoidable by writing better code. |
| **E3** | The setup wizard over Telnet? | **blocked** — `setup` and `wifi set` answer over Telnet with "only available on the serial console" | Saves the complete rework of the five blocking input paths (1.3). No real loss: flashing goes over USB anyway, and first-time provisioning runs through the SoftAP portal. `wifi set` would additionally pull the rug out from under its own session. And both print credentials — a genuine leak over plaintext Telnet (`serial_console.cpp:744-748` masks `wifi set` today only in the command echo, not the wizard output). |
| **E4** | Negotiate the echo ourselves or document the PuTTY settings? | **negotiate it ourselves** (`WILL ECHO`, `WILL SGA`), the device echoes characters including backspace | PuTTY defaults to "Local echo/line editing: Auto"; without negotiation you type character by character and blind. The decisive point, though, is the password entry: deliberately *not* sending an echo is only possible if the device holds echo authority. The PuTTY documentation route ("Local echo set to Force on") additionally goes into the README as a fallback for clients that do not negotiate. |
| **E5** | Scrollback ring buffer yes/no, how big? | **yes, 4 KB, statically allocated** | Not optional trimming but the mechanism that also solves cross-task safety and backpressure (4.2). **Static** is the point: static RAM is plentiful (59,572 B of 532,480 B used), while the heap is the scarce resource — a `static uint8_t[4096]` costs **nothing** there. |
| **E6** | Several concurrent sessions? | **one**; further connections are rejected with a note | Otherwise two operators compete for blocking commands like `connect`. The session is closed on reboot commands. |
| **E6c** | A clean logout? | **`exit` / `quit`**, over Telnet only, not in `help` (which is shared with the serial console, where there is nothing to leave) | Follows directly from E6: without a logout the single slot stays occupied until the idle timeout kicks in or the dropped connection is noticed — which blocks the next login from another machine. `plink` has no Telnet escape (`Ctrl+]` is a property of the classic `telnet` client, not of the protocol), so `exit` is the only in-band way out. |
| **E6a** | Fixed or configurable idle timeout? | **configurable and disengageable**: `telnet timeout <s>`, default 900, `0` = off; persisted in `NetworkConfig` | Long overnight captures are a real scenario in which nothing is typed for hours. What is measured is the time since the last **command line**, not since the last byte (PuTTY keepalives would otherwise reset the timer permanently) and not "no traffic in either direction" (a quiet phase of the device would silently abort the capture). Details and rationale in 4.6. |
| **E6b** | What replaces the timeout at `0`? | **keepalive/`IAC NOP` as an independent liveness check** | The timeout also frees the single session slot when the client disappears without a FIN. Without a replacement, `timeout 0` would mean a closed laptop blocks the device permanently. Solved separately ⇒ `timeout 0` is safe. |
| **E7** | Behaviour with a dead client | **never block**, discard the oldest data, print a drop counter | A blocking `client.write()` on the loop task is a reboot with a 45 s WDT — the one scenario in which this extension endangers device stability. |
| **E8** | Port and activation | **port 23**, the server starts **only when a token exists** | Port 23 is PuTTY's Telnet default. Tying it to the token is a security decision: `webserver.cpp:281` treats "no password stored" as *auth off* — defensible for the REST API, but **not** for a console with `clearconfig`/`restart`. Without a token the port therefore stays closed, in setup mode too. |
| **E9** | API versioning | **no bump** of `FHEM_API_VERSION_MAJOR/MINOR` | Telnet is not a REST/WebSocket wire format; the rule in CLAUDE.md and `config.h` does not apply. A README section + an entry in the feature overview are due nonetheless. |
| **E10** | Where does the parser logic live? | its own header `src/net/telnet_line.h` + `test/test_telnet_line` | Repository convention: Arduino-free logic is extracted so it is host-testable (CLAUDE.md, "Conventions"). The IAC filter and line splitting are exactly that. |
| **E11** | Challenge-response instead of a plaintext token? | **deferred** | A nonce + `HMAC(token, nonce)` would not be replayable and would be cryptographically sound — but it cannot be typed by hand in PuTTY. You would need a helper script on the laptop and would thereby lose the standard tool, i.e. the whole point. Noted as a later stage should the threat model change. |
| **E12** | Register an mDNS service `_telnet._tcp`? | **no** | Two things to separate: the *name resolution* `casambi-xxxx.local` comes from `MDNS.begin()` (`time_sync.cpp:38`), already exists, and is all PuTTY needs. A *service announcement* (PTR/SRV/TXT, like `addService("http","tcp",80)` in lines 40-43) additionally makes the device discoverable in service browsers (`avahi-browse -a`, Bonjour Browser, Finder). PuTTY does not search mDNS — the practical gain is zero, while the entry actively tells every LAN device that an administrative console is listening here on port 23. `setup_portal.cpp:233` shows the same thinking: with the portal open, hostname/MAC/IP are deliberately not published. **Middle ground**, should discoverability be wanted later: a TXT record `telnet=23` on the existing HTTP service instead of an entry of its own. |

## 6. Effort and risks

| Item | Size |
|---|---|
| New module `net/telnet_console.*` | ~250–350 lines |
| Parser header + host tests | ~80 + ~120 lines |
| Mechanical rename `Serial.print` → `Console.print` | 662 lines in 16 files, a pure rename |
| Hooking into `loop()`, the commands `telnet status` / `telnet timeout` | a few lines |
| Timeout field in `NetworkConfig` + `config_store` + the merge list (E6a) | a few lines, easy to forget |
| mDNS | no change (E12) |
| README section (operation, PuTTY settings, obtaining the token) | — |
| **Heap at runtime** | listening socket + 1 client ≈ a few KB of lwIP; ring buffer 0 (static) |
| **Flash** | uncritical (1.48 MiB free in the `huge_app` layout) |

**Main risk:** blocking writes to a dead client → watchdog reboot. Addressed by
E7, but it has to be tested deliberately (open a session, suspend the laptop,
observe the device under output load).

**Secondary risk:** the rename touches every file with output. It should run as
**its own commit before** the functionality, so the feature diff stays readable.

**Not addressed:** if Wi-Fi drops, the console is gone — precisely when there
are Wi-Fi problems. The fallback stays USB, which is needed for flashing in this
configuration anyway. The persistent event log (`log/event_log.*`, command
`log 2`) survives reboots and covers the coarse post-mortem.

## 7. Test plan

1. **Host tests** — `pio test -e native -f test_telnet_line`: IAC filter (3-byte
   and subnegotiation), CR/LF/CRLF/CR-NUL, backspace, overflow of the maximum
   line length.
2. **Client matrix** — PuTTY (Telnet **and** raw), Linux/macOS `telnet`, `nc`.
   In each case: the login prompt is legible, the password is not visible, the
   first command is unmangled.
3. **Auth** — a wrong token 3× ⇒ the connection is closed; no token stored ⇒
   port 23 closed (`nc -vz`).
4. **Backpressure** — open a session, `SIGSTOP` the client process, put the
   device under output load (`debug ble on` + BLE activity): no reboot, the drop
   counter appears after resuming.
5. **Blocking** — `setup` and `wifi set` over Telnet ⇒ a note, no execution; the
   same commands over serial ⇒ unchanged and functional.
6. **Idle timeout** — with a short test value (`telnet timeout 60`): the session
   drops without input; with PuTTY keepalives enabled it drops **anyway**
   (proving that command lines and not bytes are measured); with output running
   it drops as well. `telnet timeout 0` ⇒ the session survives a night. The
   value survives a reboot **and** a `refresh`.
7. **Session slot** — kill the client hard (network cable/Wi-Fi off, no FIN)
   with `timeout 0`: the slot is freed by the keepalive/NOP and a new connection
   is possible.
8. **Serial regression** — a complete command run over `/dev/ttyUSB0` after the
   rename (procedure in CLAUDE.md, "Serial monitor").
9. **Stability** — `scripts/stress_test.py` with a Telnet session open, to check
   the interaction with async_tcp and the heap.

-----

## 8. Addendum: review after the first implementation

A review of the finished implementation (focused on correctness/stability)
showed that the care taken in 4.2 had landed one-sidedly in the **output** path.
The points below have been retrofitted; decisions E1–E12 remain valid unchanged,
and E3, E6 and E7 are merely now fully implemented.

### 8.1 E7 only applied to the drainer (stability)

`socketWritable()` protected only `_drainOutput()` and the NOP probe. The
banner, `Password:`, the character echo, the prompt, "Busy", "Bye.", the idle
message and the drop message went to the socket as direct `_client.print()`
calls — and `WiFiClient::write()` is **not** non-blocking: with a full send
window the Arduino core runs a retry loop of its own
(`WIFI_CLIENT_MAX_WRITE_RETRY` attempts of 1 s `select()` each), so one call can
stall the loop task for ~10 s. Exactly the case from test plan item 4 (client
`SIGSTOP`), as soon as the client is still typing.

`select()` + `write()` is not enough either: `select()` only promises that
**one** byte fits; for the rest the core goes back into the same loop.

**Now:** a single output path. Everything is buffered in `_outBuf` and sent with
a single `send(..., MSG_DONTWAIT)` on the descriptor. If nothing more fits it is
discarded (E7) instead of waited on. The ring buffer is only tapped when a whole
chunk in its worst-case form (every byte doubled by CR-LF expansion **or IAC
escaping**) is guaranteed to fit — otherwise the cursor would run over bytes
that nobody could account for afterwards.

### 8.2 The input path was unbounded (stability, reachable before auth)

`while (_client.available())` had no budget, but the WDT is only fed once per
`loop()`. A peer sending faster than the console reads
(`cat file | nc device 23`) can hold the loop task arbitrarily long — after 45 s
a reboot, without a token, without a login. The same loop also executed **all**
lines of a paste between two watchdog feeds.

**Now:** `TELNET_INPUT_BUDGET_BYTES` per iteration and at most **one** command
line per iteration; the rest waits in the socket buffer.

### 8.3 The E3 block was bypassable (security)

`startsWith("wifi set")` checked the raw line, but `cmdWifi()` trims its
sub-command itself — so `wifi␣␣set ssid pw` went through, and for the same
reason the masking in the command echo did not apply: the Wi-Fi password ended
up in plaintext in the ring buffer and therefore on exactly the Telnet
connection E3 is supposed to keep it off.

**Now:** `net/telnet_policy.h` normalises the line (trim + collapse whitespace
runs) and checks against that; what is executed is still the trimmed original
line. Host tests under `test/test_telnet_policy` (convention E10). In addition
the command line over Telnet is now trimmed — previously `status␣` failed as
"Unknown command" while the same input worked over serial.

### 8.4 The prompt came before the output (usability)

The prompt was written straight into the socket while the command's output was
still only in the ring buffer — so the client saw `> ` **before** the command's
result. The session now remembers the pending prompt and sends it once the ring
buffer has drained. Side effect: after login it appears after the scrollback
replay, not before it.

### 8.5 E6 "the session is closed on reboot commands" was missing

`restart`, `clearconfig`, `refresh` and `wifi set` ran straight into
`ESP.restart()`. The confirmation line was only in the ring buffer, which nobody
drains any more; the connection died without a FIN. `telnetNotifyReboot()` now
sends a reason at all four sites (plus `POST /api/reboot`) and closes the
session cleanly.

### 8.6 Smaller corrections

| Point | Before | Now |
|---|---|---|
| Line overflow | the truncated line was **executed** (`ulevel 5 200…` switches a real lamp when shortened) | the parser reports `LineTooLong` and the line is discarded |
| Login slot | an unauthenticated connection occupied the single slot until the idle timeout (with `timeout 0`: permanently) | a login timeout of its own, `TELNET_LOGIN_TIMEOUT_MS` |
| Failed attempts | the counter was per connection, so a reconnect reset it; 500 ms `delay()` each in the loop task | lockout across connections (`TELNET_LOCKOUT_*`), backoff as a deadline instead of `delay()` |
| `IAC` in the output | a 0xFF in the console text ate the following byte at the client | doubled (RFC 854) |
| `telnet status` | reported "listening" as soon as the object existed | queries `listening()`, i.e. the actual bind |
| Start | only in the Wi-Fi branch of `setup()` | is caught up on a late Wi-Fi connect (like the web server/mDNS) |
| Idle timeout | `seconds * 1000` with no bound on load | clamped to `TELNET_TIMEOUT_MAX_SECONDS` on load |
| Drop message | German while the rest of the console is English | English |

### 8.7 Addition to the test plan

In addition to 7.1–7.9, to be checked specifically:

10. **Flood** — `head -c 5M /dev/urandom | nc device 23` **without** a login: the
    device stays reachable (`curl /api/info`), no WDT reboot, the boot counter
    unchanged.
11. **Stalled client** — open a session, `kill -STOP` the client, then
    `debug ble on` and BLE load: the drop message appears after `kill -CONT`, no
    reboot, and the BLE reconnect kept running in the meantime.
12. **Paste** — paste 20 commands at once: they all run one after another, with
    the prompt **after** each output; an over-long line is rejected.
13. **Blocking** — `wifi␣␣set X Y` over Telnet ⇒ rejected, no password in the
    echo; unchanged and functional over serial.
14. **Lockout** — a wrong token 6× across several connections ⇒ the port rejects
    for 60 s, then a normal login.
15. **Reboot** — `restart` over Telnet ⇒ a message + a clean connection
    teardown.

Verified on 2026-08-06 on the ATOM Lite (build 265): items 10–15 passed.
