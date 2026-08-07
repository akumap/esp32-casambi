# Code analysis: stability, function, optimisations (firmware + FHEM)

As of: July 2026, `main` @ 12904d9 (after the PR #25 stability review and the
issue #11 hardening).
Scope: the entire firmware (`src/`, ~9,000 lines) and both FHEM modules
(`FHEM/98_CasambiGW.pm`, `FHEM/98_CasambiUnit.pm`, ~1,400 lines), plus
`platformio.ini`, the CI workflow and the concept documents.

## Implementation status

All findings were implemented on this branch (in a follow-up commit to this
document), with two deliberate deviations:

| Finding | Status |
|---------|--------|
| S1 keepalive vs. WDT | ✅ WDT 45 s **and** keepalive only after ≥60 s of radio silence (`BLE_KEEPALIVE_IDLE_MS`) — done together with O1 |
| S2 `_connectedAddress` race | ✅ writes/reads under `g_configMutex` |
| S3 WS queue | ✅ depth **64** (user decision instead of 32) + resync hello on a drop |
| S4 restart policy | ✅ restart only after 10 consecutive internal errors; peer absence → permanent backoff |
| S5 log snapshot vs. file switch | ✅ `EventLog::generation()` check in the chunk streaming; plus a msgLen clamp in `writeEntryJson` |
| S6 RTC magic | ✅ layout-versioned (`sizeof(LogEntry)`/capacity mixed in) |
| F1 deviceSuffix | ✅ last two MAC octets (main.cpp + setup_portal.cpp). **Note:** the SSID/mDNS suffix of existing devices changes once |
| F2 mDNS | ✅ idempotent, called in all Wi-Fi/web-server recovery paths |
| F3 `network` reading | ✅ `networkName` in the hello, reading in the FHEM module, README |
| F4 `set on` = 100 % | ✅ restores the last brightness (`LAST_BRIGHTNESS`), falls back to 100 % |
| F5 WS reject 404 | ✅ `GET /ws` without a token → 401 |
| F6 portal `/api/info` | ✅ only `{configured, build}` remains |
| F7 ID truncation | ✅ `parseIdSegment` validates 0–255, otherwise 400 |
| F8 0x07 echo | — documented state, deliberately unchanged |
| O1 keepalive traffic | ✅ see S1 |
| O2 body-parsing duplication | ✅ helpers (`_checkBle`, `_parseBody`, `_requireUint8`, `_*FromPath`); webserver.cpp ~290 lines smaller |
| O3 ESP32-C3 | ⏸️ **stays in the build + CI** (user decision: the board is untested only for lack of hardware); documented as build-only |
| O4 NimBLE init name | ✅ `DEVICE_NAME` |
| O5 oversize bodies | ✅ >4 KiB → the connection is closed, ≤4 KiB still gets a clean 413 |
| P1 handshake backoff | ✅ 30 s backoff, log downgrade after the 1st failure, 401 hint about the password |
| P2 poll chains | ✅ dedupe + stale guard in `InfoCb` |
| P3 SendCommand status codes | ✅ `$param->{code}` checked, log with a 401 hint |
| P4 `wsState` after EOF | ✅ reset in `Read` |
| P5 `UPDATING_STATUS` | ✅ `local` guard (unit + vertical) |
| P6 plaintext password | ✅ `set <gw> password` (key store); the attribute is retained as a legacy fallback |
| P7 negative cache | ✅ known-unknown IDs stored as `undef` in `UNIT_BY_ID` |
| P8a JSON guard | ✅ `require JSON` with an error message in Define; the unnecessary `use JSON` removed from 98_CasambiUnit.pm |
| P8b FIN bit | ✅ commented as a limitation |
| P8c save hint | ✅ log after structural changes + README |
| P8d `=pod` drift | ✅ CasambiVertical documentation corrected |

## Overall assessment

The code base is unusually mature for a project of this kind: the task
boundaries (loopTask / NimBLE host / async_tcp) are documented throughout and
secured with mutex discipline, critical paths (cloud refresh before the BLE
start, a broadcast queue instead of `textAll()` from the BLE task, body
buffering only for known endpoints) are deliberately constructed, and the
two-stage event log (RTC + LittleFS) makes crashes traceable. The coarse
stability risks (AsyncTCP churn, double-response leak, string races on
`NetworkConfig`) have been fixed by the preceding reviews.

The remaining findings are mostly small. The most worthwhile fixes:

| # | Area | Finding | Severity |
|---|------|---------|----------|
| F1 | Firmware | `deviceSuffix()` returns the OUI instead of device-specific MAC bytes → mDNS/SSID collision with several gateways | medium |
| F2 | Firmware | mDNS only starts in the boot path — never when Wi-Fi arrives later | medium |
| P1 | FHEM | No backoff brake on a failed WS handshake (missing/wrong `casambiPassword`) → an endless loop at one-second intervals | medium |
| S1 | Firmware | GATT read in the keepalive: the 30 s ATT timeout hits the 30 s task WDT exactly | medium (rare) |
| S3 | Firmware | The WS broadcast queue (depth 8) can lose state pushes for scenes with >8 units — FHEM then stays stale until the next event | small–medium |

---

## 1. Firmware — stability

### S1: The keepalive GATT read can hit the task watchdog (medium, rare)

`CasambiClient::sendKeepalive()` (`src/ble/casambi_client.cpp:266`) performs a
synchronous `_authChar->readValue()` on the loopTask every 30 s. NimBLE only
aborts an unanswered GATT procedure after the ATT transaction timeout of
**30 s** — exactly the value of the task WDT (`WDT_TIMEOUT_SECONDS 30`,
`src/config.h:144`). Precisely the scenario the keepalive exists for (link alive
at LL level, ATT not answering) can therefore block the loopTask right up to the
WDT limit. The same class of race was already recognised for `connect()` and
defused with `setConnectTimeout(10 s)` (`casambi_client.cpp:128-131`) — the read
path stayed open.

Recommendation (one of the options):
- Raise the WDT to 45–60 s (simplest, most robust variant), or
- send the keepalive only when `_lastNotificationTime` is older than, say, 60 s
  (which incidentally saves radio traffic, see O1), or
- use a health check without an ATT procedure instead of `readValue()`.

### S2: String race on `_connectedAddress` (low, theoretical)

`getConnectedAddress()` (`casambi_client.h:133`) copies the Arduino `String`
without a lock; it is written in `_connectLocked()` (`casambi_client.cpp:111`)
on the loopTask and read by, among others, `/api/status` and
`_buildHelloMessage()` on the async_tcp task. The preceding
`isAuthenticated()` check is a TOCTOU: between the check and the copy the
loopTask can start an RSSI re-roll and reassign the string. It is defused in
practice because reconnects almost always write the same (equally long) address
(no realloc) — but `connect <n>` over serial to a different address genuinely
opens the window. The identical problem class was solved for `NetworkConfig`
strings with `g_configMutex`/`lockedCopy()` (`webserver.cpp:20-25`); the same
treatment for `_connectedAddress` (or a locked getter counterpart) would be
consistent and cheap.

### S3: WS broadcast queue depth 8 — loss without self-healing (small–medium)

`WS_BROADCAST_QUEUE_DEPTH 8` (`config.h:184`): a single 0x06 packet from a
group/scene switch contains one record **per affected unit** (up to ~40 records
at an MTU of ~247 B). `_applyUnitStates` fires one callback per record →
`broadcastUnitState` enqueues in the NimBLE task while the loopTask may be
sitting in `delay(10)` or a LittleFS flush. From the 9th entry on, records are
dropped (`webserver.cpp:365-368`). The code comment assumes "the next BLE
broadcast carries the state along" — but for the *dropped* unit no further
broadcast arrives until its state changes again or FHEM reconnects (hello). A
missed `off` for a luminaire can therefore stay stuck as `on` in FHEM
indefinitely.

Recommendation: raise the depth to 32 (cost: 24 × 4 B of queue slots —
negligible; the `String` payloads only exist briefly anyway) and/or set a
"resync needed" marker on a drop that triggers a fresh hello/full-status
broadcast in the next `loop()`.

### S4: Restart policy with a permanently absent BLE peer (small)

After `MAX_RECONNECT_FAILURES 10` the ESP restarts (`main.cpp:545-551`). If the
lights are simply unpowered (wall switch off overnight), this produces an
endless cycle: ~8–10 min of backoff attempts → reboot → from the top. The
restart only helps against a wedged BLE stack, not against an absent peer, and
it generates flash writes per cycle (NVS boot counter, log entries).
Recommendation: exclude connection timeouts (`BLELinkLoss` from `connect()`)
from the restart counter and stay in the 60 s backoff permanently; restart only
on internal errors (auth/stack errors indicating a wedged state).

### S5: `/api/log` streaming vs. the ping-pong file switch (edge case)

`snapshotNewest()` freezes the file sizes/indices (`webserver.cpp:872`,
`event_log.cpp:409`), but the chunks are streamed over several ticks. If the
active log file switches in the meantime (file full → the other one cleared),
the global indices shift: the response can be one-off incomplete or duplicated.
No crash, no leak — document it as a known edge case or check a generation
number in the snapshot.

### S6: RTC log layout without versioning (edge case)

`rtcLogMagic` (`event_log.cpp:17-23`) stays valid across a firmware update. If
an update changes the `LogEntry` layout (`LOG_MSG_MAX`, field order), garbage
entries are flushed to LittleFS once on the first boot. Cheap fix: mix a layout
version (e.g. `sizeof(LogEntry)`) into the magic.

### Positive (stability)

- The lock hierarchy `_mutex` → `_encMutex` is documented and adhered to;
  `_sendOperation` correctly uses the `*Locked` variants (no deadlock).
- `_sendEncryptedPacket` releases `_encMutex` **before** `writeValue()`, because
  the response notification can fire synchronously — subtle and correctly
  solved.
- Cloud refresh as a reboot marker (`REFRESH_FLAG_PATH`) instead of a runtime
  teardown eliminates the use-after-free class from issue #21 entirely; the
  marker is deleted before the download → no boot loop on failure.
- Heap restart with debouncing (3 consecutive lows), non-blocking Wi-Fi
  reconnect, WDT feeds in all wait loops.
- Event log: flash writes only from the owner task, foreign tasks park in the
  RTC ring — clean decoupling.

---

## 2. Firmware — function

### F1: `deviceSuffix()` is not device-specific (medium)

`main.cpp:98-103` and `setup_portal.cpp:26-31`:

```cpp
uint64_t mac = ESP.getEfuseMac();
sprintf(buf, "%04x", (unsigned)(mac & 0xFFFF));
```

`ESP.getEfuseMac()` puts `mac[0]` (the first octet = OUI/manufacturer prefix)
into the least significant byte. `mac & 0xFFFF` therefore yields the **first**
two MAC octets — identical for boards from the same batch. Consequences:

- Two gateways produce the same setup SSID `Casambi-Setup-XXXX` and the same
  mDNS name `casambi-XXXX.local` — exactly the multi-gateway scenario the README
  and the FHEM module are supposed to support ends up colliding.
- The README statement "last 4 hex digits of the chip MAC" is wrong.

Fix: use the last two octets, e.g.

```cpp
uint8_t m[6]; esp_efuse_mac_get_default(m);
sprintf(buf, "%02x%02x", m[4], m[5]);
```

(Adapt both copies of `deviceSuffix()`; the suffix change breaks existing
`casambi-XXXX.local` definitions in FHEM — mention it in the changelog.)

### F2: mDNS only starts in the boot path (medium)

`startMDNS()` is called exclusively in `setup()` (`main.cpp:364`), and only when
Wi-Fi is connected **at boot**. If Wi-Fi only arrives later (the router boots
more slowly than the ESP after a power cut — a realistic 24/7 scenario),
`checkAndReconnectWiFi()` does create the web server (`main.cpp:576-583`), but
mDNS is never started → `casambi-XXXX.local` stays permanently undiscoverable.
The same goes for the web-server restart in the BLE reconnect path
(`main.cpp:527-533`) and after `wifi set` (`main.cpp:1273-1280`). Fix: make
`startMDNS()` idempotent (guard flag) and call it in the recovered transition of
`checkAndReconnectWiFi()`.

### F3: The FHEM reading `network` is dead (small)

Since the security hardening, `/api/info` only returns `{configured, build}`
(`webserver.cpp:425-431`), and the hello message contains no network name
(`_buildHelloMessage`, `webserver.cpp:299-337`). The FHEM module still evaluates
`$info->{network}` though (`98_CasambiGW.pm:195`) and the README documents the
reading `network` — which is never set any more. Recommendation: include
`networkName` in the (authenticated) hello message and set it in the `hello`
handler; deliberately keep the unauthenticated `/api/info` lean.

### F4: `set <unit> on` forces 100 % (small, UX)

`/api/units/:id/on` sends `SetLevel 255` (`webserver.cpp:1097`), and FHEM's `on`
does the same (`98_CasambiGW.pm:755-757`). After dimming to 20 %, a HomeKit
"on" therefore jumps to 100 %, whereas the Casambi app restores the last level.
`CasambiVertical` already does it better (restore the last `pct` value,
`98_CasambiUnit.pm:373-383`) — the same logic in `CasambiUnit`'s `on` (send the
last `brightness` reading, fall back to 100) would be more consistent.

