#!/usr/bin/env python3
"""USB sticks on QEMU: bulk-only transport, quick removal, a pull, a re-plug.

kernel/drivers/usb/usb_storage.c and the removal model in block.h
(docs/usb-storage-design.md), end to end on QEMU's usb-storage -- the
same bulk-only transport every real stick speaks. Two emulated sticks:
a FAT32 one built on the host with a known file, and a blank one with
4096-byte logical blocks.

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. Both sticks come up as usbN with their sizes and block sizes, and
     the FAT32 stick's partition is named -- a driver that declined the
     interface still boots; the NAMES are the assertion.
  2. A file the HOST put on the stick reads back byte for byte in the
     guest (`sum` against the host's own), and it is bigger than one
     64 KiB transfer, so the chunking is in the path.
  3. A file the GUEST writes is on the stick the moment `cp` returns:
     the stick is pulled straight after (QMP device_del) and the HOST
     reads the file out of the image -- quick removal, judged by the
     data, not by a flush count.
  4. The pulled mount is `removed` and refuses writes; unmounting it
     releases usb0 -- gone from lsblk, released in the log.
  5. A stick plugged in while running (device_add) is named, its
     partition found, mounted and read; `sync` is not failed by a pulled
     stick's mount; a stick pulled with NOTHING mounted is released in the
     unbind itself and the guest carries on.
  6. The 4 KiB stick is formatted, mounted, written and read back.
  7. Nothing logged a reset recovery, a timeout or a refused transfer.

    python3 tools/usb_storage_test.py
    python3 tools/usb_storage_test.py --instance 9
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import Results, copy_disk, poll  # noqa: E402
from qmp_test import QMPSession  # noqa: E402

TOOL = "usb_storage_test"
STICK_MB = 64
BIG = os.path.join(REPO, "seed", "sync", "bin", "dash")   # a known file, several transfers long
GUEST_BIG = "/bin/dash"


def host(*cmd, **kw):
    return subprocess.run(list(cmd), capture_output=True, text=True, **kw)


def fat_stick(path, label, files):
    """An MBR with one FAT32 partition at 1 MiB, holding `files` (name -> host path)."""
    with open(path, "wb") as f:
        f.truncate(STICK_MB << 20)
    host("sfdisk", "-q", path, input="label: dos\nstart=2048, type=c\n")
    sectors = ((STICK_MB << 20) // 512 - 2048) // 2   # mkfs.fat counts 1 KiB blocks
    host("mkfs.fat", "-F", "32", "-n", label, "--offset", "2048", path, str(sectors))
    for name, src in files.items():
        host("mcopy", "-i", f"{path}@@1M", src, f"::{name}")


def guest_sum(data):
    """What the guest's /bin/sum prints for these bytes: CRC-32 and the length."""
    return "%d %d" % (zlib.crc32(data), len(data))


