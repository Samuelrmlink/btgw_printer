#!/usr/bin/env python3
"""End to end in mcusim: the standalone btgw image, our printer_hub and driver
PEX modules, and mcusim's simulated printers (PM220 and D30 over Classic, P21 and
PL70e-BT over BLE). For each printer the job is built by the web app's own
layout and packing code (tools/e2e_job.mjs), uploaded to the board, printed
with printer.print, and the page the simulator printed is compared dot for
dot with the bitmap that was sent.

  tools/e2e_sim.py                       every printer
  tools/e2e_sim.py pm220 pl70e           some
  tools/e2e_sim.py --keep                leave the paper PNGs (prints the dir)

Needs: build/standalone (tools/build.py standalone), build/drivers
(tools/build.py drivers), and mcusim's feature/printer-sims build
($MCUSIM_DIR, default ~/Work/mcusim-agents/printer-sims, with
target/release/mcusim). Exits 77 (skipped) when mcusim or the image is
missing.
"""
import argparse, glob, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCUSIM = os.path.expanduser(os.environ.get("MCUSIM_DIR", "~/Work/mcusim-agents/printer-sims"))
SKIP = 77

# printer -> (sim spec, driver module, model, media preset, sim pane name prefix, connect config)
PRINTERS = {
# (the PM220 connect leaves the model to the inquiry name, the P21 one names it
# as a saved printer does. "ble_peer:" specs are raw BLE peers: the P21 is
# nelko_ble's, over GATT ff00 at the P21's own server MTU of 23)
    "pm220": ("pm220,pin=1234,addr=C0:55:00:00:01:20", "nelko_tspl", "pm220", "nelko-pm220-50x30", "C0:55:00:00:01:20", {}),
    "p21": ("ble_peer:p21,addr=C0:55:00:00:02:21,name=P21,mtu=23", "nelko_ble", "p21", "nelko-p21-14x40", "C0:55:00:00:02:21",
            {"model": "p21"}),
    "d30": ("d30,addr=C0:55:00:00:01:22", "phomemo_d30", "d30", "phomemo-d30-12x40", "C0:55:00:00:01:22", {}),
    "pl70e": ("pl70e,addr=C0:55:00:00:02:20", "nelko_pl70e", "pl70e", "nelko-4x6", "C0:55:00:00:02:20", {}),
}


