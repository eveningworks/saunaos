#!/usr/bin/env python3
"""Vendor uACPI into kernel/third_party/uacpi/upstream/, at a pinned release.

WHAT IT DOES. Downloads the release tarball named in PIN, refuses one
whose SHA-256 differs, and copies the WHOLE release into upstream/ --
the interpreter (source/, include/), its licence, and its own test suite
(tests/, which tools/hostchecks/uacpi_hostcheck.py runs on the host).
Then it applies kernel/third_party/uacpi/patches/*.patch in name order,
to the copy.

WHY THE WHOLE TREE. Nothing in it is SaunaOS's: the configuration is the
Makefile's UACPI_FLAGS plus kernel/third_party/uacpi/glue/uacpi_libc.h,
and the kernel's callbacks are kernel/acpi/uacpi_glue.c. A partial copy
would need a list to maintain; this one is the release, byte for byte, which is what makes
"never edited in place" checkable by re-running this tool and reading
`git status`. The Makefile keeps tests/ out of the kernel build.

    python3 tools/fetch/uacpi_vendor.py               # download, check, vendor
    python3 tools/fetch/uacpi_vendor.py --work DIR    # cache the tarball in DIR
                                                      # (default ~/.cache/saunaos/uacpi)

Reaches github.com (the tag's archive, redirected to codeload.github.com).
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
PORT = os.path.join(REPO, "kernel", "third_party", "uacpi")

DEFAULT_WORK = os.path.join(os.environ.get("XDG_CACHE_HOME") or
                            os.path.expanduser("~/.cache"), "saunaos", "uacpi")

# (top directory in the tarball, URL, SHA-256). The newest release on
# the day it was vendored (2026-11-28).
PIN = ("uACPI-7.0.0",
       "https://github.com/uACPI/uACPI/archive/refs/tags/7.0.0.tar.gz",
       "4ccdb4c06d06c54116920eb861adf3a87efd3285c762e2e6f3529b52c6d20fa5")


def fetch(work):
    top, url, want = PIN
    os.makedirs(work, exist_ok=True)
    tarball = os.path.join(work, top + ".tar.gz")
    if not os.path.exists(tarball):
        print(f"uacpi_vendor: downloading {url}")
        with urllib.request.urlopen(url) as r, open(tarball + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tarball + ".part", tarball)
    got = hashlib.sha256(open(tarball, "rb").read()).hexdigest()
    if got != want:
        sys.exit(f"uacpi_vendor: {tarball} has SHA-256 {got}, the pin says {want} -- refusing")
    tree = os.path.join(work, top)
    shutil.rmtree(tree, ignore_errors=True)
    with tarfile.open(tarball) as t:
        t.extractall(work, filter="data")
    return tree


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--work", default=DEFAULT_WORK, help="where the tarball is cached and unpacked")
    args = ap.parse_args()

    tree = fetch(args.work)
    dst = os.path.join(PORT, "upstream")
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(tree, dst)

    for p in sorted(glob.glob(os.path.join(PORT, "patches", "*.patch"))):
        print(f"uacpi_vendor: applying {os.path.relpath(p, REPO)}")
        r = subprocess.run(["patch", "-p1", "-d", dst, "--no-backup-if-mismatch", "-i", p])
        if r.returncode:
            sys.exit(f"uacpi_vendor: {os.path.basename(p)} no longer applies -- fix it before re-vendoring")

    n = sum(len(f) for _r, _d, f in os.walk(dst))
    print(f"uacpi_vendor: {PIN[0]} -> {os.path.relpath(dst, REPO)} ({n} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
