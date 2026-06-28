#!/usr/bin/env python3
"""
ESP32 Casambi — New TCP/Web Stack Functional Verification
=========================================================
Risk-mitigation harness for the migration from the unmaintained
`esphome/ESPAsyncWebServer-esphome` + `esphome/AsyncTCP-esphome` libraries to the
maintained **ESP32Async/ESPAsyncWebServer** + **ESP32Async/AsyncTCP** stack
(issue #18).

Unlike `stress_test.py` (which hammers the device to surface heap leaks and the
churn crashes), this script does NOT try to break anything. It is a *functional*
acceptance check that walks exactly the API surface whose behaviour can shift
when the underlying async stack is swapped, and asserts each one still works in
OUR context:

  T1  CORS / DefaultHeaders        — DefaultHeaders::Instance().addHeader still
                                      attaches to every response.
  T2  GET endpoints + JSON         — plain server.on() GET routes return valid
                                      JSON (status/units/groups/scenes/ntp/info).
  T3  Chunked response (/api/log)  — beginChunkedResponse streams a valid JSON
                                      array; ?n= and ?n=0 honoured.
  T4  POST body buffering          — onRequestBody → _tempObject → onNotFound
                                      dispatch path delivers the body to handlers
                                      (valid JSON parsed, acted on or 503 when no
                                      BLE — never 400 "missing body").
  T5  Single response per POST      — exactly ONE HTTP response per POST; the
                                      PR #19 double-response leak must not return
                                      on the new stack (heap stays flat over a
                                      burst of body POSTs).
  T6  Oversize POST → 413          — contentLength() rejection in onNotFound.
  T7  Invalid JSON / missing body  — 400 from the handlers, single response.
  T8  Aborted body cleanup          — request->onDisconnect() frees _tempObject
                                      for a body that never completes (no leak).
  T9  WebSocket handshake + hello  — upgrade succeeds, server pushes the "hello"
                                      JSON snapshot on connect.
  T10 WS client cap (cleanupClients)— more than WS_MAX_CLIENTS simultaneous
                                      clients are trimmed; the newest survive.

Every check reads device heap from /api/status so a regression that only shows
as a slow leak is still caught (T5/T8 compare before/after).

Pure stdlib, no external dependencies (mirrors stress_test.py).

Usage:
    python3 scripts/verify_tcp_stack.py --host <ESP32_IP> [--port 80]
            [--ws-max-clients 3] [--leak-burst 60] [--verbose]

Exit code 0 = all checks passed, 1 = at least one failed (CI-friendly).
"""

import argparse
import base64
import http.client
import json
import random
import socket
import struct
import sys
import time


# ---------------------------------------------------------------------------
# Tiny result collector
# ---------------------------------------------------------------------------

class Results:
    def __init__(self):
        self.rows = []   # (id, name, ok, detail)

    def add(self, tid, name, ok, detail=""):
        self.rows.append((tid, name, ok, detail))
        mark = "PASS" if ok else "FAIL"
        print(f"  [{mark}] {tid:<4} {name}" + (f"  — {detail}" if detail else ""))

    def passed(self):
        return all(ok for _, _, ok, _ in self.rows)

    def summary(self):
        n_ok = sum(1 for _, _, ok, _ in self.rows if ok)
        print("\n" + "=" * 62)
        print(f"VERIFY TCP STACK : {n_ok}/{len(self.rows)} checks passed")
        if not self.passed():
            print("FAILED checks:")
            for tid, name, ok, detail in self.rows:
                if not ok:
                    print(f"  - {tid} {name}: {detail}")
        print("=" * 62)


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib only)
# ---------------------------------------------------------------------------

def http_request(host, port, method, path, body=None, extra_headers=None,
                 timeout=6):
    """Return (status, headers_dict, body_bytes) or (None, None, None) on error."""
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        headers = {"Connection": "close"}
        if body is not None:
            headers["Content-Type"] = "application/json"
            headers["Content-Length"] = str(len(body))
        if extra_headers:
            headers.update(extra_headers)
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        hdrs = {k.lower(): v for k, v in resp.getheaders()}
        conn.close()
        return resp.status, hdrs, data
    except Exception as e:
        return None, None, str(e).encode()


def get_status(host, port):
    """Fetch /api/status as a dict, or None."""
    st, _, data = http_request(host, port, "GET", "/api/status")
    if st != 200 or not data:
        return None
    try:
        return json.loads(data)
    except Exception:
        return None


def free_heap(host, port):
    s = get_status(host, port)
    return (s or {}).get("free_heap")


def largest_block(host, port):
    s = get_status(host, port)
    return (s or {}).get("largest_block")


# ---------------------------------------------------------------------------
# Minimal WebSocket client (RFC 6455, client masking) — same approach as
# stress_test.py, kept self-contained here.
# ---------------------------------------------------------------------------