def ble_peer_toml(spec, paper):
    """"p21,addr=..,mtu=23" as a raw [[device.ble_peer]] table: run_btgw.py's
    --printer puts a P21 on Classic, and nelko_ble wants it over BLE."""
    parts = spec.split(",")
    out = ["[[device.ble_peer]]", 'profile = "%s"' % parts[0], "paper_dir = %s" % json.dumps(paper)]
    for k, v in (p.split("=", 1) for p in parts[1:]):
        out.append("%s = %s" % (k, v if v.isdigit() else json.dumps(v)))
    return "\n".join(out) + "\n"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def put(port, path, data, timeout=120):
    req = urllib.request.Request("http://127.0.0.1:%d/api/fs/%s" % (port, path), data=data, method="PUT",
                                 headers={"Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status


class Board:
    def __init__(self, port, ws_mod):
        self.ws = ws_mod.WS("127.0.0.1", port, timeout=30)
        self.events = []

    def call(self, op, params=None, timeout=30):
        j = self.ws.call(op, params or {}, timeout=timeout, on_event=self.events.append)
        if not j.get("ok"):
            raise RuntimeError("%s %s -> %s" % (op, json.dumps(params), json.dumps(j.get("error"))))
        return j.get("result")

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            m = self.ws.recv(timeout=0.2)
            if m:
                try:
                    self.events.append(json.loads(m))
                except ValueError:
                    pass

    def wait_event(self, evt, pred, secs):
        end = time.time() + secs
        seen = 0
        while time.time() < end:
            while seen < len(self.events):
                e = self.events[seen]
                seen += 1
                if e.get("evt") == evt and pred(e.get("data") or {}):
                    return e["data"]
            self.pump(0.5)
        return None


def compare(png, expect, origin, read_png):
    """The page's burned dots against the bitmap placed at origin."""
    w, h, rows = read_png(png)
    ox, oy = origin
    bh = len(expect)
    bw = len(expect[0]) if bh else 0
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            by, bx = y - oy, x - ox
            want = 0 <= by < bh and 0 <= bx < bw and expect[by][bx] == "#"
            if rows[y][x] != want:
                bad += 1
                if first is None:
                    first = (x, y, rows[y][x], want)
    # bitmap dots that fall outside the printed page are lost: count them too
    for by in range(bh):
        for bx in range(bw):
            if expect[by][bx] == "#" and not (0 <= by + oy < h and 0 <= bx + ox < w):
                bad += 1
    return w, h, bad, first


def run_one(name, board, port, paper, read_png, log):
    spec, driver, model, media, addr, cfg = PRINTERS[name]
    pex_dir = os.path.join(ROOT, "build", "drivers", "pex")
    for mod in ("printer_hub", driver):
        f = os.path.join(pex_dir, mod + ".m33.pex")
        put(port, "pex/%s.m33.pex" % mod, open(f, "rb").read())
        board.call("pex.load", {"path": "pex/%s.m33.pex" % mod})
        board.call("pex.start", {"name": mod})
    try:
        board.pump(1.5)
        drivers = board.call("printer.drivers")
        assert any(d.get("driver") == driver for d in drivers.get("drivers", [])), "driver did not register: %s" % drivers
        board.call("printer.scan", {"ms": 6000, "driver": driver})
        board.wait_event("printer.found", lambda d: d.get("addr") == addr, 20)
        board.call("printer.connect", {"driver": driver, "addr": addr, "config": cfg})
        st = board.wait_event("printer.state", lambda d: d.get("addr") == addr and d.get("state") == "connected", 90)
        if not st:
            raise RuntimeError("%s never connected: %s" % (name, board.call("printer.status")))
        tmp = tempfile.mkdtemp()
        job_bin = os.path.join(tmp, "job.bin")
        out = subprocess.check_output(["node", os.path.join(ROOT, "tools", "e2e_job.mjs"), driver, model, media, job_bin])
        job = json.loads(out)
        put(port, "printer/var/job.bin", open(job_bin, "rb").read())
        shutil.rmtree(tmp)
        before = set(glob.glob(os.path.join(paper, "*.png")))
        params = dict(job["params"], driver=driver, addr=addr, path="printer/var/job.bin")
        jid = board.call("printer.print", params)["job"]
        done = board.wait_event("printer.job", lambda d: d.get("job") == jid and d.get("state") in ("done", "failed", "cancelled"), 240)
        if not done or done.get("state") != "done":
            raise RuntimeError("%s job %s: %s" % (name, jid, done))
        # the simulator writes the page once the job's PRINT (or raster block) is in
        png = None
        for _ in range(60):
            new = sorted(set(glob.glob(os.path.join(paper, "*.png"))) - before, key=os.path.getmtime)
            if new:
                png = new[-1]
                break
            board.pump(0.5)
        if not png:
            raise RuntimeError("%s: the simulator printed no page" % name)
        w, h, bad, first = compare(png, job["expect"], job["params"]["origin"], read_png)
        log("%-6s %s %dx%d bitmap at %s -> page %dx%d, %d dots differ%s" % (
            name, os.path.basename(png), job["params"]["width_dots"], job["params"]["rows"],
            job["params"]["origin"], w, h, bad, "" if not bad else " (first %s)" % (first,)))
        return bad == 0
    finally:
        try:
            board.call("printer.disconnect", {"driver": driver})
        except Exception:
            pass
        for mod in (driver, "printer_hub"):
            for op in ("pex.stop", "pex.unload"):
                try:
                    board.call(op, {"name": mod})
                except Exception:
                    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("printers", nargs="*", default=list(PRINTERS))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--build", default=os.path.join(ROOT, "build", "standalone"))
    a = ap.parse_args()
    binary = os.path.join(MCUSIM, "target", "release", "mcusim")
    if not os.path.exists(binary) or not os.path.exists(os.path.join(MCUSIM, "tools", "run_btgw.py")):
        print("e2e_sim: skipped, no mcusim printer-sims build at %s" % MCUSIM)
        return SKIP
    if not os.path.exists(os.path.join(a.build, "btgw.elf")):
        print("e2e_sim: skipped, no %s/btgw.elf (tools/build.py standalone)" % a.build)
        return SKIP
    if not glob.glob(os.path.join(ROOT, "build", "drivers", "pex", "*.pex")):
        rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "build.py"), "drivers"])
        if rc:
            return rc
    sys.path.insert(0, os.path.join(MCUSIM, "tools"))
    import btgw_ws  # noqa: E402
    import printer_e2e  # noqa: E402  (its PNG reader)

    paper = tempfile.mkdtemp(prefix="btgw-paper-")
    port = free_port()
    cmd = [sys.executable, os.path.join(MCUSIM, "tools", "run_btgw.py"), "--build", a.build,
           "--src", os.path.join(ROOT, "btgw"), "--sd", "none", "--port", str(port),
           "--paper-dir", paper, "--rtt", "file:" + os.path.join(paper, "console.log"), "-t", "7200s"]
    for n in a.printers:
        spec = PRINTERS[n][0]
        if spec.startswith("ble_peer:"):
            f = os.path.join(paper, "%s.toml" % n)
            with open(f, "w") as t:
                t.write(ble_peer_toml(spec[len("ble_peer:"):], paper))
            cmd += ["--peer-toml", f]
        else:
            cmd += ["--printer", spec]
    simlog = open(os.path.join(paper, "mcusim.log"), "w")
    p = subprocess.Popen(cmd, stdout=simlog, stderr=subprocess.PIPE, text=True)
    ready = threading.Event()
    lines = []

    def reader():
        for ln in p.stderr:
            lines.append(ln)
            if "btgw: ready" in ln:
                ready.set()
    threading.Thread(target=reader, daemon=True).start()
    ok = True
    try:
        # run_btgw.py's own readiness check polls /api/sd, which a standalone
        # image (no SD) never answers: wait for the API, join the fake AP and
        # wait for Bluetooth here instead.
        end = time.time() + 900
        up = False
        while time.time() < end and p.poll() is None and not ready.is_set():
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/api/fs/" % port, timeout=5).read()
                up = True
                break
            except Exception:
                time.sleep(2)
        if not up and not ready.is_set():
            print("e2e_sim: the gateway never came up\n" + "".join(l for l in lines[-60:] if "cyw43.led" not in l))
            return 1
        board = Board(port, btgw_ws)
        if not ready.is_set():
            board.call("wifi.connect", {"ssid": "mcusim", "psk": "mcusim123"})
        for _ in range(240):
            if (board.call("bt.state") or {}).get("ready"):
                break
            board.pump(0.5)
        for n in a.printers:
            try:
                ok &= run_one(n, board, port, paper, printer_e2e.read_png_gray, print)
            except Exception as e:
                ok = False
                print("%-6s FAILED: %s" % (n, e))
            sys.stdout.flush()
    finally:
        p.terminate()
        try:
            p.wait(15)
        except subprocess.TimeoutExpired:
            p.kill()
        if a.keep or not ok:
            print("e2e_sim: pages and console in", paper)
        else:
            shutil.rmtree(paper, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
