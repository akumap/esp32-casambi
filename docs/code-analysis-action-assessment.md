# Assessment of the audit report "Code analysis: esp32-casambi"

**Subject:** Reviewing and assessing the nine actions (S-01 … S-09) from the
audit report of 13 July 2026.
**Revision audited:** `main` · `e0075639a1614c8d2476ebf48fef54b34ba60e4e` — which
was also the repository HEAD at the time. The report therefore analyses the
then-current code; none of the nine actions had been implemented at that point
(the earlier commit "implement all findings from the July 2026 code analysis"
refers to a **different**, preceding analysis).
**Assessment method:** For every action, the code locations cited in the report
were looked up in the current source and the claimed control and data flow was
verified.

## Overall verdict

The report is **technically sound and correct in all nine points.** Every line
reference matches the code as audited, the described failure paths exist as
presented, and the priorities are chosen appropriately. The protections listed
under "Positive starting position" are verifiable as well (CMAC self-test
`main.cpp:335`, reboot/refresh delegation `main.cpp:370/971`, streaming of large
logs `webserver.cpp:908`, build matrix `ci.yml:13-14`).

**One factual inaccuracy** was found (S-01, a detail about the key array, see
below). It changes neither the thrust nor the priority of the action.

| ID | Priority per report | Reproduced | Assessment |
|----|---------------------|------------|------------|
| S-01 | High | ✅ | Confirmed (one detail inaccuracy, see below) |
| S-02 | High | ✅ | Confirmed |
| S-03 | High | ✅ | Confirmed |
| S-04 | Medium | ✅ | Confirmed |
| S-05 | Medium | ✅ | Confirmed |
| S-06 | Low | ✅ | Confirmed |
| S-07 | Medium | ✅ | Confirmed |
| S-08 | Medium | ✅ | Confirmed |
| S-09 | Medium | ✅ | Confirmed |

---

## Individual assessments

### S-01 · Store the configuration atomically and validated — **High, confirmed**

- `hasValidConfig()` checks nothing but `LittleFS.exists(CONFIG_FILE_PATH)`
  (`config_store.cpp:24-27`). No syntax, mandatory-field or key check.
  Confirmed.
- `saveNetworkConfig()` and `saveWiFiCredentials()` open the live file directly
  in mode `"w"` (`config_store.cpp:124`, `:294`). An abort after the open
  truncates/destroys the only copy. Confirmed (not atomic, no backup).
- **Boot path:** If `hasValidConfig()` returns `true` but `loadNetworkConfig()`
  fails on a JSON parse error, **only** `"ERROR: Failed to load configuration"`
  is logged (`main.cpp:472-474`) and the branch falls through — the setup portal
  is opened exclusively in the `else` branch (`main.cpp:475-485`, file missing).
  The device therefore ends up unusable and without a portal. The report's
  assessment ("robust fallback … not unambiguously guaranteed") is, if anything,
  an understatement.
- **Correction of one detail:** The report states that when incomplete hex keys
  are loaded "the remainder of the target array may be left undefined." In fact
  the `CasambiKey()` constructor initialises the array via
  `memset(key, 0, AES_KEY_SIZE)` (`network_config.h:25-27`) — the remainder is
  **zero**, not uninitialised. The underlying problem (a too-short or non-hex
  key is silently accepted as a valid but wrong key) stands; only the wording
  "undefined" is too strong.

**Conclusion:** Justified and rightly ranked highest priority. The recommended
approach (temp file → validation → atomic rename → backup) is appropriate.

### S-02 · Bound the request size in the setup portal — **High, confirmed**

In the body handler of `POST /api/provision`, `buf->reserve(total)` is called at
`index == 0` (`setup_portal.cpp:263`), where `total` is the `Content-Length`
reported by the client — **without an upper bound and before any JSON check.**
The portal runs on the open SoftAP. The described heap-exhaustion/reset vector
is real. The per-request `_tempObject` buffer with `onDisconnect` cleanup is
described correctly. Confirmed.

### S-03 · Propagate the BLE send result up to the REST API — **High, confirmed**

