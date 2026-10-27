#!/usr/bin/env python3
"""A mock BTGW board for developing and testing the printer app.

Serves web/ at /printer/ (and the parent's wsapi.js / fs.js at
/printer/js/btgw/, from the btgw submodule, as tools/make_dist.py does), and
plays the parts of the board the app uses:

  /ws                 sys.info, printer.* (printer_hub), wg.*, mdns.status,
                      plus test.* ops for the e2e suite
  /api/fs/...         an in-memory LittleFS (GET file/dir, PUT, DELETE,
                      PUT dir/, POST /api/fs/rename)
  /api/sd, /api/sd/files/..., /api/sd/rename   an in-memory card
  /api/wifi...        the parent's Wi-Fi routes

  python3 dev/mockserver.py --port 8096 [--mode companion|standalone] [--no-sd] [--no-hub] [--flash-readonly]
                            [--max-concurrent N [--busy-503]] [--static-delay-ms MS]

PEX: printer_hub and every driver in models.json are installed and running,
or (--pex-stopped) installed and not loaded, as after a board boot. The hub's
printer.* ops answer only while printer_hub runs, and printer.drivers lists
only the drivers that run. pex.list / installed / load / start / stop /
unload work on that; --pex-pool-drivers N makes a driver's pex.load fail
with the board's out-of-pool message while N drivers are loaded. The page's
app.json lists the same modules, as deploy writes it.

--flash-readonly answers every PUT /api/fs/... with 500 "write failed", like
the bench board's LittleFS (parent bug B3).
--max-concurrent N answers a static GET (not /api, not /ws) beyond N in
flight with 409 {"ok":false,"error":"busy"} and Connection: close, like the
board's HTTP connection limit (--busy-503: the older /sd queue's 503 "card
busy" with Retry-After: 1); --static-delay-ms holds each static GET that
long, so requests overlap.

The fake printers come from web/presets/models.json: one per model, found by
printer.scan. The scan also plays nelko_tspl's late names: LATE is reported
unnamed, then again with its name and model a second later; the PL70e's
address is reported unnamed by nelko_tspl, then gone. printer.print checks the bitmap file is exactly rows × row
bytes and reports printer.job events like the hub does.
"""
import argparse, base64, hashlib, json, os, socket, struct, threading, time, urllib.parse
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
PARENT_JS = os.path.join(ROOT, "btgw", "web", "js")
EXT = os.path.join(ROOT, "ext")
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OPTS = {"mode": "companion", "sd": True, "hub": True, "flash_ro": False, "max_static": 0, "static_delay": 0.0, "busy_503": False,
        "pex_stopped": False, "pool_drivers": 0}
HUB = "printer_hub"
POOL_MSG = "out of PEX pool memory: the file is 15128 bytes; the PEX pool has 12800 in one piece"
STATIC_IN_FLIGHT = [0]
LOCK = threading.RLock()
CLIENTS = []   # (send fn)

MIME = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript", ".css": "text/css",
        ".json": "application/json", ".png": "image/png", ".svg": "image/svg+xml", ".txt": "text/plain"}


def models():
    with open(os.path.join(WEB, "presets", "models.json")) as f:
        return json.load(f)["drivers"]


# Fixed fake addresses per model (the e2e suite names them), whatever the
# driver grouping in models.json; a new model gets the next free one.
ADDR_ORDER = ["pm220", "pl70e", "p21", "d30", "virtual"]


# A PM220 whose Bluetooth name arrives late (nelko_tspl reports it twice).
LATE = {"driver": "nelko_tspl", "addr": "AA:BB:CC:00:10:10", "name": "PM220", "rssi": -71, "model": "pm220", "transport": "classic"}


def pex_names():
    return [HUB] + [d["driver"] for d in models()]


def running(name):
    return STATE["pex"].get(name) == "running"


def app_json():
    return {"label": "Printer", "name": "Label printer", "entry": "index.html",
            "pex": [{"name": n, "file": "pex/%s.m33.pex" % n} for n in pex_names()]}


def fake_devices():
    out = [dict(LATE)] if STATE.get("late_named") and running(LATE["driver"]) else []
    flat = [(d, m) for d in models() for m in d["models"]]
    extra = [m["id"] for d, m in flat if m["id"] not in ADDR_ORDER]
    for d, m in flat:
        i = (ADDR_ORDER + extra).index(m["id"])
        if running(d["driver"]):
            out.append({"driver": d["driver"], "addr": "AA:BB:CC:00:%02X:%02X" % (i, i), "name": "%s %s" % (m.get("vendor", ""), m["name"]),
                        "rssi": -50 - i * 3, "model": m["id"], "transport": m.get("transport", "")})
    return out


