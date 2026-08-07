# Concept: Security hardening (issue #11)

Status: **largely implemented** (web auth, reboot fix, crypto hardening,
logging/validation and cloud TLS are in the code; see "Implementation status"
below).
Branch: `claude/issue-11-concept-scgdix`
Reference: issue #11 "Harden device from security point of view", including the
security review (14 findings) in the issue comment.

## Implementation status

This document still describes the *planning*; the actual state of the code by
now is:

| Finding(s) | Status | Location |
|---|---|---|
| #1 web auth + CORS wildcard | ✅ implemented | `webserver.cpp` `_authOk`/`_deriveApiToken`/`_constantTimeEquals`; no CORS header any more |
| #9 WebSocket auth | ✅ implemented | `webserver.cpp` `_wsAuthOk` (header `X-API-Key` or `?k=`), `setFilter` |
| #2 reboot (auth + no `delay()`) | ✅ implemented | `webserver.cpp` `/api/reboot` behind `_authOk`, flag in `loop()` |
| #12 `/api/info` leak | ✅ implemented | only `{configured, build}` remains |
| #6 constant-time CMAC | ✅ implemented | `encryption.cpp` OR accumulator |
| #13 secret zeroization | ✅ implemented | `key_exchange.cpp:218` |
| #14 RFC 4493 test vectors | ✅ implemented | `encryption.cpp` `selfTestRFC4493`, called from `setup()` |
| #7 `X-Forwarded-For` | ✅ implemented | `webserver.cpp` `_getClientIP` uses the peer IP only |
| #8 BLE MAC injection (FHEM) | ✅ implemented | `98_CasambiGW.pm` MAC regex validation |
| #11 Wi-Fi password in the serial echo | ✅ implemented | `main.cpp` `handleCommand` masks `wifi set` |
| #3 `_tempObject` leak | ✅ done | `onDisconnect` handler in `onRequestBody` |
| **Cloud API TLS validation** | ✅ implemented | `api_client.cpp` `_beginRequest` → `setCACertBundle` (not covered by the original review) |
| #4 data at rest | 🟡 documented | flash encryption as a recommendation (README), not enforced |
| #10 nonce reuse on reconnect | 🟠 open | needs hardware verification |
| #5 XOR-fold transport key | ⏸️ deliberately left alone | protocol fidelity, commented only |

The sections that follow are the original planning basis and are retained for
traceability.

## 1. Goal and protection objectives

The firmware was originally designed for **DMZ/hobbyist operation**: all
interfaces open, no authentication, sensitive data in plaintext in flash. Issue
#11 asks for the device to be hardened for **normal home-network operation**.

The **central building block** of this concept is **authentication of the web
interface (REST + WebSocket)**. The natural secret is the **Casambi
cloud/network password**, which sits on the ESP32 after provisioning anyway
(`NetworkConfig::casambiPassword`, persisted in `casambi_config.json`,
`config_store.cpp:54/188`). That way **no new, separately managed password** is
needed.

Protection objectives (realistic for a LAN device without TLS):

| Objective | In scope | Rationale |
|---|---|---|
| **Access control** — no control/readout by arbitrary LAN devices and browser JS | ✅ core | main attack surface after "DMZ → home network" |
| **Sensitive data at rest** (Wi-Fi password, AES keys, Casambi password) | ✅ | flash readout with physical access |
| **Robustness** (no unauthenticated reboot/DoS, no leaks/crashes) | ✅ | already marked High in the review |
| **Protection against a passive L2 sniffer** (reading the plaintext LAN) | ⚠️ only partially | real defence needs TLS — hard on the ESP32; see 3.4 |
| **Confidentiality against LAN-capable malware with root** | ❌ | outside the realistic protection scope |

## 2. Starting point / findings (grouped)

The security review lists 14 findings. Grouped by topic and cross-checked
against the state of the code:

### 2.1 Missing web authentication (core)
- **#1 [Critical]** No auth on the REST API; the CORS wildcard
  `Access-Control-Allow-Origin: *` (`webserver.cpp:271`) additionally grants
  arbitrary browser JS access.
- **#9 [Medium]** WebSocket `/ws` unauthenticated — sends the full unit snapshot
  on connect (`_buildHelloMessage`, `webserver.cpp:124`).
