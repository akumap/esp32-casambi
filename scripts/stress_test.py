#!/usr/bin/env python3
"""
ESP32 Casambi Web Server Stress / Stability Test
================================================
Exercises the ESP32 web server under controlled, staged load and watches the
device heap — both during the run and during a cooldown phase afterwards — to
distinguish a transient dip from a real leak (heap that never comes back).

The load is organised into PROFILES so you can start gently and ramp up:

  realistic  Mirrors the FHEM CasambiGW module: ONE persistent WebSocket with
             ping/pong every few seconds and only sporadic control POSTs.
             No HTTP polling while connected (FHEM doesn't either).
             This is the production load and MUST stay rock stable.

  light      realistic + a little HTTP polling and one slow WS reconnect loop.

  medium     A handful of concurrent GET/POST workers + WS churn + 1 persistent.

  heavy      Aggressive: many concurrent workers, fast WS churn, aborted POSTs.
             This is the "excessive use" scenario from issue #17 — expected to
             find the breaking point, not to pass cleanly.

Usage:
    python3 scripts/stress_test.py --host <ESP32_IP> [--profile realistic]

Common options:
    --host HOST        ESP32 IP address (required)
    --port PORT        HTTP port (default: 80)
    --profile NAME     realistic | light | medium | heavy   (default: realistic)
    --duration SECS    Active load duration (default: 60)
    --ramp SECS        Stagger worker startup over this window (default: 10)
    --cooldown SECS    After load stops, keep sampling heap this long to check
                       for recovery (default: 30)

Overrides (take precedence over the profile):
    --concurrency N    HTTP GET/POST worker count
    --ws-clients N     WebSocket worker count
    --get-rate R       Max GET requests/sec PER get worker (0 = unlimited)
    --skip-ws / --skip-abort
"""

import argparse
import base64
import collections
import http.client
import json
import os
import random
import socket
import struct
import sys
import threading
import time


# ---------------------------------------------------------------------------
# Load profiles
# ---------------------------------------------------------------------------
# Each profile is a dict of knobs the worker-spawner reads.  Keeping them in one
# place makes it easy to start gently (realistic) and step up deliberately.

PROFILES = {
    "realistic": dict(
        get_workers=0, get_rate=0.0,
        post_workers=0, post_period=0.0,
        invalid=False, oversize=False, abort_workers=0,
        ws_churn=0, ws_churn_period=0.0,
        ws_persistent=1,
        # The single persistent client behaves like FHEM: a control POST now
        # and then, otherwise it just holds the socket open and ping/pongs.
        fhem_cmd_period=8.0,
    ),
    "light": dict(
        get_workers=1, get_rate=1.0,
        post_workers=1, post_period=2.0,
        invalid=False, oversize=False, abort_workers=0,
        ws_churn=1, ws_churn_period=2.0,
        ws_persistent=1,
        fhem_cmd_period=8.0,
    ),
    "medium": dict(
        get_workers=3, get_rate=3.0,
        post_workers=2, post_period=0.4,
        invalid=True, oversize=True, abort_workers=1,
        ws_churn=2, ws_churn_period=0.4,
        ws_persistent=1,
        fhem_cmd_period=6.0,
    ),
    "heavy": dict(
        get_workers=6, get_rate=0.0,        # unlimited
        post_workers=3, post_period=0.05,
        invalid=True, oversize=True, abort_workers=2,
        ws_churn=4, ws_churn_period=0.05,
        ws_persistent=1,
        fhem_cmd_period=4.0,
    ),
}


# ---------------------------------------------------------------------------
# Thread-safe statistics
# ---------------------------------------------------------------------------

