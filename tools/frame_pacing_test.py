#!/usr/bin/env python3
"""The compositor composes at most one frame per refresh, and a lone
change after a quiet moment is composed at once.

WHAT IS UNDER TEST (userland/wm/wm.c, "FRAMES ARE PACED TO THE REFRESH"):
a fast mouse wakes toywm's loop hundreds of times a second, and before
pacing every wake that owed a frame composed one -- the ASUS composed
8161 frames in one Notepad drag, 8143 of them within a refresh of the
last, none of which the panel ever showed.

1. **A window dragged by its title bar with FAST motion** -- relative
   QMP moves with no sleep, ~3000 a second; `warp_cursor()` is ~0.3 s a
   step and would never reach the cap -- composes no more frames a
   second than the monitor's refresh (its EDID, from `lsdisplay`), plus
   a margin for the clock.
2. **The same drag with `gui pacing off` composes MORE than that.** The
   positive control, built in: a drag that composed under the cap even
   unpaced would pass check 1 without testing anything.
3. **After a quiet moment, one change waits for nothing**: the pacing
   counter (passes that owed a frame and waited) does not move across a
   single press on the title bar. A fixed frame clock would delay it.

Calculator, because the WM draws its frame: a client that draws its own
title bar (the File Manager's tabs) moves itself, and a press there is
the client's.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/frame_pacing_test.py
    python3 tools/vm.py stop
"""
import argparse
import re
import sys
import time

from gui_debug import enter_gui, DebugConsole   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

TITLE = "Calculator"
SPAWN = "/bin/wm/apps/calculator"
MOVES = 1800           # relative steps per drag, sent back to back
MARGIN = 1.25          # frames a second over the refresh still counted as paced


def refresh_hz(dbg):
    out = dbg.send("sh lsdisplay") or ""
    m = re.search(r"@ (\d+(?:\.\d+)?) Hz", out)
    return float(m.group(1)) if m else 60.0


def frames(c):
    return c["frame_full"]["n"] + c["frame_partial"]["n"]


def drag(dbg, qmp, x, y):
    """Hold the title bar at (x, y) and shake the window: MOVES steps out
    and back, so it ends where it began. Frames a second while held."""
    dbg.warp_cursor(qmp, x, y)
    time.sleep(0.3)
    qmp.mouse_down()
    time.sleep(0.2)
    c0 = dbg.json("gui compositor --json")
    t0 = time.time()
    for i in range(MOVES):
        d = 2 if (i // 150) % 2 == 0 else -2
        qmp.move_rel(d, 1 if d > 0 else -1)
    secs = time.time() - t0
    c1 = dbg.json("gui compositor --json")
    qmp.mouse_up()
    time.sleep(0.4)
    return (frames(c1) - frames(c0)) / secs, frames(c1) - frames(c0)


def run(dbg, qmp, res):
    hz = refresh_hz(dbg)
    print(f"frame pacing (the monitor says {hz:.2f} Hz)")
    win = dbg.window(TITLE) or dbg.spawn(SPAWN, TITLE)
    win = dbg.window(TITLE)
    res.check("Calculator opens", win is not None, f"window {win and win['title']}")
    if not win:
        return
    x, y = win["x"] + win["w"] // 2, win["y"] + 8
    try:
        dbg.send("gui pacing on")
        paced_rate, n_on = drag(dbg, qmp, x, y)
        dbg.send("gui pacing off")
        free_rate, n_off = drag(dbg, qmp, x, y)
    finally:
        dbg.send("gui pacing on")   # leave the machine as found
    cap = hz * MARGIN
    res.check("a fast window drag composes no more than one frame per refresh",
              n_on > 0 and paced_rate <= cap,
              f"{paced_rate:.0f}/s ({n_on} frames), cap {cap:.0f}/s")
    res.check("...and the same drag unpaced composes more (the drag tests the cap)",
              free_rate > cap, f"{free_rate:.0f}/s ({n_off} frames) unpaced")

    # 3. A lone change after a quiet moment.
    time.sleep(0.6)
    p0 = dbg.json("gui compositor --json").get("paced", 0)
    f0 = frames(dbg.json("gui compositor --json"))
    dbg.warp_cursor(qmp, x, y)
    time.sleep(0.6)
    qmp.mouse_down()
    time.sleep(0.3)
    qmp.mouse_up()
    time.sleep(0.3)
    c = dbg.json("gui compositor --json")
    res.check("after a quiet moment, a single press waits for nothing",
              c.get("paced", -1) == p0 and frames(c) > f0,
              f"paced {p0} -> {c.get('paced')}, frames {f0} -> {frames(c)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "frame_pacing_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nframe_pacing_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
