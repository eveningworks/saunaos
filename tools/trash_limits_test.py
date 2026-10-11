#!/usr/bin/env python3
"""tools/trash_limits_test.py -- the Recycle Bin's settings: shown or not,
and how long it keeps things.

WHAT THIS IS
------------
Storage > Recycle Bin in System Settings (data/etc/settings.d/
desktop.show_trash, system.trash_max_age, system.trash_max_size), kept
by `trash expire` (lib/utrash.h), which the desktop runs:

  1. `desktop.show_trash off` takes the bin's icon off the desktop and
     `on` brings it back -- and brings back a bin whose entry was deleted.
  2. With `system.trash_max_age 30`, an item deleted in 2020 goes from
     the bin within seconds of the change, by the desktop's own run of
     `trash expire`; one deleted now stays (the control). With no limit,
     `trash expire` deletes nothing.
  3. The bin's menu has "Recycle Bin settings...", which opens System
     Settings on the page, the page shows what the bins hold
     ("trash_usage"), and the sidebar is scrolled to the page's row --
     one of the last, below the first screenful.

NOT COVERED: the size limit's trimming. Its smallest step is 5% of the
volume, hundreds of MB on a test image -- more than a TCG guest writes in
a test's time.

    python3 tools/vm.py start && python3 tools/trash_limits_test.py
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402

DESK = "/home/Desktop"
BIN = "/home/.Trash"
_res = Results()
check = _res.check


def icons(dbg):
    return {i["name"]: i for i in dbg.json("gui icons --json").get("icons", [])}


def wait_for(fn, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.3)
    return fn()


def run(dbg, qmp):
    sh = lambda c: dbg.send(f"sh {c}")   # noqa: E731

    # 1. Shown or not.
    check("the bin is on the desktop by default", bool(wait_for(lambda: "Recycle Bin" in icons(dbg))))
    sh("config set desktop.show_trash off")
    check("show_trash off: it is not", bool(wait_for(lambda: "Recycle Bin" not in icons(dbg))))
    sh("config set desktop.show_trash on")
    check("...and on: it is back", bool(wait_for(lambda: "Recycle Bin" in icons(dbg))))
    sh("config set desktop.show_trash off")
    wait_for(lambda: "Recycle Bin" not in icons(dbg))
    sh(f"rm {DESK}/trash.desktop")
    time.sleep(1.0)
    sh("config set desktop.show_trash on")
    check("turning it on brings back a deleted bin", bool(wait_for(lambda: "Recycle Bin" in icons(dbg))),
          sh(f"ls {DESK}")[-200:])

    # 2. Age. An item deleted in 2020, made by hand in the spec's layout,
    # and one put there now.
    for c in (f"mkdir {BIN}", f"mkdir {BIN}/files", f"mkdir {BIN}/info",
              f"write {BIN}/files/old.txt from 2020",
              f"write {BIN}/info/old.txt.trashinfo [Trash Info]",
              f"append {BIN}/info/old.txt.trashinfo Path=/home/old.txt",
              f"append {BIN}/info/old.txt.trashinfo DeletionDate=2020-01-01T00:00:00",
              "write /home/fresh.txt from now", "trash put /home/fresh.txt"):
        sh(c)
    listed = sh("trash list")
    check("both are in the bin", "old.txt" in listed and "fresh.txt" in listed, listed[-300:])
    out = sh("trash expire")
    check("with no limit, `trash expire` deletes nothing", "no limits set" in out and "old.txt" in sh("trash list"),
          out[-200:])
    sh("config set system.trash_max_age 30")
    gone = wait_for(lambda: "old.txt" not in sh("trash list"), timeout=20)
    check("a 30-day limit: the desktop deletes the 2020 item", bool(gone), sh("trash list")[-300:])
    check("...and keeps the one deleted now", "fresh.txt" in sh("trash list"))
    sh("config set system.trash_max_age never")

    # 3. The bin's menu, and the page it opens.
    b = icons(dbg)["Recycle Bin"]
    dbg.rclick(b["x"] + b["w"] // 2, b["y"] + b["h"] // 3)
    row = dbg.ctxmenu_row("Recycle Bin settings...")
    if not check("the bin's menu has Recycle Bin settings...", row is not None, str(dbg.ctxmenu().get("rows"))):
        return
    dbg.logs()   # clears
    dbg.click(*row)
    w = wait_for(lambda: dbg.window("System Settings"), timeout=15)
    check("...which opens System Settings", w is not None)
    seen = wait_for(lambda: dbg.logs("settings: trash usage", clear=False), timeout=10)
    check("...on the page that shows what the bins hold", bool(seen) and "1 bin(s)" in seen[-1], str(seen))
    # The sidebar scrolls to the page it opened on: Recycle Bin is one of
    # the last rows, below the first screenful.
    lines = dbg.logs("settings:", clear=False)
    rows = [re.search(r"settings: row (\d+) id \d+ y -?\d+ depth \d+ (.+)", ln) for ln in lines]
    row = max((int(m.group(1)) for m in rows if m and m.group(2).strip() == "Recycle Bin"), default=-1)
    sb = [re.search(r"settings: sidebar top (-?\d+) visible (\d+)", ln) for ln in lines]
    sb = [m for m in sb if m]
    top, vis = (int(sb[-1].group(1)), int(sb[-1].group(2))) if sb else (-1, 0)
    check("...with its row scrolled into the sidebar's view", row >= 0 and top <= row < top + vis,
          f"row {row}, sidebar top {top} visible {vis}")
    dbg.close_window("System Settings")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "trash_limits_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("the Recycle Bin's settings")
    run(dbg, qmp)
    print(f"\ntrash_limits_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