class Stats:
    def __init__(self):
        self._lock = threading.Lock()
        self.counts = collections.defaultdict(int)
        self.latencies = collections.defaultdict(list)
        self.heap_samples = []   # (free_heap, largest_block|None, monotonic_ts)

    def record(self, category, success, latency_ms=None):
        with self._lock:
            self.counts[f"{category}_{'ok' if success else 'err'}"] += 1
            if latency_ms is not None and success:
                self.latencies[category].append(latency_ms)

    def record_heap(self, free_heap, largest=None):
        with self._lock:
            self.heap_samples.append((free_heap, largest, time.monotonic()))

    def snapshot(self):
        with self._lock:
            ok  = sum(v for k, v in self.counts.items() if k.endswith('_ok'))
            err = sum(v for k, v in self.counts.items() if k.endswith('_err'))
            heap = self.heap_samples[-1][0] if self.heap_samples else None
            return ok, err, heap

    def report(self, duration_s):
        print("\n" + "=" * 62)
        print("STRESS TEST RESULTS")
        print("=" * 62)

        total_ok  = sum(v for k, v in self.counts.items() if k.endswith('_ok'))
        total_err = sum(v for k, v in self.counts.items() if k.endswith('_err'))
        total     = total_ok + total_err
        rps       = total / duration_s if duration_s > 0 else 0
        print(f"\nTotal requests : {total}  ({total_ok} ok, {total_err} errors)"
              f"  {rps:.1f} req/s")

        print("\nBy category:")
        categories = sorted({k.rsplit('_', 1)[0] for k in self.counts})
        for cat in categories:
            ok  = self.counts.get(f"{cat}_ok",  0)
            err = self.counts.get(f"{cat}_err", 0)
            if ok + err == 0:
                continue
            lats = sorted(self.latencies.get(cat, []))
            lat_str = ""
            if lats:
                p50 = lats[len(lats) // 2]
                p95 = lats[min(len(lats) - 1, int(len(lats) * 0.95))]
                lat_str = f"  p50={p50:.0f}ms  p95={p95:.0f}ms  max={lats[-1]:.0f}ms"
            print(f"  {cat:<34}  ok={ok:5d}  err={err:4d}{lat_str}")

        self._report_heap()
        print()

    def _report_heap(self):
        if not self.heap_samples:
            print("\nHeap (device) : no samples collected")
            return
        first  = self.heap_samples[0][0]
        last   = self.heap_samples[-1][0]
        lo     = min(s[0] for s in self.heap_samples)
        frags  = [s[1] for s in self.heap_samples if s[1] is not None]
        frag_str = f"  largest_block_min={min(frags)//1024}KB" if frags else ""
        print(f"\nHeap (device) :"
              f"  start={first//1024}KB  end={last//1024}KB  min={lo//1024}KB"
              f"{frag_str}")


stats    = Stats()
stop_evt = threading.Event()   # set when active load should stop


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib only — no external dependencies)
# ---------------------------------------------------------------------------

def http_get(host, port, path, timeout=5):
    t0 = time.monotonic()
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request("GET", path, headers={"Connection": "close"})
        resp = conn.getresponse()
        body = resp.read()
        conn.close()
        return resp.status == 200, (time.monotonic() - t0) * 1000, body
    except Exception:
        return False, None, None


def http_post(host, port, path, body_bytes, timeout=5):
    t0 = time.monotonic()
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request(
            "POST", path, body=body_bytes,
            headers={
                "Content-Type":   "application/json",
                "Content-Length": str(len(body_bytes)),
                "Connection":     "close",
            },
        )
        resp = conn.getresponse()
        resp.read()
        conn.close()
        # 2xx and 4xx are both "handled gracefully"; only 5xx (or no response) is an error.
        return resp.status < 500, (time.monotonic() - t0) * 1000
    except Exception:
        return False, None


def http_post_abort(host, port, path, partial_body):
    """Send a partial POST body then close TCP without completing it (leak probe)."""
    try:
        s = socket.create_connection((host, port), timeout=3)
        claimed_len = len(partial_body) + 300
        header = (
            f"POST {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {claimed_len}\r\n"
            f"\r\n"
        ).encode()
        s.sendall(header + partial_body)
        time.sleep(0.03)
        s.close()
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Minimal pure-stdlib WebSocket client
# ---------------------------------------------------------------------------