### F5: WS auth rejection returns 404 instead of 401 (small, diagnostics)

An upgrade with a wrong/missing token falls through the filter
(`webserver.cpp:76-79`) into `onNotFound` → `404 Endpoint not found`
(`webserver.cpp:498-551`). The FHEM log then only says "handshake failed:
HTTP/1.1 404" — the actual reason (auth) is not discernible. An explicit 401 for
`GET /ws` without a valid token would shorten troubleshooting considerably (see
also P1).

### F6: The portal `/api/info` still discloses hostname/mac/ip (small)

The operating-mode variant was reduced to `{configured, build}`, while the
portal variant (`setup_portal.cpp:189-198`) still returns `hostname`, `mac` and
`ip` on the open AP. Low sensitivity, but inconsistent with the hardening
concept — align it.

### F7: ID parsing of the REST routes without a range check (cosmetic)

`path.substring(...).toInt()` is truncated to `uint8_t` (e.g.
`webserver.cpp:979`): `/api/units/300/on` addresses unit 44 (300 mod 256). This
usually ends in a 404 (`getUnitById`), but it can collide with real IDs. Reject
values > 255 explicitly with a 404.

### F8: The 0x07 echo only updates SetLevel/unit (documented state)

`casambi_client.cpp:920-946`: temperature/vertical echoes from other controllers
do not change the local state; in practice a 0x06 broadcast follows. No action
needed, recorded only as a behavioural limitation.

