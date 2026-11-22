#!/usr/bin/env python3
"""A program that execs another from a process that already had a window:
the second program's window opens.

/tests/execclient opens "execclient before", quits as soon as it is open,
and execs itself as `execclient after`, which opens "execclient after".
The exec lets the first program's channel ring go and the second opens
one of the SAME NAME at once; a compositor that keyed its clients by pid
alone kept reading the dead ring and never saw the second window
(base/uchan.c, the ring's QUERY_SHM serial). ScummVM's free-games card
handing over to ScummVM did exactly this, and on real hardware ScummVM
never appeared.

Run ROUNDS times: the window between the old ring going and the new one
arriving is short, and each round is a fresh pid.
"""

RUNNER = {"name": "execwindow",
          "covers": "a window from the program a windowed process execs (the channel ring reused by name)"}

import argparse
import re
import sys

import os  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
import harness  # noqa: E402
from harness import Results  # noqa: E402

CLIENT = "/tests/execclient"
ROUNDS = 5


def after_window(dbg):
    return next((w for w in dbg.windows() if w["title"] == "execclient after"), None)


def run(dbg, res):
    opened = 0
    for _ in range(ROUNDS):
        dbg.logs("execclient:", clear=True)
        out = dbg.send(f"gui spawn {CLIENT}") or ""
        m = re.search(r"as pid (\d+)", out)
        pid = int(m.group(1)) if m else None
        before = harness.poll(lambda: [l for l in dbg.logs("execclient:", clear=False)
                                       if "before open" in l], 15)
        win = harness.poll(lambda: after_window(dbg), 15) if before else None
        opened += win is not None
        if pid:
            dbg.send(f"sh kill {pid}")
        harness.poll(lambda: after_window(dbg) is None, 10)
    res.check(f"the program a windowed process execs opens its window, {ROUNDS} rounds",
              opened == ROUNDS, f"{opened} of {ROUNDS} opened")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "exec_window_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, res)
    finally:
        dbg.close()
    print(f"\nexec_window_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
