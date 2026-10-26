#!/usr/bin/env python3
"""Upload the web app for one mode to a board.

  tools/deploy.py companion  --base http://192.168.8.1
  tools/deploy.py standalone --base http://10.255.255.23 [--sync]

Builds dist/<mode>/ (tools/make_dist.py) and PUTs every .gz to /api/fs/,
then writes printer/mode.json, which tells the app whether to show the
"BTGW" switch button.

Companion mode never deletes anything: the parent UI and other projects'
files share the board's flash (the parent's upload_fw_assets.py --sync would
delete them). In standalone mode --sync removes stale .gz files, but only
the app's own (printer/**/*.gz outside printer/labels and printer/var, and
the root index.html.gz); labels, printers.json, media.json and cfg/ are never
touched.
"""
import argparse, json, os, subprocess, sys, urllib.error, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEEP = ("printer/labels/", "printer/var/")


def req(method, url, data=None, timeout=30):
    r = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        r.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        body = resp.read()
    try:
        return json.loads(body or b"{}")
    except ValueError:
        return body


def enc(path):
    return "/".join(urllib.parse.quote(p) for p in path.split("/"))


def remote_gz(base, d=""):
    """Every .gz under d on the board's flash."""
    try:
        r = req("GET", "%s/api/fs/%s" % (base, enc(d) + "/" if d else ""))
    except urllib.error.HTTPError:
        return []
    out = []
    for f in r.get("files", []):
        p = (d + "/" if d else "") + f["name"]
        if f.get("dir"):
            if p in ("cfg", "sd", "pex", "staged") or (p + "/").startswith(KEEP):
                continue
            out += remote_gz(base, p)
        elif p.endswith(".gz"):
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("companion", "standalone"))
    ap.add_argument("--base", default="http://192.168.8.1", help="the board's URL")
    ap.add_argument("--sync", action="store_true", help="standalone only: delete the app's stale files")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.sync and a.mode != "standalone":
        sys.exit("--sync is refused in companion mode: it would delete the parent UI's files")
    base = a.base.rstrip("/")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "make_dist.py"), a.mode])
    dist = os.path.join(ROOT, "dist", a.mode)
    local = []
    for dirpath, _, files in os.walk(dist):
        for n in sorted(files):
            local.append(os.path.relpath(os.path.join(dirpath, n), dist).replace(os.sep, "/"))
    for rel in sorted(local):
        print("PUT", rel)
        if not a.dry_run:
            with open(os.path.join(dist, rel), "rb") as f:
                req("PUT", "%s/api/fs/%s" % (base, enc(rel)), f.read())
    mode = json.dumps({"mode": a.mode}).encode()
    print("PUT printer/mode.json", mode.decode())
    if not a.dry_run:
        req("PUT", "%s/api/fs/printer/mode.json" % base, mode)
    if a.sync:
        have = set(local)
        stale = [p for p in remote_gz(base, "printer") if p not in have]
        for p in stale:
            print("DELETE", p)
            if not a.dry_run:
                req("DELETE", "%s/api/fs/%s" % (base, enc(p)))
    print("done: %s/printer/" % base)


if __name__ == "__main__":
    main()
