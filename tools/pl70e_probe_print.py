#!/usr/bin/env python3
"""Bench probe: send a small TSPL test job to a PL70e through the gateway's
gatt.write op (no PEX module involved). Used once to confirm the job format
before the driver existed.

  tools/pl70e_probe_print.py [--rows 200] [--density 8] [--dry]"""
import argparse, json, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "btgw", "tools"))
from wsapi import WS

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="10.255.255.23")
ap.add_argument("--addr", default="DD:0D:30:89:6C:C5")
ap.add_argument("--chr", default="3.1")
ap.add_argument("--rows", type=int, default=200)
ap.add_argument("--density", type=int, default=8)
ap.add_argument("--chunk", type=int, default=180)
ap.add_argument("--dry", action="store_true")
a = ap.parse_args()

W, H = 832, a.rows
wb = W // 8
bits = bytearray(b"\xff" * (wb * H))          # 1 = white, 0 = burn

def black(x, y):
    bits[y * wb + x // 8] &= ~(0x80 >> (x % 8)) & 0xff

for y in range(H):
    for x in range(W):
        border = x < 6 or x >= W - 6 or y < 6 or y >= H - 6
        stripe = (40 <= y < 60) and (x // 16) % 2 == 0
        ramp = 80 <= y < 140 and ((x * 17 + y * 31) % 832) < x   # dither ramp, darker to the right
        diag = abs(x - y * 4) < 3
        if border or stripe or ramp or diag:
            black(x, y)

job = (b"SIZE 101.6 mm,152.4 mm\r\nGAP 3 mm,0 mm\r\nDIRECTION 0,0\r\n"
       b"DENSITY %d\r\nCLS\r\n" % a.density)
job += b"BITMAP 0,40,%d,%d,1," % (wb, H) + bytes(bits) + b"\r\nPRINT 1,1\r\n"
print("job %d bytes" % len(job))
if a.dry:
    sys.exit(0)

ws = WS(a.host)
def op(o, p):
    ws.send({"id": 7, "op": o, "params": p})
    while True:
        m = ws.recv(timeout=10)
        if m is None:
            raise SystemExit("timeout on " + o)
        r = json.loads(m)
        if r.get("id") == 7:
            return r
        if r.get("evt") == "char.value" and r["data"].get("addr") == a.addr:
            print("notify", bytes.fromhex(r["data"]["value"]))

op("gatt.subscribe", {"device": a.addr, "id": "3.0"})
t0 = time.time()
for i in range(0, len(job), a.chunk):
    part = job[i:i + a.chunk]
    for attempt in range(50):
        r = op("gatt.write", {"device": a.addr, "id": a.chr, "value": part.hex(), "response": True})
        if r.get("ok"):
            break
        if r.get("error", {}).get("code") != "queue_full":
            raise SystemExit(json.dumps(r))
        time.sleep(0.05)
dt = time.time() - t0
print("sent in %.1f s (%.1f kB/s)" % (dt, len(job) / dt / 1000))
end = time.time() + 5
while time.time() < end:
    m = ws.recv(timeout=1)
    if m and '"char.value"' in m:
        r = json.loads(m)
        if r["data"].get("addr") == a.addr:
            print("notify", bytes.fromhex(r["data"]["value"]))
