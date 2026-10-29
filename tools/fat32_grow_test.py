#!/usr/bin/env python3
"""tools/fat32_grow_test.py -- `part resize` grows a FILLED FAT32 volume, both ways, and shrinks it.

The guest boots the ordinary system disk (pinned with `root=ata0p3`) and a
second, blank virtio disk the HOST partitioned with sfdisk and filled with
mkfs.fat and mtools -- so the volumes are another system's, not SaunaOS's:

  * p1, 64 MiB of 4 KiB clusters, ~32 MiB of files in holes (every third
    deleted, a second batch written into the gaps), long names past ASCII
    (`Käse.txt`, `日本語.txt`) and a deep directory. Grown to 200 MiB and
    then 400 MiB: its FATs must grow, so the data region SHIFTS;
  * p2, sized by the host so its FATs have spare entries, filled to its
    last cluster, and grown by those spare clusters only: the FATs
    already cover them, so NOTHING MOVES.

Each claim is believed from a path that is not `part`'s own output:

  * the HOST extracts the partition and runs `fsck.fat -n` (clean, and no
    "differences between boot sector and its backup"), compares the boot
    sector with its backup itself, and reads the BPB: the new size, and
    for p2 the same FAT size and data start;
  * mtools on the host copies EVERY file back out, byte-identical to what
    it wrote, and lists the non-ASCII long names;
  * the guest mounts it, lists the names, writes more than the old volume
    had free (`mkfiles`), verifies it, and the host's fsck.fat is clean
    over SaunaOS's writes too; a remount, and a second grow.

Then p1 SHRINKS, 400 -> 280 MiB, with files the guest put at its TOP (a
pad written under them, then deleted), so they must move below the new
end: fsck.fat clean, the host's files byte-identical, the guest's verify,
and the partition cut after the filesystem. Below FAT32's 65525-cluster
floor (~257 MiB of 4 KiB clusters) the shrink is refused.

And the refusals: a mounted volume, a shrink below what its files need, a
volume marked dirty; and the warning that FAT32 keeps no journal, before
`confirm`.

The positive controls are mutate.py runs on kernel/fs/fat32_resize.c
(docs/tools.md has them): the new FAT sectors left unwritten, the lowest
cluster of each run left unmoved, FSInfo's free count left old, and the
backup boot sector left stale -- each must turn a check here red.

On demand (ondemand_sweep.py): it boots its own guest with a second disk.
Needs sfdisk, mkfs.fat, fsck.fat and mtools on the host.

    python3 tools/fat32_grow_test.py [--instance N] [--kvm] [--keep DIR]
"""
import argparse
import hashlib
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import Results, copy_disk          # noqa: E402
import install_grub                             # noqa: E402
import port_guard                               # noqa: E402

SECTOR = 512
MIB = 2048                     # sectors
BLANK_BYTES = 1024 * 1024 * 1024
P1_START, P1_MIB = 2048, 64
P2_START = 600 * MIB
P3_START, P3_MIB = 800 * MIB, 40   # marked dirty by the host: refused
TYPE = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
MENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C.UTF-8")
NAMES = ("Käse.txt", "日本語.txt")


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, env=MENV, check=False, **kw)


def part_geom(img, n):
    """Partition n's (start, length) in sectors, as the host's sfdisk reads the GPT."""
    dump = run(["sfdisk", "--dump", img]).stdout
    for ln in dump.splitlines():
        m = re.match(rf"\S+{n} : start=\s*(\d+), size=\s*(\d+)", ln)
        if m:
            return int(m.group(1)), int(m.group(2))
    return None, None


