#!/usr/bin/env python3
"""tools/fetch/fetch_toolchain.py -- the GCC and binutils source tarballs.

WHY THIS IS FETCHED AND NOT VENDORED
------------------------------------
The x86_64-saunaos cross toolchain (docs/cc-design.md, stage 1) is GCC
and binutils with a few lines of target definition on top. The sources
are ~136 MB compressed and millions of lines; they do not go in the
repository. What does is OUR part: `toolchain/patches/<pkg>/*.patch`.
So a checkout gets the tarballs from here, and `make toolchain` builds
from them OFFLINE (`tools/build/build_toolchain.sh`).

This is the `toolchain` row in tools/fetch/fetch_extras.py's registry,
so it runs only under `EXTRAS=1` and only after the licence (GPL-3) has
been shown and accepted -- the one rule for anything that reaches the
network.

PINNED BY SHA-256, and the pins were taken from downloads whose GNU
detached signatures verified against `gnu-keyring.gpg` (2026-11-08:
gcc by Richard Biener's key, binutils by Nick Clifton's). A tarball
that does not hash to its pin is deleted and refused, whichever mirror
served it -- so the mirrors below need no trust of their own, and this
script needs no gpg. A new pin is re-verified against the .sig by hand
before it is written here.

    python3 tools/fetch/fetch_toolchain.py            # fetch what is missing
    python3 tools/fetch/fetch_toolchain.py --verify   # offline: present and pinned?
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

REPO = toolpath.REPO
DEST_DIR = os.path.join(REPO, "toolchain", "tarballs")

# (file, sha256, path under a GNU mirror's gnu/). ONE place for the pins:
# build_toolchain.sh asks --verify rather than keeping its own copy.
TARBALLS = [
    ("binutils-2.47.tar.xz",
     "154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff",
     "binutils/binutils-2.47.tar.xz"),
    ("gcc-16.2.0.tar.xz",
     "e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e",
     "gcc/gcc-16.2.0/gcc-16.2.0.tar.xz"),
]

# ftp.gnu.org first; ftpmirror.gnu.org redirects to a nearby mirror.
MIRRORS = ["https://ftp.gnu.org/gnu/", "https://ftpmirror.gnu.org/gnu/"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify():
    """(ok, problems): every tarball present and hashing to its pin."""
    problems = []
    for name, pin, _ in TARBALLS:
        path = os.path.join(DEST_DIR, name)
        if not os.path.exists(path):
            problems.append(f"{name} is missing")
        elif sha256(path) != pin:
            problems.append(f"{name} does not hash to its pin -- delete it and fetch again")
    return not problems, problems


def fetch_one(name, pin, rel):
    from fetch_extras import urlopen  # names itself; see USER_AGENT there
    path = os.path.join(DEST_DIR, name)
    part = path + ".part"
    for base in MIRRORS:
        url = base + rel
        print(f"fetch_toolchain: {url}")
        h = hashlib.sha256()
        try:
            with urlopen(url, timeout=120) as r, open(part, "wb") as out:
                for block in iter(lambda: r.read(1 << 20), b""):
                    h.update(block)
                    out.write(block)
        except Exception as e:  # noqa: BLE001 -- any failure is "try the next mirror"
            print(f"fetch_toolchain:   failed: {e}")
            continue
        if h.hexdigest() != pin:
            os.remove(part)
            print(f"fetch_toolchain:   SHA-256 {h.hexdigest()} is not the pin {pin} -- refused")
            continue
        os.replace(part, path)
        print(f"fetch_toolchain: wrote {os.path.relpath(path, REPO)}, checksum matches")
        return True
    if os.path.exists(part):
        os.remove(part)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--verify", action="store_true",
                    help="check the tarballs are present and pinned; never touches the network")
    args = ap.parse_args()

    if args.verify:
        ok, problems = verify()
        for p in problems:
            print(f"fetch_toolchain: {p}", file=sys.stderr)
        return 0 if ok else 1

    os.makedirs(DEST_DIR, exist_ok=True)
    for name, pin, rel in TARBALLS:
        path = os.path.join(DEST_DIR, name)
        if os.path.exists(path) and sha256(path) == pin:
            print(f"fetch_toolchain: {name} is present and pinned")
            continue
        if not fetch_one(name, pin, rel):
            print(f"fetch_toolchain: no mirror served {name} with the pinned checksum",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