def reset_state():
    with LOCK:
        STATE.clear()
        STATE.update({
            "fs": {},
            "sd": {} if OPTS["sd"] else None,
            "printers": {},     # addr -> {state, driver, model, battery}
            "jobs": [],         # every printer.print, with the bitmap's sha256
            "next_job": 1,
            "fail_next": None,
            "print_error": None,   # {"code","message"}: the next printer.print answers that error
            "job_hang": False,     # printer.print takes the job, then the hub drops it without a word
            "job_ms": 30,
            "wifi": {"mode": "sta", "want": "auto", "ssid": "BenchNet", "ip": "10.0.0.23", "ap": False, "ap_ssid": "BTGW-Printer",
                     "ap_psk": "printer123", "ap_clients": 0, "error": ""},
            "known": [{"ssid": "BenchNet", "psk": "secret", "current": True}],
            "wg": {"configured": True, "connected": True, "confirmed": True, "address": "10.255.255.23",
                   "endpoint": "203.0.113.5", "endpoint_port": 51820, "last_handshake_age_ms": 4000, "rx_bytes": 1000, "tx_bytes": 900},
            "cancel": set(),
            "late_named": False,
            "connects": [],        # every printer.connect, for the e2e suite
            "pex": {n: (None if OPTS["pex_stopped"] else "running") for n in pex_names()},   # None: installed, not loaded
            "pex_ops": [],         # every pex.load / start / stop / unload, as "op name"
            "pool_drivers": OPTS["pool_drivers"],   # LATE is in printer.devices once a scan has named it
        })
        if not OPTS["hub"]:
            STATE["pex"][HUB] = "absent"
        STATE["fs"]["printer/mode.json"] = json.dumps({"mode": OPTS["mode"]}).encode()
        STATE["fs"]["cfg/wireguard.json"] = json.dumps({"private_key": "x" * 44, "peer_public_key": "y" * 44, "address": "10.255.255.23",
                                                        "endpoint": "203.0.113.5", "endpoint_port": 51820, "keepalive": 25}).encode()


STATE = {}


def broadcast(evt, data):
    for send in list(CLIENTS):
        try:
            send({"evt": evt, "data": data})
        except OSError:
            pass


class ApiError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code, self.msg = code, msg


def fs_get(path):
    if path.startswith("sd/"):
        if STATE["sd"] is None:
            return None
        return STATE["sd"].get(path[3:])
    return STATE["fs"].get(path)


def op(name, p):
    S = STATE
    if name == "sys.info":
        mods = ["sys", "fs", "bt", "wifi", "wg", "mdns", "pex", "ota"]
        if S["sd"] is not None:
            mods.append("sd")
        return {"firmware": "bluetooth_rp2_webapp", "board": "pico2_w", "modules": mods, "transports": ["le", "classic"]}
    if name == "mdns.status":
        return {"hostname": "btgw-printer", "groups": []}
    if name == "wg.status":
        return dict(S["wg"])
    if name == "wg.up":
        S["wg"]["connected"] = True
        return {"up": True}
    if name == "wg.down":
        S["wg"]["connected"] = False
        return {"up": False}
    if name.startswith("test."):
        return test_op(name, p)
    if name.startswith("pex."):
        return pex_op(name, p)
    if name.startswith("printer."):
        if not running(HUB):
            raise ApiError("unknown_op", "no such op: " + name)
        return printer_op(name, p)
    raise ApiError("unknown_op", "no such op: " + name)


