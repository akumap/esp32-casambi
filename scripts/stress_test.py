#!/usr/bin/env python3
"""
ESP32 Casambi Web Server Stress Test
=====================================
Tests stability under high-frequency HTTP and WebSocket load, including
aborted connections, WebSocket connection churn, and concurrent requests.
Monitors device heap over time to detect memory leaks.

Usage:
    python3 scripts/stress_test.py --host <ESP32_IP> [options]

Options:
    --host HOST        ESP32 IP address (required)
    --port PORT        HTTP port (default: 80)
    --duration SECS    Test duration in seconds (default: 60)
    --concurrency N    Concurrent HTTP GET workers (default: 8)
    --ws-clients N     Concurrent WebSocket clients (default: 4)
    --skip-ws          Skip WebSocket tests
    --skip-abort       Skip aborted-connection tests (body-leak probing)
"""

import argparse
import base64
import collections
import http.client
import json
import random
import socket
import statistics
import struct
import sys
import threading
import time


# ---------------------------------------------------------------------------
# Thread-safe statistics
# ---------------------------------------------------------------------------

class Stats:
    def __init__(self):
        self._lock = threading.Lock()
        self.counts = collections.defaultdict(int)
        self.latencies = collections.defaultdict(list)
        self.heap_samples = []   # (free_heap, timestamp)

    def record(self, category, success, latency_ms=None):
        with self._lock:
            self.counts[f"{category}_{'ok' if success else 'err'}"] += 1
            if latency_ms is not None and success:
                self.latencies[category].append(latency_ms)

    def record_heap(self, free_heap):
        with self._lock:
            self.heap_samples.append((free_heap, time.time()))

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
                p95 = lats[int(len(lats) * 0.95)]
                lat_str = f"  p50={p50:.0f}ms  p95={p95:.0f}ms  max={lats[-1]:.0f}ms"
            print(f"  {cat:<34}  ok={ok:5d}  err={err:4d}{lat_str}")

        if self.heap_samples:
            first_heap = self.heap_samples[0][0]
            last_heap  = self.heap_samples[-1][0]
            min_heap   = min(s[0] for s in self.heap_samples)
            print(f"\nHeap (device) :"
                  f"  start={first_heap//1024} KB"
                  f"  end={last_heap//1024} KB"
                  f"  min={min_heap//1024} KB")
            drift = last_heap - first_heap
            if drift < -4096:
                print(f"  *** WARNING: Heap drifted by {drift//1024} KB"
                      f" — possible memory leak! ***")
            else:
                print("  Heap stable (no significant drift)")

        print()


stats    = Stats()
stop_evt = threading.Event()


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
        # 2xx and 4xx are both "handled gracefully"; only 5xx (or no response) counts as error
        return resp.status < 500, (time.monotonic() - t0) * 1000
    except Exception:
        return False, None


def http_post_abort(host, port, path, partial_body):
    """Send partial POST body then close TCP without completing it (leak probe)."""
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

        hdr = recv_exact(2)
        b1, b2 = hdr
        opcode  = b1 & 0x0F
        masked  = bool(b2 & 0x80)
        plen    = b2 & 0x7F
        if plen == 126:
            plen = struct.unpack("!H", recv_exact(2))[0]
        elif plen == 127:
            plen = struct.unpack("!Q", recv_exact(8))[0]
        mask    = recv_exact(4) if masked else None
        payload = recv_exact(plen)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload
    except socket.timeout:
        return None, None
    except Exception:
        return None, None


def _ws_send_close(s):
    try:
        s.sendall(bytes([0x88, 0x00]))  # FIN + close opcode, no payload
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Worker threads
# ---------------------------------------------------------------------------

def worker_get_flood(host, port):
    endpoints = [
        "/api/status", "/api/units", "/api/groups",
        "/api/scenes",  "/api/log?n=5", "/api/ntp",
    ]
    while not stop_evt.is_set():
        path = random.choice(endpoints)
        cat  = path.split("?")[0].split("/")[-1]
        ok, lat, _ = http_get(host, port, path)
        stats.record(f"GET/{cat}", ok, lat)


