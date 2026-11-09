#!/usr/bin/env python3
"""tools/tests/sweep/installer_test.py -- the Install SaunaOS window, on the live ISO,
onto a blank disk -- and then that disk, booted on its own.

Phase 1 boots saunaos-live.iso with one BLANK 512 MiB IDE disk and checks
what a live session offers before anybody asks: the "Install SaunaOS"
launcher on the desktop, the compositor's live-session notice, and NO
First-boot setup (a live session never shows it). It sets two settings
the way a person trying the session would -- the computer's name and the
region -- and REMOVES DOOM (`component remove doom`), then drives the
window through Welcome, Disk, Software and Ready, CANCELS the erase
confirm, and checks from the host that the disk is still blank. Then it
confirms, and waits for Finished.

THE SOFTWARE STEP starts from what the session has: DOOM, removed here,
unticked (read from the program's `installer: software` line AND from the
checkboxes' pixels, against a ticked row as the control), the rest ticked.
Unticking the Web page engine unticks the Web Browser that needs it;
ticking the Browser ticks the engine again; Demos and tests is left out,
and Ready names the choice. Before that, with two dozen made-up
definitions, the checklist is run through scroll_test's check_view() --
here, because the Disk step needs a disk a GUI guest does not have.

Phase 0 boots the live image with `installer=on` -- the live boot menu's
Install SaunaOS entry -- and checks the window is up full screen at once
AND that the desktop has its launcher behind it, so "Keep trying SaunaOS"
leaves a way back in; no notice, since the window is already open. Its
ISO is made here with grub-mkrescue, one entry; the phase SKIPS without it.

Phase 2 boots the disk with nothing else attached and checks it is the
installed system: its root is the disk's third partition, the name set
in the live session is there, and First-boot setup will skip exactly the
two pages that session answered (`fbs`'s report of UFBS_ANSWERED) and
nothing else. An installed machine gets no launcher and no notice. What
was left out is not there -- no /bin/wm/demos, no DOOM, /tests holding the
engine's nsmonkey alone -- and /var/lib/update/components says `removed
demos` and `removed doom`, while the Browser, the engine and Windows 3.x
support are.

THE LIVE SESSION HAS A USB STICK MOUNTED, as a live session booted off
one does: automountd puts it at /media/INSTSTICK, and the installed disk
must have that mount point empty -- the copy stays on one filesystem.

What a broken version would still pass, and why each check is there:
  * a Cancel that erased anyway -- the host reads the image's first
    sectors after it and must find no table;
  * an install that reported Finished and wrote nothing bootable --
    phase 2 boots the disk ALONE, no ISO in the drive;
  * a carry-over that marked every page answered, or none -- the
    pages are compared exactly;
  * a Software step that only LOOKED unticked -- the installed disk is
    read for the files, and a component the copy skipped but the state
    file forgot would come back with the first update, so both are read;
  * a checklist whose model and drawing disagree -- the boxes' pixels are
    read beside the program's own report.

On demand (ondemand_sweep.py): it needs `make live-iso` and a guest of
its own with a second disk. ~4 minutes under TCG.

    python3 tools/tests/sweep/installer_test.py [--instance N] [--kvm] [--keep DIR]
"""

# The Install SaunaOS window on saunaos-live.iso onto a blank disk, then
# that disk booted alone. Its own guests and image; SKIPS (77) when
# the live ISO is missing or older than the build.
RUNNER = {"covers": "the GUI installer, live ISO to a booting disk", "parallel": True}

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from harness import Results                     # noqa: E402
from harness import poll as harness_poll        # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from live_boot_test import build_unasked_iso, UNASKED_CFG   # noqa: E402
from install_test import fat_stick, STICK_LABEL, STICK_MARK  # noqa: E402
from scroll_test import check_view                           # noqa: E402

LIVE_ISO = "saunaos-live.iso"
TARGET_BYTES = 512 * 1024 * 1024
TITLE = "Install SaunaOS"
NAME = "carried-name"
LOG = []
SW_LINE = "installer: software "
FAKE_DIR = "/usr/share/components"
FAKES = 24

# What a live ISO is built from: one newer than it is a stale ISO, which
# boots and passes against the previous build (iso_guard judges only
# saunaos.iso).
LIVE_INPUTS = ("build/kernel.bin", "build/lib/libbase.so", "build/lib/libui.so",
               "build/userland/apps/system/installer/installer.elf",
               "build/userland/utils/install.elf")


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


