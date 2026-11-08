#!/usr/bin/env python3
"""tools/fetch/fetch_toolchain.py -- the GCC, binutils, GMP, MPFR and MPC source tarballs.

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

TWO SETS. `cross` is GCC and binutils, all the cross toolchain needs
(its GMP, MPFR and MPC are the host's own libraries). `native` adds
those three, because the compiler that RUNS ON SaunaOS (cc-design.md,
stage 3; tools/build/build_native_toolchain.sh) links them statically
and so needs them built for SaunaOS from source. Their pins are the
SHA-512s GCC itself lists in `contrib/prerequisites.sha512`, inside the
gcc tarball pinned above -- the versions are the ones that GCC release
names, and the trust is that tarball's.

    python3 tools/fetch/fetch_toolchain.py                      # fetch what is missing
    python3 tools/fetch/fetch_toolchain.py --verify             # offline: present and pinned?
    python3 tools/fetch/fetch_toolchain.py --set native [--verify]
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

REPO = toolpath.REPO
DEST_DIR = os.path.join(REPO, "toolchain", "tarballs")

# (file, algorithm, digest, path under a GNU mirror's gnu/). ONE place for
# the pins: the build scripts ask --verify rather than keeping a copy.
CROSS = [
    ("binutils-2.47.tar.xz", "sha256",
     "154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff",
     "binutils/binutils-2.47.tar.xz"),
    ("gcc-16.2.0.tar.xz", "sha256",
     "e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e",
     "gcc/gcc-16.2.0/gcc-16.2.0.tar.xz"),
]
# gcc-16.2.0/contrib/prerequisites.sha512, verbatim.
NATIVE = CROSS + [
    ("gmp-6.3.0.tar.bz2", "sha512",
     "3b684c9bcb9ede2b7e54d0ba4c9764bfa17c20d4f3000017c553b6f1e135b536"
     "949580ff37341680c25dc236cfe0ba1db8cfdfe619ce013656189ef0871b89f8",
     "gmp/gmp-6.3.0.tar.bz2"),
    ("mpfr-4.2.2.tar.bz2", "sha512",
     "0176e50808dcc07afbf5bc3e38bf9b7b21918e5f194aa0bfd860d99b00c47063"
     "0aef149776c4be814a61c44269c3a5b9a4b0b1c0fcd4c9feb1459d8466452da8",
     "mpfr/mpfr-4.2.2.tar.bz2"),
    ("mpc-1.3.1.tar.gz", "sha512",
     "4bab4ef6076f8c5dfdc99d810b51108ced61ea2942ba0c1c932d624360a5473d"
     "f20d32b300fc76f2ba4aa2a97e1f275c9fd494a1ba9f07c4cb2ad7ceaeb1ae97",
     "mpc/mpc-1.3.1.tar.gz"),
]
SETS = {"cross": CROSS, "native": NATIVE}

# ftp.gnu.org first; ftpmirror.gnu.org redirects to a nearby mirror.
MIRRORS = ["https://ftp.gnu.org/gnu/", "https://ftpmirror.gnu.org/gnu/"]


def digest(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify(tarballs):
    """(ok, problems): every tarball present and hashing to its pin."""
    problems = []
    for name, algo, pin, _ in tarballs:
        path = os.path.join(DEST_DIR, name)
        if not os.path.exists(path):
            problems.append(f"{name} is missing")
        elif digest(path, algo) != pin:
            problems.append(f"{name} does not hash to its pin -- delete it and fetch again")
    return not problems, problems


def fetch_one(name, algo, pin, rel):
    from fetch_extras import urlopen  # names itself; see USER_AGENT there
    path = os.path.join(DEST_DIR, name)
    part = path + ".part"
    for base in MIRRORS:
        url = base + rel
        print(f"fetch_toolchain: {url}")
        h = hashlib.new(algo)
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
            print(f"fetch_toolchain:   {algo} {h.hexdigest()} is not the pin {pin} -- refused")
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
    ap.add_argument("--set", choices=sorted(SETS), default="cross",
                    help="cross: GCC + binutils (make toolchain); native: + GMP, MPFR, "
                         "MPC (make native-toolchain)")
    ap.add_argument("--pins", action="store_true",
                    help="print the set's names and pins (a build script's cache key)")
    args = ap.parse_args()
    tarballs = SETS[args.set]

    if args.pins:
        for name, algo, pin, _ in tarballs:
            print(name, algo, pin)
        return 0

    if args.verify:
        ok, problems = verify(tarballs)
        for p in problems:
            print(f"fetch_toolchain: {p}", file=sys.stderr)
        return 0 if ok else 1

    os.makedirs(DEST_DIR, exist_ok=True)
    for name, algo, pin, rel in tarballs:
        path = os.path.join(DEST_DIR, name)
        if os.path.exists(path) and digest(path, algo) == pin:
            print(f"fetch_toolchain: {name} is present and pinned")
            continue
        if not fetch_one(name, algo, pin, rel):
            print(f"fetch_toolchain: no mirror served {name} with the pinned checksum",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