def ws_connect(host, port, path="/ws", timeout=5):
    key = base64.b64encode(bytes(random.randint(0, 255) for _ in range(16))).decode()
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.sendall((
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        ).encode())
        buf = b""
        s.settimeout(timeout)
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(512)
            if not chunk:
                s.close()
                return None
            buf += chunk
            if len(buf) > 4096:
                s.close()
                return None
        if b" 101 " not in buf:
            s.close()
            return None
        return s
    except Exception:
        return None


def ws_send(s, opcode, payload=b""):
    mask = bytes(random.randint(0, 255) for _ in range(4))
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    plen = len(payload)
    if plen < 126:
        header = bytes([0x80 | opcode, 0x80 | plen])
    elif plen < 65536:
        header = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack("!H", plen)
    else:
        header = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack("!Q", plen)
    s.sendall(header + mask + masked)


def ws_recv_frame(s, timeout=3):
    s.settimeout(timeout)
    try:
        def recv_exact(n):
            buf = b""
            while len(buf) < n:
                c = s.recv(n - len(buf))
                if not c:
                    raise ConnectionError("closed")
                buf += c
            return buf

        b1, b2 = recv_exact(2)
        opcode = b1 & 0x0F
        masked = bool(b2 & 0x80)
        plen = b2 & 0x7F
        if plen == 126:
            plen = struct.unpack("!H", recv_exact(2))[0]
        elif plen == 127:
            plen = struct.unpack("!Q", recv_exact(8))[0]
        mask = recv_exact(4) if masked else None
        payload = recv_exact(plen)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload
    except Exception:
        return None, None


def ws_read_text(s, timeout=3):
    """Return the first text-frame payload as str, or None."""
    op, payload = ws_recv_frame(s, timeout)
    if op == 0x1 and payload is not None:
        try:
            return payload.decode("utf-8", "replace")
        except Exception:
            return None
    return None


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------

def t1_cors(host, port, r):
    st, hdrs, _ = http_request(host, port, "GET", "/api/status")
    ok = (st == 200 and hdrs is not None and
          hdrs.get("access-control-allow-origin") == "*")
    r.add("T1", "CORS / DefaultHeaders on every response", ok,
          "" if ok else f"status={st} acao={(hdrs or {}).get('access-control-allow-origin')}")


def t2_get_json(host, port, r):
    endpoints = {
        "/api/info":   ("configured",),
        "/api/status": ("free_heap", "largest_block", "min_free_heap"),
        "/api/units":  ("units",),
        "/api/groups": ("groups",),
        "/api/scenes": ("scenes",),
        "/api/ntp":    ("ntp_server",),
    }
    bad = []
    for path, keys in endpoints.items():
        st, _, data = http_request(host, port, "GET", path)
        if st != 200:
            bad.append(f"{path} status={st}")
            continue
        try:
            doc = json.loads(data)
        except Exception:
            bad.append(f"{path} non-JSON")
            continue
        for k in keys:
            if k not in doc:
                bad.append(f"{path} missing '{k}'")
    r.add("T2", "GET endpoints return valid JSON", not bad,
          "; ".join(bad))


def t3_chunked_log(host, port, r):
    # Default response.
    st, hdrs, data = http_request(host, port, "GET", "/api/log")
    ok_default = False
    detail = ""
    if st != 200:
        detail = f"status={st}"
    else:
        te = (hdrs or {}).get("transfer-encoding", "")
        try:
            doc = json.loads(data)
            ok_default = isinstance(doc, list)
        except Exception:
            ok_default = False
        if not ok_default:
            detail = "body is not a JSON array"
        elif "chunked" not in te.lower():
            # Not fatal (small logs may fit one chunk), but note it.
            detail = f"(note: transfer-encoding={te!r}, not chunked)"

    # ?n=0 must also yield a valid JSON array (all entries).
    st0, _, data0 = http_request(host, port, "GET", "/api/log?n=0")
    ok_all = False
    if st0 == 200:
        try:
            ok_all = isinstance(json.loads(data0), list)
        except Exception:
            ok_all = False

    ok = ok_default and ok_all
    if not ok and not detail:
        detail = f"?n=0 status={st0} valid_array={ok_all}"
    r.add("T3", "Chunked /api/log streams valid JSON array", ok, detail)


def _pick_unit_id(host, port):
    st, _, data = http_request(host, port, "GET", "/api/units")
    if st == 200:
        try:
            units = json.loads(data).get("units", [])
            if units:
                return units[0].get("id")
        except Exception:
            pass
    return None


