#!/usr/bin/env python3
"""iso9660_test.py -- disc images mounted from a file, checked against xorriso.

    python3 tools/iso9660_test.py                     # a plain and a Joliet image
    python3 tools/iso9660_test.py --image DISC.iso    # and a real disc, sampled
    python3 tools/iso9660_test.py --instance 3

The KTESTs (kernel/fs/iso9660_test.c) check images built byte by byte by
the same author as the reader. This is the independent half: images made
by xorriso, whose own reading of them -- names, tree, every file's bytes
-- is the oracle. Each is written into a copy of disk.img, mounted in the
guest with `mount <file> <dir>`, listed folder by folder and every file
summed with `sum -a sha256`; then the image's file must refuse `rm` while
mounted and accept it after `umount`.

`--image` adds a disc of your own (a commercial CD stays on your machine:
it is copied into the scratch image only). Its root is listed and a
spread of its files summed.

Needs xorriso on the host (it is skipped, and says so, without it).
"""
import argparse
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import vm as vm_mod                   # noqa: E402
from harness import copy_disk         # noqa: E402
import mkpart_test                    # noqa: E402

WRITER = os.path.join(REPO, "tools", "tfs3_writer.py")
GUEST_DIR = "/home/isot"
SAMPLE = 24

_fail = 0


def check(name, ok, detail=""):
    global _fail
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))
    if not ok:
        _fail += 1


def xorriso(*argv):
    return subprocess.run(["xorriso", *argv], capture_output=True, text=True)


def build_fixtures(work):
    """plain.iso and joliet.iso from one tree: deep folders, long and
    non-ASCII names, an empty file (written "EMPTY." on the plain disc)."""
    tree = os.path.join(work, "tree")
    os.makedirs(os.path.join(tree, "Dir1", "Dir2", "Dir3"))
    rnd = random.Random(9660)
    files = {
        "README.TXT": b"hello from a disc\n",
        "Dir1/Dir2/Dir3/deep data.bin": rnd.randbytes(300_000),
        "Long file name with spaces.txt": b"x",
        "Ääkköset ja öö.txt": "å".encode(),
        "empty": b"",
        "Dir1/big.bin": rnd.randbytes(3 * 1024 * 1024 + 123),
    }
    for rel, data in files.items():
        with open(os.path.join(tree, rel), "wb") as f:
            f.write(data)
    out = {}
    for name, extra in (("plain", []), ("joliet", ["-J", "-joliet-long"])):
        iso = os.path.join(work, name + ".iso")
        r = subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "--norock", *extra,
                            "-V", name.upper(), "-o", iso, tree], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"iso9660_test: xorriso could not build {name}.iso:\n{r.stderr}")
        out[name] = iso
    return out


def oracle(iso, work):
    """(dirs, {path: sha256}) as xorriso reads the image -- Joliet's names
    when it has them, else the plain ones, as the kernel does."""
    dest = os.path.join(work, "x-" + os.path.basename(iso))
    shutil.rmtree(dest, ignore_errors=True)
    r = xorriso("-osirrox", "on", "-indev", iso, "-extract", "/", dest)
    if r.returncode:
        sys.exit(f"iso9660_test: xorriso could not extract {iso}:\n{r.stderr[-2000:]}")
    dirs, files = [""], {}
    for root, ds, fs in os.walk(dest):
        rel = os.path.relpath(root, dest)
        rel = "" if rel == "." else "/" + rel
        dirs += [f"{rel}/{d}" for d in ds]
        for f in fs:
            p = os.path.join(root, f)
            os.chmod(p, 0o644)
            with open(p, "rb") as fh:
                files[f"{rel}/{f}"] = hashlib.sha256(fh.read()).hexdigest()
    shutil.rmtree(dest, ignore_errors=True)
    return dirs, files


def stage(disk, isos):
    vol = mkpart_test.volume_of(disk)
    at = ["--at-lba", str(vol[0]), "--sectors", str(vol[1])] if vol[0] else []
    subprocess.run([sys.executable, WRITER, "mkdir", disk, GUEST_DIR, *at],
                   cwd=REPO, capture_output=True, text=True)
    for name, path in isos.items():
        for d in (f"{GUEST_DIR}/{name}", f"{GUEST_DIR}/mnt-{name}"):
            subprocess.run([sys.executable, WRITER, "mkdir", disk, d, *at],
                           cwd=REPO, capture_output=True, text=True)
        r = subprocess.run([sys.executable, WRITER, "write", disk, path, f"{GUEST_DIR}/{name}/disc.iso", *at],
                           cwd=REPO, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"iso9660_test: staging {name} failed:\n{r.stdout}{r.stderr}")