def _ws_connect(host, port, path="/ws", timeout=5):
    """Perform the WebSocket opening handshake.  Returns raw socket or None."""
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


def _ws_send(s, opcode, payload=b""):
    """Send a properly masked client→server frame (RFC 6455 requires masking)."""
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


def _ws_recv_frame(s, timeout=2):
    """Read one WebSocket frame.  Returns (opcode, payload_bytes) or (None, None)."""
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
        plen   = b2 & 0x7F
        if plen == 126:
            plen = struct.unpack("!H", recv_exact(2))[0]
        elif plen == 127:
            plen = struct.unpack("!Q", recv_exact(8))[0]
        mask = recv_exact(4) if masked else None
        payload = recv_exact(plen)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload
    except socket.timeout:
        return None, None
    except Exception:
        return None, None


# ---------------------------------------------------------------------------
# Worker threads
# ---------------------------------------------------------------------------

def worker_get_flood(host, port, rate, keepalive=False):
    """GET random read-only endpoints.  rate>0 caps requests/sec for this worker.

    keepalive=True reuses a single TCP connection across requests (no
    'Connection: close'), so it does NOT churn connections.  Comparing a
    keepalive run against the default (one connection per request) tells us
    whether a heap leak comes from connection churn (TCP layer) or from
    per-request handling (our code)."""
    endpoints = [
        "/api/status", "/api/units", "/api/groups",
        "/api/scenes",  "/api/log?n=5", "/api/ntp",
    ]
    min_interval = (1.0 / rate) if rate and rate > 0 else 0.0
    conn = None
    while not stop_evt.is_set():
        t0   = time.monotonic()
        path = random.choice(endpoints)
        cat  = path.split("?")[0].split("/")[-1]

        if keepalive:
            # Persistent connection: keep one open and reuse it.
            try:
                if conn is None:
                    conn = http.client.HTTPConnection(host, port, timeout=5)
                conn.request("GET", path)          # no Connection: close
                resp = conn.getresponse()
                resp.read()
                ok = resp.status == 200
                stats.record(f"GET/{cat}", ok, (time.monotonic() - t0) * 1000)
            except Exception:
                stats.record(f"GET/{cat}", False)
                try:
                    if conn:
                        conn.close()
                except Exception:
                    pass
                conn = None
        else:
            ok, lat, _ = http_get(host, port, path)
            stats.record(f"GET/{cat}", ok, lat)

        if min_interval:
            sleep = min_interval - (time.monotonic() - t0)
            if sleep > 0:
                stop_evt.wait(sleep)
    if conn:
        try:
            conn.close()
        except Exception:
            pass


def worker_post_control(host, port, period, unit):
    """POST control commands to the designated test unit ONLY.
    503 (BLE not connected) counts as ok. Never touches scenes/groups/other
    units, so no other physical device is switched."""
    cases = [
        (f"/api/units/{unit}/on",    b""),
        (f"/api/units/{unit}/off",   b""),
        (f"/api/units/{unit}/level", b'{"level":128}'),
    ]
    while not stop_evt.is_set():
        path, body = random.choice(cases)
        ok, lat = http_post(host, port, path, body)
        stats.record("POST/control", ok, lat)
        stop_evt.wait(period)


def worker_post_invalid(host, port, unit):
    """Sends malformed / semantically wrong bodies — 4xx expected (counts as ok).
    All target the test unit and are rejected, so nothing is switched/changed."""
    cases = [
        (f"/api/units/{unit}/level",       b"not json"),
        (f"/api/units/{unit}/level",       b'{"wrong":1}'),
        (f"/api/units/{unit}/color",       b'{"r":1}'),         # missing g, b
        (f"/api/units/{unit}/temperature", b'{"kelvin":500}'),  # out of range → 400
    ]
    while not stop_evt.is_set():
        path, body = random.choice(cases)
        ok, lat = http_post(host, port, path, body)
        stats.record("POST/invalid", ok, lat)
        stop_evt.wait(0.1)