- **#2 [High]** `POST /api/reboot` unauthenticated **and** with a blocking
  `delay(1000)` in the async callback (`webserver.cpp:319-323`).
- **#12 [Low]** `/api/status` discloses the network topology (SSID, IP, RSSI,
  gateway MAC, heap) without auth (`webserver.cpp:476`).

→ Addressed in **section 3**.

### 2.2 Data at rest
- **#4 [High]** The Wi-Fi password (`wifi_config.json`) and the AES
  keys/Casambi password (`casambi_config.json`) sit in LittleFS as **plaintext
  JSON** (`config_store.cpp`).

→ Addressed in **section 4**.

### 2.3 BLE/crypto implementation
- **#5 [High]** Transport key derived by **XOR fold** from SHA-256
  (`key_exchange.cpp:212`). ⚠️ **Check whether the protocol requires it**: the
  derivation reproduces the reverse-engineered Casambi protocol (comment "Python
  code reverses the bytes", `key_exchange.cpp:196`). Changing it would break
  interoperability. → probably **not a real finding** but protocol fidelity;
  verify against real hardware before any change.
- **#6 [High]** CMAC comparison is not constant-time — early-exit loop
  (`encryption.cpp:76-81`). A genuine finding, cheap to fix.
- **#10 [Medium]** AES-CTR counter reset on reconnect → nonce-reuse risk with an
  identical device nonce.
- **#13 [Low]** The ECDH shared secret + SHA hash are not zeroed from the stack
  (`key_exchange.cpp:192-208`).
- **#14 [Low]** `_leftShift`/CMAC not validated against the RFC 4493 test
  vectors.

→ Addressed in **section 5**.

### 2.4 Logging / input validation
- **#7 [Medium]** `X-Forwarded-For` is blindly trusted in `_getClientIP()`
  (`webserver.cpp:1409-1414`) → IP spoofing in the logs.
- **#8 [Medium]** The BLE MAC is unsanitised in the `fhem()` define
  (`98_CasambiGW.pm:690`) → possible command injection.
- **#11 [Medium]** The Wi-Fi password is logged to serial via a bare command
  echo (`main.cpp`).

→ Addressed in **section 6**.

### 2.5 Already fixed
- **#3 [High]** `_tempObject` leak on an aborted POST: in the current code
  `onRequestBody` registers an `onDisconnect` handler that frees `_tempObject`
  (`webserver.cpp:432-437`). **Counts as done** — carry it only as a regression
  checkpoint.

## 3. Core measure: web authentication with the Casambi password

### 3.1 Secret source and scope
- Secret = `_config->casambiPassword`. Auth is **only enforced when a password
  is set** (`casambiPassword.length() > 0`). For legacy configurations without a
  stored password (the field defaults to `""`, `config_store.cpp:188`) the
  device stays open until a `refreshCasambi`/re-provisioning stores the password
  once — so the update breaks **no** existing installation.
- The **setup portal** (first-time commissioning, no password present yet)
  necessarily stays **open**; hardening there comes from a short
  lifetime/limited range (open SoftAP, see `concept-provisioning.md`), not from
  a password.

### 3.2 Derived token instead of the plaintext password
The **raw Casambi cloud password does not belong on the wire** and not in
plaintext in the FHEM configuration — otherwise an eavesdropper could use it to
authenticate against the **Casambi cloud** as well. Instead, a **derived API
token**:

```
apiToken = hex( SHA-256( "casambi-api:" || casambiPassword ) )
```

- The ESP computes the token once when loading the config (mbedTLS SHA-256 is
  already linked in, cf. `key_exchange.cpp`).
- FHEM computes the same token from the password stored in the define/attribute
  (Perl `Digest::SHA`). That way the actual cloud password is never on the wire
  and cannot be recovered directly from the plaintext token in FHEM.
- The comparison on the ESP is **constant-time** (same pattern as #6,
  section 5).

### 3.3 Transport of the auth
- **REST:** check the header `X-API-Key: <apiToken>` before every protected
  handler. Implement it centrally through a small helper method
  `_authOk(request)` called at the start of each handler (or via a filter),
  rather than duplicating it 20 times.
- **WebSocket:** pass the token in the handshake. Since FHEM builds the
  handshake itself (`98_CasambiGW.pm:282-288`), two viable routes:
  1. an additional header `X-API-Key: <token>` in the `GET /ws` request, checked
     in the `WS_EVT_CONNECT`/handshake path, otherwise drop the connection,
     **or**
  2. the token as a query parameter `GET /ws?k=<token>`.
  Variant 1 is cleaner (no token in logs/URLs); both are a one-liner addition on
  the FHEM side.
- **CORS:** remove the wildcard. Either set no `Access-Control-Allow-Origin` at
  all (FHEM/HttpUtils does not need CORS — that is only relevant for browsers)
  or restrict it to a configurable origin. That closes the browser-JS access
  path from #1.

### 3.4 Limits (stated honestly)
Without TLS the token is a **static secret on a plaintext LAN** and can
therefore be read and replayed by a **passive sniffer in the same L2 segment**
(exactly like the control commands themselves). This is a deliberate trade-off:

- The token **stops** unauthenticated LAN devices and browser JS (the stated
  main attack surface "DMZ → home network").
- It **does not protect** against an attacker who can already read LAN traffic —
  only TLS would help there.
- **Stronger alternative (optional):** HTTP **digest auth**
  (challenge-response, password/token never in plaintext, no simple replay).
  ESPAsyncWebServer supports digest in principle. Downside: FHEM uses a
  **hand-built** handshake (DevIo) for the WebSocket and
  `HttpUtils_NonblockingGet` for REST — digest would have to be retrofitted
  there (fetch challenge, compute response). Recommendation: **start with the
  X-API-Key token** (small, FHEM-compatible, covers the main threat); evaluate
  digest/TLS as a later, separate stage.

### 3.5 Consequences for FHEM (`98_CasambiGW.pm`)
- A new attribute/define parameter for the Casambi password (or the precomputed
  token directly), e.g. `attr <gw> casambiPassword <pw>`.
- Derive the token from the password once (`Digest::SHA::sha256_hex`).
- Add the token header at **three** call sites:
  - `CasambiGW_SendCommand` (`:733` `header => ...`),
  - the `refreshCasambi` POST (`:245`),
  - the WebSocket handshake (`:282`).
- Possibly leave `/api/info` **auth-free** (discovery: FHEM uses it to
  distinguish "configured" from "setup", `:126`), but reduce its information
  content (e.g. only `{configured, build}` without MAC/IP/network name). That
  partially addresses #12 for the discovery path too.

### 3.6 Reboot endpoint (#2)
Independently of the auth, two corrections to the `/api/reboot` handler
(`webserver.cpp:319`):
1. Put it behind the auth from 3.3 (no more unauthenticated DoS).
2. Remove `delay(1000)` from the async callback: set a `_rebootRequested` flag
   instead and perform the restart in `loop()` — exactly the pattern
   `refreshCasambi` already uses via `consumeRefreshRequest()`
   (`webserver.cpp:329-340`, `:93`).

## 4. Data at rest (#4)

The Wi-Fi password, AES keys and Casambi password sit in plaintext in LittleFS.
Options:

| Option | Effort | Protection | Note |
|---|---|---|---|
| **A. ESP32 flash encryption** (`CONFIG_FLASH_ENCRYPTION_ENABLED`) | medium | high (entire flash) | transparent to the code; one-off eFuse activation, **irreversible**; mind the partition/boot aspects |
| **B. NVS with encryption** instead of LittleFS JSON for the secrets | higher | high | code rework of `config_store` for the sensitive fields |
| **C. Status quo + documentation** | low | none | just document honestly that physical access = compromise |

**Recommendation:** **document** option **A** as the *recommended production
configuration* (README + a note in `platformio.ini`) rather than enforcing it —
flash encryption is irreversible and unwanted in many hobbyist setups. Whoever
needs the security enables it; the default stays unchanged so existing flashing
workflows do not break. Option B only if selective encryption without global
flash encryption is desired.

## 5. BLE/crypto hardening

| Finding | Measure | Effort | Risk |
|---|---|---|---|
| **#6** constant-time CMAC comparison | OR accumulator instead of early exit (`diff \|= a[i]^b[i]`) in `decryptAndVerify` (`encryption.cpp:76`) | trivial | none |
| **#13** secret zeroization | `memset(secret_bytes,0,…)` + `memset(hash,0,…)` before `return` in `deriveTransportKey` (`key_exchange.cpp`) | trivial | none |
| **#14** RFC 4493 test vectors | self-test for the CMAC subkeys/MAC against RFC 4493 app. D (serial diagnostic or build check) | low | none (verification only) |
| **#10** nonce reuse on reconnect | before `Authenticated`, check that the device nonce differs from the previous session (`casambi_client.cpp`) | low–medium | test on hardware |
| **#5** XOR-fold transport key | **First establish whether the protocol requires it** (very probably yes). If so: **do not change**, only comment. Only if not: HKDF/first 16 bytes. | n/a | **high if changed** (interop) |

**#6, #13, #14** are "cheap" genuine improvements and should come first.
**#5** is very probably a **false positive** (protocol fidelity) and must not be
touched without hardware verification.

## 6. Logging / input validation

- **#7** `_getClientIP()` (`webserver.cpp:1409`): accept `X-Forwarded-For` only
  if the peer IP matches a configured proxy IP; otherwise use
  `request->client()->remoteIP()`. For the typical setup (no proxy) the simplest
  route is to **ignore the header entirely**.
- **#8** `98_CasambiGW.pm:690`: validate `$mac` against
  `/^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/i` before the `fhem("define …")`; reject the
  define and log if it does not match.
- **#11** `main.cpp` serial command echo: detect sensitive commands before
  logging and mask the arguments, e.g. suppress/truncate the echo when the
  command starts with `wifi set` (or other password-carrying commands).

## 7. Prioritisation / implementation order

Staged, biggest benefit first, each stage individually testable:

1. **Web auth foundation** (#1/#9/#12, the core):
   token derivation + `_authOk()` + `X-API-Key` on all REST handlers + WS
   handshake check; remove the CORS wildcard. Adapt the FHEM module in parallel.
2. **Reboot fix** (#2): auth + flag-in-`loop()` instead of `delay()`.
3. **Cheap crypto hardening** (#6, #13, #14): constant-time, zeroization, test
   vectors.
4. **Logging/validation** (#7, #8, #11).
5. **Data at rest** (#4): **document** flash encryption (option A).
6. **Nonce reuse** (#10): verify on hardware.
7. **#5** only after hardware verification of the protocol requirement
   (otherwise leave it and comment).

## 8. Effort and risk assessment

| Area | Effort | Risk | Remark |
|---|---|---|---|
| Web auth (ESP + FHEM) | medium | low | standard header check; well isolated for testing (curl with/without token) |
| Reboot flag | low | low | proven `consumeRefreshRequest` pattern |
| CMAC/zeroization/test vectors | low | very low | local crypto, checkable against vectors |
| Logging/validation | low | very low | purely defensive |
| Flash-encryption docs | low | low | docs only; no functional code change |
| Nonce reuse | medium | medium | only verifiable against the real Casambi network |
| XOR fold (#5) | — | **high** | do not change without proof of interop |

**Compatibility:** Auth is only enforced when a Casambi password is present →
existing devices without a stored password keep working; the FHEM update has to
set the token attribute, otherwise the ESP answers with `401` after hardening.
This migration is to be described in the README + the FHEM documentation.

## 9. Open points / to be verified

1. **#5 protocol requirement:** Is the XOR fold mandatory for Casambi auth?
   (Very probably yes — confirm on real hardware before any change.)
2. **ESPAsyncWebServer auth API:** check availability/behaviour of the header
   check in the WS handshake and of `request->authenticate()` (for the digest
   option) on the library version in use.
3. **FHEM digest capability:** only relevant if digest is chosen instead of
   X-API-Key (HttpUtils + hand-built WS handshake).
4. **`/api/info` discovery vs. leak:** the minimal auth-free scope that still
   suffices for FHEM without disclosing the topology.
5. **Flash-encryption boot implications** (partition `huge_app.csv`,
   `platformio.ini`) to be tested on the target hardware.