def software(dbg, want=lambda st: True, timeout=10.0):
    """The program's last `installer: software` line -- in=, out=, absent=,
    order= (the rows, top to bottom) -- once `want` holds, or the last seen."""
    seen = [None]

    def last():
        poll(dbg)
        for ln in reversed(LOG):
            if SW_LINE in ln:
                kv = dict(p.split("=", 1) for p in ln.split(SW_LINE, 1)[1].split() if "=" in p)
                seen[0] = {k: [x for x in v.split(",") if x] for k, v in kv.items()}
                return seen[0] if want(seen[0]) else None
        return None
    return harness_poll(last, timeout, 0.4) or seen[0]


def box_white(img, cx, cy, rect):
    """The share of near-white pixels inside a checkbox, its edge left out:
    about 1 for an empty box, about 0 for a ticked (accent-filled) one."""
    x, y, w, h = rect
    pts = [(cx + x + i, cy + y + j) for i in range(2, w - 2) for j in range(2, h - 2)]
    white = sum(1 for p in pts if min(img.getpixel(p)) > 225)
    return white / max(1, len(pts))


def shot(qmp, tmp):
    from PIL import Image
    p = os.path.join(tmp, "installer_shot.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def to_software(dbg, c):
    """Welcome -> Disk -> Software; the layout there."""
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 0 and "next" in lay.r)
    for want in (1, 2):
        dbg.click(*lay.centre("next"))
        dbg.settle()
        lay = wait(dbg, c, lambda lay, want=want: lay.state["step"] == want and "next" in lay.r)
    return lay


def open_installer(dbg):
    dbg.send("gui spawn /bin/wm/system/installer")
    win = None
    for _ in range(50):
        win = dbg.window(TITLE)
        if win and win["content"]["w"] > 100:
            break
        time.sleep(0.3)
    return win


def scroll_check(dbg, qmp, res, tmp):
    """The checklist with more rows than fit: two dozen made-up definitions
    beside the real ones, then gone again before the install that counts."""
    for i in range(FAKES):
        path = f"{FAKE_DIR}/zz{i:02d}.component"
        dbg.send(f"sh write {path} Name=Fake component {i:02d}")
        for line in ("Category=demos", "License=MIT", f"Paths=/nonexistent/zz{i:02d}", "Size=123456", "Files=1"):
            dbg.send(f"sh append {path} {line}")
    try:
        win = open_installer(dbg)
        if not res.check("the installer opens with the made-up components", win is not None):
            return
        lay = to_software(dbg, win["content"])
        if res.check("...and reaches Software", lay.state and lay.state["step"] == 2 and "software" in lay.r,
                     str(lay.state)):
            # Off the bar, inside the list's frame: its top-left corner.
            check_view(dbg, qmp, res, tmp, "Installer software checklist", TITLE, "software",
                       lambda sx, sy, w, h: (sx + 60, sy + 20), lambda sx, sy, w, h: (sx + 3, sy + 3),
                       keys=True, warp=True)
        dbg.close_window(TITLE)
    finally:
        for i in range(FAKES):
            dbg.send(f"sh rm {FAKE_DIR}/zz{i:02d}.component")
        LOG.clear()


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


