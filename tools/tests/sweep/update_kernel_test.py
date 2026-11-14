#!/usr/bin/env python3
"""/bin/update's kernel install: the running kernel kept, GRUB's fallback, one run at a time.

WHAT IT PROVES, each read back through the guest's own `sum`, `cat`,
`dmesg` and `bootcfg`, or the host's mtools -- never update's own report:

  1. ONE RUN AT A TIME: a second `update` started while one is running is
     refused, naming the other, and the first finishes.
  2. ROOM FIRST: with /boot filled from the host, a kernel update is
     refused before anything is fetched ("not enough room on /boot"), and
     /boot/kernel.bin is untouched.
  3. A machine whose grub.cfg is from before the previous-kernel word and
     `set fallback` (both stripped on the host) gains both when a kernel
     installs, and the running kernel becomes kernel.old.
  4. A RESCUE BOOT KEEPS THE KERNEL THAT WORKS: booted from the
     previous-kernel entry, the next kernel update replaces kernel.bin and
     leaves kernel.old -- the kernel this boot runs -- byte for byte; the
     boot after runs the new one. The old rotation deleted it.
  5. GRUB FALLS BACK BY ITSELF: with kernel.bin deleted on the host (a cut
     between the install's delete and rename), the machine still boots,
     on the previous kernel, with nobody at the menu.

Each kernel is this build's with one boot message changed to a same-length
twin, so which one booted is read from `dmesg`.

**IT NEVER LEAVES THIS MACHINE**: update_test.py's server, in this process
on a loopback port, reached through SLIRP's 10.0.2.2. Needs mtools.

    python3 tools/tests/sweep/update_kernel_test.py
"""

RUNNER = {"covers": "/bin/update's kernel: rescue boot keeps kernel.old, GRUB fallback, lock, free space"}

import argparse
import os
import re
import shutil
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
ROOT = os.path.dirname(HERE)

from net_test import launch  # noqa: E402
import update_server  # noqa: E402
from harness import Results, copy_disk, poll  # noqa: E402
from update_test import Server, summed, GATEWAY, BOOT_MSG  # noqa: E402
from update_cut_test import kill_and_wait  # noqa: E402
from fat32_test import esp_window, mtools_at, mrun  # noqa: E402

TWIN = b"SaunaOS: KERNEL heap initialized"    # the first new kernel
TWIN2 = b"SaunaOS: kernel HEAP initialized"   # the one installed after the rescue boot


def kernel_saying(tmp, msg, name):
    with open(os.path.join(ROOT, "build", "kernel.bin"), "rb") as fh:
        data = fh.read()
    if data.count(BOOT_MSG) != 1 or len(msg) != len(BOOT_MSG):
        return None
    out = os.path.join(tmp, name)
    with open(out, "wb") as fh:
        fh.write(data.replace(BOOT_MSG, msg))
    return out


def crc_of(path):
    with open(path, "rb") as fh:
        data = fh.read()
    return zlib.crc32(data), len(data)


class Esp:
    """The image's boot volume, from the host."""

    def __init__(self, disk):
        start, _ = esp_window(disk)
        self.at = mtools_at(disk, start) if start is not None else None
        self.local = disk + ".esp"

    def read(self, path):
        r = mrun(["mtype", "-i", self.at, "::" + path])
        return None if r.returncode else r.stdout

    def write(self, path, text):
        with open(self.local, "w") as fh:
            fh.write(text)
        return mrun(["mcopy", "-o", "-i", self.at, self.local, "::" + path]).returncode == 0

    def free(self):
        # mdir's last line: "  N files  M bytes" then "  F bytes free",
        # counted from the FAT itself.
        r = mrun(["mdir", "-i", self.at, "::/"])
        for ln in r.stdout.splitlines():
            if ln.strip().endswith("bytes free"):   # "60 563 456 bytes free": grouped
                digits = "".join(ch for ch in ln.split("bytes free")[0] if ch.isdigit())
                return int(digits) if digits else None
        return None

    def fill(self, leave):
        free = self.free()
        if free is None or free <= leave:
            return False
        with open(self.local, "wb") as fh:
            fh.truncate(free - leave)
        return mrun(["mcopy", "-o", "-i", self.at, self.local, "::/filler"]).returncode == 0

    def delete(self, path):
        return mrun(["mdel", "-i", self.at, "::" + path]).returncode == 0


def make_old_style(esp):
    """grub.cfg as a machine from before this change has it: a menu (timeout
    1), no `set fallback`, no previous-kernel word."""
    text = esp.read("/grub/grub.cfg")
    if not text or "set timeout=0" not in text:
        return False
    text = text.replace("set timeout=0", "set timeout=1")
    text = "\n".join(ln for ln in text.splitlines() if not ln.strip().startswith("set fallback=")) + "\n"
    text = text.replace(" kernel=previous", "")
    return esp.write("/grub/grub.cfg", text)


