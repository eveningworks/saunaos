#!/usr/bin/env python3
"""tools/theme_test.py -- themes (userland/ui/utheme_file.h), the
compositor's frame drawn from them, and the Theme Editor
(userland/gui/apps/themeedit.c).

What this asks, against PIXELS where the claim is "it is drawn":

  1. A LIVE SWITCH reaches the frame: `config set desktop.theme dark`
     turns a focused title bar from Light's blue to Dark's, and High
     contrast's to yellow, with no restart -- while a desktop pixel far
     from any window stays what it was (the control: a whole-screen
     change would pass the first half too).
  2. ...and reaches an open app: the Calculator's page goes dark.
  3. The THEME EDITOR's preview is drawn in the theme being edited:
     a colour typed into the picker changes the preview's title text.
  4. Save as writes /etc/themes/<stem>.theme, and Apply makes it the
     desktop's: `desktop.theme` names it, the theme's bundled settings
     are written (Paper: round corners, the light taskbar), and the real
     title bars draw the new colour.

Everything changed is put back: the theme, the bundle's settings, the
file in /etc/themes, the layout log.

    python3 tools/vm.py start
    python3 tools/theme_test.py
"""

import argparse
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from PIL import Image                                  # noqa: E402
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from harness import Results                            # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402

LIGHT_BAR = (50, 90, 160)
DARK_BAR = (31, 63, 112)
HC_BAR = (255, 212, 0)
STEM = "testred"


def sh(dbg, cmd):
    lines = (dbg.send(f"sh {cmd}") or "").strip().splitlines()
    return lines[-1].strip() if lines else ""


def shot(qmp, tmp, name):
    p = os.path.join(tmp, name)
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def bar_pixel(dbg, qmp, tmp, title, name):
    """A pixel of `title`'s title bar, inside its top edge and clear of
    the text, the icon and the buttons."""
    w = dbg.window(title)
    if not w:
        return None, None
    x, y = w["x"] + w["w"] * 3 // 5, w["y"] + 3
    return shot(qmp, tmp, name).getpixel((x, y)), (x, y)


def wait_bar(dbg, qmp, tmp, title, want, name, timeout=8.0):
    got = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        got, _ = bar_pixel(dbg, qmp, tmp, title, name)
        if got == want:
            return got
        time.sleep(0.4)
    return got


def layout(dbg, app):
    """`<app>: layout <name> x y w h` lines, newest last -> {name: rect}."""
    out = {}
    for ln in dbg.logs(app + ":", clear=True):
        m = re.search(app + r": layout (\S+) (-?\d+) (-?\d+) (\d+) (\d+)", ln)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out


def run(dbg, qmp, tmp, res):
    keep = {k: sh(dbg, f"config get {k}") for k in
            ("desktop.theme", "desktop.corners", "desktop.taskbar_theme", "desktop.layout_log",
             "desktop.wallpaper", "desktop.wallpaper_type")}
    try:
        phase_switch(dbg, qmp, tmp, res)
        phase_editor(dbg, qmp, tmp, res)
    finally:
        for k, v in keep.items():
            if v:
                sh(dbg, f"config set {k} {v}")
        sh(dbg, f"rm /etc/themes/{STEM}.theme")
        for title in ("Calculator", "Theme Editor"):
            for w in reversed(dbg.windows() or []):
                if title in w.get("title", ""):
                    dbg.send(f"gui close {w['z']}")


def phase_switch(dbg, qmp, tmp, res):
    sh(dbg, "config set desktop.theme light")
    dbg.send("gui spawn /bin/wm/apps/calculator")
    w = None
    for _ in range(40):
        w = dbg.window("Calculator")
        if w:
            break
        time.sleep(0.3)
    if not res.check("the Calculator opens", w is not None):
        return
    dbg.warp_cursor(qmp, 1260, 20)
    got = wait_bar(dbg, qmp, tmp, "Calculator", LIGHT_BAR, "light.png")
    res.check("Light: the focused title bar is Light's blue", got == LIGHT_BAR, f"{got}")
    desk = (w["x"] + w["w"] + 40, w["y"] + w["h"] + 40)
    before = shot(qmp, tmp, "before.png").getpixel(desk)

    sh(dbg, "config set desktop.theme dark")
    got = wait_bar(dbg, qmp, tmp, "Calculator", DARK_BAR, "dark.png")
    res.check("Dark: the same title bar turns Dark's, live", got == DARK_BAR, f"{got}")
    img = shot(qmp, tmp, "dark2.png")
    res.check("control: the desktop beside it is unchanged", img.getpixel(desk) == before,
              f"{img.getpixel(desk)} vs {before}")
    c = dbg.window("Calculator")["content"]
    page = img.getpixel((c["x"] + 4, c["y"] + c["h"] - 4))
    res.check("Dark: the open app's page went dark", sum(page) < 3 * 90, f"{page}")

    sh(dbg, "config set desktop.theme high-contrast")
    got = wait_bar(dbg, qmp, tmp, "Calculator", HC_BAR, "hc.png")
    res.check("High contrast: the title bar is yellow", got == HC_BAR, f"{got}")
    sh(dbg, "config set desktop.theme light")
    dbg.close_window("Calculator")