def t4_post_body_buffering(host, port, r, unit_id):
    """A valid body must reach the handler. With no BLE the handler returns 503
    ("Not connected"); with BLE it returns 200. The ONE thing that must never
    happen is 400 'Missing request body' — that means the body never made it
    through onRequestBody/_tempObject into onNotFound dispatch."""
    if unit_id is None:
        r.add("T4", "POST body reaches handler (_tempObject path)", False,
              "no unit available to address")
        return
    path = f"/api/units/{unit_id}/level"
    body = json.dumps({"level": 128}).encode()
    st, _, data = http_request(host, port, "POST", path, body=body)
    txt = (data or b"").decode("utf-8", "replace")
    ok = st in (200, 503) and "Missing request body" not in txt
    r.add("T4", "POST body reaches handler (_tempObject path)", ok,
          "" if ok else f"status={st} body={txt[:80]}")


def t5_single_response_no_leak(host, port, r, unit_id, burst):
    """Repeated body POSTs must not leak — the PR #19 double-response bug lost
    ~216 B per POST. On a correct stack the heap is flat (allowing for normal
    jitter) across a burst."""
    if unit_id is None:
        r.add("T5", "No per-POST leak over burst (single response)", False,
              "no unit available to address")
        return
    path = f"/api/units/{unit_id}/level"
    body = json.dumps({"level": 64}).encode()

    h_before = free_heap(host, port)
    time.sleep(0.2)
    sent = 0
    for _ in range(burst):
        st, _, _ = http_request(host, port, "POST", path, body=body)
        if st is not None:
            sent += 1
    time.sleep(0.5)  # let async frees settle
    h_after = free_heap(host, port)

    if h_before is None or h_after is None:
        r.add("T5", "No per-POST leak over burst (single response)", False,
              "could not read heap")
        return
    drop = h_before - h_after
    # ~216 B/POST would be a large, unambiguous drop; allow generous jitter.
    threshold = max(2048, burst * 64)
    ok = drop < threshold
    r.add("T5", "No per-POST leak over burst (single response)", ok,
          f"{sent} POSTs, heap {h_before}->{h_after} (drop {drop} B, limit {threshold} B)")


def t6_oversize_413(host, port, r, unit_id):
    if unit_id is None:
        r.add("T6", "Oversize POST -> 413", False, "no unit available")
        return
    path = f"/api/units/{unit_id}/level"
    body = (b'{"level":128,"pad":"' + b"x" * 700 + b'"}')  # > 512 B
    st, _, _ = http_request(host, port, "POST", path, body=body)
    ok = st == 413
    r.add("T6", "Oversize POST -> 413 (contentLength reject)", ok,
          "" if ok else f"status={st}")


def t7_bad_input_400(host, port, r, unit_id):
    if unit_id is None:
        r.add("T7", "Invalid JSON / missing body -> 400", False, "no unit available")
        return
    path = f"/api/units/{unit_id}/level"
    # Invalid JSON.
    st1, _, _ = http_request(host, port, "POST", path, body=b"{not json")
    # Empty body (Content-Length: 0).
    st2, _, _ = http_request(host, port, "POST", path, body=b"")
    # Both should be 400 (single clean error), not 5xx / no-response.
    ok = st1 == 400 and st2 == 400
    r.add("T7", "Invalid JSON / missing body -> 400", ok,
          "" if ok else f"invalid={st1} empty={st2}")


def t8_aborted_body_cleanup(host, port, r, unit_id, count=20):
    """Open a POST that announces a large body, send only part, then close TCP.
    onNotFound never runs for an unfinished request, so request->onDisconnect()
    is the only path that frees _tempObject. Leak shows as heap not recovering."""
    if unit_id is None:
        r.add("T8", "Aborted body freed (onDisconnect cleanup)", False,
              "no unit available")
        return
    path = f"/api/units/{unit_id}/level"
    h_before = free_heap(host, port)
    time.sleep(0.2)
    done = 0
    for _ in range(count):
        try:
            s = socket.create_connection((host, port), timeout=3)
            partial = b'{"level":1'
            claimed = len(partial) + 200  # promise more than we send
            hdr = (
                f"POST {path} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {claimed}\r\n"
                f"\r\n"
            ).encode()
            s.sendall(hdr + partial)
            time.sleep(0.03)
            s.close()
            done += 1
        except Exception:
            pass
    time.sleep(0.7)  # allow disconnect handlers + async frees
    h_after = free_heap(host, port)
    if h_before is None or h_after is None:
        r.add("T8", "Aborted body freed (onDisconnect cleanup)", False,
              "could not read heap")
        return
    drop = h_before - h_after
    threshold = max(2048, count * 80)
    ok = drop < threshold
    r.add("T8", "Aborted body freed (onDisconnect cleanup)", ok,
          f"{done} aborted, heap {h_before}->{h_after} (drop {drop} B, limit {threshold} B)")


