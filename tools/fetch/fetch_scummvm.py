#!/usr/bin/env python3
"""tools/fetch/fetch_scummvm.py -- the ScummVM, zlib, libogg and libvorbis source tarballs.

ScummVM (docs/sdl-design.md, stage 4) is FETCHED, NOT VENDORED, as GNU
make is: its source is 212 MiB compressed, and what this repository
holds is our part -- `userland/third_party/scummvm/patches/` and
tools/build/build_scummvm.sh. zlib, libogg and libvorbis are built
beside it and linked into it statically (its zip themes, compressed
saves, and Drascula's Ogg music); they reach the SDK only when a second
port needs them.

This is the `scummvm` row in tools/fetch/fetch_extras.py's registry, so
it runs only under `EXTRAS=1` (or `--only scummvm`), after the licence
has been shown and accepted.

PINNED BY SHA-256 (2026-11-17), each against its publisher's own
checksum: ScummVM's `.sha256` beside the tarball on downloads.scummvm.org,
Xiph's SHA256SUMS for libogg and libvorbis, and zlib 1.3.1's widely
published hash (zlib.net lists only its newest release). A tarball
that does not hash to its pin is deleted and refused, whichever mirror
served it.

    python3 tools/fetch/fetch_scummvm.py            # fetch what is missing
    python3 tools/fetch/fetch_scummvm.py --verify   # offline: present and pinned?
    python3 tools/fetch/fetch_scummvm.py --pins     # the pins, for a build stamp
    python3 tools/fetch/fetch_scummvm.py --games    # Lure's zip, for the test

--games fetches Lure of the Temptress (5.4 MiB, freeware) into
data/scummvm/ for tools/tests/gui/scummvm_data_test.py to serve on
loopback -- the guest's own downloader fetches the games on a machine.
Pinned by the same SHA-256 as userland/apps/apps/scummvm/scummvm.c.
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

REPO = toolpath.REPO
DEST_DIR = os.path.join(REPO, "toolchain", "tarballs")

# (file, sha256, url). ONE place for the pins: the build script asks
# --verify rather than keeping a copy.
TARBALLS = [
    ("scummvm-2026.4.0.tar.xz",
     "cd660b34104da7f85ed0a11b7c6c2320c629bb87ee297935711ebc1e09731f12",
     "https://downloads.scummvm.org/frs/scummvm/2026.4.0/scummvm-2026.4.0.tar.xz"),
    ("zlib-1.3.1.tar.gz",
     "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
     "https://zlib.net/fossils/zlib-1.3.1.tar.gz"),
    ("libogg-1.3.6.tar.xz",
     "5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061",
     "https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz"),
    ("libvorbis-1.3.7.tar.xz",
     "b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b",
     "https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz"),
]

GAMES_DIR = os.path.join(REPO, "data", "scummvm")
GAMES = [
    ("lure-1.1.zip",
     "f3178245a1483da1168c3a11e70b65d33c389f1f5df63d4f3a356886c1890108",
     "https://downloads.scummvm.org/frs/extras/Lure%20of%20the%20Temptress/lure-1.1.zip"),
]


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name, want, url, dest_dir=DEST_DIR):
    from fetch_extras import urlopen  # names itself; see USER_AGENT there
    dest = os.path.join(dest_dir, name)
    part = dest + ".part"
    print(f"fetch_scummvm: {url}")
    with urlopen(url) as r, open(part, "wb") as out:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
    got = digest(part)
    if got != want:
        os.unlink(part)
        print(f"fetch_scummvm: {name} hashed to {got}, not its pin {want} -- refused",
              file=sys.stderr)
        return False
    os.replace(part, dest)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verify", action="store_true", help="check, fetch nothing")
    ap.add_argument("--pins", action="store_true", help="print the pins")
    ap.add_argument("--games", action="store_true", help="Lure's zip into data/scummvm, for the test")
    args = ap.parse_args()
    if args.pins:
        for name, sha, _ in TARBALLS:
            print(sha, name)
        return 0
    rows, dest_dir = (GAMES, GAMES_DIR) if args.games else (TARBALLS, DEST_DIR)
    os.makedirs(dest_dir, exist_ok=True)
    ok = True
    for name, sha, url in rows:
        path = os.path.join(dest_dir, name)
        if os.path.exists(path):
            if digest(path) == sha:
                continue
            print(f"fetch_scummvm: {name} does not match its pin", file=sys.stderr)
            if args.verify:
                ok = False
                continue
            os.unlink(path)
        elif args.verify:
            print(f"fetch_scummvm: {name} is missing", file=sys.stderr)
            ok = False
            continue
        ok = fetch(name, sha, url, dest_dir) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
