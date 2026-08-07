# Raw captures — 0x06 status-broadcast record framing investigation

Context: `docs/casambi-protocol-reference.md` §D.5.1 flags an unresolved ambiguity between the
current capability-heuristic parser (`src/ble/packet_parse.h`) and the `state_len`/`priority`
framing documented by `casambi-bt`'s `PROTOCOL_PARSING.md`. These are the raw serial captures
taken to verify which interpretation the real network actually produces, before touching the
parser. Captured via the ESP32's own `debug parse on` / `debug ble on` serial commands (see
CLAUDE.md "Serial monitor" section) against the live network at `192.168.178.111`.

Kept verbatim (not just the extracted hex) so any later question about this investigation —
including ones the current analysis didn't think to ask — can be answered by re-reading the
original data instead of re-capturing it.

## 01 — Manual per-unit sweep (all 9 units, full BLE control)

- **Files:** `01_manual-sweep-9units_serial.log` (raw serial output, `debug parse on` +
  `debug ble on`), `01_units_before.json` / `01_units_after.json` (full `GET /api/units`
  snapshot immediately before and after — confirms every unit was restored byte-exact via
  `POST /api/units/:id/state` afterward).
- **Method:** every unit (ids 1,2,3,5,7,8,9,10,11 — 5 distinct fixture types: 14097, 1422,
  26295, 19425, 23795) was driven through a value sweep (dimmer 0/1/mid/254/255; vertical and
  temperature likewise where the fixture has those controls) via the documented
  `POST /api/units/:id/state` REST endpoint, then restored to the exact pre-sweep value.
- **Yield:** 39 raw `PARSE 0x06 raw` packets (grep for that string). 0 malformed/partial under
  the current parser.
- **Headline finding:** `b8` (byte 2) lower nibble was `0x03` in all 39 packets; `flags` only
  took values `{0x00, 0x03, 0x07, 0x10, 0x13, 0x17}`. Both the current capability-heuristic
  framing and the documented `state_len`/`priority` framing compute the **identical record
  length** for every single packet — the record-framing fix is safe. Also: `flags` bit 0
  ("on") and bit 1 ("online") were identical in every packet, including several where the
  dimmer was explicitly set to exactly 0 — bit 0 tracked reachability, not brightness.

## 02 — Offline / power-cycle sweep (mains power, not BLE)

- **Files:** `02_offline-powercycle_serial.log`, `02_units_before.json` / `02_units_after.json`
  (snapshots bracketing the capture; units 5/7/8/9 read `online: false` in the "after" snapshot
  because they were still mains-powered-off at that instant — expected, not a bug).
- **Method:** all units except IFC (11) and AZ-Stehleuchte (1) — kept powered to preserve the
  ESP32's BLE gateway connection — were mains power-cycled (on briefly, then off again) by the
  user, independent of any BLE command. This is the one scenario no prior capture had: every
  earlier golden vector (including the historical ones in
  `test/test_packet_parse/test_main.cpp`) came from permanently mains-powered units.
- **Yield:** 15 raw packets, including the first organically-captured **multi-record packet**
  from live traffic (`05 13 13 0c 0c ff 08 13 13 0a 0a ff` — units 5 and 8 changed together)
  and, crucially, the first `flags` values outside `{0x00,0x03,0x07,0x10,0x13,0x17}`:
  **`0x04`** and **`0x14`**.

### Combined result (56 records total, both captures, multi-record packets split)

- **Record length: 0 mismatches, 0 desyncs**, across all 56 records — the documented
  `state_len`/`priority` framing and the current capability-heuristic framing compute the
  identical record length for every packet seen so far, including the newly-seen offline
  `flags` values. The record-framing rewrite is confirmed safe.
- **`on` (flags bit 0) and `online` (flags bit 1) were identical in every one of the 56
  records — zero divergences**, across full dimmer/vertical/temperature sweeps, real power-off
  events, and the pre-existing historical captures. On this firmware/network, bit 0 carries no
  information beyond bit 1.
- **The *current* `online` heuristic (`flags` low nibble != 0) is demonstrably wrong**, twice,
  on real data: `flags=0x04` (unit 3) and `flags=0x14` (units 2, 10) both occurred during the
  user's power-cycle-off action — genuine offline transitions — yet both have a nonzero low
  nibble, so the current heuristic reports `online: true` for a unit that just went dark. The
  flags-bit-1 interpretation correctly reports `online: false` in both cases.
- **The *current* `on` heuristic (`level > 0`) is also demonstrably wrong** for the same
  offline transitions: the packet's `level` byte still carries the *last known* brightness
  (e.g. unit 5: stored level 13, unit 9: stored level 254) rather than reflecting the device
  going dark, so `on: true` is reported for a unit that is actually powered off. The flags-bit-0
  interpretation correctly reports `on: false`.

### Decision this evidence informed

Despite `on`/`online` being indistinguishable from each other in every observed packet, both
are still a **strict improvement** over the current heuristics specifically for the offline
case above — so the ESP32 passes the device's raw flags bits through generically (no
`level`-derived re-interpretation), while deliberately not inventing any *further* semantics
(e.g. "is this light currently glowing") on top — that judgment call is left to downstream
consumers (FHEM etc.), per the user's explicit direction during this investigation.
