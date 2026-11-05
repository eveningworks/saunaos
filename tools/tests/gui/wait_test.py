#!/usr/bin/env python3
"""`gui wait` / `gui waited` and `gui sync TITLE`: conditions the window manager decides.

WHY THIS EXISTS. Most of the GUI suite's flakes were a test waiting on the wrong thing -- a window polled for
with the host's clock, a widget read before its client reported it (docs/gui-test-sync-design.md). These
commands let the window manager decide instead: `gui wait` answers a ticket at once (a `gui` command cannot
block -- one kernel slot, about two seconds), the WM evaluates the condition every frame and on every
`gui waited`, and a `--pid` that exits ends the wait at once.

THE CHECKS, each one a broken version would fail:
  - a window wait ends `done` only once the window has PRESENTED (first_frame > 0 when it says done);
  - a program that exits ends its wait `exited`, long before the wait's own timeout;
  - a widget that never comes ends `timeout`, at about the time asked and not before;
  - a widget wait ends `done` with the name in the window's map; a closed window's `gone` ends `done`;
  - `gui sync TITLE` also asks THAT window's client, which is neither focused nor under the pointer.

  python3 tools/tests/gui/wait_test.py [--instance N]
"""

RUNNER = {
    "covers": "`gui wait`/`gui waited` and `gui sync TITLE`: window, exit, timeout, widget, gone; a titled sync",
}

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402


def decided(dbg, reply, host_limit=90):
    """Ask after a ticket until it is decided; the final reply."""
    deadline = time.time() + host_limit
    while reply.get("state") == "pending" and time.time() < deadline:
        dbg.settle(0, sync=False)
        reply = dbg.json(f"gui waited {reply['ticket']}")
    return reply


def spawn_pid(dbg, path):
    m = re.search(r"as pid (\d+)", dbg.send(f"gui spawn {path}") or "")
    return int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "wait_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("wait: conditions the window manager decides")
    try:
        # A window: done only once it has presented.
        pid = spawn_pid(dbg, "/bin/wm/apps/notepad")
        r = decided(dbg, dbg.json(f"gui wait window --pid {pid} untitled"))
        w = dbg.window("untitled")
        res.check("a window wait ends done, the window presented", r.get("state") == "done" and w is not None
                  and w.get("first_frame", {}).get("w", 0) > 0, f"{r} first_frame {w and w.get('first_frame')}")
        if not w:
            return res.finish("wait_test")

        # A program that exits: `exited`, well inside the wait's own minute.
        pid = spawn_pid(dbg, "/tests/exit_test")
        t0 = time.time()
        r = decided(dbg, dbg.json(f"gui wait window --pid {pid} --ms 60000 No Such Window"))
        res.check("a program that exits ends its wait `exited`, not at the timeout",
                  r.get("state") == "exited" and r.get("ms", 1e9) < 20000,
                  f"{r}, {time.time() - t0:.1f} s on the host")

        # A widget that never comes: `timeout`, at about the time asked.
        r = decided(dbg, dbg.json("gui wait widget --ms 800 no-such-widget untitled"))
        res.check("a widget that never comes ends `timeout`, not before the time asked",
                  r.get("state") == "timeout" and 800 <= r.get("ms", 0) < 10000, f"{r}")

        # A widget that is there.
        # One with a real rect: a closed dialog sits in the map at 0x0, and a
        # widget wait counts only what has a rect (that case is the timeout above).
        ws = dbg.widgets("untitled")
        names = sorted(k for k, v in ws.items() if v.get("w", 0) > 0 and v.get("h", 0) > 0)
        if res.check("Notepad names its widgets", bool(names), f"{sorted(ws)}"):
            r = decided(dbg, dbg.json(f"gui wait widget {names[0]} untitled"))
            res.check("a widget wait ends done for a widget the map has", r.get("state") == "done", f"{r} {names[0]}")

        # A sync with a TITLE asks that window's client too. The counter
        # opens over Notepad and takes the focus; the pointer is parked on
        # the desktop, so Notepad is neither focused nor under it.
        counter = dbg.spawn("/bin/wm/demos/counter", "Counter")
        dbg.warp_cursor(qmp, 4, 4)
        dbg.settle()
        plain = dbg.json("gui sync")
        titled = dbg.json("gui sync untitled")
        res.check("`gui sync TITLE` also asks that window's client",
                  counter is not None and titled.get("clients", 0) == plain.get("clients", 0) + 1,
                  f"untitled {plain}, titled {titled}")
        res.check("...and settle() completes it", dbg.sync(title="untitled"))

        # Gone.
        for title in ("Counter", "untitled"):
            win = dbg.window(title)
            if win:
                dbg.send(f"gui close {win['z']}")
        r = decided(dbg, dbg.json("gui wait gone Counter"))
        res.check("a closed window's `gone` wait ends done", r.get("state") == "done"
                  and dbg.window("Counter") is None, f"{r}")
        res.check("an unknown ticket says so", dbg.json("gui waited 999999").get("state") == "unknown")
        res.check("the harness's wait_gone() agrees", dbg.wait_gone("untitled", 30))
    finally:
        dbg.close()
    return res.finish("wait_test")


if __name__ == "__main__":
    sys.exit(main())