def phase1(dbg, qmp, res, target, tmp):
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

    # What the session has is what the Software step starts from.
    out = sh("component remove doom")
    res.check("DOOM is removed in the live session", "doom: removed" in out, out.strip()[-120:])

    scroll_check(dbg, qmp, res, tmp)

    win = open_installer(dbg)
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
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 2 and "software" in lay.r)
    if not res.check("...then Software, with its checklist", lay.state["step"] == 2 and "software" in lay.r,
                     str(lay.state)):
        return False
    st = software(dbg, lambda st: "order" in st)
    order = st["order"] if st else []
    res.check("Software starts from the session's set: DOOM, removed here, unticked, the rest ticked",
              st is not None and st.get("out") == ["doom"] and "doom" in st.get("absent", [])
              and sorted(st.get("in", [])) == sorted(x for x in order if x != "doom"), str(st))
    rows = {cid: i for i, cid in enumerate(order)}
    if not res.check("...listing the engine, the Browser, Demos and DOOM",
                     all(k in rows for k in ("webengine", "browser", "demos", "doom")), str(order)):
        return False
    # The pixels say the same: DOOM's box empty, the Browser's filled.
    img = shot(qmp, tmp)
    doom_box = lay.r.get(f"software.box {rows['doom']}")
    browser_box = lay.r.get(f"software.box {rows['browser']}")
    if res.check("...and each row reports its checkbox", doom_box and browser_box):
        dw, bw = box_white(img, lay.cx, lay.cy, doom_box), box_white(img, lay.cx, lay.cy, browser_box)
        res.check("...DOOM's box is drawn empty, the Browser's ticked (read from pixels)",
                  dw > 0.6 and bw < 0.3, f"white share doom {dw:.2f} browser {bw:.2f}")

    press(f"software.item {rows['webengine']}")
    st = software(dbg, lambda st: "webengine" in st.get("out", []))
    res.check("unticking the Web page engine unticks the Web Browser that needs it",
              st and "webengine" in st.get("out", []) and "browser" in st.get("out", []), str(st))
    if browser_box:
        img = shot(qmp, tmp)
        res.check("...and the Browser's box is drawn empty now",
                  box_white(img, lay.cx, lay.cy, browser_box) > 0.6)
    press(f"software.item {rows['browser']}")
    st = software(dbg, lambda st: "browser" in st.get("in", []))
    res.check("ticking the Browser ticks the engine again",
              st and "browser" in st.get("in", []) and "webengine" in st.get("in", []), str(st))
    press(f"software.item {rows['doom']}")
    st = software(dbg)
    res.check("DOOM, not on this system, cannot be ticked", st and "doom" in st.get("out", []), str(st))
    press(f"software.item {rows['demos']}")
    st = software(dbg, lambda st: "demos" in st.get("out", []))
    res.check("Demos and tests is left out", st and sorted(st.get("out", [])) == ["demos", "doom"], str(st))
    # The keyboard: the click focused the list on Demos' row; an arrow
    # moves to the next row, Space flips it, and flips it back.
    if rows["browser"] == rows["demos"] + 1:
        dbg.key("0xf781")                      # KEY_ARROW_DOWN
        dbg.key("0x20")
        st = software(dbg, lambda st: "browser" in st.get("out", []))
        res.check("Down then Space unticks the next row (the Browser)",
                  st and "browser" in st.get("out", []) and "webengine" in st.get("in", []), str(st))
        dbg.key("0x20")
        st = software(dbg, lambda st: "browser" in st.get("in", []))
        res.check("...and Space ticks it again", st and "browser" in st.get("in", []), str(st))

    press("next")
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 3 and "summary" in lay.r)
    res.check("...then Ready, with its summary", lay.state["step"] == 3 and "summary" in lay.r)
    said = "\n".join(poll(dbg))
    m = re.findall(r"installer: ready: software (.*)", said)
    res.check("Ready names the choice", bool(m) and "Demos and tests" in m[-1] and "DOOM" in m[-1]
              and "left out" in m[-1], m[-1] if m else "no line")

    # Install asks first; Cancel must change NOTHING.
    press("next")
    lay = wait(dbg, c, lambda lay: "dialog.button 1" in lay.r)
    if not res.check("Install asks before it erases", "dialog.button 1" in lay.r):
        return False
    press("dialog.button 0")
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    res.check("Cancel closes the question and stays on Ready",
              "dialog.button 0" not in lay.r and lay.state["step"] == 3)
    res.check("...and the disk is still blank, read by the host", disk_is_blank(target))

    press("next")
    lay = wait(dbg, c, lambda lay: "dialog.button 1" in lay.r)
    press("dialog.button 1")
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 4)
    res.check("Erase and install moves on to Installing", lay.state["step"] == 4)
    lay = wait(dbg, c, lambda lay: lay.state["finished"] == 1, timeout=400)
    res.check("the install finishes, and says it worked", lay.state["finished"] == 1 and lay.state["ok"] == 1,
              str(lay.state))
    lay = wait(dbg, c, lambda lay: lay.state["step"] == 5)
    res.check("...on the Finished page", lay.state["step"] == 5)
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
    # What the Software step left out is not there, and stays out.
    demos = sh("ls /bin/wm/demos")
    res.check("Demos and tests, left out, was not copied: no /bin/wm/demos",
              "no such" in demos.lower() or not demos.strip(), demos.strip()[-120:])
    tests = sh("ls /tests").split()
    res.check("...and /tests holds the engine's nsmonkey alone", tests == ["nsmonkey"], " ".join(tests)[-120:])
    res.check("...nor DOOM, removed in the session", "no such" in sh("ls /bin/wm/apps/doom").lower())
    state = sh("cat /var/lib/update/components")
    res.check("the new machine's state says `removed demos` and `removed doom`",
              "removed demos" in state and "removed doom" in state, state.strip()[-200:])
    kept = [p for p in ("/bin/wm/apps/browser", "/bin/webcontent", "/bin/w16", "/bin/wm/apps/retrolauncher")
            if "no such" in sh(f"ls {p}").lower()]
    res.check("...while the Browser, the engine and Windows 3.x support are there", not kept, " ".join(kept))
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
        r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
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
        base = [sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance), "--disk", target,
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
                    if not phase1(dbg, qmp, res, target, tmp):
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
