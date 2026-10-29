#!/usr/bin/env python3
"""tools/tfs3_grow_test.py -- `part resize` grows a FILLED tfs3 volume, twice.

The guest boots the ordinary system disk (pinned with `root=ata0p3`) and a
second, blank virtio disk the HOST partitioned with sfdisk: a 48 MiB
partition with free space after it, and a 64 MiB one further on. The
guest formats the first tfs3 and fills it with `mkfiles`, then grows it
with `/bin/part` -- the table, then the filesystem -- and each claim is
believed from a path that is not `part`'s own output:

  * the HOST reads the GPT back (sfdisk --dump) for the new length, and
    the tfs3 superblock inside the partition for the new block count --
    a table grown under an ungrown filesystem fails here;
  * `fsck` on the mounted volume finds nothing;
  * every file holds its own bytes (`mkfiles --verify`, content derived
    from index and offset) and two of them hash as before (`sum`);
  * the NEW space is used: more files are written than the old volume
    could hold, verified, and fsck'd again;
  * a remount, and a SECOND grow from three groups to six -- the case
    where group 1's backup lies inside the old volume.

And the refusals, each leaving the table as it was: no `confirm`, a
mounted partition, a shrink, an exFAT partition, the system disk's.

A grow cut short at each of its writes is the KTEST's job
(kernel/fs/tfs3_resize_test.c). The positive controls are mutate.py runs
(docs/tools.md has them): `part` with its filesystem step removed must
fail the host's superblock check, and a grow that leaves the old last
group's tail marked in use must fail fsck.

On demand (ondemand_sweep.py): it boots its own guest with a second disk.

    python3 tools/tfs3_grow_test.py [--instance N] [--kvm] [--keep DIR]
"""
import argparse
import os
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
P1_START, P1_MIB = 2048, 48
P2_START, P2_MIB = 900 * MIB, 64
SFDISK_SCRIPT = f"""label: gpt
start={P1_START}, size={P1_MIB * MIB}, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="grow"
start={P2_START}, size={P2_MIB * MIB}, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="exfat"
"""
T3_SB_BLOCK = 8


def part_len(img, n):
    """Partition n's length in sectors, as the host's sfdisk reads the GPT."""
    dump = subprocess.run(["sfdisk", "--dump", img], capture_output=True, text=True).stdout
    for ln in dump.splitlines():
        m = re.match(rf"\S+{n} : start=\s*(\d+), size=\s*(\d+)", ln)
        if m:
            return int(m.group(2))
    return None


def t3_total_blocks(img, start):
    """The tfs3 superblock's block count, read off the image by the host."""
    with open(img, "rb") as f:
        f.seek(start * SECTOR + T3_SB_BLOCK * 4096)
        sb = f.read(48)
    if sb[:4] != b"TFS3":
        return None
    return struct.unpack_from("<I", sb, 8)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None, metavar="N",
                    help="the VM slot (vm.py's numbering); default: the lowest free one")
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the images here")
    args = ap.parse_args()
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    if not shutil.which("sfdisk"):
        print("tfs3_grow_test: needs sfdisk (util-linux) on the host -- the table's writer and reader")
        return 1
    tmp = args.keep or tempfile.mkdtemp(prefix="tfs3grow-")
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
    with open(blank, "wb") as f:
        f.truncate(BLANK_BYTES)
    subprocess.run(["sfdisk", "--quiet", blank], input=SFDISK_SCRIPT.encode(), check=True)

    def vm(*argv, timeout=900):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", root, "--extra-disk", blank, "--extra-disk-kind", "virtio",
                            *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start")):
        return 1
    try:
        return drive(args, res, vm, blank)
    finally:
        vm("stop")


