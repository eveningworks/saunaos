#!/usr/bin/env python3
"""Back straight after a double click: the File Manager's history records
a move AS THE PANE MAKES IT, not on the next tick.

A double click descends from inside uui_fileview, and the move used to
reach the history only at the app's next tick (fm_history_sync(), every
half second or so). Back pressed sooner stepped from the folder BEFORE
the one just entered: the pane stayed put, and two or three such
presses emptied the history -- "nothing to go back to", Back greyed.
Measured before the fix: Back failed 8 of 20 at 0.1 s after arriving,
0 of 20 at 0.6 s.

1. **Each round trip lands**: double-click a folder, Alt+Left at once,
   and the pane is back in the folder it left -- six times over three
   folders, so a history that loses one entry shows by the next.
2. **The history is still whole after them**: Back from the fixture's
   top reaches the folder the window opened on before it.

A broken version passes neither: (1) leaves the pane in the subfolder,
and (2) reads "nothing to go back to".
"""

RUNNER = {
    "name": "fmback",
    "covers": "Back at once after a double click goes back, six times, and the history stays whole",
}

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import filemanager_test as fm  # noqa: E402
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

BASE = "/hb"
TOP = f"{BASE}/top"
ROUNDS = 6


def row_xy(lay, i):
    px, _, pw, _ = lay.pane[0]
    return lay.ox + px + pw // 3, lay.oy + lay.rowy[0] + lay.rowh * i + lay.rowh // 2


def run(dbg, qmp, res):
    dbg.send(f"sh rm -r {BASE}")
    dbg.send(f"sh mkdir {BASE}")
    dbg.send(f"sh mkdir {TOP}")
    for d in ("d1", "d2", "d3"):
        dbg.send(f"sh mkdir {TOP}/{d}")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {BASE}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == BASE and l.rowy.get(0) is not None
                         and l.rowh)
    if lay is None:
        res.check("the listing is laid out", False, f"last={fm.last_layout()}")
        return
    dbg.key("0x74")                        # 't': type-ahead to top/
    fm.wait_layout(dbg, win, lambda l: l.selected == "top")
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == TOP and l.rowy.get(0) is not None)
    res.check("Enter goes into the fixture", lay is not None, f"dir={fm.last_layout() and fm.last_layout().dir}")
    if lay is None:
        return

    # 1.
    landed, why = 0, ""
    for k in range(ROUNDS):
        i = 1 + k % 3                      # rows: "..", d1, d2, d3
        want = f"{TOP}/d{i}"
        x, y = row_xy(fm.layout_now(dbg, win) or lay, i)
        dbg.warp_cursor(qmp, x, y)
        qmp.click()
        qmp.click()
        if not fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == want, timeout=5):
            why = f"round {k}: the double click did not reach {want}"
            break
        dbg.key(fm.K_LEFT, mods="alt")     # at once: no tick in between
        if not fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == TOP, timeout=4):
            last = fm.last_layout()
            why = f"round {k}: Back left the pane in {last and last.dir.get(0)}"
            break
        landed += 1
    res.check(f"Back straight after a double click goes back, {ROUNDS} times",
              landed == ROUNDS, why or f"{landed} of {ROUNDS}")

    # 2.
    dbg.key(fm.K_LEFT, mods="alt")
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == BASE, timeout=4)
    res.check("...and the history is whole: Back from there reaches where the window opened",
              lay is not None, f"dir={fm.last_layout() and fm.last_layout().dir.get(0)}")
    dbg.send(f"sh rm -r {BASE}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_back_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_back_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
