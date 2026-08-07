# Concept: Stability under WebSocket/TCP connection churn (issue #18)

Status: **verified on hardware.** Addresses the reboots under high-frequency
connection churn documented in #18. The diagnostic instrumentation (`WSDBG`,
`WiFiRC:`) and the stress harness are already in `main` (from PR #19).
Branch: `claude/migrate-async-tcp-stack-esp32async`

**Implementation status:**
- ✅ `platformio.ini`: async stack switched to `ESP32Async/AsyncTCP#v3.4.10` +
  `ESP32Async/ESPAsyncWebServer#v3.11.1` (fixed git tags).
- ✅ `src/main.cpp`: the Wi-Fi reconnect is now non-blocking / watchdog-safe.
- ✅ `src/web/webserver.h`: the obsolete `HTTP_GET/POST/DELETE` macro workaround
  was **removed** — the new stack declares the methods itself (details in
  4.1.1). Otherwise no change to `webserver.cpp` (all remaining touch points are
  stable).
- ✅ Compile fix after the first build attempt on the host (macro collision with
  the new stack's enum).
- ✅ Build on the PlatformIO host successful; **two-stage acceptance passed**
  (results in section 6) — the new stack is also noticeably faster.
- ✅ Long-term soak (`realistic`, 900 s) ran clean — no leak in production mode.
- ✅ Diagnostic instrumentation gated, the instability notes removed from
  `README.md` (section 7).

## 1. Goal

The device must **no longer reboot** under **abusive connection churn** (many
WebSocket connects/disconnects per second, e.g. a client in a fast reconnect
loop). The fix only counts as done when the WS churn reproduction runs cleanly
with the existing stress harness — **no** `Guru Meditation`, **no**
`REBOOT DETECTED`, with the heap and largest block recovering.

Two symptoms with a partly shared root have to be covered:

- **(A) A panic in the AsyncTCP stack** while accepting/closing TCP connections
  (primary, reproducible) — occurs **with a healthy heap**.
- **(B) A watchdog hang in `WiFi.begin()`** after a load-induced Wi-Fi drop (the
  original report in #18) — occurs **with a tight heap**.

## 2. Starting point (as-is)

This is **separate** from the double-response heap leak of the dynamic POST
routes — that was an application bug and was fixed in PR #19. #18 is **not** in
the application code.

Current async dependencies (`platformio.ini:8`):

```
esphome/ESPAsyncWebServer-esphome @ 3.2.2
esphome/AsyncTCP-esphome          @ 2.1.4
build_flags = -DCORE_DEBUG_LEVEL=0 -DCONFIG_ASYNC_TCP_RUNNING_CORE=1 -DCONFIG_ASYNC_TCP_USE_WDT=0
```

Both `*-esphome` forks are **not actively maintained**.

## 3. Root cause

### 3.1 (A) Races in `AsyncTCP-esphome`
A pure WS churn run (fast connects/disconnects only, no GET/POST) reboots even
though the heap is **healthy** (free ~100 KB, largest block ~80 KB throughout).
Two crash sites, both in the lwIP `tcpip_thread`, both in library code:

```
LoadProhibited, EXCVADDR=0x30
  AsyncServer::_accept(tcp_pcb*, signed char)   AsyncTCP.cpp:1421
LoadProhibited, EXCVADDR=0x00
  AsyncClient::_lwip_fin(tcp_pcb*, signed char)  AsyncTCP.cpp:920
```

These are **null-deref/use-after-free races** during accept (`_accept`) and
during FIN processing (`_lwip_fin`) under churn. The `WSDBG` instrumentation
shows: the client count stays within the cap and oscillates normally, and the
heap recovers after **every** connect/disconnect cycle (no WS leak) — so a
**logic race**, not OOM.

### 3.2 (B) Blocking `WiFi.begin()`
In a mixed run, the heap had previously been driven low by a separate churn
episode (`free ~36 KB, largest_block 7156`, stuck for ~1 min); afterwards Wi-Fi
dropped and the reconnect path hung. The `WiFiRC:` checkpoints (`main.cpp:436`)
show `-> begin()` as the last line, `-> wait loop` is never reached →
**`WiFi.begin()` blocked the loopTask for the full 30 s** → WDT reboot.

Likely chain: **low/fragmented heap → the Wi-Fi subsystem cannot allocate → the
link drops → `WiFi.begin()` stalls under heap pressure → watchdog reboot.**

> Important: realistic load triggers none of this. The FHEM gateway holds **one
> persistent WebSocket without churn**; the `realistic` profile ran without
> error (0 errors, heap stable). (A)/(B) need an abusive churn pattern.

## 4. Solution

Two **independent** measures — separately implementable and testable.

### 4.1 Migrate the async stack to ESP32Async (against A)

Switch from the `*-esphome` forks to the actively maintained **ESP32Async** org:

```
ESP32Async/ESPAsyncWebServer   (current, ~v3.11.x)
ESP32Async/AsyncTCP            (current, ~v3.4.10+)
```

Rationale: the ESP32Async/AsyncTCP changelog contains **targeted fixes in the
close/FIN path** — v3.4.10 "Defer close on fin to async task", v3.4.6
"Error/closing stability", v3.4.7 (abort/dispose + memleak), v3.4.5 ("Replace
closed_slots with double indirection"). These aim directly at the **`_lwip_fin`**
crash. For **`_accept` there is no confirmed fix** → that path is unverified and
**must** be secured through the WS churn reproduction.

→ This is a **standalone, carefully tested dependency migration** (like BLE/
NimBLE before it), not a quick change. Revertible on a branch basis; keep the
`*-esphome` versions noted above for a fast re-pin.

#### Steps
1. A dedicated branch off `main` (after PR #19 is merged, so the
   double-response fix is in the base).
2. Switch `lib_deps` to the ESP32Async libraries; drop both `esphome/*` entries.
3. Review the recommended `CONFIG_ASYNC_TCP_*` build flags from the
   ESP32Async/AsyncTCP README (queue size, priority, stack size, running core,
   max ack time); keep `RUNNING_CORE=1`. Cross-check the interaction with the
   tight heap (NimBLE coexistence).
4. Clarify API/compile changes in `src/web/webserver.cpp` / `.h` — the critical
   touch points:
   - `request->_tempObject` (body buffering) — confirm the field/semantics.
   - `request->onDisconnect([...])` (cleanup of aborted bodies).
   - `request->beginChunkedResponse(...)` (streaming source for `/api/log`).
   - `AsyncWebSocket::cleanupClients(WS_MAX_CLIENTS)`, `textAll`, `count()`,
     `client->text()/close()`.
   - `DefaultHeaders::Instance().addHeader(...)`.
   - **Catch-all routing** (`onNotFound` + `onRequestBody`): the interplay can
     differ in the new fork → **re-verify that exactly one `request->send()`
     happens per POST** (the core of the PR #19 fix). Re-run the
     oversize/control/invalid isolation so that no per-request leak returns.

#### 4.1.1 Result of the API review (implemented)
One source change was necessary: the **`HTTP_GET/POST/DELETE` macro workaround**
in `webserver.h` had to be **removed**. The old `*-esphome` fork wrapped its
method enum in `#ifndef HTTP_ANY`, so macros set beforehand suppressed it. The
ESP32Async stack, by contrast, declares the methods in the enum
`AsyncWebRequestMethod` (values include `HTTP_DELETE=1<<0`, `HTTP_GET=1<<1`)
**without** that guard and exports them globally itself (`using namespace`,
switchable off via `ASYNCWEBSERVER_NO_GLOBAL_HTTP_METHODS`). The old macros
destroyed this enum at compile time (`expected identifier before numeric
constant`) — hence their removal. A collision with the Arduino core's
`HTTPMethod` enum (`<HTTPClient.h>`, via `cloud/api_client.h`) only arises at
the actual point of use; no bare `HTTP_*` is referenced next to `HTTPClient` in
the relevant translation units, so no qualification is needed.

All remaining touch points in `webserver.cpp` are unchanged (checked against
v3.11.1):
- `request->_tempObject` (`void*`), `request->onDisconnect(...)`,
  `request->beginChunkedResponse("application/json", filler)` with a
  `size_t(uint8_t*, size_t, size_t)` lambda — unchanged.
- `AsyncWebSocket`: `cleanupClients`, `textAll`, `count`,
  `client->text/close/id` and the event callback signature — unchanged.
- `DefaultHeaders::Instance().addHeader(...)` — unchanged.

**One relevant behavioural change** (no compile break): as of ESPAsyncWebServer
v3.11.0, `CloseClientOnQueueFull` defaults to **`false`** — a WS client with a
full send queue is **no longer** disconnected; the message is dropped instead.
That suits our broadcast model (every `unit_state`/`connection_state` message is
a **complete** state snapshot; a dropped message is corrected by the next one —
the same drop tolerance as our `broadcastUnitState` queue). It is therefore
**deliberately left at the default**; acceptance on hardware has to confirm that
a slow client does not build up a queue (WS cap = 3, low broadcast volume).

`cleanupClients(WS_MAX_CLIENTS)` (in `loop()`) enforces the cap correctly on the
new stack — it closes the oldest client (one per call, asynchronously via a
close frame). Since the close is asynchronous and `count()` still counts the
closing client that has not been removed yet, opening several clients
simultaneously can push the population **briefly just below** the cap (e.g. 2
with a cap of 3). That is uncritical: the cap is an **upper bound**; what
matters is that it is never **exceeded**. Confirmed on hardware (acceptance
T10).

### 4.2 Make the Wi-Fi reconnect watchdog-safe (against B)

Independent of the library swap and **taking precedence**, since it turns the
hard reboot into a soft retry — even with a low heap.

The old path `checkAndReconnectWiFi()` called, in sequence on the loopTask,
`WiFi.disconnect()` → `delay(100)` → **`WiFi.begin()`** → a 5 s wait loop.
`WiFi.begin()` was the blocking spot.

**Implemented** (`src/main.cpp`): a non-blocking reconnect.
- The blocking `begin()` + wait loop is **removed**.
- Primary recovery goes through `WiFi.setAutoReconnect(true)`, already active
  from the first connect (`main.cpp:238`) — the IDF Wi-Fi task reconnects by
  itself.
- Per 30 s tick there is only a **non-blocking nudge** via `WiFi.reconnect()`
  (uses the stored config, lighter than `begin()`, returns immediately); the new
  status is observed on a later tick rather than waited for here.
- The follow-up work on reconnection (NTP re-arm, defensive web-server restart)
  now runs **once**, on the *disconnected → connected* transition.
- The `WiFiRC:` checkpoints stay in for verification (now
  `-> reconnect() [non-blocking]` and `<- reconnected`).

> Note: since (B) is heap-driven, the leak fix from PR #19 may already defuse
> the trigger. The watchdog-safe reconnect is still needed because it keeps the
> behaviour graceful under heap pressure too.

## 5. Risk mitigation

A stack migration is risky because behaviour can change **subtly** without
anything obviously breaking: the `*-esphome` and ESP32Async forks share the same
API surface, but the **behaviour** of the catch-all callbacks, the chunked
response path or WS client management can differ. Pure stress tests
(`stress_test.py`) find crashes and leaks, but do **not** check whether every
function still works *correctly* on the new stack. That is exactly the gap the
mitigation closes.

### 5.1 Strategy
- **Revertible on a branch basis** (like BLE/NimBLE): the `*-esphome` versions
  stay noted above; on a regression, discard the branch and re-pin.
- **Small, verifiable steps**: first swap only `lib_deps`/flags and build, then
  compile fixes, then functional verification, and only then stress.
- **Two-stage acceptance**: a **functional** verification of the
  migration-sensitive points (5.2) *before* the existing **stress/churn**
  acceptance (section 6). Function first — a stack that does not crash under
  load but buffers bodies incorrectly is no solution.

### 5.2 The functional verification script `scripts/verify_tcp_stack.py`

A new, standalone script (pure stdlib, the same WS/HTTP helpers as
`stress_test.py`). It **does not load the device**; it specifically checks the
**API touch points that can shift with the stack change** — every point from
4.1 step 4 gets its own assertion. CI-capable: exit code 0 = all checks green,
1 = at least one red.

| ID | Checks | Migration relevance |
|---|---|---|
| T1 | CORS header on every response | `DefaultHeaders::Instance().addHeader` |
| T2 | GET routes return valid JSON | `server.on()` GET paths |
| T3 | `/api/log` streams a valid JSON array (`?n=`, `?n=0`) | `beginChunkedResponse` |
| T4 | The POST body reaches the handler (no "missing body") | `onRequestBody` → `_tempObject` → `onNotFound` dispatch |
| T5 | No per-POST leak across a burst | **single response** (the PR #19 regression must not return) |
| T6 | An oversized POST → 413 | `contentLength()` reject in `onNotFound` |
| T7 | Invalid JSON / empty body → 400 | a clean single response from the handlers |
| T8 | An aborted body is freed | `request->onDisconnect()` cleanup |
| T9 | WS handshake + `hello` snapshot on connect | `AsyncWebSocket` upgrade + `client->text()` |
| T10 | The WS client cap is enforced | `cleanupClients(WS_MAX_CLIENTS)` |

T5 and T8 compare the heap (`free_heap` from `/api/status`) before/after a burst
so that a slow leak shows up too. At the end the script uses `boot_count` to
verify that the device did **not** reboot during the checks.

Invocation:
```
python3 scripts/verify_tcp_stack.py --host <ip> --ws-max-clients 3
```
Run before a BLE connection is established, T4 is satisfied by a `503` (instead
of `200`) — all that matters is that the body arrives, not the BLE success.

## 6. Acceptance (must pass before #18 counts as solved)

**Stage 1 — function** (see 5.2): `verify_tcp_stack.py` has to complete with
exit code 0. Only then stage 2.

**Stage 2 — stability:** flash, `debug heap on`, then with
`scripts/stress_test.py`:

```
# WS churn — what previously crashed _accept / _lwip_fin
python3 scripts/stress_test.py --host <ip> --profile medium \
        --skip-get --skip-post --skip-abort --duration 120

# Full mixed load
python3 scripts/stress_test.py --host <ip> --profile medium --duration 120

# Realistic regression — has to stay perfect
python3 scripts/stress_test.py --host <ip> --profile realistic --duration 120
```

Pass criteria: no `REBOOT DETECTED`, no `Guru Meditation` (in particular no
`_accept` / `_lwip_fin`), free heap recovers, "largest block recovered". The
WSDBG trace shows no client accumulation.

### 6.1 Result (on hardware, M5Stack ATOM Lite / ESP32-PICO-D4)

- **Stage 1 — function:** `verify_tcp_stack.py` **10/10 passed**. Evidence
  includes: T5 no per-POST leak (60 POSTs, drift ~1.3 KB ≪ limit), T8 aborted
  bodies ~0 B drift (the `onDisconnect` cleanup works), T10 the cap held as an
  upper bound.
- **Stage 2 — stability:** all profiles **passed, no reboot, no
  `Guru Meditation`** (in particular no `_accept` / `_lwip_fin`):
  - `medium` (WS churn only), `medium` (mixed load), `realistic` — clean.
  - **`heavy`** (180 s, the former "breaking point" profile from #17/#18):
    5842 requests, ~0.9 % errors (pure timeouts / deliberate aborts under
    abusive churn). **Free heap RECOVERED** (90 KB → 58 KB under load →
    96 KB after cooldown). **Largest block recovered** (39 KB → min 13 KB →
    83 KB). No persistent leak, no lasting fragmentation.
  - **Long-term soak** (`realistic`, 900 s): ran clean, no reboot, heap stable —
    no slow leak in production mode.
- The new stack is also **noticeably faster** than the `*-esphome` fork.

→ The churn crashes documented in #18 (`_accept` / `_lwip_fin`) **no longer**
occur on the ESP32Async stack; the watchdog-safe reconnect prevents the
`WiFi.begin()` WDT hang. #18 is thereby solved in substance.

> ⚠ The numbers in 6.1 date from **before** the migration from Bluedroid to
> NimBLE. The BLE stack materially co-determines heap occupancy, so they are not
> a valid basis for comparison with today's state — see 6.2.

### 6.2 Fragmentation in isolation (current stack, ATOM Lite)

All runs under `heavy`, 180 s, on the same device without a reboot in between.
The mixed-load run reproducibly showed "largest block did not recover", which
raised the question of which share of the load causes it. Isolated via the
`--skip-*` flags:

|Run                          |Start |Minimum|after cooldown|Verdict           |
|-----------------------------|------|-------|--------------|------------------|
|**A** — WS churn only        |75 KB |15 KB  |**37 KB**     |fragmented        |
|**B** — HTTP only (`--skip-ws`)|37 KB|7 KB   |**37 KB**     |**recovers**      |

```bash
# A — WebSocket churn only
--profile heavy --duration 180 \
  --skip-get --skip-post --skip-control --skip-invalid --skip-oversize --skip-abort
# B — HTTP only
--profile heavy --duration 180 --skip-ws
```

**Finding: the lasting fragmentation comes from the WebSocket churn, not from
the HTTP paths.** Run B started at the value run A had left behind, drove 423
`/api/units` and 422 `/api/log` requests (both chunked, each holding a ~1 KB
generator structure for the full duration of the response) and ended at exactly
37 KB again. The obvious suspicion that these small long-lived allocations are
the cause is thereby **refuted**.

Putting it in context:

- Run A makes 2605 connections in 180 s ≈ **14.5 connection setups per second**.
  Real operation is FHEM plus the occasional browser, i.e. a handful per day.
  The profile is deliberately abusive.
- The value **converges**: across five runs by now, the largest block after
  cooldown lands in the 33–49 KB band regardless of the starting value
  (41–75 KB). That points to a characteristic level, not to unbounded decay.
- Operationally nothing is at risk at 37 KB: a hello needs 7–10 KB, and the
  framework's chunk buffer ~5.5 KB.
- The allocations are inside ESPAsyncWebServer/AsyncTCP (client structures and
  their queues), not in the application code — per WS client the firmware only
  allocates the hello payload itself, and that is short-lived.

### 6.3 Does it accumulate? No.

Three further WS churn runs back to back, without a reboot in between:

|Run |Start |Minimum|after cooldown|Delta  |Script verdict (old)|
|----|------|-------|--------------|-------|--------------------|
|C1  |79 KB |27 KB  |43 KB         |−36 KB |fragmented          |
|C2  |43 KB |20 KB  |**71 KB**     |**+28 KB**|recovered        |
|C3  |71 KB |25 KB  |51 KB         |−20 KB |fragmented          |

**C2 ends 28 KB above its starting value.** The sequence 79 → 43 → 71 → 51 KB
oscillates; it does not fall. Across five WS runs there is no downward trend —
the lasting fragmentation the script reported is not one.

Supporting values: `min_free_heap` stayed at 16 KB in C2 and C3 (no new low),
free heap after cooldown was a constant 93 KB in all three runs, with 1–2 errors
per ~5850 requests. And `largest_block_min` under pure WS churn is 20–27 KB,
i.e. considerably higher than under mixed load (7–10 KB) — churn alone does not
drive the heap down particularly far at all.

A side finding: between run B and C1 the largest block rose from 37 to 79 KB
while idle. The heap therefore coalesces back on its own if given time; the
script's 30 s cooldown is not always enough for that.

**Consequence for the measurement tool:** the verdict "largest block did not
recover" only compared the start and end of **one** run (`end < start × 0.9`).
On a value that wanders by tens of KB between runs, that is a coin toss — which
is exactly why the same load reported "fragmented" twice and "recovered" once in
three consecutive runs. `stress_test.py` now judges against the operationally
relevant floor instead (largest single allocation < 10 KB) and points out
explicitly that a single-run delta is not evidence.

## 7. Cleanup after verification

- ✅ Diagnostic instrumentation **gated rather than removed**: `WSDBG` stays
  behind `debug heap on`; the previously unconditional `WiFiRC:` prints now also
  run behind `heapDebugEnabled` (`debug heap on`). The `esp_task_wdt_reset()`
  calls in the reconnect stay unconditional (function, not debug). That keeps
  the #18 toolkit reactivatable for future analyses without production noise.
- ✅ `platformio.ini` pinned to fixed version tags.
- ✅ `README.md`: outdated instability notes (ESPAsyncWebServer crash under
  polling, "do not poll more often than once a minute") removed; replaced with a
  correct note about the stress-tested ESP32Async stack.

## 8. Affected files (planned)

- `platformio.ini` — `lib_deps` to ESP32Async, version pins, possibly
  `CONFIG_ASYNC_TCP_*` flags
- `src/web/webserver.cpp` / `.h` — API adaptations to the new stack,
  re-verification of the single-response behaviour
- `src/main.cpp` — non-blocking, watchdog-safe Wi-Fi reconnect
- `scripts/verify_tcp_stack.py` — **new**: functional verification of the
  migration-sensitive API points (risk mitigation, see 5.2)

## 9. References

- Crash backtraces: the #18 body (`_accept` AsyncTCP.cpp:1421, `_lwip_fin`
  AsyncTCP.cpp:920).
- AsyncTCP close/FIN fixes: ESP32Async/AsyncTCP releases v3.4.5–v3.4.10.
- `_accept` fix status: not confirmed in the release notes → prove it with the
  WS churn test.
- Related (the same crash family, unsolved in the esphome fork):
  esphome/issues#5676.
