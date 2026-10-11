#!/usr/bin/env python3
"""Corners (desktop.corners) and the always-visible scrollbar
(desktop.scrollbars) reach a running app, by pixel.

WHAT IT CHECKS, and what a broken version would still pass:

  - About's Close button, its exact top-left pixel: under Square it is
    the button's own edge, under Small (4 px) the window behind it -- and
    the change lands in an app ALREADY OPEN (WIN_EV_SETTING ->
    utheme_settings_reload()). A radius that only read the setting at
    start would pass a test that opened the app afterwards;
  - a settled frame of a scrollable list at rest: with Always the bar's
    column shows the groove, with "When pointed at" plain field white.
    A pointer resting on the list would widen the bar in either mode, so
    the pointer is parked away first;
  - the bar's far column is the groove, not field: an inset left a
    one-pixel gap that a whole-widget focus ring filled only while the
    selection was scrolled away.

Geometry from `gui widgets`; pixels from settled screendumps.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

_res = Results()
check = _res.check


def luminance(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def pixel(qmp, path, x, y):
    from PIL import Image
    qmp.screenshot(path)
    with Image.open(path) as im:
        return im.convert("RGB").getpixel((x, y))


def setting(dbg, name, value):
    dbg.send(f"sh config set {name} {value}")
    time.sleep(1.5)
    dbg.settle()


def finish():
    return _res.finish("corners_test")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "corners_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    shot = os.path.join(args.tmp, f"corners_{os.getpid()}.png")
    print("corners and scrollbars (ui/utheme.h)")

    win = dbg.spawn("/bin/wm/system/about", "About")
    if not check("About opens", win is not None):
        return finish()
    dbg.settle()
    w = dbg.widgets("About").get("close")
    if not check("About reports its Close button", w is not None):
        return finish()
    cx, cy = w["screen"]["x"], w["screen"]["y"]
    dbg.move(10, 10)
    setting(dbg, "desktop.corners", "square")
    sq = pixel(qmp, shot, cx, cy)
    setting(dbg, "desktop.corners", "small")
    sm = pixel(qmp, shot, cx, cy)
    check("under Square the button's corner pixel is its edge, under Small the window behind it",
          luminance(sm) - luminance(sq) > 40, f"square {sq} small {sm}")

    # A scrollable list at rest: First-boot setup's keyboard layouts.
    dbg.send("sh fbs now")
    end = time.time() + 10
    while time.time() < end and "next" not in dbg.widgets("First-boot setup"):
        time.sleep(0.3)
    nx, ny = dbg.widget_center("next", "First-boot setup")
    dbg.click(nx, ny)
    time.sleep(1.5)
    lb = dbg.widgets("First-boot setup").get("layouts")
    if not check("the Keyboard step reports its layout list", lb is not None):
        return finish()
    bx = lb["screen"]["x"] + lb["w"] - 4
    by = lb["screen"]["y"] + lb["h"] - 6     # low in the bar, below the thumb at the top
    dbg.move(10, 10)
    setting(dbg, "desktop.scrollbars", "always")
    al = pixel(qmp, shot, bx, by)
    edge = pixel(qmp, shot, lb["screen"]["x"] + lb["w"] - 1, by)
    setting(dbg, "desktop.scrollbars", "auto")
    au = pixel(qmp, shot, bx, by)
    setting(dbg, "desktop.scrollbars", "always")
    check("Always draws the groove at rest; When pointed at leaves the field white there",
          luminance(au) - luminance(al) > 4 and luminance(au) > 245, f"always {al} auto {au}")
    check("the bar is flush with the list's far edge -- no column of field between",
          abs(luminance(edge) - luminance(al)) <= 4, f"edge {edge} groove {al}")
    return finish()


if __name__ == "__main__":
    sys.exit(main())
