#!/usr/bin/env python3
"""Build the web assets for one mode into dist/<mode>/.

  tools/make_dist.py companion     dist/companion/printer/... + js/ext/printer-switch/
  tools/make_dist.py standalone    dist/standalone/printer/... + index.html (redirect)
  tools/make_dist.py all

Every file is gzipped (level 9, mtime 0, so the same input gives the same
bytes) and stored as <name>.gz, which is what the board serves with
Content-Encoding: gzip and what the parent's tools/upload_fw_assets.py
uploads. Nothing is minified. The parent's web/js/wsapi.js and web/js/fs.js
are copied from the btgw submodule into printer/js/btgw/.
"""
import argparse, gzip, io, os, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
PARENT_JS = os.path.join(ROOT, "btgw", "web", "js")
EXT = os.path.join(ROOT, "ext", "printer-switch")
PARENT_MODULES = ("wsapi.js", "fs.js")
SKIP_DIRS = {"node_modules", "tests", "__pycache__"}
SKIP_EXT = {".md", ".map"}
SEG_MAX = 64          # LittleFS name limit on the board (src/fs/fs.h)
PATH_MAX = 128

REDIRECT = b"""<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="refresh" content="0; url=/printer/"><title>BTGW Printer</title></head>
<body><a href="/printer/">BTGW Printer</a></body></html>
"""


def gz(data):
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, compresslevel=9, mtime=0) as f:
        f.write(data)
    return buf.getvalue()


def put(out, rel, data, report):
    for seg in rel.split("/"):
        if len(seg) + 3 > SEG_MAX:
            sys.exit("%s: name too long for the board (%d > %d)" % (rel, len(seg) + 3, SEG_MAX))
    if len(rel) + 3 > PATH_MAX:
        sys.exit("%s: path too long for the board" % rel)
    dst = os.path.join(out, rel + ".gz")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    z = gz(data)
    with open(dst, "wb") as f:
        f.write(z)
    report.append((rel, len(data), len(z)))


def build(mode, dist_root):
    out = os.path.join(dist_root, mode)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    report = []
    for dirpath, dirs, files in os.walk(WEB):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if os.path.splitext(name)[1] in SKIP_EXT:
                continue
            src = os.path.join(dirpath, name)
            rel = "printer/" + os.path.relpath(src, WEB).replace(os.sep, "/")
            with open(src, "rb") as f:
                put(out, rel, f.read(), report)
    for m in PARENT_MODULES:
        src = os.path.join(PARENT_JS, m)
        if not os.path.exists(src):
            sys.exit("%s missing: run git submodule update --init" % src)
        with open(src, "rb") as f:
            put(out, "printer/js/btgw/" + m, f.read(), report)
    if mode == "standalone":
        put(out, "index.html", REDIRECT, report)
    else:
        with open(os.path.join(EXT, "index.js"), "rb") as f:
            put(out, "js/ext/printer-switch/index.js", f.read(), report)
    raw = sum(r[1] for r in report)
    packed = sum(r[2] for r in report)
    blocks = sum((r[2] + 4095) // 4096 for r in report)
    print("%s: %d files, %d B raw, %d B gzipped, %d KB in 4 KB flash blocks -> %s" %
          (mode, len(report), raw, packed, blocks * 4, os.path.relpath(out, ROOT)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("companion", "standalone", "all"))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    a = ap.parse_args()
    for m in (("companion", "standalone") if a.mode == "all" else (a.mode,)):
        build(m, a.out)


if __name__ == "__main__":
    main()