def drive(args, res, vm, blank):
    del args
    lsblk = vm("exec", "lsblk")
    disk = next((ln.split()[0] for ln in lsblk.splitlines()
                 if ln.strip().startswith("virtio") and "p" not in ln.split()[0]), None)
    if not res.check("the second disk is named", disk is not None, lsblk):
        return res.finish("tfs3_grow_test")
    p1, p2 = f"{disk}p1", f"{disk}p2"

    out = vm("exec", f"mkfs {p1} confirm", f"mkfs -t exfat {p2} confirm", "mkdir /mnt/g",
             f"mount {p1} /mnt/g", "mkdir /mnt/g/a", "mkfiles /mnt/g/a 160 200000-260000", "sum -a sha256 /mnt/g/a/f000007 /mnt/g/a/f000150",
             "df")
    res.check("a 48 MiB tfs3 volume is filled (160 files, ~36 MiB)", "created 160 file(s)" in out, out)
    sums0 = re.findall(r"^([0-9a-f]{64})\s", out, re.M)
    res.check("two files are hashed", len(sums0) == 2, out)
    old_blocks = t3_total_blocks(blank, P1_START)
    res.check("the host reads the tfs3 superblock", old_blocks is not None)

    # The refusals, each changing nothing.
    out = vm("exec", f"part resize {p1} 300M confirm")
    res.check("a mounted partition is refused", "mounted" in out and "cannot be resized" in out, out)
    vm("exec", "umount /mnt/g")
    out = vm("exec", f"part resize {p1} 300M")
    res.check("without `confirm` it says what would change",
              "48.0 MiB to 300.0 MiB" in out and "tfs3 filesystem follows" in out, out)
    out = vm("exec", f"part resize {p1} 20M confirm")
    res.check("a shrink is refused, with the reason", "cannot shrink" in out and "not built yet" in out, out)
    out = vm("exec", f"part resize {p2} 100M confirm")
    res.check("an exFAT partition is refused, with the reason", "exFAT volumes cannot be resized yet" in out, out)
    out = vm("exec", "part resize ata0p3 +1M confirm")
    res.check("the system disk's partition is refused", "system disk" in out, out)
    res.check("no refusal touched the table", part_len(blank, 1) == P1_MIB * MIB and part_len(blank, 2) == P2_MIB * MIB)
    res.check("...or the filesystem", t3_total_blocks(blank, P1_START) == old_blocks)

    # 1. 48 -> 300 MiB: one group to three.
    out = vm("exec", f"part resize {p1} 300M confirm")
    res.check("part resize grows it", "Resized" in out and "300.0 MiB" in out, out)
    res.check("the host's GPT says 300 MiB", part_len(blank, 1) == 300 * MIB, str(part_len(blank, 1)))
    blocks = t3_total_blocks(blank, P1_START)
    res.check("the host's superblock says 300 MiB", blocks == 300 * MIB // 8, f"{blocks} blocks (was {old_blocks})")

    out = vm("exec", "mount " + p1 + " /mnt/g", "fsck /mnt/g", "mkfiles --verify /mnt/g/a 160 200000-260000",
             "sum -a sha256 /mnt/g/a/f000007 /mnt/g/a/f000150", "df")
    res.check("fsck is clean after the grow", "fsck: clean." in out, out)
    res.check("every file holds its bytes", "verified 160 file(s)" in out, out)
    res.check("the two hashes are the same", len(sums0) == 2 and re.findall(r"^([0-9a-f]{64})\s", out, re.M) == sums0,
              out)

    # The new space: 200 more files, ~45 MiB -- past what 48 MiB held.
    out = vm("exec", "mkdir /mnt/g/b", "mkfiles /mnt/g/b 200 200000-260000", "mkfiles --verify /mnt/g/b 200 200000-260000",
             "fsck /mnt/g")
    res.check("the new space takes 200 more files", "created 200 file(s)" in out and "verified 200 file(s)" in out, out)
    res.check("fsck is clean after filling it", "fsck: clean." in out, out)

    # A remount: nothing lived only in memory.
    out = vm("exec", "umount /mnt/g", "mount " + p1 + " /mnt/g", "mkfiles --verify /mnt/g/a 160 200000-260000",
             "mkfiles --verify /mnt/g/b 200 200000-260000", "fsck /mnt/g", "umount /mnt/g")
    res.check("after a remount both sets verify", "verified 160 file(s)" in out and "verified 200 file(s)" in out, out)
    res.check("...and fsck is clean", "fsck: clean." in out, out)

    # 2. 300 -> 700 MiB: three groups to six.
    out = vm("exec", f"part resize {p1} 700M confirm")
    res.check("a second grow", "Resized" in out and "700.0 MiB" in out, out)
    res.check("the host's GPT says 700 MiB", part_len(blank, 1) == 700 * MIB)
    blocks = t3_total_blocks(blank, P1_START)
    res.check("the host's superblock says 700 MiB", blocks == 700 * MIB // 8, str(blocks))
    out = vm("exec", "mount " + p1 + " /mnt/g", "fsck /mnt/g", "mkfiles --verify /mnt/g/a 160 200000-260000",
             "mkfiles --verify /mnt/g/b 200 200000-260000", "mkdir /mnt/g/c", "mkfiles /mnt/g/c 40 1000000",
             "mkfiles --verify /mnt/g/c 40 1000000", "fsck /mnt/g", "umount /mnt/g")
    res.check("after the second grow every file verifies",
              "verified 160 file(s)" in out and "verified 200 file(s)" in out and "verified 40 file(s)" in out, out)
    res.check("...and fsck is clean, twice", out.count("fsck: clean.") == 2, out)

    # Already that size: nothing to do, nothing changed.
    out = vm("exec", f"part resize {p1} 700M confirm")
    res.check("the same size again changes nothing", "already" in out, out)
    return res.finish("tfs3_grow_test")


if __name__ == "__main__":
    sys.exit(main())
