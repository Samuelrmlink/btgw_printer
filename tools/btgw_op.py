#!/usr/bin/env python3
"""Run one gateway op and print its reply plus the events for one address.

  tools/btgw_op.py gatt.tree '{"device":"DD:0D:30:89:6C:C5"}'
  tools/btgw_op.py gatt.write '{...}' --listen 3 --addr DD:0D:30:89:6C:C5

Events are filtered to --addr (and --evt prefixes) because a busy gateway
streams telemetry from every connected device."""
import argparse, json, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "btgw", "tools"))
from wsapi import WS  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("op"); ap.add_argument("params", nargs="?", default="{}")
ap.add_argument("--host", default=os.environ.get("BTGW_HOST", "10.255.255.23"))
ap.add_argument("--listen", type=float, default=0)
ap.add_argument("--addr"); ap.add_argument("--evt", action="append", default=[])
a = ap.parse_args()
ws = WS(a.host)
ws.send({"id": 1, "op": a.op, "params": json.loads(a.params)})
end = None
while True:
    t = (end - time.time()) if end else 15
    if t <= 0: break
    m = ws.recv(timeout=t)
    if m is None: break
    try: o = json.loads(m)
    except Exception: continue
    if o.get("id") == 1:
        print(json.dumps(o)); end = time.time() + a.listen
        if not a.listen: break
        continue
    if "evt" in o and end:
        d = o.get("data") or {}
        if a.addr and isinstance(d, dict) and d.get("addr") not in (None, a.addr): continue
        if a.evt and not any(o["evt"].startswith(p) for p in a.evt): continue
        if o["evt"] == "log" and not a.evt: continue
        print(json.dumps(o))