class Guest:
    def __init__(self, disk, instance):
        self.base = [sys.executable, os.path.join(REPO, "tools", "vm.py"), "--disk", disk,
                     "--instance", str(instance), "--mem", "1024"]

    def run(self, *argv, timeout=None):
        cmd = self.base + (["--timeout", str(timeout)] if timeout else []) + list(argv)
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        return r.stdout + r.stderr

    def sh(self, line, timeout=None):
        return [l for l in self.run("exec", line, timeout=timeout).splitlines()
                if not l.startswith("sh ")]


def q(path):
    return '"' + path + '"'


def names_in(guest, d):
    return {l.strip().rstrip("/") for l in guest.sh(f"ls {q(d)}")
            if l.strip() and ": " not in l and l.strip() not in (".", "..")}


def check_disc(guest, name, dirs, files, sample=None):
    img, pt = f"{GUEST_DIR}/{name}/disc.iso", f"{GUEST_DIR}/mnt-{name}"
    print(f"{name}")
    out = "\n".join(guest.sh(f"mount {q(img)} {pt}"))
    check("mount <file> <dir> mounts it", "exit" not in out and "usage" not in out, out[-300:])
    listing = "\n".join(guest.sh("mount"))
    check("`mount` lists it as an image, read-only", f"{pt}" in listing and "ro,image" in listing, listing[-400:])

    want_dirs = dirs if sample is None else [""]
    bad = []
    for d in want_dirs:
        want = {p.rsplit("/", 1)[1] for p in list(files) + dirs if p and p.rsplit("/", 1)[0] == d}
        got = names_in(guest, pt + d)
        if got != want:
            bad.append(f"{d or '/'}: missing {sorted(want - got)[:4]}, extra {sorted(got - want)[:4]}")
    check(f"every folder lists what xorriso reads ({len(want_dirs)} folders)", not bad, "; ".join(bad[:3]))

    paths = sorted(files) if sample is None else sample
    # One file a call: the debug console's line is short, and a long one
    # arrives cut off.
    wrong, said = [], ""
    for i in range(0, len(paths)):
        chunk = paths[i:i + 1]
        out = guest.sh("sum -a sha256 " + " ".join(q(pt + p) for p in chunk), timeout=600)
        got = {}
        for l in out:
            if "  " in l:
                h, p = l.split("  ", 1)
                got[p.strip()] = h.strip()
        for p in chunk:
            if got.get(pt + p) != files[p]:
                wrong.append(p)
                said = said or " | ".join(out[-3:])
    check(f"every byte matches xorriso's ({len(paths)} files)", not wrong, f"{wrong[:4]} {said}")

    first = next(p for p in sorted(files) if files[p] != hashlib.sha256(b"").hexdigest())
    up = pt + first.swapcase()
    out = guest.sh(f"sum -a sha256 {q(up)}")
    check("a name is found whatever its case", any(files[first] in l for l in out), f"{up}: {out[-2:]}")

    out = "\n".join(guest.sh(f"rm {q(img)}"))
    still = "\n".join(guest.sh(f"ls {q(GUEST_DIR + '/' + name)}"))
    check("the image cannot be deleted while it is mounted", "exit" in out and "disc.iso" in still, out[-300:])
    out = "\n".join(guest.sh(f"umount {pt}") + guest.sh(f"rm {q(img)}"))
    gone = "\n".join(guest.sh(f"ls {q(GUEST_DIR + '/' + name)}"))
    check("after umount it can", "exit" not in out and "disc.iso" not in gone, out[-300:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", help="a disc image of your own, sampled")
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="leave the scratch disk image")
    args = ap.parse_args()
    if not shutil.which("xorriso"):
        print("iso9660_test: SKIPPED -- xorriso is not installed")
        return 0
    if not os.path.exists(os.path.join(REPO, "disk.img")):
        print("iso9660_test: no disk.img -- run `make iso` first")
        return 2

    work = tempfile.mkdtemp(prefix="iso9660_test.")
    disk = os.path.join(work, "disk.img")
    guest = None
    try:
        isos = build_fixtures(work)
        if args.image:
            isos["own"] = os.path.abspath(args.image)
        print("iso9660_test: reading the images with xorriso")
        oracles = {n: oracle(p, work) for n, p in isos.items()}
        copy_disk(os.path.join(REPO, "disk.img"), disk)
        print("iso9660_test: staging them into a copy of disk.img")
        stage(disk, isos)
        guest = Guest(disk, args.instance)
        if not vm_mod.started_ok(guest.run("start")):
            print("iso9660_test: could not start the VM")
            return 2
        for name in ("plain", "joliet"):
            check_disc(guest, name, *oracles[name])
        if args.image:
            dirs, files = oracles["own"]
            rnd = random.Random(1)
            sample = sorted(set(rnd.sample(sorted(files), min(SAMPLE, len(files)))))
            check_disc(guest, "own", dirs, files, sample=sample)
    finally:
        if guest:
            guest.run("stop")
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f"iso9660_test: kept {work}")
    print(f"iso9660_test: {'PASS' if not _fail else f'{_fail} FAILED'}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