---

## 3. Firmware — optimisations

- **O1 — keepalive radio traffic:** the 30 s GATT read also runs while
  notifications are arriving continuously. `_lastNotificationTime` already
  exists — triggering the read only after > 60 s of radio silence saves radio
  traffic and mutex contention and defuses S1 along the way.
- **O2 — ~400 lines of duplicated body parsing:** the 10 body-POST handlers in
  `webserver.cpp` identically repeat "check _tempObject → parse JSON → free →
  validate field". A helper (`bool parseBody(request, doc)` +
  `bool requireUint8(doc, key, out)`) reduces the file by ~300 lines and
  prevents the copies from diverging.
- **O3 — ESP32-C3 half-removed:** the README has not listed the board since
  1137bbf, while `platformio.ini:64-85` and the CI matrix (`ci.yml:14`) still
  build it. Either document it as "untested, build-only" or remove the
  environment + CI entry (saving CI time).
- **O4 — `NimBLEDevice::init("ESP32-Casambi")`** vs. the `DEVICE_NAME` constant
  ("ESP32 Casambi") — a duplicate; use one source.
- **O5 — abort oversized bodies early:** `onNotFound` only rejects > 512 B after
  full reception; with absurd content lengths the handler could close the
  connection immediately. Only relevant against deliberately malicious clients
  on the LAN — low priority.

