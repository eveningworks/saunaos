#!/usr/bin/env python3
"""System Update offers a newer boot loader, asks, and writes it.

WHAT IT CHECKS, and what a broken version would still pass:

  - a fresh image's record (/boot/grub/core.modules, written by
    install_grub.py) is the staged one, byte for byte -- so the bar is
    HIDDEN there: an app that always showed it passes nothing below;
  - with an OLDER record planted (no `videoinfo`, an older GRUB) the
    window says so in its log AND the bar is drawn: accent-tinted pixels
    in the bar's reported rect, not merely a widget in the map;
  - its button opens the confirm dialog, which names the disk and
    what is written (its rows, as the app logs them) -- a bar that wrote
    on the click would skip this and fail here;
  - Update boot loader writes it: the app logs "written", the KERNEL logs
    `boot sector written` (an independent reader), and the record on
    disk is now the staged one, read by `sum` -- not by the app;
  - after the write the offer is gone for a second window: a new copy
    computes "same" from the records alone.

`--positive-control` plants nothing and still expects the bar, so the
check that the planted record SHOWS it must go red.

The disk's boot loader is rewritten with the images it was built with,
so the guest ends as it began. `install_test.py --media bootloader`
proves a disk boots on what this path writes.
"""

RUNNER = {"covers": "System Update's boot-loader offer: bar, confirm, write, record"}

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

APP = "/bin/wm/system/sysupdate"
TITLE = "System Update"
RECORD = "/boot/grub/core.modules"
STAGED = "/install/core.modules"
# A record from before videoinfo, by an older GRUB -- short, since the
# debug console's shell refuses a long `write` line rather than cut it.
OLD = "biosdisk part_gpt fat normal multiboot2 probe gzio loadenv"
OLD_VERSION = "grub-version=2.12-1"

_res = Results()
check = _res.check


def app_lines(dbg, lines):
    lines.extend(dbg.logs("sysupdate:", clear=True))
    return lines


START = "sysupdate: notes drawn by"   # the first line a new window logs


def wait_line(dbg, lines, pattern, timeout=90):
    """The first line matching `pattern` logged by the NEWEST window: one
    from a window already closed must not answer for the one just opened."""
    def find():
        got = app_lines(dbg, lines)
        start = max((i for i, ln in enumerate(got) if START in ln), default=0)
        for ln in got[start:]:
            m = re.search(pattern, ln)
            if m:
                return m
        return None
    return poll(find, timeout, step=0.3)


def sh(dbg, cmd, timeout=None):
    return dbg.send(f"sh {cmd}", timeout=timeout) or ""


def boot_writable(dbg, on):
    """/boot is mounted read-only; umount and mount again, as `mount` says."""
    sh(dbg, "umount /boot")
    sh(dbg, "mount 2 /boot" if on else "mount -r 2 /boot")


def same_bytes(dbg):
    """The record and the staged one by sha256 (`sum`), or the reason not."""
    hashes = []
    for path in (RECORD, STAGED):
        out = sh(dbg, f"sum -a sha256 {path}")
        m = re.search(r"^([0-9a-f]{64})\s", out, re.M)
        if not m:
            return False, f"{path}: {out.strip()[-120:]}"
        hashes.append(m.group(1))
    return hashes[0] == hashes[1], f"{hashes[0][:12]} vs {hashes[1][:12]}"


def accent_px(im, rect):
    x, y, w, h = rect
    n = 0
    for yy in range(y + 1, y + h - 1, 2):
        for xx in range(x + 1, x + w - 1, 3):
            r, g, b = im.getpixel((xx, yy))[:3]
            if b > r + 25 and b > 150 and r < 235:   # the accent's tint, not white or grey
                n += 1
    return n


