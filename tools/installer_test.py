#!/usr/bin/env python3
"""tools/installer_test.py -- the Install SaunaOS window, on the live ISO,
onto a blank disk -- and then that disk, booted on its own.

Phase 1 boots saunaos-live.iso with one BLANK 512 MiB IDE disk and checks
what a live session offers before anybody asks: the "Install SaunaOS"
launcher on the desktop, the compositor's live-session notice, and NO
First-boot setup (a live session never shows it). It sets two settings
the way a person trying the session would -- the computer's name and the
region -- then drives the window through Welcome, Disk and Ready, CANCELS
the erase confirm, and checks from the host that the disk is still
blank. Then it confirms, and waits for Finished.

Phase 0 boots the live image with `installer=on` -- the live boot menu's
Install SaunaOS entry -- and checks the window is up full screen at once
AND that the desktop has its launcher behind it, so "Keep trying SaunaOS"
leaves a way back in; no notice, since the window is already open. Its
ISO is made here with grub-mkrescue, one entry; the phase SKIPS without it.

Phase 2 boots the disk with nothing else attached and checks it is the
installed system: its root is the disk's third partition, the name set
in the live session is there, and First-boot setup will skip exactly the
two pages that session answered (`fbs`'s report of UFBS_ANSWERED) and
nothing else. An installed machine gets no launcher and no notice.

THE LIVE SESSION HAS A USB STICK MOUNTED, as a live session booted off
one does: automountd puts it at /media/INSTSTICK, and the installed disk
must have that mount point empty -- the copy stays on one filesystem.

What a broken version would still pass, and why each check is there:
  * a Cancel that erased anyway -- the host reads the image's first
    sectors after it and must find no table;
  * an install that reported Finished and wrote nothing bootable --
    phase 2 boots the disk ALONE, no ISO in the drive;
  * a carry-over that marked every page answered, or none -- the
    pages are compared exactly.

On demand (ondemand_sweep.py): it needs `make live-iso` and a guest of
its own with a second disk. ~4 minutes under TCG.

    python3 tools/installer_test.py [--instance N] [--kvm] [--keep DIR]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from harness import Results                     # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from live_boot_test import build_unasked_iso, UNASKED_CFG   # noqa: E402
from install_test import fat_stick, STICK_LABEL, STICK_MARK  # noqa: E402

LIVE_ISO = "saunaos-live.iso"
TARGET_BYTES = 512 * 1024 * 1024
TITLE = "Install SaunaOS"
NAME = "carried-name"
LOG = []

# What a live ISO is built from: one newer than it is a stale ISO, which
# boots and passes against the previous build (iso_guard judges only
# saunaos.iso).
LIVE_INPUTS = ("build/kernel.bin", "build/lib/libuapp.so", "build/userland/gui/system/installer.elf",
               "build/userland/bin/install.elf")


class Layout:
    """The window's last self-reported rects (uapp_log_layout()) and its
    `state` line: wizard step, install stage, finished, ok."""

    def __init__(self, lines, content):
        self.r, self.state = {}, None
        self.cx, self.cy = content["x"], content["y"]
        starts = [i for i, ln in enumerate(lines) if "installer: layout state " in ln]
        if not starts:
            return
        last = starts[-1]
        prev = starts[-2] + 1 if len(starts) > 1 else 0
        for line in lines[prev:last + 1]:
            if "installer: layout " not in line:
                continue
            parts = line.split("installer: layout ", 1)[1].split()
            try:
                nums = [int(p) for p in parts[1:]]
            except ValueError:
                continue
            if parts[0] == "state":
                self.state = dict(zip(("step", "stage", "finished", "ok"), nums))
            elif len(nums) == 5:
                self.r[f"{parts[0]} {nums[0]}"] = tuple(nums[1:])
            elif len(nums) == 4:
                self.r[parts[0]] = tuple(nums)

    def centre(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x + w // 2, self.cy + y + h // 2)


def poll(dbg):
    LOG.extend(dbg.logs("installer:", clear=True))
    return LOG


def wait(dbg, content, want, timeout=20.0):
    deadline = time.time() + timeout
    while True:
        lay = Layout(poll(dbg), content)
        if (lay.state and want(lay)) or time.time() > deadline:
            return lay
        time.sleep(0.5)


def disk_is_blank(path):
    """No MBR signature and no GPT header -- nothing was written."""
    with open(path, "rb") as f:
        head = f.read(1024)
    return head[510:512] != b"\x55\xaa" and head[512:520] != b"EFI PART"


def live_iso_stale():
    iso = os.path.join(REPO, LIVE_ISO)
    if not os.path.exists(iso):
        return f"no {LIVE_ISO} -- run `make live-iso`"
    m = os.path.getmtime(iso)
    for rel in LIVE_INPUTS:
        p = os.path.join(REPO, rel)
        if os.path.exists(p) and os.path.getmtime(p) > m:
            return f"{rel} is newer than {LIVE_ISO} -- run `make live-iso`"
    return None


def phase1(dbg, res, target):
    sh = lambda c: dbg.send(f"sh {c}")   # noqa: E731
    desktop = sh("ls /home/Desktop")
    res.check("a live session has the Install SaunaOS launcher on its desktop", "installer.desktop" in desktop,
              desktop.strip().replace("\n", " ")[:120])
    klog = sh("log -n 3000")
    res.check("...and the compositor put up the live-session notice", "wm: live-session notice" in klog)
    res.check("...and First-boot setup did not run", "firstboot: step" not in klog)
    res.check("...nor does the desktop wait for it", "holding the desktop back" not in klog)
    res.check("fbs says a live session never shows setup", "live session never shows it" in sh("fbs"))

    # What a person trying the session would set: one setup page each.
    sh(f"config set system.hostname {NAME}")
    sh("config set locale.region fi")

    mounted = ""
    for _ in range(15):
        mounted = sh(f"ls /media/{STICK_LABEL}")
        if STICK_MARK in mounted:
            break
        time.sleep(2)
    res.check("a USB stick is mounted under /media in the live session", STICK_MARK in mounted,
              mounted.strip()[-80:])

    dbg.send("gui spawn /bin/wm/system/installer")
    win = None
    for _ in range(50):
        win = dbg.window(TITLE)
        if win and win["content"]["w"] > 100:
            break
        time.sleep(0.3)
    if not res.check("the installer window opens", win is not None):
        return False
    c = win["content"]
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 0 and "next" in lay.r)
    if not res.check("it opens on Welcome", lay.state and lay.state["step"] == 0):
        return False

    def press(key):
        dbg.click(*lay.centre(key))
        dbg.settle()

    press("next")
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 1 and "disks" in lay.r)
    res.check("Next goes to Disk, with the disk list and the plan",
              lay.state["step"] == 1 and "disks" in lay.r and "plan" in lay.r)
    press("next")
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 2 and "summary" in lay.r)
    res.check("...then Ready, with its summary", lay.state["step"] == 2 and "summary" in lay.r)

    # Install asks first; Cancel must change NOTHING.
    press("next")
    lay = wait(dbg, c, lambda lay: "dialog.button 1" in lay.r)
    if not res.check("Install asks before it erases", "dialog.button 1" in lay.r):
        return False
    press("dialog.button 0")
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    res.check("Cancel closes the question and stays on Ready",
              "dialog.button 0" not in lay.r and lay.state["step"] == 2)
    res.check("...and the disk is still blank, read by the host", disk_is_blank(target))

    press("next")
    lay = wait(dbg, c, lambda lay: "dialog.button 1" in lay.r)
    press("dialog.button 1")
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 3)
    res.check("Erase and install moves on to Installing", lay.state["step"] == 3)
    lay = wait(dbg, c, lambda lay: lay.state["finished"] == 1, timeout=400)
    res.check("the install finishes, and says it worked", lay.state["finished"] == 1 and lay.state["ok"] == 1,
              str(lay.state))
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 4)
    res.check("...on the Finished page", lay.state["step"] == 4)
    # The end is reported ONCE: a tick that kept treating it as new re-read
    # the clock for Took every quarter second (and would ask to restart
    # every tick with that box ticked).
    time.sleep(2)
    poll(dbg)
    ends = [ln for ln in LOG if re.search(r"installer: (done|FAILED) in", ln)]
    res.check("...and the end is reported once, not every tick after", len(ends) == 1, f"{len(ends)} reports")
    said = "\n".join(LOG)
    res.check("every stage ran, in order",
              all(f"installer: [{i}/5]" in said for i in range(1, 6)),
              " ".join(re.findall(r"installer: \[(\d)/5\]", said)))
    return True


def phase0(dbg, res):
    """installer=on: the window, full screen, with the launcher behind it."""
    sh = lambda c: dbg.send(f"sh {c}")   # noqa: E731
    win = None
    for _ in range(60):
        win = dbg.window(TITLE)
        if win:
            break
        time.sleep(0.5)
    res.check("installer=on opens the installer by itself", win is not None)
    res.check("...full screen", win is not None and win.get("state") == "fullscreen",
              str(win.get("state")) if win else "")
    res.check("...with the Install SaunaOS launcher on the desktop behind it",
              "installer.desktop" in sh("ls /home/Desktop"))
    klog = sh("log -n 3000")
    res.check("...and no live-session notice", "wm: live-session notice" not in klog and
              "notice not asked for" in klog)


def phase2(dbg, res):
    sh = lambda c: dbg.send(f"sh {c}")   # noqa: E731
    blk = sh("lsblk")
    res.check("the installed disk boots on its own, rooted on its third partition",
              re.search(r"ata0p3 .* yes", blk) is not None, blk.strip().splitlines()[-1] if blk.strip() else "")
    res.check("the computer's name set in the live session came along",
              NAME in sh("config get system.hostname"))
    fbs = sh("fbs")
    m = re.search(r"so not asked: (.*)", fbs)
    carried = sorted(p.strip() for p in m.group(1).split(";") if p.strip()) if m else []
    res.check("First-boot setup skips exactly the two pages the session answered",
              carried == ["Computer name", "Region & formats"], str(carried))
    res.check("...and has not run yet", "not done" in fbs)
    res.check("an installed machine gets no launcher", "installer.desktop" not in sh("ls /home/Desktop"))
    media = sh(f"ls /media/{STICK_LABEL}")
    res.check("the live session's USB stick was not copied onto the disk",
              STICK_MARK not in media and "no such" not in media.lower(), media.strip()[-80:])
    klog = sh("log -n 3000")
    res.check("...and no live-session notice", "live-session notice" not in klog)
    # Setup is due here, so the compositor shows no desktop until setup's
    # window is up -- let go BY setup, not by the timeout.
    res.check("the desktop is held back until First-boot setup is up",
              "holding the desktop back" in klog and "let go -- First-boot setup is up" in klog)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the disk image here")
    args = ap.parse_args()
    stale = live_iso_stale()
    if stale:
        # Exit 77, automake's SKIP: a stale ISO would test the previous
        # build and pass, and a missing one tests nothing.
        print(f"installer_test: SKIP -- {stale}")
        return 77
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    tmp = args.keep or tempfile.mkdtemp(prefix="installer-")
    os.makedirs(tmp, exist_ok=True)
    try:
        return run_in(args, tmp)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)


def run_in(args, tmp):
    target = os.path.join(tmp, "target.img")
    with open(target, "wb") as f:
        f.truncate(TARGET_BYTES)
    stick = os.path.join(tmp, "stick.img")
    fat_stick(stick)
    res = Results()

    def vm(*argv, live=True):
        media = (["--iso", LIVE_ISO, "--boot", "cd", "--usb-stick", stick] if live
                 else ["--boot", "disk"])
        env = dict(os.environ, SAUNAOS_ALLOW_STALE_ISO="1")   # judged above, by live_iso_stale()
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", target, *media, *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=600, env=env)
        return r.stdout + r.stderr

    sock = port_guard.instance_sock(args.instance)

    # Phase 0: a one-entry ISO asking for installer=on.
    cfg = UNASKED_CFG.replace("multiboot2 /boot/kernel.bin", "multiboot2 /boot/kernel.bin live installer=on debugcon")
    on_iso = build_unasked_iso(tmp, os.path.join(REPO, "build/kernel.bin"), os.path.join(REPO, "build/live.img"),
                               cfg)
    if not on_iso:
        res.skip("installer=on", "no grub-mkrescue")
    else:
        print("installer_test: installer=on")
        env = dict(os.environ, SAUNAOS_ALLOW_STALE_ISO="1")
        base = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance), "--disk", target,
                "--iso", on_iso, "--boot", "cd", *(["--kvm"] if args.kvm else [])]
        run = lambda *a: subprocess.run(base + list(a), cwd=REPO, capture_output=True, text=True,   # noqa: E731
                                        timeout=600, env=env).stdout
        run("stop")
        if res.check("the guest came up (installer=on)", "vm: ready" in run("--timeout", "180", "start")):
            try:
                qmp = QMPSession(port=port_guard.instance_qmp(args.instance))
                enter_gui(qmp, sock, timeout=60)
                dbg = DebugConsole(sock, timeout=15)
                try:
                    phase0(dbg, res)
                finally:
                    dbg.close()
                    qmp.close()
            finally:
                run("stop")

    for phase, live in (("live session", True), ("the installed disk", False)):
        vm("stop", live=live)
        print(f"installer_test: {phase}")
        # The installed disk's first boot under TCG takes longer than
        # vm.py's default wait for the console.
        out = vm("--timeout", "180", "start", live=live)
        if not res.check(f"the guest came up ({phase})", "vm: ready" in out, out.strip()[-200:]):
            break
        try:
            qmp = QMPSession(port=port_guard.instance_qmp(args.instance))
            enter_gui(qmp, sock, timeout=60)
            dbg = DebugConsole(sock, timeout=15)
            try:
                if live:
                    if not phase1(dbg, res, target):
                        break
                else:
                    phase2(dbg, res)
            finally:
                dbg.close()
                qmp.close()   # an open QMP client holds the slot's port into the next boot
        finally:
            vm("stop", live=live)

    print("\n" + res.summary("installer_test"))
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