def worker_post_oversize(host, port, unit):
    """POSTs a body > 512 bytes — device must return 413, not crash (no switch)."""
    body = b'{"level":128,"pad":"' + b"x" * 600 + b'"}'
    while not stop_evt.is_set():
        ok, lat = http_post(host, port, f"/api/units/{unit}/level", body)
        stats.record("POST/oversize", ok, lat)
        stop_evt.wait(0.3)


def worker_post_abort(host, port, unit):
    """Aborts connections mid-body to probe for the _tempObject memory leak.
    Body never completes, so the command is never executed (no switch)."""
    partials = [b'{"level":', b'{"le', b'']
    while not stop_evt.is_set():
        ok = http_post_abort(host, port, f"/api/units/{unit}/level",
                             random.choice(partials))
        stats.record("POST/abort", ok)
        stop_evt.wait(0.15)


def worker_ws_churn(host, port, period):
    """Opens a WebSocket, reads the hello message, then closes it."""
    while not stop_evt.is_set():
        t0 = time.monotonic()
        s  = _ws_connect(host, port)
        if s is None:
            stats.record("WS/connect", False)
            stop_evt.wait(max(period, 0.2))
            continue
        stats.record("WS/connect", True, (time.monotonic() - t0) * 1000)

        opcode, payload = _ws_recv_frame(s, timeout=2)
        if opcode == 1:
            try:
                stats.record("WS/hello", json.loads(payload).get("type") == "hello")
            except Exception:
                stats.record("WS/hello", False)
        try:
            _ws_send(s, 0x08)   # masked close
        except Exception:
            pass
        s.close()
        stop_evt.wait(period)


def worker_ws_fhem(host, port, cmd_period, unit):
    """
    Mirrors the FHEM CasambiGW client: ONE persistent WebSocket that
      - reads the initial hello,
      - replies to server pings with pongs,
      - sends a client ping periodically (FHEM does every 30s),
      - issues an occasional control POST (user toggling a light),
      - reconnects after a short delay if the link drops (like DevIo).
    """
    last_ping = time.monotonic()
    last_cmd  = time.monotonic()
    while not stop_evt.is_set():
        s = _ws_connect(host, port)
        if s is None:
            stats.record("FHEM/connect", False)
            stop_evt.wait(2.0)          # DevIo-style reconnect delay
            continue
        stats.record("FHEM/connect", True)

        while not stop_evt.is_set():
            opcode, payload = _ws_recv_frame(s, timeout=1)
            now = time.monotonic()

            if opcode == 8:             # server close
                break
            elif opcode == 9:           # ping → pong
                try:
                    _ws_send(s, 0x0A, payload or b"")
                except Exception:
                    break
            elif opcode in (0, 1):      # data push
                stats.record("FHEM/push", True)

            # Periodic client ping (compressed cadence vs FHEM's 30s).
            if now - last_ping >= 5.0:
                last_ping = now
                try:
                    _ws_send(s, 0x09)
                except Exception:
                    break

            # Occasional control command, like a user action (test unit only).
            if cmd_period and now - last_cmd >= cmd_period:
                last_cmd = now
                ok, _ = http_post(host, port, f"/api/units/{unit}/level",
                                  b'{"level":128}')
                stats.record("FHEM/cmd", ok)

        s.close()


def worker_heap_monitor(host, port, interval=4):
    """Polls /api/status to track device heap.  Runs through cooldown too."""
    time.sleep(2)
    while not _monitor_stop.is_set():
        ok, _, body = http_get(host, port, "/api/status")
        if ok and body:
            try:
                data = json.loads(body)
                stats.record_heap(data.get("free_heap", 0))
            except Exception:
                pass
        _monitor_stop.wait(interval)


