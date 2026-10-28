#!/usr/bin/env python3
"""Lay the files in todisk/ over a disk image's TFS3 volume.

    python3 tools/todisk.py                  # copy todisk/ onto disk.img
    python3 tools/todisk.py --disk /tmp/copy.img --dir ~/stuff --dry-run
    make iso TODISK=1                        # seed the image, then do this

WHAT IT IS FOR. Files that must be on the guest's disk but never in the
repository have no home in `data/` or `seed/` (both are published). `todisk/`
is that home: IGNORED BY GIT, wholly, so nothing in it can be committed by a
`git add -A`; only this script is tracked. The script makes no folder of its
own and knows nothing about what is in it: it copies what it finds.

THE FOLDER IS THE GUEST'S ROOT, as written: `todisk/a/b/c.txt` becomes
`/a/b/c.txt`. Folders are made on the disk as needed. A file already there with
the same bytes is left alone; one with different bytes is replaced. NOTHING IS
EVER DELETED -- a file removed from todisk/ stays on the image, as `make iso`'s
seeding leaves it (`make clean-disk` starts over).

THE VOLUME IS FOUND, NOT ASSUMED: a bootable disk.img keeps TFS3 in a later
partition (tools/mkpart_test.py's volume_of()), so this works on the image
`make iso` makes and on a flat one alike. It refuses an image that holds no
TFS3 volume, and one a running QEMU has open -- QEMU holds the image, and a
host-side write under it corrupts the filesystem (CLAUDE.md: test against a
COPY). Use `cp --reflink=auto --sparse=always disk.img /tmp/copy.img` and boot
that with `tools/vm.py --disk /tmp/copy.img start`.
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import mkpart_test                                   # noqa: E402
import seed_disk                                     # noqa: E402
import tfs3_writer as t3                             # noqa: E402

def qemu_holding(disk):
    """The pid of a qemu-system process whose command line names this image, or None."""
    real = os.path.realpath(disk)
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                args = f.read().split(b"\0")
        except OSError:
            continue
        if not args or b"qemu-system" not in os.path.basename(args[0]):
            continue
        for a in args[1:]:
            for part in a.decode(errors="replace").replace("file=", ",").split(","):
                if part and os.path.exists(part) and os.path.realpath(part) == real:
                    return int(pid)
    return None


def walk(folder):
    """(guest folders, [(host path, guest path)]) for everything under the folder."""
    dirs, files = [], []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames.sort()
        rel = os.path.relpath(dirpath, folder)
        # Guest paths in the writer's byte-per-char form (tfs3_writer's
        # disk_path()): a host's UTF-8 name lands as the same UTF-8 bytes.
        for d in dirnames:
            dirs.append(t3.disk_path("/" + os.path.normpath(os.path.join(rel, d)).lstrip("./")))
        for fn in sorted(filenames):
            files.append((os.path.join(dirpath, fn),
                          t3.disk_path("/" + os.path.normpath(os.path.join(rel, fn)).lstrip("./"))))
    return dirs, files


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"), help="the image (default: disk.img)")
    ap.add_argument("--dir", default=os.path.join(ROOT, "todisk"), help="the folder (default: todisk/)")
    ap.add_argument("--dry-run", action="store_true", help="say what would be written; change nothing")
    args = ap.parse_args()
    folder = os.path.expanduser(args.dir)

    if not os.path.isdir(folder):
        sys.exit(f"todisk: {folder} does not exist -- make it and put the files to copy in it")
    if not os.path.isfile(args.disk):
        sys.exit(f"todisk: {args.disk} does not exist -- `make iso` makes disk.img")
    pid = qemu_holding(args.disk)
    if pid and not args.dry_run:
        sys.exit(f"todisk: qemu (pid {pid}) has {args.disk} open -- close it, or copy the image and use the copy")

    base, sectors = mkpart_test.volume_of(args.disk)
    if seed_disk.probe(args.disk, base) != "tfs3":
        sys.exit(f"todisk: {args.disk} holds no TFS3 volume (looked at LBA {base}) -- `make iso` seeds one")

    dirs, files = walk(folder)
    img = t3.Tfs3Image(args.disk, writable=not args.dry_run, base_lba=base, sectors=sectors)
    wrote = same = made = 0
    try:
        for d in dirs:
            if img.lookup(d) is None:
                made += 1
                if not args.dry_run:
                    t3.mkdir_path(img, d)
        for host, guest in files:
            with open(host, "rb") as f:
                data = f.read()
            ino = img.lookup(guest)
            if ino is not None and hashlib.sha256(img.read_file_data(ino)).digest() == hashlib.sha256(data).digest():
                same += 1
                continue
            wrote += 1
            print(f"todisk: {'would write' if args.dry_run else 'write'} {t3.host_path(guest)} ({len(data)} bytes)")
            if not args.dry_run:
                t3.write_file(img, data, guest)
    finally:
        img.close()
    where = f"partition at LBA {base}" if base else "flat image"
    print(f"todisk: {wrote} {'to write' if args.dry_run else 'written'}, {same} unchanged, {made} "
          f"folder{'s' if made != 1 else ''} {'to make' if args.dry_run else 'made'} -> {args.disk} ({where})")
    if not files:
        print(f"todisk: {folder} has no files -- put some under it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