- `_sendEncryptedPacket()` is `void` and aborts silently on mutex timeout
  (`casambi_client.cpp:713`), missing encryption (`:718`) and empty ciphertext
  (`:733`); the return value of `writeValue()` (`:740`) is **not** evaluated.
- `_sendOperation()` increments `_outPacketCount++` (`:688`) **after** the void
  call, i.e. regardless of success. `_buildOperation()` increments `_origin++`
  (`:701`) as early as packet assembly.
- All public setters (`setUnitLevel`, `setSceneLevel`, …) are `void`
  (`casambi_client.h:172-180`).
- Web handlers report success unconditionally after the call, e.g.
  `_client->setUnitLevel(unitId, 255); … _sendJsonSuccess(request);`
  (`webserver.cpp:1124-1127`). `_checkBle()` only inspects the state **before**
  sending.

Result: a switching command can report HTTP 200 even though no packet was
transmitted; packet and origin counters keep advancing on a send failure. All
confirmed. Rightly priority High, since external automation (FHEM) trusts the
bogus success response.

### S-04 · Preserve local settings across a cloud refresh — **Medium, confirmed**

In `runScheduledCloudRefresh()` the following are carried over from
`networkConfig` into `freshConfig`: `autoConnectEnabled`, `autoConnectAddress`
and the five debug flags (`main.cpp:1047-1053`) plus `casambiPassword` (`:1044`).
**Not** carried over are `gatewayName` and `ntpServer`
(`network_config.h:99-106`) — they fall back to the constructor defaults (`""`
and `NTP_SERVER_DEFAULT` respectively, `network_config.h:123-124`). A
user-defined NTP server and the gateway name are therefore lost after a refresh.
Confirmed. The recommendation to encapsulate the local fields in
`preserveLocalSettings()` is sensible and stops the mistake from recurring as
new fields are added.

### S-05 · Evaluate the HTTP status in the FHEM refresh callback — **Medium, confirmed**

The callback (`98_CasambiGW.pm:334-347`) distinguishes only `$err` (transport
error) from the success case. In the `else` branch,
`"refreshCasambi accepted by ESP: $data"` is logged unconditionally (`:345`) —
`$param->{code}` is never evaluated. A 401/403/409/500 with an error JSON body
would thus appear as "accepted". Confirmed.

### S-06 · Brightness conversion — **Low, confirmed**

`my $level = int(($value // 0) * 2.55);` (`98_CasambiGW.pm:857`). Because `2.55`
is represented as `2.5499999…` in IEEE-754 double, `100 * 2.55 ≈ 254.9999…` and
`int()` truncates to **254**. The `on` command, by contrast, sends `255`
(`:852`). The discrepancy is real. The proposed replacement formula
`int($value * 255 / 100 + 0.5)` produces the target values quoted in the report
(0→0, 1→3, 50→128, 100→255) correctly. Confirmed.

### S-07 · Delegate EventLog deletion out of the async_tcp task — **Medium, confirmed**

`_handleDeleteLog()` calls `EventLog::clear()` **directly** in the async web
callback (`webserver.cpp:919`). `clear()` takes the mutex with `portMAX_DELAY`
(unbounded, `event_log.cpp:274`) and performs three `LittleFS.remove()`
operations (`:276-278`). Blocking flash I/O in the TCP task, exactly as
described — and the reboot/refresh pattern for delegating to the loop task
already exists in the project (reusable). Confirmed.

### S-08 · Do not log the cloud session token in full — **Medium, confirmed**

`Serial.printf("API: Session created: %s\n", sessionToken.c_str());`
(`api_client.cpp:175`) prints the complete, still-valid session token to the
serial interface. Confirmed. Since serial logs are frequently shared, the
redaction recommendation is justified.

### S-09 · Extend CI beyond plain compilation — **Medium, confirmed**

`ci.yml` runs nothing but `pio run -e devkit-v4` and `pio run -e esp32-c3`
(`ci.yml:13-14, 33-34`). No unit or regression tests, no `perl -c` for the FHEM
modules, no Python syntax check of the scripts. The regressions listed in the
report (100 %→254, HTTP 200 despite a BLE error, counter drift, missing body
limit) would all have gone undetected in a build-only CI. Confirmed. The
cross-references to S-01/S-03 (test seams) are consistent.