def worker_post_control(host, port):
    """POST unit/scene commands; 503 (BLE not connected) counts as ok."""
    cases = [
        ("/api/units/1/on",      b""),
        ("/api/units/1/off",     b""),
        ("/api/units/1/level",   b'{"level":128}'),
        ("/api/scenes/1/on",     b""),
        ("/api/groups/1/level",  b'{"level":200}'),
    ]
    while not stop_evt.is_set():
        path, body = random.choice(cases)
        ok, lat = http_post(host, port, path, body)
        stats.record("POST/control", ok, lat)
        time.sleep(0.05)


def worker_post_invalid(host, port):
    """Sends malformed / semantically wrong bodies — 4xx expected."""
    cases = [
        ("/api/units/1/level",       b"not json"),
        ("/api/units/1/level",       b'{"wrong":1}'),
        ("/api/units/1/color",       b'{"r":1}'),          # missing g, b
        ("/api/units/1/temperature", b'{"kelvin":500}'),   # out of range
        ("/api/ntp",                 b'{}'),               # missing server
        ("/api/ntp",                 b'{"server":""}'),    # empty server
    ]
    while not stop_evt.is_set():
        path, body = random.choice(cases)
        ok, lat = http_post(host, port, path, body)
        stats.record("POST/invalid", ok, lat)
        time.sleep(0.1)


def worker_post_oversize(host, port):
    """POSTs a body > 512 bytes — device must return 413, not crash."""
    body = b'{"level":128,"pad":"' + b"x" * 600 + b'"}'
    while not stop_evt.is_set():
        ok, lat = http_post(host, port, "/api/units/1/level", body)
        stats.record("POST/oversize", ok, lat)
        time.sleep(0.3)


def worker_post_abort(host, port):
    """Aborts connections mid-body to probe for the _tempObject memory leak."""
    partials = [
        b'{"level":',          # incomplete JSON
        b'{"le',               # even more partial
        b'',                   # empty — body accumulation never starts
    ]
    while not stop_evt.is_set():
        partial = random.choice(partials)
        ok = http_post_abort(host, port, "/api/units/1/level", partial)
        stats.record("POST/abort", ok)
        time.sleep(0.15)   # pace: avoid exhausting TCP port space


def worker_ws_churn(host, port):
    """Opens a WebSocket, reads the hello message, then closes it."""
    while not stop_evt.is_set():
        t0 = time.monotonic()
        s  = _ws_connect(host, port)
        if s is None:
            stats.record("WS/connect", False)
            time.sleep(0.2)
            continue
        stats.record("WS/connect", True, (time.monotonic() - t0) * 1000)

        opcode, payload = _ws_recv_frame(s, timeout=2)
        if opcode == 1:  # text frame
            try:
                msg = json.loads(payload)
                stats.record("WS/hello", msg.get("type") == "hello")
            except Exception:
                stats.record("WS/hello", False)

        _ws_send_close(s)
        s.close()
        time.sleep(0.05)


def worker_ws_persistent(host, port):
    """Holds a WebSocket open and counts received push messages."""
    while not stop_evt.is_set():
        s = _ws_connect(host, port)
        if s is None:
            stats.record("WS/persist_connect", False)
            time.sleep(1)
            continue
        stats.record("WS/persist_connect", True)

        while not stop_evt.is_set():
            opcode, payload = _ws_recv_frame(s, timeout=1)
            if opcode is None:
                break        # timeout → reconnect
            if opcode == 8:  # server-initiated close
                break
            if opcode == 1:  # text push
                stats.record("WS/push_msg", True)

        s.close()


def worker_heap_monitor(host, port):
    """Polls /api/status every few seconds to track device heap."""
    time.sleep(3)
    while not stop_evt.is_set():
        ok, _, body = http_get(host, port, "/api/status")
        if ok and body:
            try:
                data = json.loads(body)
                stats.record_heap(data["free_heap"])
            except Exception:
                pass
        time.sleep(4)