def bpb(path, offset=0):
    """The fields a grow changes, and where the data starts, in 512-byte sectors."""
    with open(path, "rb") as f:
        f.seek(offset)
        b = f.read(512)
    k = struct.unpack_from("<H", b, 11)[0] // SECTOR
    spc, res, nfats = b[13] * k, struct.unpack_from("<H", b, 14)[0] * k, b[16]
    total, fatsz = struct.unpack_from("<I", b, 32)[0] * k, struct.unpack_from("<I", b, 36)[0] * k
    data = res + nfats * fatsz
    return {"spc": spc, "res": res, "nfats": nfats, "total": total, "fat": fatsz, "data": data,
            "clusters": (total - data) // spc, "backup": struct.unpack_from("<H", b, 50)[0] * k}


def copy_in(src, dst, start):
    """The partition image `src` into the disk image `dst` at sector `start`."""
    with open(src, "rb") as s, open(dst, "r+b") as d:
        d.seek(start * SECTOR)
        shutil.copyfileobj(s, d, 4 << 20)


def extract(img, start, count, out):
    """Partition sectors out of the disk image, sparse, for fsck.fat and mtools."""
    with open(img, "rb") as s, open(out, "wb") as d:
        s.seek(start * SECTOR)
        left = count * SECTOR
        while left:
            chunk = s.read(min(left, 4 << 20))
            if chunk.count(0) == len(chunk):
                d.seek(len(chunk), 1)
            else:
                d.write(chunk)
            left -= len(chunk)
        d.truncate(count * SECTOR)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fill_p1(work, img):
    """mkfs.fat and ~32 MiB of files in holes. Returns {"::/path": sha256} of what stays."""
    run(["mkfs.fat", "-F", "32", "-s", "8", "-n", "GROW", img])
    rng = random.Random(1964)
    src = os.path.join(work, "p1src")
    os.makedirs(os.path.join(src, "photos"))
    os.makedirs(os.path.join(src, "deep", "a", "b", "c"))
    files = {}

    def put(rel, data):
        with open(os.path.join(src, rel), "wb") as f:
            f.write(data)
        files["::/" + rel] = sha(data)

    put(NAMES[0], "Emmentaler, Gruyère.\n".encode() * 40)
    put(NAMES[1], "日本語のテキスト\n".encode() * 300)
    put("README.TXT", b"An 8.3 name.\r\n")
    put("deep/a/b/c/notes.txt", b"deep\n" * 2000)
    for i in range(90):
        put(f"photos/IMG_{i:04d}.jpg", rng.randbytes(rng.randint(150_000, 600_000)))
    r = run(["mcopy", "-s", "-i", img] + [os.path.join(src, e) for e in sorted(os.listdir(src))] + ["::/"])
    if r.returncode:
        return None, r.stderr
    gone = [f"::/photos/IMG_{i:04d}.jpg" for i in range(1, 90, 3)]
    r = run(["mdel", "-i", img] + gone)
    if r.returncode:
        return None, r.stderr
    for g in gone:
        del files[g]
    # A second batch, into the holes: chains that skip about.
    os.makedirs(os.path.join(src, "music"))
    for i in range(20):
        put(f"music/track {i:02d} – live.ogg", rng.randbytes(rng.randint(200_000, 900_000)))
    r = run(["mcopy", "-s", "-i", img, os.path.join(src, "music"), "::/"])
    if r.returncode:
        return None, r.stderr
    return files, ""


def p2_size(work):
    """A p2 length (sectors) whose mkfs.fat FATs have room for 16+ clusters more."""
    probe = os.path.join(work, "probe.img")
    for mib8 in range(48 * 8, 64 * 8):           # in 128 KiB steps
        n = mib8 * 256
        with open(probe, "wb") as f:
            f.truncate(n * SECTOR)
        run(["mkfs.fat", "-F", "32", "-s", "8", probe])
        g = bpb(probe)
        spare = g["fat"] * 128 - 2 - g["clusters"]
        if spare >= 16:
            os.remove(probe)
            return n, spare
    return None, 0


def fill_p2(work, img):
    """mkfs.fat, a named file, and one file to its last free cluster or two."""
    run(["mkfs.fat", "-F", "32", "-s", "8", "-n", "SPARE", img])
    g = bpb(img)
    src = os.path.join(work, "p2src")
    os.makedirs(src)
    files = {}
    data = "Käse\n".encode() * 100
    with open(os.path.join(src, NAMES[0]), "wb") as f:
        f.write(data)
    files["::/" + NAMES[0]] = sha(data)
    # The root holds one cluster and each file at least one; leave two free.
    big = random.Random(7).randbytes((g["clusters"] - 1 - 1 - 2) * g["spc"] * SECTOR)
    with open(os.path.join(src, "fill.bin"), "wb") as f:
        f.write(big)
    files["::/fill.bin"] = sha(big)
    r = run(["mcopy", "-i", img, os.path.join(src, NAMES[0]), os.path.join(src, "fill.bin"), "::/"])
    return (files if not r.returncode else None), r.stderr


def host_checks(res, label, blank, n, work, files, expect_total=None, same_layout=None):
    """fsck.fat, the backup boot sector, and every file read back with mtools."""
    start, count = part_geom(blank, n)
    part = os.path.join(work, f"p{n}.img")
    extract(blank, start, count, part)
    g = bpb(part)
    if expect_total is not None:
        res.check(f"{label}: the host's BPB says the volume fills the partition",
                  0 <= expect_total - g["total"] < g["spc"], f"total {g['total']}, partition {expect_total}")
    if same_layout is not None:
        res.check(f"{label}: the FATs kept their size and the data did not move",
                  g["fat"] == same_layout["fat"] and g["data"] == same_layout["data"],
                  f"fat {same_layout['fat']} -> {g['fat']}, data {same_layout['data']} -> {g['data']}")
    fsck = subprocess.run(["fsck.fat", "-n", part], capture_output=True, text=True)
    text = fsck.stdout + fsck.stderr
    res.check(f"{label}: fsck.fat -n is clean", fsck.returncode == 0 and "Free cluster summary" not in text,
              text[-600:])
    res.check(f"{label}: fsck.fat finds the backup boot sector the same", "its backup" not in text, text[-600:])
    with open(part, "rb") as f:
        boot = f.read(512)
        f.seek(g["backup"] * SECTOR)
        backup = f.read(512)
    res.check(f"{label}: the backup boot sector is the primary, byte for byte", boot == backup)
    out = os.path.join(work, f"p{n}out")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    r = run(["mcopy", "-s", "-n", "-i", part, "::/*", out])
    bad = []
    for path, want in files.items():
        local = os.path.join(out, path[3:])
        if not os.path.exists(local):
            bad.append(path + " missing")
            continue
        with open(local, "rb") as f:
            if sha(f.read()) != want:
                bad.append(path + " differs")
    res.check(f"{label}: mtools reads all {len(files)} files back byte-identical", not bad and not r.returncode,
              f"{bad[:8]} {r.stderr[-300:]}")
    listing = run(["mdir", "-/", "-b", "-i", part, "::/"]).stdout
    res.check(f"{label}: mdir lists the long names past ASCII",
              all(f"::/{nm}" in listing for nm in NAMES if "::/" + nm in files), listing[:400])
    return g


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None, metavar="N",
                    help="the VM slot (vm.py's numbering); default: the lowest free one")
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the images here")
    args = ap.parse_args()
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    missing = [t for t in ("sfdisk", "mkfs.fat", "fsck.fat", "mcopy", "mdir", "mdel") if not shutil.which(t)]
    if missing:
        print(f"fat32_grow_test: needs {', '.join(missing)} on the host (util-linux, dosfstools, mtools)")
        return 1
    tmp = args.keep or tempfile.mkdtemp(prefix="fat32grow-")
    os.makedirs(tmp, exist_ok=True)
    try:
        return run_in(args, tmp)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)


