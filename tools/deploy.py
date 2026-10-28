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
stay gzipped, as on flash; the board's GET /sd/<path> route serves <path>.gz
with Content-Encoding: gzip and <dir>/index.html for a folder. The app opens
at --sd-url (default /sd/<sd-dir>/). Files of ours left over in that folder
are deleted.

It also writes app.json there: the parent UI (bluetooth_rp2_webapp
web/js/launchers.js) turns every apps/<id>/app.json on the card into a
toolbar button with its "label", opening /sd/apps/<id>/. The modules named
by --pex are copied to <sd-dir>/pex/ for installing. With --target sd
nothing is written outside --sd-dir: no file on flash, nothing the parent
or another project owns.
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


def manifest(pex):
    """sd:/apps/printer/app.json. The parent reads "label" (its toolbar
    button); the other keys are ours and ignored by the parent."""
    with open(os.path.join(ROOT, "package.json")) as f:
        version = json.load(f).get("version", "0")
    return json.dumps({
        "label": "Printer", "name": "Label printer", "version": version,
        "description": "Design and print labels on Bluetooth label printers",
        "entry": "index.html", "icon": "icon.svg",
        "pex": [{"name": n, "file": "pex/%s.m33.pex" % n} for n in pex]}, indent=1).encode()


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
    ap.add_argument("--pex", default="printer_hub,nelko_tspl,nelko_ble,nelko_pl70e,phomemo_d30",
                    help="--target sd: modules to copy to <sd-dir>/pex/ and list in app.json "
                         "(from build/drivers/pex/; '' for none). printer_hub first: the app starts it; "
                         "it loads a driver only when a printer of that driver is scanned for or connected")
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
        # The app's registration and parts for the parent (R12): manifest,
        # icon and its modules.
        pex = [n for n in a.pex.split(",") if n]
        extra = {"app.json": manifest(pex)}
        with open(os.path.join(ROOT, "ext", "app-icon.svg"), "rb") as f:
            extra["icon.svg"] = f.read()
        for n in pex:
            src = os.path.join(ROOT, "build", "drivers", "pex", n + ".m33.pex")
            if not os.path.exists(src):
                sys.exit("%s not built: run tools/build.py drivers" % src)
            with open(src, "rb") as f:
                extra["pex/%s.m33.pex" % n] = f.read()
        for name, body in sorted(extra.items()):
            put(base, "sd", sd_dir + "/" + name, body, a.dry_run)
            sent.add(sd_dir + "/" + name)
        for p in remote_sd(base, sd_dir):
            if p not in sent:
                print("DELETE sd:/" + p)
                if not a.dry_run:
                    req("DELETE", "%s/api/sd/files/%s" % (base, enc(p)))
        if a.mode == "standalone":
            print("  note: the root redirect (index.html.gz) is not written with --target sd; "
                  "open %s directly" % sd_url)
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