# ---------------------------------------------------------------------------
# Main
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
        print("Device reachable (no /api/info, setup portal?)")
        return True
    print(f"ERROR: Device at {host}:{port} is not reachable.")
    return False


def main():
    parser = argparse.ArgumentParser(
        description="ESP32 Casambi web server stress test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--host",        required=True,      help="ESP32 IP address")
    parser.add_argument("--port",        type=int, default=80)
    parser.add_argument("--duration",    type=int, default=60,
                        help="Test duration in seconds (default: 60)")
    parser.add_argument("--concurrency", type=int, default=8,
                        help="Concurrent GET workers (default: 8)")
    parser.add_argument("--ws-clients",  type=int, default=4,
                        help="WebSocket workers (default: 4)")
    parser.add_argument("--skip-ws",     action="store_true")
    parser.add_argument("--skip-abort",  action="store_true")
    args = parser.parse_args()

    print(f"\nESP32 Casambi — Web Server Stress Test")
    print(f"Target   : http://{args.host}:{args.port}/")
    print(f"Duration : {args.duration}s")
    print(f"Workers  : {args.concurrency} HTTP GET"
          f"  +POST"
          f"{'  +WS' if not args.skip_ws else ''}"
          f"{'  +abort' if not args.skip_abort else ''}\n")

    if not check_reachable(args.host, args.port):
        sys.exit(1)
    print()

    threads = []

    def add(target, *targs):
        t = threading.Thread(target=target, args=targs, daemon=True)
        threads.append(t)

    # GET flood
    for _ in range(max(1, args.concurrency // 2)):
        add(worker_get_flood, args.host, args.port)

    # POST control (expects 503 when BLE not connected)
    for _ in range(max(1, args.concurrency // 4)):
        add(worker_post_control, args.host, args.port)

    # POST invalid / oversize
    add(worker_post_invalid, args.host, args.port)
    add(worker_post_oversize, args.host, args.port)

    # Abort / body-leak probe
    if not args.skip_abort:
        for _ in range(max(1, args.concurrency // 4)):
            add(worker_post_abort, args.host, args.port)

    # WebSocket
    if not args.skip_ws:
        n_churn = max(1, args.ws_clients // 2)
        n_pst   = max(1, args.ws_clients - n_churn)
        for _ in range(n_churn):
            add(worker_ws_churn, args.host, args.port)
        for _ in range(n_pst):
            add(worker_ws_persistent, args.host, args.port)

    # Heap monitor
    add(worker_heap_monitor, args.host, args.port)

    print(f"Starting {len(threads)} worker threads …")
    for t in threads:
        t.start()

    t_start = time.monotonic()
    try:
        while True:
            elapsed = time.monotonic() - t_start
            if elapsed >= args.duration:
                break
            ok, err, heap = stats.snapshot()
            heap_str = f"  heap={heap//1024}KB" if heap else ""
            print(
                f"  [{elapsed:5.0f}s / {args.duration}s]"
                f"  ok={ok}  err={err}{heap_str}",
                end="\r", flush=True,
            )
            time.sleep(min(5, args.duration - elapsed))
    except KeyboardInterrupt:
        print("\nInterrupted.")

    print()
    stop_evt.set()
    for t in threads:
        t.join(timeout=4)

    duration_s = time.monotonic() - t_start
    stats.report(duration_s)

    # One final status poll to capture post-test heap
    ok, _, body = http_get(args.host, args.port, "/api/status")
    if ok and body:
        try:
            d = json.loads(body)
            print(f"Post-test device status:")
            print(f"  free_heap  : {d.get('free_heap', '?')} bytes")
            print(f"  uptime_ms  : {d.get('uptime_ms', '?')}")
            print(f"  ble_connected: {d.get('ble_connected', '?')}")
            print()
        except Exception:
            pass


if __name__ == "__main__":
    main()
