#!/usr/bin/env python3
"""The boot splash: the logo from the framebuffer's first moment to the
desktop, never the kernel log -- and the ways back to the log.

One QEMU, four boots, a screendump burst through each:

  A. an ordinary graphical boot: a frame shows the splash (its ground and
     the logo's plate), NO frame after the mode switch is the text
     console, and the kernel hands the splash to the desktop;
  B. Esc pressed during that boot: the log comes up instead;
  C. First-boot setup due: the compositor's held frame carries the
     kernel's logo ON THE SAME PIXELS (the handover must not jump), its
     spinner turns, and the hold is let go by setup;
  D. the text target: no splash, the log -- which is also what proves the
     console-frame test can see a console frame at all.

    python3 tools/boot_splash_test.py [--instance N]

Exit 0 when every check passed, 1 otherwise.
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

from PIL import Image                             # noqa: E402
from harness import Results, copy_disk            # noqa: E402
from qmp_test import QMPSession                   # noqa: E402
import port_guard                                 # noqa: E402

# kernel/include/api/splash_layout.h, as QEMU's 32bpp screendump reads them.
SPLASH_BG = (11, 15, 23)
SPLASH_PLATE = (40, 96, 230)


def is_console(im):
    """The text console: a black page with light grey text on it. The
    mode switch's blank frame is black too, but has no text."""
    # By exact colour at full size: a few greys and a lot of black. A
    # desktop has thousands of colours and returns None here.
    cols = im.convert("RGB").getcolors(4096)
    if not cols:
        return False
    total = im.size[0] * im.size[1]
    black = sum(n for n, c in cols if c == (0, 0, 0))
    grey = sum(n for n, c in cols if c[0] > 120 and c[0] == c[1] == c[2])
    return black > total * 0.8 and grey > 200