_monitor_stop = threading.Event()   # heap monitor keeps running until this is set


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def check_reachable(host, port):
    ok, _, body = http_get(host, port, "/api/info", timeout=8)
    if ok and body:
        try:
            info = json.loads(body)
            print(f"Device reachable —"
                  f" configured={info.get('configured')}"
                  f"  build={info.get('build')}"
                  f"  network={info.get('network')!r}")
            return True
        except Exception:
            pass
    ok2, _, _ = http_get(host, port, "/api/status", timeout=8)
    if ok2:
        print("Device reachable (no /api/info)")
        return True
    print(f"ERROR: Device at {host}:{port} is not reachable.")
    return False


def main():
    parser = argparse.ArgumentParser(
        description="ESP32 Casambi web server stress / stability test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--host",     required=True, help="ESP32 IP address")
    parser.add_argument("--port",     type=int, default=80)
    parser.add_argument("--profile",  choices=list(PROFILES), default="realistic",
                        help="Load profile (default: realistic)")
    parser.add_argument("--duration", type=int, default=60,
                        help="Active load duration in seconds (default: 60)")
    parser.add_argument("--ramp",     type=int, default=10,
                        help="Stagger worker startup over this window (default: 10)")
    parser.add_argument("--cooldown", type=int, default=30,
                        help="Post-load heap-recovery observation (default: 30)")
    parser.add_argument("--unit",     type=int, default=11,
                        help="Casambi unit ID that control POSTs target. "
                             "Default 11 = 'IFC'. ONLY this unit is ever switched; "
                             "no scenes/groups/other units are touched.")
    # Overrides
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--ws-clients",  type=int, default=None)
    parser.add_argument("--get-rate",    type=float, default=None)
    parser.add_argument("--keepalive",   action="store_true",
                        help="GET workers reuse one TCP connection (no churn) — "
                             "use to tell connection-churn leaks from per-request ones")
    parser.add_argument("--skip-ws",     action="store_true")
    parser.add_argument("--skip-post",   action="store_true",
                        help="Disable all POST workers (control/invalid/oversize)")
    parser.add_argument("--skip-abort",  action="store_true")
    args = parser.parse_args()

    prof = dict(PROFILES[args.profile])   # copy so overrides don't mutate the table

    # Apply overrides
    if args.concurrency is not None:
        prof["get_workers"]  = max(1, args.concurrency // 2)
        prof["post_workers"] = max(1, args.concurrency // 4)
    if args.ws_clients is not None:
        prof["ws_churn"]     = max(0, args.ws_clients - 1)
        prof["ws_persistent"] = 1 if args.ws_clients >= 1 else 0
    if args.get_rate is not None:
        prof["get_rate"] = args.get_rate
    if args.skip_ws:
        prof["ws_churn"] = prof["ws_persistent"] = 0
    if args.skip_post:
        prof["post_workers"] = 0
        prof["invalid"] = prof["oversize"] = False
    if args.skip_abort:
        prof["abort_workers"] = 0

    print(f"\nESP32 Casambi — Web Server Stress Test")
    print(f"Target   : http://{args.host}:{args.port}/")
    print(f"Profile  : {args.profile}")
    print(f"Test unit: {args.unit}  (ONLY this unit is switched)")
    print(f"Duration : {args.duration}s active  +  {args.cooldown}s cooldown"
          f"  (ramp {args.ramp}s)")
    print(f"Workers  : get={prof['get_workers']}"
          f"{' (keepalive)' if args.keepalive else ''}"
          f" post={prof['post_workers']}"
          f" abort={prof['abort_workers']}"
          f" ws_churn={prof['ws_churn']} ws_persistent={prof['ws_persistent']}\n")

    if not check_reachable(args.host, args.port):
        sys.exit(1)
    print()

    # Build the worker list (function, args) — started later with ramp.
    jobs = []
    for _ in range(prof["get_workers"]):
        jobs.append((worker_get_flood,
                     (args.host, args.port, prof["get_rate"], args.keepalive)))
    for _ in range(prof["post_workers"]):
        jobs.append((worker_post_control,
                     (args.host, args.port, prof["post_period"], args.unit)))
    if prof["invalid"]:
        jobs.append((worker_post_invalid, (args.host, args.port, args.unit)))
    if prof["oversize"]:
        jobs.append((worker_post_oversize, (args.host, args.port, args.unit)))
    for _ in range(prof["abort_workers"]):
        jobs.append((worker_post_abort, (args.host, args.port, args.unit)))
    for _ in range(prof["ws_churn"]):
        jobs.append((worker_ws_churn, (args.host, args.port, prof["ws_churn_period"])))
    for _ in range(prof["ws_persistent"]):
        jobs.append((worker_ws_fhem,
                     (args.host, args.port, prof["fhem_cmd_period"], args.unit)))

    # Heap monitor runs independently through the cooldown phase.
    mon = threading.Thread(target=worker_heap_monitor, args=(args.host, args.port),
                           daemon=True)
    mon.start()

    threads = []
    ramp_gap = (args.ramp / len(jobs)) if (args.ramp and jobs) else 0.0

    print(f"Starting {len(jobs)} worker thread(s)"
          f"{' with ramp' if ramp_gap else ''} …")
    t_start = time.monotonic()
    for fn, fargs in jobs:
        t = threading.Thread(target=fn, args=fargs, daemon=True)
        t.start()
        threads.append(t)
        if ramp_gap:
            time.sleep(ramp_gap)

    # Active phase
    try:
        while True:
            elapsed = time.monotonic() - t_start
            if elapsed >= args.duration:
                break
            ok, err, heap = stats.snapshot()
            heap_str = f"  heap={heap//1024}KB" if heap else ""
            print(f"  [{elapsed:5.0f}s / {args.duration}s active]"
                  f"  ok={ok}  err={err}{heap_str}", end="\r", flush=True)
            time.sleep(min(3, max(0.5, args.duration - elapsed)))
    except KeyboardInterrupt:
        print("\nInterrupted — stopping load.")

    print("\nStopping active load …")
    stop_evt.set()
    for t in threads:
        t.join(timeout=5)

    duration_s = time.monotonic() - t_start
    heap_before_cd = stats.snapshot()[2]

    # Cooldown: no load, keep sampling heap to see whether it recovers.
    if args.cooldown > 0:
        print(f"Cooldown {args.cooldown}s — watching heap recovery (load stopped) …")
        cd_start = time.monotonic()
        while time.monotonic() - cd_start < args.cooldown:
            _, _, heap = stats.snapshot()
            heap_str = f"  heap={heap//1024}KB" if heap else ""
            print(f"  [cooldown {time.monotonic()-cd_start:4.0f}s / {args.cooldown}s]"
                  f"{heap_str}", end="\r", flush=True)
            time.sleep(3)
        print()

    _monitor_stop.set()
    mon.join(timeout=5)

    stats.report(duration_s)

    # Leak verdict: compare heap at end of cooldown against pre-test baseline.
    if stats.heap_samples:
        baseline = stats.heap_samples[0][0]
        final    = stats.heap_samples[-1][0]
        recovered = final >= baseline * 0.9
        print("Heap recovery verdict:")
        print(f"  baseline (pre-load) : {baseline//1024} KB")
        if heap_before_cd:
            print(f"  end of load         : {heap_before_cd//1024} KB")
        print(f"  after cooldown      : {final//1024} KB")
        if recovered:
            print("  => RECOVERED — no persistent leak detected.\n")
        else:
            lost = (baseline - final) // 1024
            print(f"  => NOT recovered — ~{lost} KB still missing after load"
                  f" stopped (likely leak / fragmentation).\n")


if __name__ == "__main__":
    main()