---

## 4. FHEM modules

### P1: No backoff brake after a handshake failure (medium)

The sequence with a missing/wrong `casambiPassword` (or a generally rejected
upgrade): handshake fails → `CasambiGW_StartInfoPoll`
(`98_CasambiGW.pm:356-363`) → poll after **+1 s** (`:147`) → `/api/info` reports
`configured:true` → immediately a new WS attempt → failure → from the top.
Result: an endless loop at a ~1–2 s cadence, unbounded, with a level-2 log entry
per round ("WebSocket handshake failed: HTTP/1.1 404 …") and corresponding load
on the ESP. Recommendation:

- After a handshake failure, keep polling with `INFO_POLL_OFFLINE` (30 s)
  instead of 1 s (pass a failure flag into `StartInfoPoll`).
- In combination with F5 (401 instead of 404), detect the auth case and log it
  once in plain words: "check the casambiPassword attribute".

### P2: Poll timer chains can double up (small)

`StartInfoPoll` does remove pending timers, but an `/api/info` request that is
already in flight (HttpUtils, not cancellable) schedules a **second** chain in
its callback (`:178`, `:187`, `:208`). FHEM does not deduplicate
`InternalTimer` — after several `set reconnect` calls during running polls the
query runs several times in parallel (harmless, but unnecessary traffic/log).
Fix: `RemoveInternalTimer($hash, "CasambiGW_Poll")` at the start of
`CasambiGW_InfoCb`, or carry a poll generation in the hash.

### P3: `CasambiGW_SendCommand` ignores HTTP status codes (small)

