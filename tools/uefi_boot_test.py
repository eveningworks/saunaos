#!/usr/bin/env python3
"""A UEFI boot off the disk's own loader, and the screen size it can reach.

The disk's ESP carries GRUB's EFI build at EFI/BOOT/BOOTX64.EFI
(tools/install_grub.py), so a machine with CSM off boots it, and GRUB sets
the framebuffer through GOP at the size grubenv names -- what lets a card
with no driver here (an RTX) leave the VESA BIOS's list behind. On a copy
of disk.img, under OVMF with QXL (no driver: vesafb on GOP's framebuffer):

  1. with the loader deleted, UEFI cannot boot the disk -- the control
     that the rest is about the loader and not the ISO or luck;
  2. `install --bootloader confirm` from a BIOS boot puts it back, the
     in-place upgrade an installed machine takes;
  3. UEFI then boots the disk alone to the desktop at GRUB's 1280x720;
  4. 2560x1440, refused on the BIOS boot (as the X99's VESA BIOS did), is
     still offered under UEFI -- each firmware keeps its own refusals --
     and `bootcfg resolution 2560x1440` makes the next UEFI boot that size,
     one QXL's VESA BIOS does not offer.

Needs OVMF on the host (tools/ovmf.py); without it the run is a SKIP.

    python3 tools/uefi_boot_test.py [--instance N]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")

import install_grub  # noqa: E402
import ovmf  # noqa: E402
from harness import Results, copy_disk, poll  # noqa: E402

TOOL = "uefi_boot_test"


def vm(inst, *args, timeout=240):
    try:
        p = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=REPO,
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ""
    return p.stdout + p.stderr


def boot(inst, disk, uefi, timeout=120):
    """Start the guest; True once its debug console answers."""
    vm(inst, "stop")
    args = ["--disk", disk, "--boot", "disk", "--vga", "qxl", "--timeout", str(timeout)]
    if uefi:
        args.append("--uefi")
    return "ready" in vm(inst, *args, "start", timeout=timeout + 60)


def screen(inst):
    """The size the display layer took, and whether the desktop started."""
    out = vm(inst, "exec", "dmesg -n 4000", timeout=30)
    m = re.search(r'display: using "(\w+)" -- (\d+x\d+)', out)
    return (m.group(1), m.group(2)) if m else (None, None), "wm: entering GUI mode" in out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    inst = ap.parse_args().instance
    r = Results()
    if not ovmf.firmware():
        r.skip("UEFI boot", ovmf.NOT_FOUND)
        return r.finish(TOOL)

    tmp = tempfile.mkdtemp(prefix=f"{TOOL}_")
    disk = os.path.join(tmp, "disk.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    try:
        if not r.check("disk.img carries a UEFI loader", install_grub.has_efi_loader(disk)):
            return r.finish(TOOL)
        _bios, esp = install_grub.parts_of(disk)
        subprocess.run(["mdel", "-i", install_grub.mtools_at(disk, esp),
                        install_grub.EFI_LOADER], check=True, capture_output=True)

        # 1. the control
        r.check("without EFI/BOOT/BOOTX64.EFI, UEFI does not boot the disk",
                not boot(inst, disk, uefi=True, timeout=45))

        # 2. the in-place upgrade, from a BIOS boot
        if not r.check("the copy boots BIOS", boot(inst, disk, uefi=False)):
            return r.finish(TOOL)
        out = vm(inst, "exec", "config get firmware.uefi", timeout=30)
        r.check("the kernel says BIOS/CSM", re.search(r"^0$", out, re.M), out[-200:])
        # What the X99 learned from its VESA BIOS on 2026-10-27.
        conf = os.path.join(tmp, "display.conf")
        with open(conf, "w") as f:
            f.write("firmware_refused=2560x1440\n")
        vm(inst, "put", conf, "/etc/display.conf", timeout=60)
        out = vm(inst, "exec", "bootcfg resolution", timeout=30)
        r.check("a BIOS-refused 2560x1440 is marked refused there", "2560x1440(refused)" in out,
                out[-300:])
        out = vm(inst, "exec", "install --bootloader confirm", "ls /boot/EFI/BOOT", timeout=120)
        r.check("install --bootloader writes the UEFI loader",
                "BOOTX64.EFI" in out and "FAILED" not in out, out[-600:])
        vm(inst, "exec", "sync")

        # 3. UEFI, off the disk alone
        if not r.check("UEFI boots the disk", boot(inst, disk, uefi=True)):
            return r.finish(TOOL)
        got = poll(lambda: (lambda s: s if s[1] else None)(screen(inst)), 90)
        (drv, size), desktop = got or ((None, None), False)
        r.check("to the desktop, on vesafb at GRUB's own 1280x720",
                desktop and drv == "vesafb" and size == "1280x720", f"{drv} {size}")

        # 4. a size past the VESA list
        out = vm(inst, "exec", "config get firmware.uefi", "bootcfg resolution", timeout=30)
        r.check("the kernel says UEFI", re.search(r"^1$", out, re.M), out[-300:])
        r.check("and the BIOS's refusal does not grey 2560x1440 out here",
                "2560x1440" in out and "2560x1440(refused)" not in out, out[-300:])
        out = vm(inst, "exec", "bootcfg resolution 2560x1440", "cat /boot/grub/grubenv")
        r.check("the next boot's size is set in grubenv", "toy_mode_try=2560x1440" in out, out[-300:])
        vm(inst, "exec", "sync")
        r.check("the next UEFI boot comes up", boot(inst, disk, uefi=True))
        got = poll(lambda: (lambda s: s if s[1] else None)(screen(inst)), 90)
        (drv, size), desktop = got or ((None, None), False)
        r.check("at 2560x1440, through GOP", desktop and size == "2560x1440", f"{drv} {size}")
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)
    return r.finish(TOOL)


if __name__ == "__main__":
    sys.exit(main())
