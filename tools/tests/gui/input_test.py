#!/usr/bin/env python3
"""`gui input`: a sequence of keys and pointer steps the window manager times, not the host.

WHY THIS EXISTS. An Alt+Tab TAP sent as two `gui key` commands is timed by two serial round trips, and under
the parallel suite that is past the switcher's 100 ms: the tap becomes a hold (docs/gui-test-sync-design.md,
14 of 77 flakes were input or animation timing). `gui input` takes the whole sequence at once and feeds each
step when the GUEST's clock says it is due.

THE CHECKS, each one a broken version would fail:
  - a tap is a tap and a hold is a hold, as the SWITCHER measured them (`gui switcher`'s last_ms and
    whether its card drew) -- a `wait` that was not honoured makes the hold a tap;
  - a title-bar drag that WAITS with the button down, while the REAL pointer moves, still moves the
    window: the held button is re-delivered on every pass the real motion wakes (without that the loop
    reads the real mouse, button up, and the move after the wait is no drag);
  - a sequence that ends with a button held is refused, and so is a second sequence while one runs.

  python3 tools/tests/gui/input_test.py [--instance N]
"""

RUNNER = {
    "covers": "`gui input`: a WM-timed tap vs hold (the switcher's own measure), a held drag across a wait, refusals",
}

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

KEY_ALT = 0xF798   # api/keyboard.h, as switcher_test.py names it


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "input_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("input: sequences the window manager times")
    try:
        note = dbg.spawn("/bin/wm/apps/notepad", "untitled")
        counter = dbg.spawn("/bin/wm/demos/counter", "Counter")
        if not res.check("two windows to switch between", note is not None and counter is not None):
            return res.finish("input_test")

        def sw():
            return dbg.json("gui switcher --json")

        # A tap: Alt released at once.
        ran = dbg.input(f"key 0x09 alt; keyup {hex(KEY_ALT)}")
        st = sw()
        res.check("a tap runs, and the switcher saw a tap: released inside 100 ms, no card",
                  ran and 0 <= st.get("last_ms", -1) < 100 and not st.get("last_drawn"), f"ran={ran} {st}")
        # A hold: Alt released 400 ms of GUEST time later.
        ran = dbg.input(f"key 0x09 alt; wait 400; keyup {hex(KEY_ALT)}")
        st = sw()
        res.check("a 400 ms hold is a hold: held 400 ms or more, and the card drew",
                  ran and st.get("last_ms", 0) >= 400 and bool(st.get("last_drawn")), f"ran={ran} {st}")

        # A held drag across a wait moves the window -- WITH THE REAL POINTER
        # MOVING during the wait. Each real motion wakes the loop, which
        # reads the real mouse (button up): only the held press re-delivered
        # every pass keeps the drag. With nothing waking the loop it simply
        # sleeps through the wait, and an idle check could not tell.
        w = dbg.window("Counter")
        x, y = w["x"] + w["w"] // 2, w["y"] + 8
        out = dbg.send(f"gui input move {x} {y}; press; wait 1500; move {x + 60} {y + 40}; wait 100; release")
        for _ in range(12):
            qmp.move_rel(4, 0)
            qmp.move_rel(-4, 0)
            dbg.settle(0, sync=False)
        r = {"state": "pending"}
        m = re.search(r'"ticket":(\d+)', out or "")
        for _ in range(200):
            if not m or r.get("state") != "pending":
                break
            r = dbg.json(f"gui waited {m.group(1)}")
            dbg.settle(0, sync=False)
        ran = r.get("state") == "done"
        moved = poll(lambda: (lambda v: v if v and (v["x"], v["y"]) == (w["x"] + 60, w["y"] + 40) else None)(
            dbg.window("Counter")), 5)
        res.check("a title-bar drag that waits with the button down still moves the window",
                  ran and moved is not None, f"ran={ran} from {(w['x'], w['y'])} to "
                  f"{(dbg.window('Counter') or {}).get('x')},{(dbg.window('Counter') or {}).get('y')}")

        held = dbg.send(f"gui input move {x} {y}; press")
        res.check("a sequence that ends with a button held is refused", "refused" in held, held.strip())
        dbg.send(f"gui input wait 3000; move {x} {y + 200}")   # harmless when it lands
        busy = dbg.send("gui input tap b")
        res.check("a second sequence while one runs is refused", "still running" in busy, busy.strip())
    finally:
        dbg.close()
    return res.finish("input_test")


if __name__ == "__main__":
    sys.exit(main())