def test_op(name, p):
    S = STATE
    if name == "test.reset":
        OPTS["hub"] = OPTS.get("hub_default", True)
        reset_state()
        return {"ok": True}
    if name == "test.jobs":
        return {"jobs": S["jobs"]}
    if name == "test.connects":
        return {"connects": S["connects"]}
    if name == "test.pex_ops":
        return {"ops": S["pex_ops"]}
    if name == "test.set":
        for k in ("fail_next", "job_ms", "print_error", "job_hang"):
            if k in p:
                S[k] = p[k]
        if "hub" in p:
            OPTS["hub"] = bool(p["hub"])
            S["pex"][HUB] = "running" if p["hub"] else "absent"
        for n, st in (p.get("pex") or {}).items():   # {"name": "running" | "loaded" | null}
            S["pex"][n] = st
        if "pool_drivers" in p:
            S["pool_drivers"] = int(p["pool_drivers"])
        return {"ok": True}
    if name == "test.file":
        b = fs_get(p.get("path", ""))
        return {"exists": b is not None, "b64": base64.b64encode(b).decode() if b is not None else None}
    if name == "test.printer_state":
        st = S["printers"].setdefault(p["addr"], {"driver": p.get("driver", ""), "model": p.get("model", "")})
        st.update({k: v for k, v in p.items() if k != "addr"})
        broadcast("printer.state", {"addr": p["addr"], **st})
        return {"ok": True}
    raise ApiError("unknown_op", name)


def drivers_caps():
    return [{"driver": d["driver"], "abi": 1, "models": d["models"]} for d in models() if running(d["driver"])]


def pex_op(name, p):
    S = STATE
    P = S["pex"]
    n = p.get("name", "")
    if name == "pex.list":
        return {"arch": "m33", "up": True, "modules": [{"name": k, "state": v} for k, v in P.items() if v in ("loaded", "running")]}
    if name == "pex.installed":
        return {"mounted": True, "modules": [{"name": k, "loaded": v in ("loaded", "running")} for k, v in P.items() if v != "absent"]}
    if name not in ("pex.load", "pex.start", "pex.stop", "pex.unload"):
        raise ApiError("unknown_op", "no such op: " + name)
    S["pex_ops"].append("%s %s" % (name, n))
    if n not in P or P[n] == "absent":
        raise ApiError("not_installed", "%s is not installed" % n)
    if name == "pex.load":
        if P[n] is not None:
            raise ApiError("already_loaded", "%s is already loaded" % n)
        loaded_drivers = [k for k, v in P.items() if k != HUB and v in ("loaded", "running")]
        if n != HUB and S["pool_drivers"] and len(loaded_drivers) >= S["pool_drivers"]:
            raise ApiError("no_memory", POOL_MSG)
        P[n] = "loaded"
    elif name == "pex.start":
        if P[n] is None:
            raise ApiError("not_loaded", "%s is not loaded" % n)
        if P[n] == "running":
            raise ApiError("already_running", "%s is already running" % n)
        P[n] = "running"
    elif name == "pex.stop":
        if P[n] != "running":
            raise ApiError("not_running", "%s is not running" % n)
        P[n] = "loaded"
    elif name == "pex.unload":
        if P[n] == "running":
            raise ApiError("busy", "%s is running: stop it first" % n)
        if P[n] is None:
            raise ApiError("not_loaded", "%s is not loaded" % n)
        P[n] = None
    return {"ok": True, "name": n}


