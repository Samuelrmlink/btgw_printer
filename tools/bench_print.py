#!/usr/bin/env python3
"""Bench print through printer_hub on a running gateway, without the web app.

Renders a test page the way the web app would (1 bit per dot, MSB first,
black = the model's black_bit), uploads it to printer/var/job.bin, then
printer.connect + printer.print, and follows printer.job / printer.state.

  tools/bench_print.py --driver nelko_pl70e --addr DD:0D:30:89:6C:C5 --media 4x6
  tools/bench_print.py ... --media 3x1 --density 10 --rows 120   (a short strip)
  tools/bench_print.py ... --pbm out.pbm --dry                    (render only)
"""
import argparse, json, os, sys, time, urllib.request
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "btgw", "tools"))
from wsapi import WS  # noqa: E402

MEDIA = {  # w_mm, h_mm, gap_mm
    "4x6": (101.6, 152.4, 3.0),
    "3x1": (76.2, 25.4, 3.0),
    "4x4": (101.6, 101.6, 3.0),
    "50x30": (50.0, 30.0, 2.0),
    "14x40": (14.0, 40.0, 2.0),
}


def render(wd, h, black_bit=0):
    """A border, a stripe ruler every 8 mm, a checker block, a diagonal and a
    dithered ramp. Returns packed rows."""
    wb = (wd + 7) // 8
    rows = bytearray()
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    for y in range(h):
        row = bytearray(wb)
        for x in range(wd):
            on = x < 4 or x >= wd - 4 or y < 4 or y >= h - 4
            on |= (y % 64) < 3 and x % 16 < 10                 # ruler ticks
            on |= 80 <= y < 200 and 40 <= x < 300 and ((x // 20) + (y // 20)) % 2 == 0
            on |= abs(x - y * wd // max(h, 1)) < 2              # diagonal
            if 220 <= y < 320:                                  # ramp, dark to the right
                on |= bayer[y % 4][x % 4] < (x * 16) // wd
            bit = (black_bit if on else 1 - black_bit)
            if bit:
                row[x // 8] |= 0x80 >> (x % 8)
        rows += row
    return bytes(rows), wb


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("BTGW_HOST", "10.255.255.23"))
    ap.add_argument("--driver", default="nelko_pl70e")
    ap.add_argument("--model", default="pl70e")
    ap.add_argument("--addr", required=False, default="DD:0D:30:89:6C:C5")
    ap.add_argument("--media", default="4x6", choices=sorted(MEDIA))
    ap.add_argument("--dots", type=int, default=832, help="bitmap width in dots")
    ap.add_argument("--dpi", type=int, default=203)
    ap.add_argument("--rows", type=int, help="bitmap height (default: the label height)")
    ap.add_argument("--density", type=int, default=8)
    ap.add_argument("--copies", type=int, default=1)
    ap.add_argument("--path", default="printer/var/job.bin")
    ap.add_argument("--pbm")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    w_mm, h_mm, gap = MEDIA[a.media]
    dots_per_mm = a.dpi / 25.4
    wd = min(a.dots, int(round(w_mm * dots_per_mm)) // 8 * 8)
    h = a.rows or int(h_mm * dots_per_mm)
    rows, wb = render(wd, h)
    print("bitmap %dx%d, %d bytes" % (wd, h, len(rows)))
    if a.pbm:
        with open(a.pbm, "wb") as f:
            f.write(b"P4\n%d %d\n" % (wb * 8, h) + bytes(b ^ 0xff for b in rows))
    if a.dry:
        return
    # sd/... is the card (/api/sd/files, parents made by the PUT); anything
    # else is flash, whose folder may need making first.
    if a.path.startswith("sd/"):
        url = "http://%s/api/sd/files/%s" % (a.host, a.path[3:])
    else:
        url = "http://%s/api/fs/%s" % (a.host, a.path)
        try:
            urllib.request.urlopen(urllib.request.Request("http://%s/api/fs/printer/var/" % a.host, method="PUT", data=b""), timeout=10)
        except Exception:
            pass
    r = urllib.request.urlopen(urllib.request.Request(url, method="PUT", data=rows), timeout=60)
    print("PUT", a.path, r.status)
    ws = WS(a.host)
    seq = [0]

    def op(name, params, timeout=15):
        seq[0] += 1
        ws.send({"id": seq[0], "op": name, "params": params})
        end = time.time() + timeout
        while time.time() < end:
            m = ws.recv(timeout=1)
            if not m:
                continue
            o = json.loads(m)
            if o.get("id") == seq[0]:
                return o
            if str(o.get("evt", "")).startswith("printer."):
                print("  ", m[:300])
        raise SystemExit("no reply to " + name)

    print(op("printer.connect", {"driver": a.driver, "addr": a.addr}))
    end = time.time() + 30
    while time.time() < end:
        st = op("printer.status", {"driver": a.driver})
        if '"connected"' in json.dumps(st):
            break
        time.sleep(1)
    print(st)
    job = op("printer.print", {
        "driver": a.driver, "addr": a.addr, "model": a.model, "path": a.path,
        "width_dots": wd, "rows": h, "origin": [0, 0],
        "media": {"w_mm": w_mm, "h_mm": h_mm, "gap_mm": gap, "type": "gap"},
        "density": a.density, "speed": -1, "copies": a.copies, "rotate": 0, "options": {}})
    print(job)
    end = time.time() + 240
    while time.time() < end:
        m = ws.recv(timeout=1)
        if not m:
            continue
        if '"printer.' in m:
            print("  ", m[:300])
            if '"printer.job"' in m and any(s in m for s in ('"done"', '"failed"', '"cancelled"')):
                break


if __name__ == "__main__":
    main()