def logo_box(w, h):
    """The plate's rect, from splash_layout.c's arithmetic."""
    size = max(48, min(192, h * 2 // 15))
    p = size * 56 // 64
    ox, oy = (w - p) // 2, h * 2 // 5 - p // 2
    return ox, oy, ox + p, oy + p


def spinner_box(w, h):
    """The spinner's ring, near the bottom (splash_layout.c), with room
    for its dots."""
    size = max(48, min(192, h * 2 // 15))
    r = size // 7 + max(2, size // 32) + 3
    return w // 2 - r, h * 3 // 4 - r, w // 2 + r, h * 3 // 4 + r


def has_spinner(im):
    """Any dot drawn in the spinner's box -- the compositor's frame; the
    kernel's static splash leaves it empty."""
    box = im.convert("RGB").crop(spinner_box(*im.size))
    return any(c != SPLASH_BG for _, c in box.getcolors(65536) or [(0, None)])


def is_splash(im):
    w, h = im.size
    x0, y0, x1, y1 = logo_box(w, h)
    rgb = im.convert("RGB")
    # Just inside the plate's top edge, above the top brick (at 1/7).
    return rgb.getpixel((10, 10)) == SPLASH_BG and \
        rgb.getpixel(((x0 + x1) // 2, y0 + (y1 - y0) // 14)) == SPLASH_PLATE


def burst(qmp, seconds, during=None, every=0.12):
    """Screendumps for `seconds`, oldest first; `during(frame)` sees each.
    A failed dump is COUNTED, never dropped: a burst that captured
    nothing must not read as a boot that showed nothing wrong."""
    frames, failed, t0 = [], 0, time.time()
    path = os.path.join(tempfile.gettempdir(), f"splash-{os.getpid()}.png")
    while time.time() - t0 < seconds:
        try:
            qmp.screenshot(path, stable=False, settle=0)
            frames.append(Image.open(path).copy())
            if during:
                during(frames[-1])
        except Exception as e:
            if not failed:
                print(f"  (screendump failed: {e!r})")
            failed += 1
        time.sleep(every)
    if failed:
        print(f"  ({failed} screendumps failed)")
    return frames


def this_boot(frames):
    """The frames after the firmware's text mode: before it is the
    PREVIOUS boot going down (its desktop, then the console a dying
    desktop hands back), which is not this boot's splash's business."""
    last_text = max((i for i, f in enumerate(frames) if f.size[0] < 800), default=None)
    return frames[last_text + 1:] if last_text is not None else []


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    args = ap.parse_args()
    inst = args.instance if args.instance is not None else port_guard.find_free_instance()
    res = Results()
    tmp = tempfile.mkdtemp(prefix="splash-")
    disk = copy_disk(os.path.join(REPO, "disk.img"), os.path.join(tmp, "disk.img"))

    def vm(*argv, timeout=300):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(inst),
                            "--disk", disk, *argv], cwd=REPO, capture_output=True, text=True,
                           timeout=timeout)
        return r.stdout + r.stderr

    def start_async(*argv):
        return subprocess.Popen([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(inst),
                                 "--disk", disk, *argv], cwd=REPO, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def dmesg():
        return vm("exec", "dmesg")

    def connect(old=None):
        # A FRESH session per boot: key presses' replies share the socket
        # with the screendumps', and one out of step fails every dump after.
        if old:
            old.close()
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                return QMPSession(port=port_guard.instance_qmp(inst))
            except Exception:
                time.sleep(0.05)
        return None

    qmp = None
    try:
        # A: an ordinary boot, to the desktop.
        print("boot_splash_test: A -- an ordinary boot")
        p = start_async("--reboot", "start")
        qmp = connect()
        if not res.check("QEMU came up", qmp is not None):
            return res.finish("boot_splash_test")
        frames = burst(qmp, 12)
        frames = frames[next((i for i, f in enumerate(frames) if f.size[0] >= 800), len(frames)):]
        out = p.communicate(timeout=300)[0] or ''
        print('  ' + (out.strip().splitlines() or [''])[-1])
        log = dmesg()
        res.check("A: the kernel put the splash up", "splash: on" in log)
        res.check("A: a frame shows the splash (its ground and the logo's plate)",
                  any(is_splash(f) for f in frames), f"{len(frames)} frames")
        consoles = sum(1 for f in frames if is_console(f))
        res.check("A: no frame shows the text console", consoles == 0, f"{consoles} of {len(frames)}")
        res.check("A: the splash was handed to the desktop", "splash: off -- handed to the desktop" in log)
        a_frames = frames

        # B: Esc during the splash brings the log.
        print("boot_splash_test: B -- Esc")
        qmp = connect(qmp)
        p = start_async("--timeout", "180", "reboot")
        def esc_on_splash(f):
            # Esc only while THIS boot's splash is up: later it would go
            # to the desktop.
            if is_splash(f):
                qmp.send_key("esc")
        frames = this_boot(burst(qmp, 10, during=esc_on_splash))
        out = p.communicate(timeout=300)[0] or ''
        print('  ' + (out.strip().splitlines() or [''])[-1])
        log = dmesg()
        res.check("B: Esc ended the splash with the log", "splash: Esc pressed" in log and
                  "splash: off -- the boot log is on screen" in log)
        res.check("B: the log came up on screen", any(is_console(f) for f in frames))

        # C: setup is due -- the compositor's hold carries the logo on.
        print("boot_splash_test: C -- First-boot setup's hold")
        vm("exec", "fbs again")
        qmp = connect(qmp)
        p = start_async("--timeout", "180", "reboot")
        frames = this_boot(burst(qmp, 14, every=0.08))
        out = p.communicate(timeout=300)[0] or ''
        print('  ' + (out.strip().splitlines() or [''])[-1])
        log = dmesg()
        splashes = [f for f in frames if is_splash(f)]
        if res.check("C: the boot showed the splash", splashes, f"{len(frames)} frames"):
            w, h = splashes[0].size
            x0, y0, x1, y1 = logo_box(w, h)
            first = splashes[0].convert("RGB").crop((x0, y0, x1, y1)).tobytes()
            same = all(f.convert("RGB").crop((x0, y0, x1, y1)).tobytes() == first for f in splashes)
            res.check("C: the logo is on the same pixels in every splash frame, kernel's and compositor's",
                      same)
            # Two different spinner frames mean it turns -- among the
            # compositor's frames only: the kernel's splash has no spinner.
            spins = [f.convert("RGB").crop(spinner_box(w, h)).tobytes() for f in splashes if has_spinner(f)]
            res.check("C: the compositor's spinner turns", len(set(spins)) >= 2,
                      f"{len(set(spins))} distinct spinner frames of {len(spins)}")
            res.check("C: the kernel's own splash has no spinner (it cannot animate)",
                      any(not has_spinner(f) for f in splashes))
        res.check("C: the compositor held the desktop back and setup let it go",
                  "holding the desktop back" in log and "let go -- First-boot setup is up" in log)
        res.check("C: no frame shows the text console", not any(is_console(f) for f in frames))

        # E: system.boot_messages=detailed -- an ordinary boot shows the
        # compositor's "Loading desktop..." frame before the desktop; the
        # default (A) goes straight from the kernel's splash to it.
        print("boot_splash_test: E -- detailed startup messages")
        res.check("A: by default the desktop follows the kernel's splash directly",
                  not any(has_spinner(f) for f in a_frames if is_splash(f)))
        vm("exec", "config set system.boot_messages detailed")
        qmp = connect(qmp)
        p = start_async("--timeout", "180", "reboot")
        frames = this_boot(burst(qmp, 10, every=0.08))
        out = p.communicate(timeout=300)[0] or ''
        print('  ' + (out.strip().splitlines() or [''])[-1])
        res.check("E: the compositor held a splash frame on an ordinary boot",
                  any(is_splash(f) and has_spinner(f) for f in frames), f"{len(frames)} frames")
        vm("exec", "config set system.boot_messages simple")

        # D: the text target -- no splash; the log is the screen.
        print("boot_splash_test: D -- the text target")
        vm("exec", "config set system.default_target text")
        qmp = connect(qmp)
        p = start_async("--timeout", "180", "reboot")
        frames = this_boot(burst(qmp, 8))
        out = p.communicate(timeout=300)[0] or ''
        print('  ' + (out.strip().splitlines() or [''])[-1])
        log = dmesg()
        res.check("D: a text boot puts the log on screen", "splash: off -- the boot log is on screen" in log
                  and "handed to the desktop" not in log)
        res.check("D: the console-frame test sees it (the control for A and C)",
                  any(is_console(f) for f in frames))
    finally:
        if qmp:
            qmp.close()
        vm("stop")
        shutil.rmtree(tmp, ignore_errors=True)   # a disk copy per run
    return res.finish("boot_splash_test")


if __name__ == "__main__":
    sys.exit(main())
