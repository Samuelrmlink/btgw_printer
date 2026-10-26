#!/usr/bin/env python3
"""Run btgw_printer's tests.

  tools/run_tests.py              every tier that needs no board
  tools/run_tests.py drivers      native driver tests (fake host, gcc -m32)
  tools/run_tests.py pex          cross-build the .pex modules and dump them
  tools/run_tests.py web          web unit tests (node --test) and Playwright e2e on the mock board
  tools/run_tests.py sim          end to end in mcusim: standalone image + our modules + simulated
                                  printers (tools/e2e_sim.py; skipped without mcusim's printer-sims build)
Other suites (web) add themselves to SUITES.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("BTGW_PRINTER_BUILD", os.path.join(ROOT, "build"))


def drivers():
    return subprocess.call(["make", "-C", os.path.join(ROOT, "drivers", "test"),
                            "BUILD=" + os.path.join(BUILD, "test")])


def pex():
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "build.py"), "drivers",
                          "--build-dir", BUILD])
    if rc:
        return rc
    out = os.path.join(BUILD, "drivers", "pex")
    tool = os.path.join(ROOT, "btgw", "pex", "tools", "pexlink.py")
    for f in sorted(os.listdir(out)) if os.path.isdir(out) else []:
        if f.endswith(".pex"):
            print("==", f, os.path.getsize(os.path.join(out, f)), "bytes")
            rc |= subprocess.call([sys.executable, tool, "dump", os.path.join(out, f)],
                                  stdout=subprocess.DEVNULL)
    return rc


def web():
    rc = subprocess.call(["npm", "run", "-s", "test:unit"], cwd=ROOT)
    return rc | subprocess.call(["npx", "playwright", "test"], cwd=ROOT)


def sim():
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "e2e_sim.py")])
    if rc == 77:
        print("sim: skipped")
        return 0
    return rc


SUITES = {"drivers": drivers, "pex": pex, "web": web, "sim": sim}
# the default run: everything that needs no board and no simulator build
DEFAULT = ["drivers", "pex", "web"]


def main():
    names = sys.argv[1:] or DEFAULT
    rc = 0
    for n in names:
        if n not in SUITES:
            sys.exit("unknown suite %s (have %s)" % (n, ", ".join(SUITES)))
        print("### %s" % n, flush=True)
        r = SUITES[n]()
        print("### %s: %s" % (n, "ok" if r == 0 else "FAILED"), flush=True)
        rc |= r != 0
    sys.exit(1 if rc else 0)


if __name__ == "__main__":
    main()
