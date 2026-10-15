#!/usr/bin/env python3
"""tools/caption_corner_test.py -- a maximized window's close button owns
the screen corner.

WHAT IS UNDER TEST. On a maximized window the caption buttons reach the
screen edges they sit against -- the top row for all three, the right
column for close -- so throwing the pointer into the top-right corner
and clicking closes the window, as on Windows and KWin. The drawn
buttons stop short of the edge (the hairline is there), so a hit test
that used the drawn rects left the corner dead.

What a broken version would still pass, and so what this checks:
- `gui probe` alone: it reports what the hit test says, but a probe
  that re-derived the rects could agree with itself and not with the
  click. So the corner is also clicked with the REAL pointer, which
  goes through press (arm) and release (commit), and the window must go.
- an "extend everywhere" bug: a RESTORED window's top-right pixel must
  still NOT be the close button -- it is the frame's.
- a too-greedy extension: the corner column below the title bar is
  still the window's content, not the close button.

    python3 tools/vm.py start
    python3 tools/caption_corner_test.py
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402
from harness import Results  # noqa: E402

APP = "System Settings"


def close_all(dbg, timeout=12.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ws = dbg.json("gui windows --json")["windows"]
        if not ws:
            return True
        dbg.send(f"gui close {len(ws) - 1}")
        dbg.settle()
        time.sleep(0.4)
    return False


def find(dbg, title):
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] == title:
            return w
    return None


def wait_window(dbg, pred, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        dbg.settle()
        w = find(dbg, APP)
        if pred(w):
            return w
        time.sleep(0.3)
    return find(dbg, APP)


def region(dbg, x, y):
    return dbg.probe(x, y).get("region")


def run(dbg, qmp, res):
    sw = dbg.json("gui state --json")["screen"]["w"]
    close_all(dbg)

    dbg.open_app(APP)
    w = wait_window(dbg, lambda w: w is not None)
    if not res.check("System Settings opens", w is not None):
        return
    # Never opens maximized: windows.conf keeps the restore geometry only.
    band = w["content"]["y"] - w["y"]
    mid = w["y"] + band // 2

    # THE CONTROL: a restored window's top-right pixel is its frame.
    r = region(dbg, w["x"] + w["w"] - 1, w["y"])
    res.check("a restored window's top-right pixel is NOT its close button",
              r != "close-button", f"probe said {r}")

    # Maximize it the way a person does: a real click on the button.
    max_x = None
    for x in range(w["x"] + w["w"] - 2, w["x"], -2):
        if region(dbg, x, mid) == "maximize-button":
            max_x = x
            break
    if not res.check("...and it has a maximize button", max_x is not None):
        return
    dbg.warp_cursor(qmp, max_x - 3, mid)
    qmp.click()
    w = wait_window(dbg, lambda w: w and w["state"] == "maximized")
    if not res.check("the window maximizes", w and w["state"] == "maximized",
                     f"state {w and w['state']}"):
        return

    corner = (w["x"] + w["w"] - 1, w["y"])
    res.check("the screen's top-right pixel is the close button",
              corner[0] == sw - 1 and region(dbg, *corner) == "close-button",
              f"corner {corner}, screen {sw}, probe {region(dbg, *corner)}")
    # The top row above the other two buttons, found from their own
    # middles so the theme's button width is not hard-coded here.
    for name in ("minimize-button", "maximize-button"):
        xs = [x for x in range(w["x"] + w["w"] - 2, w["x"] + w["w"] // 2, -2)
              if region(dbg, x, mid) == name]
        top = region(dbg, xs[len(xs) // 2], w["y"]) if xs else None
        res.check(f"the top row over {name} is that button", top == name, f"probe {top}")
    below = region(dbg, corner[0], w["content"]["y"] + 4)
    res.check("the corner column BELOW the title bar is not the close button",
              below != "close-button", f"probe {below}")

    # THE REAL GESTURE: pointer into the corner, press, release.
    got = dbg.warp_cursor(qmp, *corner)
    res.check("the pointer reaches the corner", tuple(got) == corner, f"at {got}")
    qmp.click()
    gone = wait_window(dbg, lambda w: w is None, timeout=8.0) is None
    res.check("clicking the corner closes the maximized window", gone)
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser()
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "caption_corner_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Results()
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()

    print(f"\ncaption_corner_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
