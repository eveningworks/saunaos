#!/usr/bin/env python3
"""tools/part_test.py -- `part delete` against a table a FOREIGN tool wrote.

The host's own sfdisk writes a GPT onto a blank disk image, with what
toy-os's whole-table writer cannot carry: a Linux-filesystem type GUID,
a non-ASCII partition name and the RequiredPartition attribute bit. The
guest deletes partition 1 with `/bin/part`, and the HOST reads the image
back. That split is the point: the promise is "every other byte of the
table", and only a reader that shares no code with the writer can hold
the kernel to it --

  * every entry but the deleted one is byte-identical, in BOTH arrays;
  * the disk GUID is the one sfdisk chose, not a regenerated one;
  * sfdisk --verify accepts the result (both headers and their CRCs);
  * in the guest, the survivors keep their device NAMES (`p2`, `p3`),
    because a partition is numbered by its slot.

And the refusals, each of which must leave the image untouched: no
`confirm`, a whole disk, a mounted partition, the system disk's.

On demand (ondemand_sweep.py): it boots its own guest with a second disk.

    python3 tools/part_test.py [--kvm] [--keep DIR]
"""
import argparse
import os
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
BLANK_BYTES = 256 * 1024 * 1024

# Linux filesystem, a non-ASCII name, the RequiredPartition attribute;
# then two Microsoft basic data partitions. Sizes in sectors.
SFDISK_SCRIPT = """label: gpt
start=2048, size=65536, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="Lösung", attrs="RequiredPartition"
start=67584, size=65536, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="keep"
start=133120, size=65536, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="Säilytä", attrs="RequiredPartition"
"""


def gpt_raw(img):
    """(primary header, primary array, backup header, backup array) bytes."""
    with open(img, "rb") as f:
        f.seek(SECTOR)
        h = f.read(SECTOR)
        alt, = struct.unpack_from("<Q", h, 32)
        lba, = struct.unpack_from("<Q", h, 72)
        count, size = struct.unpack_from("<II", h, 80)
        f.seek(lba * SECTOR)
        arr = f.read(count * size)
        f.seek(alt * SECTOR)
        bh = f.read(SECTOR)
        blba, = struct.unpack_from("<Q", bh, 72)
        f.seek(blba * SECTOR)
        barr = f.read(count * size)
    return h, arr, bh, barr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the images here")
    args = ap.parse_args()
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    if not shutil.which("sfdisk"):
        print("part_test: needs sfdisk (util-linux) on the host -- the foreign writer and reader")
        return 1
    tmp = args.keep or tempfile.mkdtemp(prefix="part-")
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
    h0, arr0, bh0, barr0 = gpt_raw(blank)

    def vm(*argv):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", root, "--extra-disk", blank, "--extra-disk-kind", "virtio",
                            *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=300)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check("the guest came up", "ready" in vm("start")):
        return 1
    try:
        lsblk = vm("exec", "lsblk")
        disk = next((ln.split()[0] for ln in lsblk.splitlines()
                     if ln.strip().startswith("virtio") and "p" not in ln.split()[0]), None)
        if not res.check("the second disk is named", disk is not None, lsblk):
            return 1
        names = [f"{disk}p1", f"{disk}p2", f"{disk}p3"]
        res.check("its three partitions are named by slot", all(n in lsblk for n in names), lsblk)

        out = vm("exec", f"part delete {names[0]}")
        res.check("without `confirm` it says what would go", "DELETES" in out and "confirm" in out, out)
        res.check("...and changes nothing", gpt_raw(blank)[1] == arr0)
        out = vm("exec", f"part delete {disk} confirm")
        res.check("a whole disk is refused", "is a disk" in out, out)
        out = vm("exec", "part delete ata0p3 confirm")
        res.check("the system disk's partition is refused", "not deleted" in out and "system disk" in out, out)
        out = vm("exec", f"mkfs {names[1]} confirm", f"mount {names[1]} /mnt", f"part delete {names[1]} confirm")
        res.check("a mounted partition is refused", "mounted" in out and "not deleted" in out, out)
        vm("exec", "umount /mnt")
        # mkfs writes INSIDE p2; the table is sfdisk's still.
        res.check("no refusal touched the table", gpt_raw(blank)[1] == arr0)

        out = vm("exec", f"part delete {names[0]} confirm")
        res.check("partition 1 is deleted", "Deleted" in out, out)
        lsblk = vm("exec", "lsblk")
        res.check("its device is gone at once", f"{names[0]} " not in lsblk and names[0] + "\n" not in lsblk, lsblk)
        res.check("the others keep their names", names[1] in lsblk and names[2] in lsblk, lsblk)
    finally:
        vm("stop")

    h1, arr1, bh1, barr1 = gpt_raw(blank)
    res.check("the deleted entry is zero", arr1[:128] == bytes(128))
    res.check("every other entry is byte-identical", arr1[128:] == arr0[128:])
    res.check("the backup array matches the primary", barr1 == arr1)
    res.check("the disk GUID is sfdisk's", h1[56:72] == h0[56:72] and bh1[56:72] == h0[56:72])
    v = subprocess.run(["sfdisk", "--verify", blank], capture_output=True, text=True)
    res.check("sfdisk --verify accepts the table", v.returncode == 0 and "No errors detected" in v.stdout,
              v.stdout + v.stderr)
    dump = subprocess.run(["sfdisk", "--dump", blank], capture_output=True, text=True).stdout
    # --dump ESCAPES a non-ASCII name: "Säilytä" prints as S\xc3\xa4ilyt\xc3\xa4.
    named = 'name="Säilytä"' in dump or r'name="S\xc3\xa4ilyt\xc3\xa4"' in dump
    res.check("the non-ASCII name and the attribute survive", named and "RequiredPartition" in dump, dump)
    return res.finish("part_test")


if __name__ == "__main__":
    sys.exit(main())