The callback (`:782-786`) only logs transport errors (`$err`). If the ESP
answers with `503 Not connected to BLE gateway` or `401 Unauthorized`, the
command vanishes without a trace — the user sees an optimistically set
`state on` in FHEM while the lamp stays off. Fix: check `$param->{code} != 200`
and log it with the device name/command (level 3); on a 401, point at
`casambiPassword`.

### P4: `wsState` stays "connected" after a read EOF (cosmetic)

If `DevIo_SimpleRead` returns undef (`:339-340`), DevIo internally calls
`DevIo_Disconnected`, but `$hash->{wsState}` stays "connected" until the next
handshake overwrites it. The ping timer meanwhile fires into the void
(self-healing via the pong timeout, but confusing while debugging). Set
`wsState = "disconnected"` on the undef return in `CasambiGW_Read`.

### P5: `UPDATING_STATUS` without exception protection (edge case)

`CasambiUnit_UpdateFromState` sets the flag and clears it at the end
(`98_CasambiUnit.pm:167/195`). If something dies in between (notify handlers in
the event chain of `readingsEndUpdate` can run arbitrary user code), the flag
stays 1 and the device **permanently ignores all set commands** (SetFn:
`return undef if $hash->{UPDATING_STATUS}`). Robust:
`local $hash->{UPDATING_STATUS} = 1;` (automatic reset even on a die) — same
spot in `CasambiVertical_UpdateFromParent`.

### P6: `casambiPassword` as a plaintext attribute (hardening note)

The password appears in `fhem.cfg`/`list` output. The FHEM-idiomatic route for
credentials is the `setKeyValue`/`getKeyValue` store (uniqueID-encrypted) with a
`set <gw> password <pw>` command. Since the derived token travels the LAN in
plaintext anyway this is no acute risk — but it would be consistent with the
firmware hardening.

### P7: Unknown `unit_state` IDs cause repeated full scans (perf, small)

If pending units were discarded via `discardChanges`, every further
`unit_state` push for those units runs the rebuild path (`:687-700`) including
`sort keys %defs` over **all** FHEM devices — unnecessary continuous load in
large installations with chatty units. Fix: cache negative hits in the
`UNIT_BY_ID` hash as `undef` (until the next hello).

### P8: Minor points

- `use JSON;` directly (`:2`) — the FHEM convention would be an eval guard, or
  using `JSON::XS` with a fallback without `json2nameValue`; on systems without
  the JSON module, loading the module otherwise fails.
- Fragmented WS frames (FIN bit) are not reassembled (`:375-415`) — the ESP side
  practically never fragments; record it as a known limitation in a comment.
- After `applyChanges`, new devices are only persistent after a `save` — a log
  hint (or optional autosave) saves puzzlement after FHEM restarts.
- The `=pod` documentation of `CasambiVertical` mentions
  `genericDeviceType dimmer` and an old `homebridgeMapping` format — the code
  sets `light` and the `cmdOn/cmdOff` format (`:275-280`). Align the
  documentation.

### Positive (FHEM)

- MAC regex validation before `fhem("define …")` prevents command injection via
  manipulated hello data (`98_CasambiGW.pm:725`).
- The pending-changes concept (explicit `applyChanges`) protects against
  autocreate/delete accidents caused by transient network states.
- MAC-based identity + FUUID preservation keeps HomeKit assignments stable
  across Casambi reconfigurations.
- The 300 ms debounce of slider commands, the feedback-loop guard and the
  `/api/info` gate before the WS connect — well-considered integration details.

---

## 5. Prioritised recommendations

1. **F1** switch `deviceSuffix()` to the last MAC octets (2 places).
2. **F2** make `startMDNS()` idempotent and call it in the Wi-Fi recovery path.
3. **P1 + F5** handshake failure: 401 for `/ws` in the firmware, a 30 s backoff
   and a one-off auth hint in FHEM.
4. **S3** `WS_BROADCAST_QUEUE_DEPTH` to 32; optionally a resync marker on a
   drop.
5. **S1/O1** send the keepalive only during radio silence (or WDT > 30 s).
6. **P3** log HTTP status codes in `CasambiGW_SendCommand`.
7. **S4** restrict the restart counter to internal errors.
8. **P5** `local` guard for `UPDATING_STATUS`.
9. **F3** reactivate `networkName` in the hello + the FHEM reading (align with
   the README).
10. The rest (S2, S5, S6, F4, F6, F7, O2–O5, P2, P4, P6–P8) as opportunity
    allows.