def phase_editor(dbg, qmp, tmp, res):
    sh(dbg, "config set desktop.layout_log on")
    dbg.logs("themeedit:", clear=True)
    dbg.send("gui spawn /bin/wm/apps/themeedit paper")
    w, lay = None, {}
    deadline = time.time() + 15
    while time.time() < deadline and not ("colour_hex" in lay and "roles" in lay and "toolbar.button 3" in lay):
        time.sleep(0.5)
        lay.update(layout(dbg, "themeedit"))
        w = dbg.window("Paper - Theme Editor")
    if not res.check("the Theme Editor opens on Paper, laid out", w is not None and "colour_hex" in lay,
                     f"window {bool(w)}, layout {sorted(lay)[:8]}"):
        return
    cx, cy = w["content"]["x"], w["content"]["y"]

    def green_buttons():
        """The toolbar's green (Create-coloured) icons, left to right:
        Save as, then Apply. Its buttons are not in the layout log."""
        x0, y0, tw, th = lay["toolbar"]
        img = shot(qmp, tmp, "toolbar.png")
        xs, run_start, prev = [], None, False
        for x in range(cx + x0, cx + x0 + tw):
            hit = any(abs(img.getpixel((x, y))[0] - 44) < 30 and abs(img.getpixel((x, y))[1] - 122) < 30
                      and abs(img.getpixel((x, y))[2] - 61) < 30 for y in range(cy + y0 + 4, cy + y0 + th - 4))
            if hit and not prev:
                run_start = x
            if prev and not hit:
                xs.append(((run_start + x) // 2, cy + y0 + th // 2))
            prev = hit
        return xs

    def click(name, dx=None, dy=None):
        x, y, ww, hh = lay[name]
        dbg.warp_cursor(qmp, cx + x + (ww // 2 if dx is None else dx), cy + y + (hh // 2 if dy is None else dy))
        qmp.click()
        time.sleep(0.5)

    # Row 2 of the role list is Title text (row 0 the group's heading).
    rx, ry, rw, rh = lay["roles"]
    row = (rh // max(1, rh // 22)) if rh else 22
    dbg.warp_cursor(qmp, cx + rx + rw // 2, cy + ry + row * 2 + row // 2)
    qmp.click()
    time.sleep(0.6)
    click("colour_hex")
    qmp.combo(["ctrl", "a"])
    for ch in "ff0000":          # lower case: the harness types no Shift
        qmp.send_key(ch)
        time.sleep(0.12)
    time.sleep(1.0)
    dbg.warp_cursor(qmp, 1260, 700)
    px, py, pw, ph = lay["preview"]
    img = shot(qmp, tmp, "editor.png").crop((cx + px, cy + py, cx + px + pw, cy + py + ph // 3))
    reds = sum(1 for p in img.getdata() if p[0] > 200 and p[1] < 60 and p[2] < 60)
    res.check("the preview draws the edited title text", reds > 30, f"{reds} red pixels")

    # Save as (toolbar button 1), a name, Enter; then Apply (button 3).
    greens = green_buttons()
    if not res.check("the toolbar shows Save as and Apply", len(greens) >= 2, f"{greens}"):
        return
    dbg.warp_cursor(qmp, *greens[0])
    qmp.click()
    time.sleep(0.8)
    qmp.combo(["ctrl", "a"])
    for ch in STEM:
        qmp.send_key(ch)
        time.sleep(0.12)
    qmp.send_key("ret")
    time.sleep(1.5)
    listing = dbg.send("sh ls /etc/themes") or ""
    res.check("Save as wrote /etc/themes/" + STEM + ".theme", f"{STEM}.theme" in listing, listing.strip()[-120:])
    dbg.warp_cursor(qmp, *greens[1])
    qmp.click()
    time.sleep(2.5)
    res.check("Apply made it the desktop's theme", sh(dbg, "config get desktop.theme") == STEM)
    res.check("...with Paper's bundled settings", sh(dbg, "config get desktop.corners") == "round" and
              sh(dbg, "config get desktop.taskbar_theme") == "light")
    title = next((x["title"] for x in dbg.windows() or [] if "Theme Editor" in x.get("title", "")), None)
    ww = dbg.window(title) if title else None
    if res.check("the editor is still up", ww is not None):
        img = shot(qmp, tmp, "applied.png").crop((ww["x"], ww["y"], ww["x"] + ww["w"], ww["y"] + 34))
        reds = sum(1 for p in img.getdata() if p[0] > 200 and p[1] < 60 and p[2] < 60)
        res.check("the REAL title bar draws the new title colour", reds > 20, f"{reds} red pixels")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--shot", metavar="DIR", help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "theme_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = args.shot or tempfile.mkdtemp(prefix="theme-")
    os.makedirs(tmp, exist_ok=True)
    print("theme_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\ntheme_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
