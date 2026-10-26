#!/usr/bin/env python3
"""Run btgw_printer's tests.

  tools/run_tests.py              every tier that needs no board
  tools/run_tests.py drivers      native driver tests (fake host, gcc -m32)
  tools/run_tests.py pex          cross-build the .pex modules and dump them
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


SUITES = {"drivers": drivers, "pex": pex}


def main():
    names = sys.argv[1:] or list(SUITES)
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