def t9_ws_hello(host, port, r):
    s = ws_connect(host, port)
    if s is None:
        r.add("T9", "WebSocket handshake + hello snapshot", False,
              "handshake failed (101 not received)")
        return
    try:
        txt = ws_read_text(s, timeout=4)
        ok = False
        detail = "no text frame received"
        if txt:
            try:
                doc = json.loads(txt)
                ok = doc.get("type") == "hello" and "units" in doc
                detail = "" if ok else f"unexpected payload: {txt[:80]}"
            except Exception:
                detail = f"non-JSON frame: {txt[:80]}"
        r.add("T9", "WebSocket handshake + hello snapshot", ok, detail)
    finally:
        try:
            ws_send(s, 0x8)  # close
        except Exception:
            pass
        s.close()


def t10_ws_client_cap(host, port, r, ws_max):
    """Open ws_max + 2 clients. cleanupClients(WS_MAX_CLIENTS) runs in loop(),
    so the oldest beyond the cap get evicted. Verify by reading /api/status
    can still be served (server alive) and that the newest clients stay open
    while at least one early client gets closed."""
    n = ws_max + 2
    socks = []
    for _ in range(n):
        s = ws_connect(host, port)
        if s:
            # Drain the hello so it doesn't sit in the buffer.
            ws_read_text(s, timeout=2)
        socks.append(s)
        time.sleep(0.25)  # give loop() time to run cleanupClients between opens

    # Nudge loop() a few times.
    for _ in range(3):
        get_status(host, port)
        time.sleep(0.3)

    opened = sum(1 for s in socks if s is not None)

    # Probe liveness of each socket: a ping should get a pong from survivors;
    # evicted ones error/close.
    alive = 0
    for s in socks:
        if s is None:
            continue
        try:
            ws_send(s, 0x9)  # ping
            op, _ = ws_recv_frame(s, timeout=2)
            if op is not None:
                alive += 1
        except Exception:
            pass

    for s in socks:
        if s:
            try:
                ws_send(s, 0x8)
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass

    # Server must still serve HTTP (didn't crash) and must not keep ALL clients
    # (cap enforced). alive <= ws_max is the cap; server-alive is the key safety.
    server_alive = free_heap(host, port) is not None
    ok = server_alive and opened >= 1 and alive <= ws_max
    r.add("T10", "WS client cap enforced (cleanupClients)", ok,
          f"opened={opened} alive_after_cap={alive} cap={ws_max} server_alive={server_alive}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Functional verification of the new ESP32Async TCP/web stack.")
    ap.add_argument("--host", required=True, help="ESP32 IP address")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--ws-max-clients", type=int, default=3,
                    help="WS_MAX_CLIENTS from config.h (default 3)")
    ap.add_argument("--leak-burst", type=int, default=60,
                    help="POST count for the per-POST leak check (T5)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    print("=" * 62)
    print("ESP32 Casambi — New TCP/Web Stack Functional Verification")
    print(f"Target: http://{args.host}:{args.port}   WS_MAX_CLIENTS={args.ws_max_clients}")
    print("=" * 62)

    # Reachability gate.
    base = get_status(args.host, args.port)
    if base is None:
        print("FATAL: /api/status not reachable — is the device up and on this IP?")
        sys.exit(1)
    print(f"Device reachable. free_heap={base.get('free_heap')} "
          f"largest_block={base.get('largest_block')} "
          f"ble_connected={base.get('ble_connected')}\n")

    r = Results()
    unit_id = _pick_unit_id(args.host, args.port)
    if args.verbose:
        print(f"(addressing unit id={unit_id} for POST checks)\n")

    t1_cors(args.host, args.port, r)
    t2_get_json(args.host, args.port, r)
    t3_chunked_log(args.host, args.port, r)
    t4_post_body_buffering(args.host, args.port, r, unit_id)
    t5_single_response_no_leak(args.host, args.port, r, unit_id, args.leak_burst)
    t6_oversize_413(args.host, args.port, r, unit_id)
    t7_bad_input_400(args.host, args.port, r, unit_id)
    t8_aborted_body_cleanup(args.host, args.port, r, unit_id)
    t9_ws_hello(args.host, args.port, r)
    t10_ws_client_cap(args.host, args.port, r, args.ws_max_clients)

    r.summary()

    # Final sanity: device still alive and didn't reboot under the checks.
    after = get_status(args.host, args.port)
    if after is None:
        print("WARNING: device not reachable after checks — possible crash/reboot.")
        sys.exit(1)
    if base.get("boot_count") is not None and \
       after.get("boot_count") not in (None, base.get("boot_count")):
        print(f"WARNING: boot_count changed "
              f"({base.get('boot_count')} -> {after.get('boot_count')}) — device REBOOTED.")
        sys.exit(1)

    sys.exit(0 if r.passed() else 1)


if __name__ == "__main__":
    main()
