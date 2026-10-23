#!/usr/bin/env python3
"""tools/desktop_many_test.py -- a desktop with more icons than its old table.

WHAT THIS IS
------------
The desktop held 64 icons (`DESKTOP_MAX_ICONS`) and dropped the rest of
its folder without a word; its Delete built one `/bin/trash put` argv for
the whole selection, which past about 1 KiB of paths is refused at spawn
(SPAWN_ARGS_MAX) and moved nothing. Now every per-icon table grows with
the folder and the Delete runs in batches (lib/uargbatch.h).

  1. 75 files go into /home/Desktop; ALL 75 must become icons
     (`gui icons --json`), beside the stock launchers.
  2. A rubber band over the columns that hold only test files selects
     every icon it covers -- worked out from the icons' own rects, so the
     layout is read, not assumed.
  3. Delete, then the dialog's Move: every selected file must leave the
     folder and arrive in the Recycle Bin (`trash list`). The selection's
     paths are over 1 KiB, so this needs more than one spawn.

Its positive control is a build before the change: it lists 64 icons in
all, and a Delete of more than about 55 of them moves none.

    python3 tools/vm.py start && python3 tools/desktop_many_test.py
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402

DESK = "/home/Desktop"
N = 75
NAMES = [f"m{k:02d}" for k in range(N)]
K_DELETE = "0xf788"
_res = Results()
check = _res.check


def icons(dbg):
    return (dbg.json("gui icons --json") or {}).get("icons", [])


def wait_for(fn, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.5)
    return fn()


def inside(x, y, i):
    return i["x"] <= x < i["x"] + i["w"] and i["y"] <= y < i["y"] + i["h"]


def binned(dbg):
    out = dbg.send("sh trash list") or ""
    return {ln[:24].strip() for ln in out.splitlines() if ln.strip()}


def run(dbg):
    dbg.send("sh trash empty")
    # The debug console takes a 128-byte line: six paths a `touch`.
    for at in range(0, N, 6):
        dbg.send("sh touch " + " ".join(f"{DESK}/{n}" for n in NAMES[at:at + 6]))
    want = set(NAMES)
    have = wait_for(lambda: want <= {i["name"] for i in icons(dbg)})
    ic = icons(dbg)
    mine = [i for i in ic if i["name"] in want]
    check(f"all {N} files are icons, beside the {len(ic) - len(mine)} stock ones",
          bool(have) and len(mine) == N, f"{len(mine)} of {N}; {len(ic)} icons in all")
    if len(mine) < N:
        return

    # 2. The band: from the first column holding nothing but test files to
    # the screen's right edge, over the work area's height.
    st = dbg.json("gui state --json") or {}
    sw = st.get("screen", {}).get("w", 1280)
    sh_ = st.get("screen", {}).get("h", 720) - st.get("taskbar_h", 40)
    others = [i for i in ic if i["name"] not in want]
    x0 = max((i["x"] + i["w"] for i in others), default=0) + 2
    starts = [i for i in mine if i["x"] >= x0]
    if starts:
        x0 = min(i["x"] for i in starts) - 2
    start = (x0, 2)
    covered = [i for i in mine if i["x"] + i["w"] > x0 and i["x"] < sw - 1 and i["y"] < sh_ - 1]
    check("the band starts on bare desktop", not any(inside(*start, i) for i in ic), f"start {start}")
    dbg.drag(start[0], start[1], sw - 2, sh_ - 2, steps=12)
    time.sleep(0.5)
    dbg.settle()
    sel = {i["name"] for i in icons(dbg) if i.get("selected")}
    expect = {i["name"] for i in covered}
    check(f"the band selects every icon it covers ({len(expect)})",
          sel == expect and len(expect) > 0, f"selected {len(sel)}, covered {len(expect)}")
    paths = sum(len(f"{DESK}/{n}") + 1 for n in sel)
    check("...and their paths need more than one spawn", paths > 1024 - 20, f"{paths} bytes")

    # 3. Delete -> Move -> all of them in the bin.
    dbg.key(K_DELETE)
    dbg.settle()
    btns = (dbg.json("gui dialog --json") or {}).get("buttons", [])
    check("Delete asks first", len(btns) == 2 and btns[0].get("label") == "Move",
          str([b.get("label") for b in btns]))
    if len(btns) != 2:
        return
    dbg.send(f"gui click {btns[0]['cx']} {btns[0]['cy']}")
    dbg.settle()
    gone = wait_for(lambda: not (sel & {i["name"] for i in icons(dbg)}), timeout=60)
    left = sel & {i["name"] for i in icons(dbg)}
    check("every selected icon leaves the desktop", bool(gone), f"{len(left)} left")
    bin_ = binned(dbg)
    check("...and every one is in the Recycle Bin", sel <= bin_, f"{len(sel - bin_)} missing")
    kept = want - sel
    check("the icons the band did not cover stay", kept <= {i["name"] for i in icons(dbg)},
          f"{len(kept - {i['name'] for i in icons(dbg)})} lost")
    dbg.send("sh trash empty")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "desktop_many_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("a desktop past its old 64 icons")
    run(dbg)
    print(f"\ndesktop_many_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
