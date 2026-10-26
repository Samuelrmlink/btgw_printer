#!/usr/bin/env python3
"""Upload the web app for one mode to a board.

  tools/deploy.py companion  --base http://192.168.8.1
  tools/deploy.py standalone --base http://10.255.255.23 [--sync]
  tools/deploy.py companion  --base http://10.255.255.23 --target sd

Builds dist/<mode>/ (tools/make_dist.py) and PUTs every .gz to /api/fs/,
then writes printer/mode.json, which tells the app whether to show the
"BTGW" switch button.

--target sd puts the app on the micro SD card instead (PUT /api/sd/files/),
under --sd-dir (default apps/printer), with mode.json beside it. The files
stay gzipped, as on flash, so the board's GET /sd/<path> route has to serve
<path>.gz with Content-Encoding: gzip and <dir>/index.html for a folder
(docs/parent-requirements.md R2); the app opens at --sd-url (default
/sd/<sd-dir>/). Files of ours left over in that folder are deleted. The parent UI's switch button
(js/ext/printer-switch/) still has to live on flash, because the parent loads
extensions from there; it is rewritten to open --sd-url. Labels, printer
configs and job files are not moved by this; the app picks their store.

Companion mode never deletes anything: the parent UI and other projects'
files share the board's flash (the parent's upload_fw_assets.py --sync would
delete them). In standalone mode --sync removes stale .gz files, but only
the app's own (printer/**/*.gz outside printer/labels and printer/var, and
the root index.html.gz); labels, printers.json, media.json and cfg/ are never
touched.
"""
import argparse, gzip, json, os, subprocess, sys, urllib.error, urllib.parse, urllib.request

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


def fetch(base, path):
    try:
        with urllib.request.urlopen("%s/%s" % (base, path), timeout=30) as resp:
            return resp.read()
    except urllib.error.HTTPError:
        return None


def put(base, target, rel, data, dry):
    api = "api/sd/files" if target == "sd" else "api/fs"
    print("PUT", ("sd:/" if target == "sd" else "") + rel, "(%d B)" % len(data))
    if dry:
        return
    for attempt in range(3):
        try:
            req("PUT", "%s/%s/%s" % (base, api, enc(rel)), data)
            return
        except urllib.error.HTTPError:
            raise                     # the board answered: retrying won't help
        except (urllib.error.URLError, OSError) as e:
            if attempt == 2:
                raise
            print("  retry (%s)" % e)


def point_switch(data, url):
    """The parent UI's switch button, opening url instead of /printer/."""
    src = gzip.decompress(data).decode()
    old = 'const TARGET = "/printer/";'
    if old not in src:
        sys.exit("ext/printer-switch/index.js: TARGET line not found")
    return gzip.compress(src.replace(old, "const TARGET = %s;" % json.dumps(url)).encode(), 9, mtime=0)


def remote_sd(base, d):
    """Every file under d on the card (d is the app's own folder)."""
    try:
        r = req("GET", "%s/api/sd/files/%s" % (base, enc(d)))
    except urllib.error.HTTPError:
        return []
    out = []
    for f in r.get("files", []) if isinstance(r, dict) else []:
        p = d + "/" + f["name"]
        out += remote_sd(base, p) if f.get("dir") else [p]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("companion", "standalone"))
    ap.add_argument("--base", default="http://192.168.8.1", help="the board's URL")
    ap.add_argument("--sync", action="store_true", help="standalone only: delete the app's stale files")
    ap.add_argument("--target", choices=("flash", "sd"), default="flash", help="where the app's files go")
    ap.add_argument("--sd-dir", default="apps/printer", help="--target sd: the app's folder on the card")
    ap.add_argument("--sd-url", help="--target sd: the URL the board serves it at (default /sd/<sd-dir>/)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)   # keep warnings in order when piped
    if a.sync and a.target == "sd":
        sys.exit("--sync is for --target flash only")
    sd_dir = a.sd_dir.strip("/")
    sd_url = a.sd_url or "/sd/%s/" % sd_dir
    if a.sync and a.mode != "standalone":
        sys.exit("--sync is refused in companion mode: it would delete the parent UI's files")
    base = a.base.rstrip("/")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "make_dist.py"), a.mode])
    dist = os.path.join(ROOT, "dist", a.mode)
    local = []
    for dirpath, _, files in os.walk(dist):
        for n in sorted(files):
            local.append(os.path.relpath(os.path.join(dirpath, n), dist).replace(os.sep, "/"))
    def data(rel):
        with open(os.path.join(dist, rel), "rb") as f:
            return f.read()
    mode = json.dumps({"mode": a.mode}).encode()
    if a.target == "sd":
        sent = set()
        for rel in sorted(local):
            if rel.startswith("printer/"):
                name, body = rel[len("printer/"):], data(rel)
                put(base, "sd", sd_dir + "/" + name, body, a.dry_run)
                sent.add(sd_dir + "/" + name)
        put(base, "sd", sd_dir + "/mode.json", mode, a.dry_run)
        sent.add(sd_dir + "/mode.json")
        for p in remote_sd(base, sd_dir):
            if p not in sent:
                print("DELETE sd:/" + p)
                if not a.dry_run:
                    req("DELETE", "%s/api/sd/files/%s" % (base, enc(p)))
        # Off-card files: the switch button (flash only) and, in standalone,
        # the root redirect. Flash may be full or failing, so warn, don't stop.
        for rel in sorted(local):
            if rel.startswith("printer/"):
                continue
            body = data(rel)
            if rel == "js/ext/printer-switch/index.js.gz":
                body = point_switch(body, sd_url)
            elif rel == "index.html.gz":
                body = gzip.compress(gzip.decompress(body).replace(b"/printer/", sd_url.encode()), 9, mtime=0)
            try:
                put(base, "flash", rel, body, a.dry_run)
                # Read it back: a board with a failing LittleFS has answered
                # OK without keeping the file (parent-requirements B3).
                if not a.dry_run and fetch(base, "api/fs/" + enc(rel)) != body:
                    raise OSError("the board answered OK but the file is not there")
            except (urllib.error.URLError, OSError) as e:
                print("  warning: %s not written to flash (%s)" % (rel, e), file=sys.stderr)
        print("done: %s%s" % (base, sd_url))
        return
    for rel in sorted(local):
        put(base, "flash", rel, data(rel), a.dry_run)
    put(base, "flash", "printer/mode.json", mode, a.dry_run)
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