def booted_words(sh):
    # Escapes stripped: the console's colour reset trails the line.
    for ln in re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", sh.run("bootcfg list")).splitlines():
        if "this boot's words:" in ln:
            return ln.split("this boot's words:", 1)[1].split()
    return []


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the files in the temporary directory")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("update_kernel_test: SKIP -- no disk.img; run `make iso` first")
        return 0
    if not shutil.which("mtools") and not shutil.which("mcopy"):
        print("update_kernel_test: SKIP -- needs mtools")
        return 0
    why = update_server._stale_reason(os.path.join(ROOT, "seed", "sync"))
    if why:
        print(f"update_kernel_test: FAIL -- the staging tree is stale ({why}); run `make iso`")
        return 1

    c = Results()
    tmp = tempfile.mkdtemp(prefix="update_kernel_test.")
    srv = Server()
    url = f"http://{GATEWAY}:{srv.port}"
    pidfile = None
    try:
        twin = kernel_saying(tmp, TWIN, "kernel.twin")
        twin2 = kernel_saying(tmp, TWIN2, "kernel.twin2")
        if not c.ok("two twin kernels can be made (one boot message changed)", twin and twin2):
            return 1
        disk = os.path.join(tmp, "kernel.img")
        copy_disk(disk_src, disk)
        esp = Esp(disk)
        c.ok("the image's grub.cfg can be made old-style from the host", esp.at and make_old_style(esp))

        # 1. one run at a time
        print("update_kernel_test: booting")
        sh, pidfile = launch(disk, tmp, "k1", "e1000")
        # The image's own kernel, as the guest reads it: the medium's may
        # be stripped or gzipped, so never build/kernel.bin's bytes.
        orig = summed(sh, "/boot/kernel.bin")
        c.ok("the image's kernel can be read back", orig is not None)
        # The first in the background (`spawn`: the debug console has no
        # `&`); it compares every file's crc, so it runs for many seconds.
        sh.run(f"spawn /bin/update --from {url} --check")
        sh.run("sleep 2")
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("a second update while one runs is refused, naming it",
             "another update is running" in out and "pid" in out, out.strip()[-200:])
        # Its own log, which the refused run must not have truncated.
        first = poll(lambda: "compared" in sh.run("cat /var/log/update.log"), 600, step=3)
        c.ok("...and the first one finishes, its log intact", first,
             sh.run("cat /var/log/update.log").strip()[-200:])

        # 2. room on /boot before anything is fetched
        sh.run("sync")
        kill_and_wait(pidfile)
        c.ok("/boot can be filled from the host", esp.fill(256 * 1024))
        sh, pidfile = launch(disk, tmp, "k2", "e1000")
        srv.manifest.kernel_elf, srv.manifest.kernel_media = twin, ""
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("with /boot full a kernel update is refused before fetching",
             "not enough room on /boot" in out and "fetching" not in out, out.strip()[-240:])
        c.ok("...and /boot/kernel.bin is untouched", summed(sh, "/boot/kernel.bin") == orig)
        sh.run("sync")
        kill_and_wait(pidfile)
        c.ok("the filler can be removed from the host", esp.delete("/filler"))

        # 3. an install on an old-style grub.cfg
        sh, pidfile = launch(disk, tmp, "k3", "e1000")
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("the kernel installs", "kernel: installed; the running kernel is kept" in out, out.strip()[-240:])
        c.ok("...the running kernel is now kernel.old", summed(sh, "/boot/kernel.old") == orig)
        cfg = sh.run("cat /boot/grub/grub.cfg")
        c.ok("...grub.cfg gained `set fallback=`", "set fallback=1" in cfg)
        c.ok("...and the previous-kernel word on the kernel.old entry",
             any("/kernel.old" in ln and "kernel=previous" in ln for ln in cfg.splitlines()))
        c.ok("...and the staged kernel is gone from the root",
             "kernel.upd" not in sh.run("ls /var/lib/update"))

        # 4. a rescue boot, then an update
        out = sh.run("bootcfg default 1")
        sh.run("sync")
        kill_and_wait(pidfile)
        sh, pidfile = launch(disk, tmp, "k4", "e1000")
        dm = sh.run("dmesg")
        c.ok("the rescue boot runs the previous kernel (the original)",
             BOOT_MSG.decode() in dm and TWIN.decode() not in dm)
        c.ok("...and GRUB told it so", "kernel=previous" in booted_words(sh), " ".join(booted_words(sh)))
        srv.manifest.kernel_elf = twin2
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("an update on the rescue boot installs, saying it keeps kernel.old",
             "this boot runs the previous kernel" in out, out.strip()[-240:])
        c.ok("...kernel.old is still the kernel that works", summed(sh, "/boot/kernel.old") == orig)
        c.ok("...and kernel.bin is the new one", summed(sh, "/boot/kernel.bin") == crc_of(twin2))
        sh.run("bootcfg default 0")
        sh.run("sync")
        kill_and_wait(pidfile)
        sh, pidfile = launch(disk, tmp, "k5", "e1000")
        c.ok("the next boot runs the new kernel", TWIN2.decode() in sh.run("dmesg"))

        # 5. GRUB's fallback
        sh.run("sync")
        kill_and_wait(pidfile)
        c.ok("kernel.bin can be deleted from the host", esp.delete("/kernel.bin"))
        sh, pidfile = launch(disk, tmp, "k6", "e1000")
        dm = sh.run("dmesg")
        c.ok("with no kernel.bin the machine still boots, on the previous kernel",
             "kernel=previous" in booted_words(sh) and BOOT_MSG.decode() in dm,
             " ".join(booted_words(sh)))
    finally:
        if pidfile:
            kill_and_wait(pidfile)
        srv.srv.shutdown()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"update_kernel_test: files kept in {tmp}")

    bad = c.failed()
    print(f"\nupdate_kernel_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
