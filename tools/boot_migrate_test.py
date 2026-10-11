#!/usr/bin/env python3
"""A machine with the old /boot/boot layout moves up to /boot by itself,
and boots from the moved files afterwards.

Disks written before the flat layout kept kernel.bin and grub/ one level
down on the boot volume, with GRUB's core image reading `/boot/grub`. The
`bootmigrate` service (`install --migrate confirm`) copies them up,
rewrites the core image to read `/grub`, then removes the old tree. Only
a real boot can tell whether GRUB found the result, so this:

  1. writes the OLD layout onto a COPY of disk.img
     (`install_grub.py --legacy-layout`), with a kernel.old beside the
     kernel and a marker word on the default entry's command line;
  2. boots it -- the old core reads the old tree, which the marker on the
     running command line proves -- and lets the service run;
  3. checks /boot is flat, /boot/boot is gone, kernel.old came along and
     grub.cfg names /kernel.bin with the marker intact;
  4. reboots, and checks the machine comes back with the marker still on
     its command line: that boot can only have read /boot/grub/grub.cfg
     through the NEW core image, since the old tree no longer exists.

    python3 tools/boot_migrate_test.py [--instance N]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import copy_disk  # noqa: E402
import install_grub  # noqa: E402

VM = os.path.join(HERE, "vm.py")
MARKER = "migrate_marker"


class Result:
    def __init__(self):
        self.fails = 0

    def check(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}"
              + (f"\n          {detail}" if detail and not ok else ""))
        self.fails += 0 if ok else 1


def vm(inst, *args, timeout=180):
    p = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=REPO,
                       capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def wait_for(inst, cmd, want, deadline=240):
    """Poll `cmd` until its output holds `want`, bounded. Across a reboot
    the debug console is gone until the machine is back."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        try:
            out = vm(inst, "exec", cmd, timeout=20)
        except subprocess.TimeoutExpired:
            continue
        if want in out:
            return out
        time.sleep(2)
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    a = ap.parse_args()
    inst = a.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="boot_migrate_test_")
    disk = os.path.join(tmp, "disk.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    try:
        cfg = os.path.join(tmp, "grub.cfg")
        with open(os.path.join(REPO, "build", "grub-disk.cfg")) as f:
            text = f.read()
        # The marker goes on the default entry only: it is what says
        # which grub.cfg the running kernel was started from.
        text = text.replace("multiboot2 /kernel.bin ", f"multiboot2 /kernel.bin {MARKER} ", 1)
        with open(cfg, "w") as f:
            f.write(text)
        kernel = os.path.join(REPO, "build", "kernel.media")
        install_grub.install(disk, kernel, cfg, verbose=False, legacy=True)
        _bios, esp = install_grub.parts_of(disk)
        install_grub.mcopy_into(disk, esp, [kernel], "::/boot/kernel.old")
        legacy_cfg = install_grub.mtype(disk, esp, "::/boot/grub/grub.cfg") or ""
        if f"multiboot2 /boot/kernel.bin {MARKER}" not in legacy_cfg or \
                install_grub.mtype(disk, esp, "::/grub/grub.cfg") is not None:
            print("boot_migrate_test: the old-layout fixture was not written\n" + legacy_cfg[-400:])
            return 1

        print("boot_migrate_test: booting the old layout")
        out = vm(inst, "--disk", disk, "--boot", "disk", "--reboot", "start", timeout=300)
        if "ready" not in out:
            print(f"boot_migrate_test: guest did not boot\n{out}")
            return 1
        words = vm(inst, "exec", "bootcfg")
        r.check("the first boot read the OLD grub.cfg (marker on the command line)",
                MARKER in words.split("this boot's words:")[-1], words[-300:])

        done = wait_for(inst, "service status bootmigrate", "done")
        r.check("the bootmigrate service ran to completion", bool(done),
                vm(inst, "exec", "service status bootmigrate")[-300:])
        log = vm(inst, "exec", "dmesg")
        listing = vm(inst, "exec", "ls /boot", "ls /boot/boot")
        r.check("/boot holds grub/, kernel.bin and kernel.old",
                all(n in listing for n in ("grub/", "kernel.bin", "kernel.old")), listing)
        r.check("/boot/boot is gone", "no such file" in listing.split("ls /boot/boot")[-1].lower(),
                listing + "\n" + "\n".join(ln for ln in log.splitlines() if "install" in ln)[-600:])
        moved = vm(inst, "exec", "cat /boot/grub/grub.cfg")
        r.check("grub.cfg names /kernel.bin and keeps the machine's own words",
                f"multiboot2 /kernel.bin {MARKER}" in moved and "/boot/kernel" not in moved,
                moved[-400:])
        mounts = vm(inst, "exec", "mount")
        r.check("/boot is read-only again", any(
            ln.split()[1:2] == ["/boot"] and ln.split()[-1].startswith("ro")
            for ln in mounts.splitlines() if len(ln.split()) >= 4), mounts)

        try:
            vm(inst, "exec", "reboot", timeout=30)
        except subprocess.TimeoutExpired:
            pass
        time.sleep(5)
        back = wait_for(inst, "bootcfg", "this boot's words")
        r.check("the machine came back after the reboot", bool(back))
        r.check("...through the NEW core image: the marker is on the command line, "
                "and only /boot/grub/grub.cfg still has it",
                bool(back) and MARKER in back.split("this boot's words:")[-1], back[-300:])
        again = vm(inst, "exec", "install --migrate")
        r.check("a second migrate has nothing to do", "nothing to move" in again, again[-200:])
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"boot_migrate_test: {'PASS' if not r.fails else f'{r.fails} FAILED'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