---

## Recommended implementation order

The order proposed in the report is plausible and is confirmed:

1. **Phase 1 (outage / data-loss risks):** S-01, S-02, S-03 — highest priority,
   each with a hardware test.
2. **Phase 2 (consistency / task safety):** S-04, S-05, S-07.
3. **Phase 3 (security / accuracy):** S-06, S-08.
4. **Cross-cutting:** S-09 alongside, since S-01/S-03 require testable
   interfaces anyway.

The "quick wins" with very low effort and obvious correctness — **S-06**
(deterministic formula), **S-08** (a single log line) and **S-05** (isolated
FHEM fix) — can also be pulled forward, as they carry no hardware risk.

---

## Implementation

All nine actions were subsequently implemented.

| ID | Implementation | Key files |
|----|----------------|-----------|
| S-01 | Atomic save (temp → validation → backup swap), recovery from backup on load, semantic and hex-key validation, setup-portal fallback on a load failure | `storage/config_store.cpp`, `storage/config_validation.h`, `config.h`, `main.cpp` |
| S-02 | Body size limit (4 KB) + allocation check before `reserve`, 413/503 | `web/setup_portal.cpp` |
| S-03 | Send result as `bool` through the BLE and web layers; counter rollback on failure; 503 instead of 200 | `ble/casambi_client.{h,cpp}`, `web/webserver.{h,cpp}` |
| S-04 | `preserveLocalSettings()` including `gatewayName`/`ntpServer` | `cloud/network_config.h`, `main.cpp` |
| S-05 | `CasambiGW_ClassifyRefreshResponse()` evaluates `$param->{code}` | `FHEM/98_CasambiGW.pm` |
| S-06 | `CasambiGW_PercentToByte()` with rounded integer arithmetic + clamping | `FHEM/98_CasambiGW.pm` |
| S-07 | `DELETE /api/log` only sets a flag (202); `EventLog::clear()` runs in the loop task | `web/webserver.{h,cpp}`, `main.cpp` |
| S-08 | The session token is no longer logged (length only) | `cloud/api_client.cpp` |
| S-09 | Native unit tests (`test/`), Perl tests (`FHEM/t/`), CI extended with `pio test -e native`, `perl -c`, `py_compile` | `platformio.ini`, `.github/workflows/ci.yml`, `test/`, `FHEM/t/` |

**Verification (host-side, without target hardware):**
- `config_validation.h` compiled and checked against the real ArduinoJson —
  15/15 checks passed.
- `FHEM/t/CasambiGW_helpers.t` — 17/17 tests passed (including 100 % → 255 and
  the HTTP classification).
- `perl -c` on both FHEM modules and `py_compile` on the scripts, both clean.
- The firmware build (ESP32) and `pio test -e native` run in CI; locally the
  PlatformIO registry was blocked by the network policy, so the validation logic
  was verified standalone against ArduinoJson.

> Note on S-01: `hasValidConfig()` deliberately remains a cheap existence check
> (live or backup file), because it also runs in the asynchronous web handler
> (`POST /api/refreshCasambi`) — a full flash parse there would reintroduce
> exactly the problem S-07 addresses (flash I/O in the async_tcp task). The real
> parse and semantic check, along with recovery, happen in `loadNetworkConfig()`,
> which carries the actual boot decision (operation vs. setup).

> Note on the protocol version (S-01, addendum): `MAX_PROTOCOL_VERSION` was
> raised to **11** (current networks use v11). In addition, the protocol range
> check is now **soft**: a structurally valid config with a protocol version
> outside `[MIN, MAX]` is **accepted with a warning in the log** rather than
> rejected (`CFG_UNSUPPORTED_PROTOCOL` / `isCommittable()`), since the BLE
> handling is version-tolerant anyway. Only genuine corruption (missing
> mandatory fields, absent or too-short hex keys, a missing or zero protocol
> version) still blocks saving and loading.
