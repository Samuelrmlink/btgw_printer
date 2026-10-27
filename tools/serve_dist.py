#!/usr/bin/env python3
"""Serve a built dist from this PC, with a real board as the backend.

  tools/serve_dist.py [companion|standalone] [--build] [--port 8097] [--bind 127.0.0.1]
                      [--board 10.255.255.23] [--board-port 80]

Serves dist/<mode>/printer/ at / exactly as the board would: each file is
stored as <name>.gz (tools/make_dist.py) and sent with Content-Encoding: gzip
and the Content-Type of <name>; <dir>/ is <dir>/index.html; a directory without
its trailing slash is a 301. Nothing is cached (Cache-Control: no-store).
mode.json comes from dist when it is there, else {"mode": <mode>}.

Open the printed URL: ?host=/&port= point the page's WebSocket and file API at
the board (btgw/web/js/fs.js, web/device.js), and the board answers CORS *.
For hardware testing while the board's /sd route cannot serve the app's
parallel module loads (parent-requirements R2). --build runs
tools/make_dist.py <mode> first; otherwise the dist must already exist.
"""
import argparse, json, mimetypes, os, posixpath, subprocess, sys, urllib.parse
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript", ".css": "text/css",
         ".json": "application/json", ".png": "image/png", ".svg": "image/svg+xml", ".txt": "text/plain; charset=utf-8",
         ".ico": "image/x-icon", ".woff2": "font/woff2"}


def ctype(name):
    ext = os.path.splitext(name)[1].lower()
    return TYPES.get(ext) or mimetypes.guess_type(name)[0] or "application/octet-stream"


def handler(top, mode):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            sys.stderr.write("%s %s\n" % (self.command, fmt % a))

        def reply(self, code, body=b"", typ="text/plain; charset=utf-8", extra=()):
            self.send_response(code)
            self.send_header("Content-Type", typ)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            path = posixpath.normpath(urllib.parse.unquote(url.path))
            if url.path.endswith("/") and path != "/":
                path += "/"
            rel = path.lstrip("/")
            if ".." in rel.split("/"):
                return self.reply(404, b"not found\n")
            disk = os.path.join(top, *rel.split("/")) if rel else top
            if os.path.isdir(disk):
                if not path.endswith("/"):
                    loc = path + "/" + ("?" + url.query if url.query else "")
                    return self.reply(301, b"", extra=[("Location", loc)])
                rel += "index.html"
                disk = os.path.join(disk, "index.html")
            name = posixpath.basename(rel)
            for f, gz in ((disk + ".gz", True), (disk, False)):
                if os.path.isfile(f):
                    with open(f, "rb") as h:
                        body = h.read()
                    return self.reply(200, body, ctype(name), [("Content-Encoding", "gzip")] if gz else [])
            if rel == "mode.json":
                return self.reply(200, json.dumps({"mode": mode}).encode(), "application/json")
            return self.reply(404, b"not found\n")
    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", choices=("companion", "standalone"), default="companion")
    ap.add_argument("--build", action="store_true", help="run tools/make_dist.py <mode> first")
    ap.add_argument("--dist", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--port", type=int, default=8097)
    ap.add_argument("--bind", default="127.0.0.1", help="0.0.0.0 to serve other machines too")
    ap.add_argument("--board", default="10.255.255.23", help="the board the page talks to")
    ap.add_argument("--board-port", type=int, default=80)
    a = ap.parse_args()
    if a.build:
        rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "make_dist.py"), a.mode, "--out", a.dist])
        if rc:
            sys.exit(rc)
    top = os.path.join(a.dist, a.mode, "printer")
    if not os.path.isfile(os.path.join(top, "index.html.gz")):
        sys.exit("%s has no index.html.gz: run tools/make_dist.py %s (or pass --build)" % (top, a.mode))
    srv = ThreadingHTTPServer((a.bind, a.port), handler(top, a.mode))
    host = "localhost" if a.bind in ("127.0.0.1", "0.0.0.0", "::") else a.bind
    print("serving %s on %s:%d" % (os.path.relpath(top, ROOT), a.bind, a.port))
    print("open http://%s:%d/?host=%s&port=%d" % (host, a.port, a.board, a.board_port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