def run_in(args, tmp):
    res = Results()
    root = os.path.join(tmp, "root.img")
    blank = os.path.join(tmp, "blank.img")
    copy_disk("disk.img", root, cwd=REPO)
    ok, why = install_grub.add_boot_word(root, "root=ata0p3")
    if not res.check(f"the system disk is pinned as the root {why}", ok):
        return 1
    p2_len, spare = p2_size(tmp)
    if not res.check("the host finds a p2 size whose FATs have spare entries", p2_len is not None):
        return 1
    with open(blank, "wb") as f:
        f.truncate(BLANK_BYTES)
    script = (f"label: gpt\nstart={P1_START}, size={P1_MIB * MIB}, type={TYPE}, name=\"shift\"\n"
              f"start={P2_START}, size={p2_len}, type={TYPE}, name=\"inplace\"\n"
              f"start={P3_START}, size={P3_MIB * MIB}, type={TYPE}, name=\"dirty\"\n")
    subprocess.run(["sfdisk", "--quiet", blank], input=script.encode(), check=True)

    p1img, p2img, p3img = (os.path.join(tmp, f"p{n}src.img") for n in (1, 2, 3))
    for path, n in ((p1img, P1_MIB * MIB), (p2img, p2_len), (p3img, P3_MIB * MIB)):
        with open(path, "wb") as f:
            f.truncate(n * SECTOR)
    # p3: Linux's dirty bit (BS_Reserved1 bit 0), as a crash leaves it. Set
    # before the guest boots: a write under a running guest can sit behind
    # its block cache.
    run(["mkfs.fat", "-F", "32", "-s", "8", "-n", "DIRTY", p3img])
    with open(p3img, "r+b") as f:
        f.seek(65)
        f.write(b"\x01")
    copy_in(p3img, blank, P3_START)
    files1, err = fill_p1(tmp, p1img)
    if not res.check("the host fills p1 with mtools (holes, long names, a deep directory)", files1 is not None, err):
        return 1
    files2, err = fill_p2(tmp, p2img)
    if not res.check("the host fills p2 to its last clusters", files2 is not None, err):
        return 1
    copy_in(p1img, blank, P1_START)
    copy_in(p2img, blank, P2_START)
    g1 = host_checks(res, "before", blank, 1, tmp, files1)
    g2 = bpb(p2img)

    def vm(*argv, timeout=1200):
        # Each guest command may run minutes (the shrink's 220 MB pad):
        # vm.py's own per-command wait is the default 30 s otherwise.
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", root, "--extra-disk", blank, "--extra-disk-kind", "virtio",
                            "--timeout", str(min(timeout, 900)),
                            *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start")):
        return 1
    try:
        drive(res, vm, blank, tmp, files1, files2, g1, g2, p2_len, spare)
    finally:
        vm("stop")
    return res.finish("fat32_grow_test")


