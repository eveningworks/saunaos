#!/usr/bin/env python3
"""tools/scale_test.py -- the display scale (`display.scale`, fontd's
beacon, ugfx_px()): at 150% the taskbar, the title bar and an open
window grow by half, the window stays above the taskbar, Settings'
sample draws at the step, and 100% puts every one of them back.

WHAT THIS ASSERTS THAT "THE TEXT GOT BIGGER" WOULD NOT
-----------------------------------------------------
* A FIXED PIXEL FOLLOWS: the taskbar is a px setting (48 at 100%), so
  72 at 150% is ugfx_px() working, not the font.
* IT IS DRAWN, not only reported: a pixel 60 rows up from the bottom,
  desktop at 100%, is the strip's colour at 150%.
* AN OPEN WINDOW GROWS BY THE RATIO (WM_DPICHANGED's rule), and is kept
  above the taskbar; back at 100% it is the size it was, within a pixel.
* THE SAMPLE IS AT THE STEP: its taskbar band is 48 x scale rows of the
  strip's colour, counted in the screendump.

    python3 tools/vm.py --disk <copy> start
    python3 tools/scale_test.py
    echo $?

POSITIVE CONTROL, both at once: read_height() without its ugfx_px()
reddens the two 150% taskbar checks, and wm_scale_changed() doing
nothing reddens "the window grows by half" and the round trip after it
(the last title check too, since a taskbar that never moved ends the
100% wait at once). Leaves `display.scale` unset.
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

STRIP = (30, 30, 34)   # uui_scalesample's taskbar


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def near(a, b, tol):
    return abs(a - b) <= tol


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "scale_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="scale_")
    res = Results()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        for k in ("display.scale", "desktop.taskbar.height", "desktop.taskbar.float"):
            dbg.send(f"sh config unset {k}")
        for _ in range(12):
            left = [w for w in dbg.windows() if not w.get("popup") and not w.get("dialog")]
            if not left:
                break
            dbg.send(f"gui close {left[-1]['z']}")
            if not poll(lambda: len(dbg.windows()) < len(left), 5):
                break
        poll(lambda: dbg.taskbar()["h"] == 48, 5)

        def notepad():
            return next((w for w in dbg.windows() if w.get("app_id") == "notepad"), None)

        def shot(name):
            p = os.path.join(tmp, name)
            qmp.screenshot(p)
            return Image.open(p).convert("RGB")

        def set_scale(pct):
            if pct == 100:
                dbg.send("sh config unset display.scale")
            else:
                dbg.send(f"sh config set display.scale {pct}")
            want = 48 * pct // 100
            return poll(lambda: dbg.taskbar()["h"] == want and dbg.taskbar(), 10) or dbg.taskbar()

        dbg.open_app("Notepad")
        w0 = poll(notepad, 15)
        if not w0:
            res.check("Notepad opens", False, str(dbg.windows()))
            return res.finish("scale_test")
        dbg.settle()
        w0 = notepad()
        # A NORMAL window: a snapped or maximized one takes its rect from
        # the work area by design. A machine that remembers Notepad so
        # (wm_geometry.c) forgets it, and the window is opened again.
        if w0.get("snap") not in (None, "", "none") or w0.get("state") != "normal":
            dbg.key("0x20", mods="alt")   # the window menu
            row = poll(lambda: dbg.ctxmenu_row("Forget Saved Position"), 3)
            if row:
                dbg.click(*row)
            dbg.send(f"gui close {notepad()['z']}")
            poll(lambda: notepad() is None, 5)
            dbg.open_app("Notepad")
            poll(notepad, 15)
            dbg.settle()
            w0 = notepad()
        title0 = w0["content"]["y"] - w0["y"]
        tb = dbg.taskbar()
        sw, sh = shot("s100.png").size
        probe = (tb["tray_x"] - 20, sh - 60)
        before = shot("s100.png").getpixel(probe)

        # --- 150% ---------------------------------------------------------
        t = set_scale(150)
        res.check("at 150% the taskbar is 72 px (48 at 100%)", t["h"] == 72, f"h {t['h']}")
        im = shot("s150.png")
        res.check("...and drawn: 60 rows up from the bottom is the strip, not the desktop",
                  im.getpixel(probe) == rgb(t["bar"]) and before != rgb(t["bar"]),
                  f"pixel {im.getpixel(probe)} (was {before}), bar {rgb(t['bar'])}")
        w1 = poll(lambda: (lambda w: w if w and near(w["w"], w0["w"] * 3 // 2, 3) else None)(notepad()), 5) \
            or notepad()
        res.check("an open window grows by half", near(w1["w"], w0["w"] * 3 // 2, 3),
                  f"width {w0['w']} -> {w1['w']}")
        res.check("...and is kept above the taskbar", w1["y"] + w1["h"] <= sh - t["h"],
                  f"bottom {w1['y'] + w1['h']}, taskbar top {sh - t['h']}")
        title1 = w1["content"]["y"] - w1["y"]
        res.check("the title bar grows by about half", near(title1, title0 * 3 // 2, 3),
                  f"title {title0} -> {title1}")

        # --- the sample in Settings, at the step ---------------------------
        dbg.send("gui spawn /bin/wm/system/settings display.scale")
        poll(lambda: any(w["title"] == "System Settings" for w in dbg.windows()), 15)
        dbg.settle()

        def sample():
            r = dbg.send("gui widgets System Settings") or ""
            for line in r.splitlines():
                f = line.split()
                if f and f[0] == "scale_sample":
                    w, h = (int(v) for v in f[3].split("x"))
                    return int(f[5]), int(f[6]), w, h
            return None

        page = None
        r = dbg.send("gui widgets System Settings") or ""
        for line in r.splitlines():
            f = line.split()
            if f and f[0] == "page":
                page = (int(f[5]), int(f[6]), *(int(v) for v in f[3].split("x")))
        s = sample()
        if page and s:
            dbg.warp_cursor(qmp, page[0] + page[2] // 2, page[1] + page[3] // 2)
            for _ in range(8):   # wheel until the sample's bottom is on screen
                s = sample()
                if s and s[1] + s[3] <= page[1] + page[3]:
                    break
                dbg.wheel(-3)
            dbg.settle()
            s = sample()
            im = shot("sample.png")
            x = s[0] + s[2] - 6   # the strip's right end, clear of the button
            rows = sum(1 for y in range(s[1], s[1] + s[3]) if im.getpixel((x, y)) == STRIP)
            res.check("Settings' sample draws the taskbar at the step: 72 rows at 150%",
                      near(rows, 72, 1), f"{rows} strip rows in the sample at {s}")
        else:
            res.check("Settings shows the scale sample", False, r)
        w = next((w for w in dbg.windows() if w["title"] == "System Settings"), None)
        if w:
            dbg.send(f"gui close {w['z']}")

        # --- back to 100% ---------------------------------------------------
        t = set_scale(100)
        res.check("back at 100% the taskbar is 48 px", t["h"] == 48, f"h {t['h']}")
        w2 = poll(lambda: (lambda w: w if w and near(w["w"], w0["w"], 2) else None)(notepad()), 5) \
            or notepad()
        # A window the work area cut at 150% comes back two thirds of what
        # it had there, as Windows' would: the cut is not undone.
        want_h = w0["h"] if near(w1["h"], w0["h"] * 3 // 2, 3) else w1["h"] * 2 // 3
        res.check("...and the window is the size it was (or two thirds of its cut height)",
                  near(w2["w"], w0["w"], 2) and near(w2["h"], want_h, 2),
                  f"{w0['w']}x{w0['h']} -> {w1['w']}x{w1['h']} -> {w2['w']}x{w2['h']}")
        res.check("...and its title bar too", w2["content"]["y"] - w2["y"] == title0,
                  f"title {title0} -> {w2['content']['y'] - w2['y']}")
        dbg.send("sh config unset display.scale")

    return res.finish("scale_test")


if __name__ == "__main__":
    sys.exit(main())