def close_app(dbg, lines):
    """Closed once its check has ended: a close DURING a job only cancels
    it (sysupdate's on_close), and the window stays for the next close."""
    wait_line(dbg, lines, r"sysupdate: view \d+")
    dbg.close_window(TITLE)
    dbg.wait_gone(TITLE)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    ap.add_argument("--positive-control", action="store_true",
                    help="plant no older record and still expect the bar")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sysupdate_bootloader_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("System Update: the boot loader (base/ubootrefresh.h)")
    # No server is needed: the check fails at once on a closed port, and
    # the offer is computed after any check, failed or not.
    sh(dbg, "update --server http://127.0.0.1:9")

    check("a fresh image's record is the staged one", *same_bytes(dbg))

    lines = []
    dbg.logs("sysupdate:", clear=True)
    if not args.positive_control:
        boot_writable(dbg, True)
        sh(dbg, f"write {RECORD} {OLD}")
        sh(dbg, f"append {RECORD} {OLD_VERSION}")
        boot_writable(dbg, False)
        planted = sh(dbg, f"cat {RECORD}")
        if not check("an older record is planted", "videoinfo" not in planted and
                     "2.12-1" in planted and "2.16" not in planted, planted.strip()[-300:]):
            return _res.finish("sysupdate_bootloader_test")

    dbg.spawn(APP, TITLE)
    m = wait_line(dbg, lines, r"boot loader offer (\w+)(?: pending)? '([^']*)' \((\S+) -> (\S+)\)")
    check("the window finds the older record and says what is newer",
          m is not None and m.group(1) == "newer" and "modules" in m.group(2)
          and m.group(3) == "2.12-1", m.group(0) if m else "no offer line")
    # The button is greyed while a check runs; the check's view line is
    # its end (it fails at once on the closed port).
    wait_line(dbg, lines, r"sysupdate: view \d+")
    dbg.settle()
    w = dbg.widgets(TITLE)
    bar = w.get("bootbar")
    shot = os.path.join(args.tmp, f"sysupdate_boot_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    rect = (bar["screen"]["x"], bar["screen"]["y"], bar["w"], bar["h"]) if bar else None
    tint = accent_px(im, rect) if rect and rect[2] > 0 and rect[3] > 0 else 0
    if not check("the bar is drawn, in the accent's tint", tint > 50,
                 f"rect {rect}, {tint} tinted px (sampled)"):
        return _res.finish("sysupdate_bootloader_test")

    # Its button, from the bar's own report (uui_infobar's describe op).
    act = None
    for ln in reversed(app_lines(dbg, lines)):
        mm = re.search(r"sysupdate: layout bootbar\.action (-?\d+) (-?\d+) (\d+) (\d+)", ln)
        if mm:
            x, y, ww, hh = (int(v) for v in mm.groups())
            ox, oy = bar["screen"]["x"] - bar["x"], bar["screen"]["y"] - bar["y"]
            act = (ox + x + ww // 2, oy + y + hh // 2)
            break
    if not check("the bar reports its button", act is not None):
        return _res.finish("sysupdate_bootloader_test")
    dbg.click(*act)
    m = wait_line(dbg, lines, r"boot loader dialog, (\d+) rows", timeout=20)
    check("its button asks first, with what is written", m is not None and int(m.group(1)) >= 4,
          m.group(0) if m else "no dialog")
    dbg.settle()
    dlg = None
    for ln in reversed(app_lines(dbg, lines)):
        mm = re.search(r"sysupdate: layout dialog\.button 1 (-?\d+) (-?\d+) (\d+) (\d+)", ln)
        if mm:
            dlg = tuple(int(v) for v in mm.groups())
            break
    if not check("the dialog reports its Update button", dlg is not None):
        return _res.finish("sysupdate_bootloader_test")
    d = w["dialog"]
    ox, oy = d["screen"]["x"] - d["x"], d["screen"]["y"] - d["y"]
    dbg.click(ox + dlg[0] + dlg[2] // 2, oy + dlg[1] + dlg[3] // 2)
    m = wait_line(dbg, lines, r"boot loader (written|NOT written)", timeout=120)
    check("Update boot loader writes it", m is not None and m.group(1) == "written",
          m.group(0) if m else "no result line")
    klog = sh(dbg, "dmesg", timeout=30)
    check("...the kernel logged the boot sector", "boot sector written" in klog,
          "" if "boot sector written" in klog else "no `boot sector written` in dmesg")
    check("...and the record on disk is now the staged one", *same_bytes(dbg))

    close_app(dbg, lines)
    dbg.spawn(APP, TITLE)
    m = wait_line(dbg, lines, r"boot loader offer (\w+)")
    check("a new window offers nothing more", m is not None and m.group(1) == "same",
          m.group(0) if m else "no offer line")
    close_app(dbg, lines)

    # STAGED FOR THE NEXT BOOT: an update that touched the kernel leaves
    # /install's files as .upd until the restart, and writing before then
    # would write the OLD core. Said as pending, with no button, and the
    # shell's refresh refuses.
    sh(dbg, f"write {STAGED}.upd {OLD} videoinfo")
    sh(dbg, f"append {STAGED}.upd grub-version=9.99")
    dbg.spawn(APP, TITLE)
    m = wait_line(dbg, lines, r"boot loader offer (\w+)( pending)? '([^']*)'")
    check("an update waiting for the restart is said as pending",
          m is not None and m.group(2) == " pending", m.group(0) if m else "no offer line")
    close_app(dbg, lines)
    out = sh(dbg, "install --bootloader confirm", timeout=60)
    check("...and the refresh is refused until the restart", "restart first" in out and
          "boot sector" not in out, out.strip()[-200:])
    sh(dbg, f"rm {STAGED}.upd")

    # A GRUB OLDER than the disk's is a difference, not an upgrade, even
    # with modules to add: a record claiming 9.99 makes it "different".
    boot_writable(dbg, True)
    sh(dbg, f"write {RECORD} {OLD}")
    sh(dbg, f"append {RECORD} grub-version=9.99")
    boot_writable(dbg, False)
    dbg.spawn(APP, TITLE)
    m = wait_line(dbg, lines, r"boot loader offer (\w+)")
    check("an older staged GRUB is offered as different, never newer",
          m is not None and m.group(1) == "different", m.group(0) if m else "no offer line")
    close_app(dbg, lines)
    # The disk's record put back as the staged one, so the guest ends as it began.
    boot_writable(dbg, True)
    sh(dbg, f"cp {STAGED} {RECORD}")
    boot_writable(dbg, False)
    return _res.finish("sysupdate_bootloader_test")


if __name__ == "__main__":
    sys.exit(main())