def drive(res, vm, blank, tmp, files1, files2, g1, g2, p2_len, spare):
    lsblk = vm("exec", "lsblk")
    disk = next((ln.split()[0] for ln in lsblk.splitlines()
                 if ln.strip().startswith("virtio") and "p" not in ln.split()[0]), None)
    if not res.check("the second disk is named", disk is not None, lsblk):
        return
    p1, p2 = f"{disk}p1", f"{disk}p2"

    # The refusals, each changing nothing.
    out = vm("exec", "mkdir /mnt/f", f"mount {p1} /mnt/f", f"part resize {p1} 200M confirm", "umount /mnt/f")
    res.check("a mounted volume is refused", "mounted" in out and "cannot be resized" in out, out)
    out = vm("exec", f"part resize {p1} 200M")
    res.check("without `confirm` it says what would change, and that FAT32 keeps no journal",
              "64.0 MiB to 200.0 MiB" in out and "fat32 filesystem follows" in out and "no journal" in out, out)
    out = vm("exec", f"part resize {p1} 20M confirm")
    res.check("a shrink below what its files need is refused, with the size", "at least" in out, out)
    out = vm("exec", f"part resize {disk}p3 +8M confirm")
    res.check("a volume marked dirty is refused", "not unmounted cleanly" in out, out)
    res.check("no refusal touched the table",
              part_geom(blank, 1)[1] == P1_MIB * MIB and part_geom(blank, 3)[1] == P3_MIB * MIB)

    # 1. p1, 64 -> 200 MiB: the FATs grow and the data shifts.
    out = vm("exec", f"part resize {p1} 200M confirm")
    res.check("part resize grows p1 (shifting), and says not to pull the disk",
              "Resized" in out and "200.0 MiB" in out and "do not power off" in out, out)
    res.check("the host's GPT says 200 MiB", part_geom(blank, 1)[1] == 200 * MIB)
    g = host_checks(res, "p1 at 200 MiB", blank, 1, tmp, files1, expect_total=200 * MIB)
    res.check("the FATs grew and the data start moved", g["fat"] > g1["fat"] and g["data"] > g1["data"],
              f"fat {g1['fat']} -> {g['fat']}, data {g1['data']} -> {g['data']}")

    # The guest reads it, and fills more than the old volume had free.
    out = vm("exec", f"mount {p1} /mnt/f", "ls /mnt/f", "sum -a sha256 /mnt/f/README.TXT", "mkdir /mnt/f/new",
             "mkfiles /mnt/f/new 20 2000000", "mkfiles --verify /mnt/f/new 20 2000000", "df", "umount /mnt/f")
    res.check("the guest lists the long names", all(nm in out for nm in NAMES), out)
    res.check("the guest reads a file as the host wrote it", files1["::/README.TXT"] in out, out)
    res.check("the new space takes 40 MB more (the old volume had ~32 MiB free) and it verifies",
              "verified 20 file(s)" in out, out)
    newfiles = dict(files1)
    host_checks(res, "p1 after the guest wrote", blank, 1, tmp, newfiles)

    # A remount, then a second grow, 200 -> 400 MiB.
    out = vm("exec", f"mount {p1} /mnt/f", "mkfiles --verify /mnt/f/new 20 2000000", "umount /mnt/f",
             f"part resize {p1} 400M confirm")
    res.check("after a remount the guest's files verify", "verified 20 file(s)" in out, out)
    res.check("a second grow", "Resized" in out and "400.0 MiB" in out, out)
    g_400 = host_checks(res, "p1 at 400 MiB", blank, 1, tmp, files1, expect_total=400 * MIB)
    out = vm("exec", f"mount {p1} /mnt/f", "mkfiles --verify /mnt/f/new 20 2000000", "umount /mnt/f",
             f"part resize {p1} 400M confirm")
    res.check("after the second grow the guest's files verify", "verified 20 file(s)" in out, out)
    res.check("the same size again changes nothing", "already" in out, out)

    # 2. p2, in place: its FATs already cover `spare` more clusters.
    grow = spare * g2["spc"]
    out = vm("exec", f"part resize {p2} {p2_len + grow} confirm")
    res.check(f"part resize grows p2 by its {spare} spare clusters", "Resized" in out, out)
    g = host_checks(res, "p2 in place", blank, 2, tmp, files2, expect_total=p2_len + grow, same_layout=g2)
    res.check("p2 gained exactly its spare clusters", g["clusters"] == g2["clusters"] + spare,
              f"{g2['clusters']} -> {g['clusters']}")
    need = (spare - 1) * g2["spc"] * SECTOR       # more than the two clusters it had free
    out = vm("exec", f"mount {p2} /mnt/f", "mkdir /mnt/f/n", f"mkfiles /mnt/f/n 1 {need}",
             f"mkfiles --verify /mnt/f/n 1 {need}", "umount /mnt/f")
    res.check("the new clusters take a file the old volume had no room for",
              "created 1 file(s)" in out and "verified 1 file(s)" in out, out)
    host_checks(res, "p2 after the guest wrote", blank, 2, tmp, files2)

    # 3. p1 SHRINKS, 400 -> 280 MiB, its top files moving below the end.
    out = vm("exec", f"part resize {p1} 200M confirm")
    res.check("below FAT32's 65525-cluster floor the shrink is refused", "at least" in out, out)
    res.check("...and the table is untouched", part_geom(blank, 1)[1] == 400 * MIB)
    out = vm("exec", f"mount {p1} /mnt/f", "mkdir /mnt/f/pad", "mkfiles /mnt/f/pad 110 2000000",
             "mkdir /mnt/f/top", "mkfiles /mnt/f/top 10 2000000", "rm -r /mnt/f/pad", "df", "umount /mnt/f",
             timeout=2400)
    res.check("the guest puts files at the top of p1", "created 10 file(s)" in out, out[-600:])
    out = vm("exec", f"part resize {p1} 280M", f"part resize {p1} 280M confirm")
    res.check("before `confirm` a shrink says FAT32 keeps no journal", "no journal" in out, out)
    res.check("part resize shrinks p1, and says not to pull the disk",
              "Resized" in out and "280.0 MiB" in out and "shrinking" in out, out)
    res.check("the host's GPT says 280 MiB", part_geom(blank, 1)[1] == 280 * MIB)
    g = host_checks(res, "p1 at 280 MiB", blank, 1, tmp, files1, expect_total=280 * MIB)
    res.check("the FATs and the data start stayed", g["data"] == g_400["data"] and g["fat"] == g_400["fat"],
              f"data {g_400['data']} -> {g['data']}, fat {g_400['fat']} -> {g['fat']}")
    out = vm("exec", f"mount {p1} /mnt/f", "mkfiles --verify /mnt/f/top 10 2000000",
             "mkfiles --verify /mnt/f/new 20 2000000", "umount /mnt/f")
    res.check("the moved files verify in the guest", out.count("verified") >= 2 and "verified 10 file(s)" in out
              and "verified 20 file(s)" in out, out)


if __name__ == "__main__":
    sys.exit(main())
