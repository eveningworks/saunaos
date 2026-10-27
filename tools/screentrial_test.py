#!/usr/bin/env python3
"""A screen size tried at a restart: GRUB sets it once, screentrial asks,
and every answer -- or none -- ends where it should.

lib/ubootmode.h keeps the size in grubenv for a display toy-os cannot set
itself. QEMU's QXL adapter has no driver here, so it runs on GRUB's VESA
framebuffer exactly as an RTX on a BIOS boot does. On a guest that
reboots:

  1. `bootcfg resolution 1024x768` sets a ONE-BOOT trial; the boot after
     is 1024x768 and the "Keep this resolution?" window (titled Display)
     is up; Enter keeps it, and the boot after THAT is still 1024x768
     with no window;
  2. a size the firmware does not offer (1366x768 here) comes up at GRUB's
     own size, says so, is remembered in /etc/display.conf, and the
     countdown restarts onto the kept 1024x768;
  3. a trial nobody answers (800x600) restarts by itself, onto 1024x768.

Runs against a COPY of disk.img and reboots it several times.

    python3 tools/screentrial_test.py [--instance N]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from harness import copy_disk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")
GUI = os.path.join(HERE, "gui_debug.py")


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


def gui(inst, cmd):
    p = subprocess.run([sys.executable, GUI, os.path.join(REPO, f".vm.{inst}.serial"), cmd],
                       cwd=REPO, capture_output=True, text=True, timeout=30)
    return p.stdout + p.stderr


MARK = "/tmp/screentrial_test.old"   # ramfs: gone once the guest has rebooted


def screen(inst):
    """The size vesafb took this boot, or None while the guest is down or
    still the boot reboot() left."""
    try:
        out = vm(inst, "exec", f"ls {MARK}", "dmesg -n 4000", timeout=20)
    except subprocess.TimeoutExpired:
        return None
    if "no such file" not in out.split("--- dmesg")[0]:
        return None
    m = re.search(r'display: using "\w+" -- (\d+x\d+)', out)
    return m.group(1) if m else None


def wait_screen(inst, want, deadline=240):
    """Bounded: poll until a boot reports `want`."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        if screen(inst) == want:
            return True
        time.sleep(3)
    return False


def has_window(inst, title):
    """`gui windows` rows are `z  title  x y w h | ...`, the title unquoted."""
    return re.search(rf"^\s*\d+\s+{re.escape(title)}\s", gui(inst, "gui windows"), re.M) is not None


def wait_window(inst, title, deadline=60):
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        if has_window(inst, title):
            return True
        time.sleep(1)
    return False


def reboot(inst):
    vm(inst, "exec", f"touch {MARK}")
    try:
        vm(inst, "exec", "reboot", timeout=30)
    except subprocess.TimeoutExpired:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    a = ap.parse_args()
    inst = a.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="screentrial_test_")
    disk = os.path.join(tmp, "disk.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    try:
        print("screentrial_test: booting a QXL guest that will reboot")
        out = vm(inst, "--disk", disk, "--vga", "qxl", "--reboot", "start", timeout=300)
        if "ready" not in out:
            print(f"screentrial_test: guest did not boot\n{out}")
            return 1
        base = screen(inst)
        r.check("QXL runs on GRUB's framebuffer at its own 1280x720", base == "1280x720", str(base))

        # 1. a trial, kept
        out = vm(inst, "exec", "bootcfg resolution 1024x768", "cat /boot/grub/grubenv")
        r.check("a size is set for the next boot only",
                "next boot only: 1024x768" in out and "toy_mode_try=1024x768" in out, out[-400:])
        reboot(inst)
        r.check("the next boot is 1024x768", wait_screen(inst, "1024x768"))
        r.check("and asks whether to keep it", wait_window(inst, "Display"))
        gui(inst, "gui key 0x0a")   # Enter: Keep, the default
        time.sleep(2)
        out = vm(inst, "exec", "bootcfg resolution")
        r.check("Keep makes it every boot's size", "every boot   1024x768" in out, out[-400:])
        r.check("and the trial is answered", "a trial of" not in out, out[-400:])
        reboot(inst)
        r.check("the boot after is still 1024x768", wait_screen(inst, "1024x768"))
        time.sleep(5)
        r.check("and asks nothing", not has_window(inst, "Display"))

        # 2. a size the firmware does not offer
        vm(inst, "exec", "bootcfg resolution 1366x768")
        reboot(inst)
        r.check("an unoffered size comes up at GRUB's own size", wait_screen(inst, "1280x720"))
        r.check("and says so", wait_window(inst, "Display"))
        conf = vm(inst, "exec", "cat /etc/display.conf")
        r.check("the refusal is remembered", "firmware_refused=1366x768" in conf, conf[-300:])
        r.check("its countdown restarts onto the kept size", wait_screen(inst, "1024x768", 120))

        # 3. a trial nobody answers
        vm(inst, "exec", "bootcfg resolution 800x600")
        reboot(inst)
        r.check("an unanswered trial boots at its size", wait_screen(inst, "800x600"))
        r.check("asks", wait_window(inst, "Display"))
        r.check("and restarts by itself onto the kept size", wait_screen(inst, "1024x768", 120))
        out = vm(inst, "exec", "bootcfg resolution")
        r.check("which is still every boot's size", "every boot   1024x768" in out, out[-400:])
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"screentrial_test: {'PASS' if not r.fails else f'{r.fails} FAILED'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