class Guest:
    def __init__(self, instance, disk, sticks):
        self.base = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance)]
        self.instance = instance
        args = self.base + ["--disk", disk, "--mem", "1024"]
        for s in sticks:
            args += ["--usb-stick", s]
        r = subprocess.run(args + ["start"], cwd=REPO, capture_output=True, text=True)
        self.ok = r.returncode == 0
        self.why = (r.stdout + r.stderr).strip()[-300:]

    def run(self, *commands, timeout=60):
        r = subprocess.run(self.base + ["--timeout", str(timeout), "exec", *commands], cwd=REPO,
                           capture_output=True, text=True)
        return r.stdout

    def qmp(self):
        return QMPSession(port=4445 + self.instance)

    def stop(self):
        subprocess.run(self.base + ["stop"], cwd=REPO, capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=9)
    a = ap.parse_args()
    for t in ("sfdisk", "mkfs.fat", "mcopy", "mtype"):
        if not shutil.which(t):
            print(f"{TOOL}: needs {t} on the host (dosfstools, mtools)")
            return 1
    if not os.path.exists(BIG):
        print(f"{TOOL}: {BIG} missing -- run `make iso` first")
        return 1
    res = Results()
    tmp = tempfile.mkdtemp(prefix="usbstorage.")
    g = None
    try:
        disk = os.path.join(tmp, "disk.img")
        copy_disk(os.path.join(REPO, "disk.img"), disk, cwd=REPO)
        hello = os.path.join(tmp, "hello.txt")
        with open(hello, "w") as f:
            f.write("hello from a usb stick\n")
        stick = os.path.join(tmp, "stick.img")
        fat_stick(stick, "TESTSTICK", {"HELLO.TXT": hello, "BIG.BIN": BIG})
        blank4k = os.path.join(tmp, "blank4k.img")
        with open(blank4k, "wb") as f:
            f.truncate(STICK_MB << 20)
        replug = os.path.join(tmp, "replug.img")
        fat_stick(replug, "REPLUG", {"HELLO.TXT": hello})

        g = Guest(a.instance, disk, [stick, blank4k + ",4k"])
        if not res.check("the guest boots with two sticks attached", g.ok, g.why):
            return res.finish(TOOL)

        blk = g.run("lsblk")
        res.check("both sticks are named, with their block sizes",
                  "usb0" in blk and "usb1" in blk and "4096" in blk and "usb0p1" in blk, blk[-500:])

        big = open(BIG, "rb").read()
        want = guest_sum(big)
        out = g.run("mkdir /mnt/stick", "mount usb0p1 /mnt/stick", "cat /mnt/stick/HELLO.TXT",
                    "sum /mnt/stick/BIG.BIN")
        res.check("a file the host wrote reads back in the guest, past one transfer",
                  "hello from a usb stick" in out and want in out and len(big) > 65536,
                  f"want sum {want} of {len(big)} bytes; got {out.strip()[-200:]}")

        g.run(f"cp {GUEST_BIG} /mnt/stick/COPY.BIN")
        q = g.qmp()
        q.hmp("device_del ustick0")
        time.sleep(1)
        got = subprocess.run(["mtype", "-i", f"{stick}@@1M", "::COPY.BIN"], capture_output=True)
        res.check("a file the guest wrote is on the stick the moment cp returned (pulled at once)",
                  got.stdout == big, f"{len(got.stdout)} bytes on the host image, want {len(big)}; "
                                     f"{got.stderr.decode(errors='replace').strip()[:120]}")

        gone = poll(lambda: "removed" in g.run("mount"), 10)
        mounts = g.run("mount", "write /mnt/stick/X.TXT no")
        res.check("the pulled mount says removed and refuses a write",
                  gone and "read-only" in mounts, mounts.strip()[-300:])
        # A pulled stick's mount must not make a sync of everything else
        # fail: the mount is skipped, not flushed into a dead device.
        out = g.run("sync", "dmesg")
        res.check("sync succeeds while a pulled stick is still mounted",
                  "sync FAILED" not in out and "could not commit" not in out,
                  " | ".join(l for l in out.splitlines() if "sync" in l)[-300:])
        out = g.run("umount /mnt/stick", "lsblk", "dmesg")
        res.check("unmounting the pulled stick releases its disk",
                  "usb0p1" not in out.split("--- dmesg ---")[0] and "block: usb0 released" in out,
                  out.split("--- dmesg ---")[0].strip()[-300:])

        q.hmp(f"drive_add 0 if=none,id=ustickdrv9,file={replug},format=raw")
        q.hmp("device_add usb-storage,id=ustick9,drive=ustickdrv9,bus=xhci.0")
        # usb1 (the 4 KiB stick) is still there, so the lowest free name is usb0.
        named = poll(lambda: "usb0p1" in g.run("lsblk"), 15)
        out = g.run("mkdir /mnt/replug", "mount usb0p1 /mnt/replug", "cat /mnt/replug/HELLO.TXT")
        res.check("a stick plugged in while running takes the lowest free name, mounts, reads",
                  named and "hello from a usb stick" in out, out.strip()[-300:])

        out = g.run("mkfs -t fat32 usb1 confirm", "mkdir /mnt/k4", "mount usb1 /mnt/k4",
                    f"cp {GUEST_BIG} /mnt/k4/BIG", "sum /mnt/k4/BIG", timeout=180)
        res.check("the 4 KiB-block stick formats, mounts, writes and reads back",
                  want in out, out.strip()[-400:])

        # A pull with NOTHING mounted: the block layer releases the disk
        # inside the unbind itself -- the path that once read the device
        # after freeing it. The guest must still answer afterwards.
        g.run("umount /mnt/k4")
        q.hmp("device_del ustick1")
        released = poll(lambda: "block: usb1 released" in g.run("dmesg"), 10)
        alive = "usb1" not in g.run("lsblk") and "hello" in g.run("echo hello")
        res.check("a stick pulled with nothing mounted is released, and the guest carries on",
                  released and alive, f"released={released} alive={alive}")

        log = g.run("dmesg")
        bad = [ln for ln in log.splitlines()
               if any(k in ln for k in ("reset recovery", "never became ready", "failed:",
                                        "misaligned", "refused on usb"))]
        res.check("nothing logged a reset recovery, a timeout or a refused transfer",
                  not bad, " | ".join(bad[:4]))
    finally:
        if g:
            g.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    return res.finish(TOOL)


if __name__ == "__main__":
    sys.exit(main())