def printer_op(name, p):
    S = STATE
    if name == "printer.drivers":
        return {"drivers": drivers_caps()}
    if name == "printer.scan":
        started = [d["driver"] for d in models() if running(d["driver"]) and p.get("driver") in (None, "", d["driver"])]
        tspl = "nelko_tspl" in started
        pl70e = {"addr": "AA:BB:CC:00:01:01", "rssi": -53}

        def later():
            if tspl:
                broadcast("printer.found", dict(LATE, name="", model=None))
                broadcast("printer.found", {"driver": "nelko_tspl", "addr": pl70e["addr"], "name": "", "rssi": pl70e["rssi"],
                                            "model": None, "transport": "classic"})
            for dev in fake_devices():
                if dev["addr"] == LATE["addr"] or dev["driver"] not in started:
                    continue
                time.sleep(0.05)
                broadcast("printer.found", dev)
            if tspl:
                time.sleep(0.8)
                broadcast("printer.found", {"driver": "nelko_tspl", "addr": pl70e["addr"], "name": "PL70e-BT", "gone": True})
                STATE["late_named"] = True
                broadcast("printer.found", dict(LATE))
        threading.Thread(target=later, daemon=True).start()
        return {"started": started}
    if name == "printer.devices":
        return {"devices": fake_devices()}
    if name == "printer.connect":
        addr, drv = p.get("addr"), p.get("driver")
        if not addr or not drv:
            raise ApiError("bad_request", "driver and addr required")
        if not running(drv):
            raise ApiError("no_driver", "no driver %s" % drv)
        S["connects"].append({"driver": drv, "addr": addr})
        st = S["printers"].setdefault(addr, {})
        st.update({"driver": drv, "model": (p.get("config") or {}).get("model", ""), "state": "connecting", "battery": 87})

        def up():
            time.sleep(0.15)
            st["state"] = "connected"
            broadcast("printer.state", {"driver": drv, "addr": addr, "state": "connected"})
            broadcast("printer.status", {"driver": drv, "addr": addr, "battery": 87, "paper": True})
        broadcast("printer.state", {"driver": drv, "addr": addr, "state": "connecting"})
        threading.Thread(target=up, daemon=True).start()
        return {"pending": True}
    if name == "printer.disconnect":
        for addr, st in S["printers"].items():
            if not p.get("addr") or p["addr"] == addr:
                st["state"] = "idle_disconnected"
                st["state"] = "disconnected"
                broadcast("printer.state", {"driver": st.get("driver"), "addr": addr, "state": "disconnected"})
        return {"ok": True}
    if name == "printer.status":
        cur = next((j for j in S["jobs"] if j["state"] in ("queued", "sending") and not j.get("dropped")), None)
        job = {"job": cur["job"], "state": cur["state"], "driver": cur["params"].get("driver")} if cur else None
        return {"printers": [{"addr": a, **st} for a, st in S["printers"].items()], "job": job}
    if name == "printer.cancel":
        S["cancel"].add(p.get("job"))
        S["cancel"].add(None)
        return {"ok": True}
    if name == "printer.print":
        addr = p.get("addr")
        st = S["printers"].get(addr)
        if not st or st.get("state") not in ("connected", "idle"):
            raise ApiError("not_connected", "printer not connected")
        if S["print_error"]:
            e, S["print_error"] = S["print_error"], None
            raise ApiError(e.get("code", "error"), e.get("message", "failed"))
        w, rows = int(p.get("width_dots", 0)), int(p.get("rows", 0))
        data = fs_get(p.get("path", ""))
        if data is None:
            raise ApiError("bad_request", "no such job file: %s" % p.get("path"))
        rb = (w + 7) // 8
        if w <= 0 or rows <= 0 or len(data) != rb * rows:
            raise ApiError("bad_request", "job file is %d bytes, expected %d × %d" % (len(data), rb, rows))
        jid = S["next_job"]
        S["next_job"] += 1
        rec = {"job": jid, "params": p, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
               "b64": base64.b64encode(data).decode() if len(data) <= 200000 else None, "state": "queued"}
        S["jobs"].append(rec)
        fail = S["fail_next"]
        S["fail_next"] = None
        copies = max(1, int(p.get("copies", 1)))
        if S["job_hang"]:
            rec["dropped"] = True   # queued, then gone: no printer.job, not in printer.status
            return {"job": jid}

        def run():
            total = len(data)
            steps = 4
            for c in range(copies):
                for i in range(1, steps + 1):
                    time.sleep(S["job_ms"] / 1000.0)
                    if jid in S["cancel"] or None in S["cancel"]:
                        S["cancel"].discard(None)
                        rec["state"] = "cancelled"
                        broadcast("printer.job", {"job": jid, "state": "cancelled", "copy": c, "copies": copies, "sent": 0, "total": total})
                        return
                    if fail and i == 2:
                        rec["state"] = "failed"
                        broadcast("printer.job", {"job": jid, "state": "failed", "message": fail, "copy": c, "copies": copies, "sent": 0, "total": total})
                        return
                    broadcast("printer.job", {"job": jid, "state": "sending", "copy": c, "copies": copies,
                                              "sent": total * i // steps, "total": total})
            rec["state"] = "done"
            broadcast("printer.job", {"job": jid, "state": "done", "copy": copies - 1, "copies": copies, "sent": total, "total": total})
        broadcast("printer.job", {"job": jid, "state": "queued", "copy": 0, "copies": copies, "sent": 0, "total": len(data)})
        threading.Thread(target=run, daemon=True).start()
        return {"job": jid}
    raise ApiError("unknown_op", name)


def ws_send(sock, text):
    b = text.encode()
    if len(b) < 126:
        hdr = struct.pack("!BB", 0x81, len(b))
    elif len(b) < 1 << 16:
        hdr = struct.pack("!BBH", 0x81, 126, len(b))
    else:
        hdr = struct.pack("!BBQ", 0x81, 127, len(b))
    sock.sendall(hdr + b)


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        c = sock.recv(n - len(data))
        if not c:
            return None
        data += c
    return data


def ws_recv(sock):
    h = recv_exact(sock, 2)
    if not h:
        return None
    ln = h[1] & 0x7F
    if ln == 126:
        ln = struct.unpack("!H", recv_exact(sock, 2))[0]
    elif ln == 127:
        ln = struct.unpack("!Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if h[1] & 0x80 else None
    data = recv_exact(sock, ln) if ln else b""
    if data is None:
        return None
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    if (h[0] & 0x0F) == 0x8:
        return None
    return data.decode(errors="replace")


def list_dir(store, d):
    prefix = d.strip("/") + "/" if d.strip("/") else ""
    files, dirs = [], set()
    for k, v in store.items():
        if not k.startswith(prefix):
            continue
        rest = k[len(prefix):]
        if "/" in rest:
            dirs.add(rest.split("/", 1)[0])
        elif rest:
            files.append({"name": rest, "size": len(v)})
    for k in list(store.keys()):
        pass
    return [{"name": n, "dir": True} for n in sorted(dirs)] + sorted(files, key=lambda f: f["name"])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        # headers and body go out as two writes: without this, Nagle + delayed
        # ACK hold every reply ~40 ms, which the --static-delay-ms runs then
        # cannot tell from their own delay
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, fmt, *a):
        if os.environ.get("MOCK_VERBOSE"):
            super().log_message(fmt, *a)

    def _reply(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass   # the page went away mid-reply (a reload in a test)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _path(self):
        return urllib.parse.unquote(self.path.split("?", 1)[0])

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = self._path()
        if path == "/ws":
            return self._ws()
        if path.startswith("/api/"):
            return self._api("GET", path)
        with LOCK:
            STATIC_IN_FLIGHT[0] += 1
            busy = OPTS["max_static"] and STATIC_IN_FLIGHT[0] > OPTS["max_static"]
        try:
            if busy and OPTS["busy_503"]:
                return self._reply(503, {"ok": False, "error": "card busy"}, extra={"Retry-After": "1"})
            if busy:
                self.close_connection = True
                return self._reply(409, {"ok": False, "error": "busy"}, extra={"Connection": "close"})
            if OPTS["static_delay"]:
                time.sleep(OPTS["static_delay"])
            return self._static(path)
        finally:
            with LOCK:
                STATIC_IN_FLIGHT[0] -= 1

    def do_PUT(self):
        return self._api("PUT", self._path())

    def do_POST(self):
        return self._api("POST", self._path())

    def do_DELETE(self):
        return self._api("DELETE", self._path())

    def _static(self, path):
        if path == "/":
            if OPTS["mode"] == "standalone":
                return self._reply(302, b"", "text/plain", {"Location": "/printer/"})
            return self._reply(200, PARENT_STUB, "text/html; charset=utf-8")
        if path.startswith("/js/ext/printer-switch/"):
            f = os.path.join(EXT, "printer-switch", path[len("/js/ext/printer-switch/"):])
            return self._file(f)
        if path == "/printer":
            return self._reply(301, b"", "text/plain", {"Location": "/printer/"})
        if path == "/printer/app.json":
            return self._reply(200, app_json())
        if path.startswith("/printer/js/btgw/"):
            return self._file(os.path.join(PARENT_JS, path[len("/printer/js/btgw/"):]))
        if path.startswith("/printer/"):
            rel = path[len("/printer/"):] or "index.html"
            return self._file(os.path.join(WEB, rel))
        return self._reply(404, {"ok": False, "error": "not found"})

    def _file(self, f):
        f = os.path.realpath(f)
        if not (f.startswith(os.path.realpath(ROOT))) or not os.path.isfile(f):
            return self._reply(404, {"ok": False, "error": "not found"})
        with open(f, "rb") as h:
            data = h.read()
        return self._reply(200, data, MIME.get(os.path.splitext(f)[1], "application/octet-stream"))

    def _api(self, method, path):
        with LOCK:
            try:
                return self._api_locked(method, path)
            except ApiError as e:
                return self._reply(400, {"ok": False, "error": e.msg})

    def _api_locked(self, method, path):
        S = STATE
        if path == "/api/op" and method == "POST":
            msg = json.loads(self._body() or b"{}")
            try:
                return self._reply(200, {"id": msg.get("id", 0), "ok": True, "result": op(msg.get("op", ""), msg.get("params") or {})})
            except ApiError as e:
                return self._reply(200, {"id": msg.get("id", 0), "ok": False, "error": {"code": e.code, "message": e.msg}})
        if path.startswith("/api/wifi"):
            return self._wifi(method, path)
        if path.startswith("/api/sd"):
            return self._sd(method, path)
        if path == "/api/fs/rename" and method == "POST":
            b = json.loads(self._body() or b"{}")
            if b.get("from") not in S["fs"]:
                return self._reply(404, {"ok": False, "error": "not found"})
            S["fs"][b["to"]] = S["fs"].pop(b["from"])
            return self._reply(200, {"ok": True})
        if path.startswith("/api/fs/") and method == "PUT" and OPTS["flash_ro"]:
            self._body()
            return self._reply(500, {"ok": False, "error": "write failed"})
        if path == "/api/fs" or path.startswith("/api/fs/"):
            rel = path[len("/api/fs/"):] if path.startswith("/api/fs/") else ""
            return self._store(S["fs"], method, rel, flash=True)
        return self._reply(404, {"ok": False, "error": "not found"})

    def _store(self, store, method, rel, flash):
        if method == "GET":
            if rel in store:
                return self._reply(200, store[rel], "application/octet-stream")
            d = rel.rstrip("/")
            entries = list_dir(store, d)
            if entries or d == "" or any(k.startswith(d + "/") for k in store):
                out = {"ok": True, "files": entries}
                if flash:
                    out["block"] = 4096
                return self._reply(200, out)
            return self._reply(404, {"ok": False, "error": "not found"})
        if method == "PUT":
            body = self._body()
            if rel.endswith("/"):
                return self._reply(200, {"ok": True})
            if flash and len(body) > 1024 * 1024:
                return self._reply(413, {"ok": False, "error": "too large"})
            store[rel] = body
            return self._reply(200, {"ok": True, "size": len(body)})
        if method == "DELETE":
            if rel not in store:
                return self._reply(404, {"ok": False, "error": "not found"})
            del store[rel]
            return self._reply(200, {"ok": True})
        return self._reply(400, {"ok": False, "error": "bad method"})

    def _sd(self, method, path):
        S = STATE
        if S["sd"] is None:
            return self._reply(404, {"ok": False, "error": "not found"})
        if path == "/api/sd":
            return self._reply(200, {"ok": True, "present": True, "mounted": True, "state": "ok", "fs": "exfat",
                                     "card": {"bytes": 32 * 10 ** 9, "type": "SDHC"}})
        if path == "/api/sd/rename" and method == "POST":
            b = json.loads(self._body() or b"{}")
            if b.get("from") not in S["sd"]:
                return self._reply(404, {"ok": False, "error": "not found"})
            S["sd"][b["to"]] = S["sd"].pop(b["from"])
            return self._reply(200, {"ok": True})
        if path == "/api/sd/files" or path.startswith("/api/sd/files/"):
            rel = path[len("/api/sd/files/"):] if path.startswith("/api/sd/files/") else ""
            return self._store(S["sd"], method, rel, flash=False)
        return self._reply(404, {"ok": False, "error": "not found"})

    def _wifi(self, method, path):
        S = STATE
        w = S["wifi"]
        if path == "/api/wifi":
            if method == "PUT":
                b = json.loads(self._body() or b"{}")
                if "mode" in b:
                    if b["mode"] not in ("auto", "sta", "ap", "both", "off"):
                        return self._reply(400, {"ok": False, "error": "invalid value"})
                    w["want"] = b["mode"]
                if "ap_ssid" in b:
                    w["ap_ssid"] = b["ap_ssid"]
                if "ap_psk" in b:
                    w["ap_psk"] = b["ap_psk"]
            return self._reply(200, {"ok": True, "driver": True, "scanning": False, **w})
        if path == "/api/wifi/scan":
            if method == "POST":
                return self._reply(200, {"ok": True, "scanning": True})
            return self._reply(200, {"ok": True, "scanning": False, "aps": [
                {"ssid": "BenchNet", "rssi": -48, "chan": 6, "auth": "wpa2", "known": True},
                {"ssid": "Cafe", "rssi": -70, "chan": 11, "auth": "open", "known": False}]})
        if path == "/api/wifi/networks":
            if method == "PUT":
                b = json.loads(self._body() or b"{}")
                k = next((x for x in S["known"] if x["ssid"] == b.get("ssid")), None)
                if k:
                    if "psk" in b:
                        k["psk"] = b["psk"]
                else:
                    S["known"].append({"ssid": b["ssid"], "psk": b.get("psk", ""), "current": False})
                return self._reply(200, {"ok": True})
            return self._reply(200, {"ok": True, "networks": S["known"]})
        if path == "/api/wifi/forget" and method == "POST":
            b = json.loads(self._body() or b"{}")
            S["known"] = [x for x in S["known"] if x["ssid"] != b.get("ssid")]
            return self._reply(200, {"ok": True})
        if path == "/api/wifi/connect" and method == "POST":
            b = json.loads(self._body() or b"{}")
            w["ssid"] = b.get("ssid", "")
            return self._reply(200, {"ok": True, "connecting": True})
        return self._reply(404, {"ok": False, "error": "not found"})

    def _ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            return self._reply(400, {"ok": False})
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc)
        self.end_headers()
        sock = self.connection
        lock = threading.Lock()

        def send(obj):
            with lock:
                ws_send(sock, json.dumps(obj))
        CLIENTS.append(send)
        try:
            while True:
                txt = ws_recv(sock)
                if txt is None:
                    break
                try:
                    msg = json.loads(txt)
                except ValueError:
                    continue
                mid = msg.get("id", 0)
                try:
                    with LOCK:
                        res = op(msg.get("op", ""), msg.get("params") or {})
                    send({"id": mid, "ok": True, "result": res})
                except ApiError as e:
                    send({"id": mid, "ok": False, "error": {"code": e.code, "message": e.msg}})
                except Exception as e:  # noqa
                    send({"id": mid, "ok": False, "error": {"code": "internal", "message": str(e)}})
        except OSError:
            pass
        finally:
            CLIENTS.remove(send)
        self.close_connection = True


# The parent UI, as far as the switch button cares: a #toolbar with a
# #settings-btn, and js/ext/printer-switch loaded the way extensions.js does.
PARENT_STUB = """<!doctype html><html><head><meta charset="utf-8"><title>BTGW (mock parent)</title></head>
<body><div id="toolbar"><span id="conn-status">mock</span><span class="spacer"></span>
<button class="tool-btn" id="settings-btn">Settings</button></div>
<script type="module">
import * as ext from "/js/ext/printer-switch/index.js";
const app = { addWindow() {}, windows: new Map() };
ext.register(app);
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8096)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--mode", choices=("companion", "standalone"), default="companion")
    ap.add_argument("--no-sd", action="store_true")
    ap.add_argument("--no-hub", action="store_true")
    ap.add_argument("--flash-readonly", action="store_true", help="PUT /api/fs/... answers 500 write failed")
    ap.add_argument("--max-concurrent", type=int, default=0, help="static GETs in flight past this answer 503 card busy")
    ap.add_argument("--pex-stopped", action="store_true", help="our PEX modules installed but not loaded (a board after boot)")
    ap.add_argument("--pex-pool-drivers", type=int, default=0, help="a driver's pex.load fails (pool full) while this many drivers are loaded")
    ap.add_argument("--busy-503", action="store_true", help="answer past --max-concurrent with 503 card busy (the old /sd queue)")
    ap.add_argument("--static-delay-ms", type=int, default=0, help="hold each static GET this long")
    a = ap.parse_args()
    OPTS.update(mode=a.mode, sd=not a.no_sd, hub=not a.no_hub, hub_default=not a.no_hub, flash_ro=a.flash_readonly,
                max_static=a.max_concurrent, static_delay=a.static_delay_ms / 1000.0, busy_503=a.busy_503,
                pex_stopped=a.pex_stopped, pool_drivers=a.pex_pool_drivers)
    reset_state()
    print("mock board on http://%s:%d/printer/ (%s%s%s%s)" % (a.bind, a.port, a.mode, "" if OPTS["sd"] else ", no SD",
                                                              "" if OPTS["hub"] else ", no printer_hub",
                                                              ", flash read-only" if OPTS["flash_ro"] else ""), flush=True)
    if OPTS["max_static"]:
        print("static GETs: at most %d at once (then %s), %d ms each" % (OPTS["max_static"], "503" if OPTS["busy_503"] else "409",
                                                                         a.static_delay_ms), flush=True)
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
