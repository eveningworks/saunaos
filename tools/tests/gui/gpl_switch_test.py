#!/usr/bin/env python3
"""The GPL switch: software.show_gpl off takes GPL-licensed apps out of the Start menu and off the desktop.

docs/software-design.md, stage 4. A VIEW over the optional components:
toywm's own scan of /usr/wm/applications (gui_apps.c) and the desktop
folder skip an entry whose file -- or whose program -- lies in a
GPL-licensed component while the switch is off. Nothing is removed.

WHAT IT CHECKS, through the compositor's own reports (`gui menu --json`,
`gui icons --json`) rather than pixels, and what a broken version would
still pass:

  - with the switch on, the Start menu lists DOOM (GPL-2.0-or-later) and
    the Web Browser (MIT, needing the GPL engine), and a DOOM launcher
    copied to the desktop is an icon there;
  - turned off, with NOTHING in /usr/wm/applications changing (the reload
    must follow the SETTING, not the directory), DOOM leaves the menu and
    the desktop -- and the Web Browser, MIT, stays: it is the licence of
    the component that holds an app that counts;
  - THE ROWS THAT REMAIN STILL LAUNCH WHAT THEY SAY: the Games folder is
    Minesweeper alone now, and its row opens Minesweeper (the Start
    menu's rows are positional -- desktop_entries_test.py's trap);
  - DOOM's files are still there, and Task Manager -- which NAMES
    running programs from their entries -- reads as many entries with
    the switch off as on (`taskmgr: names from N desktop entries`);
  - turned back on, DOOM returns to both.

    python3 tools/tests/gui/gpl_switch_test.py --instance 8
"""

RUNNER = {"name": "gpl_switch", "covers": "software.show_gpl hides GPL-licensed apps from the Start menu and desktop"}

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

LAUNCHER = "/home/Desktop/doom.desktop"
KEY_SUPER = "0xf795"


def icons(dbg):
    return {i["name"] for i in (dbg.json("gui icons --json") or {}).get("icons", [])}


def menu_apps(dbg):
    """The Start menu's apps, {label: folder}, reported whether it is open
    or not -- and asked CLOSED: a reload waits while the menu is open."""
    return dbg.menu_apps()


def taskmgr_entries(dbg):
    """How many desktop entries Task Manager read to name what runs: its
    `taskmgr: names from N desktop entries` line at start. None if absent."""
    dbg.logs("taskmgr:", clear=True)
    dbg.spawn("/bin/wm/system/taskmgr", "Task Manager")
    line = poll(lambda: next((ln for ln in dbg.logs("taskmgr:", clear=False)
                              if "desktop entries" in ln), None), 15, 0.3)
    dbg.close_window("Task Manager")
    m = re.search(r"names from (\d+) desktop entries", line or "")
    return int(m.group(1)) if m else None


def open_menu(dbg, want=True, tries=6):
    """Super, both edges (the gesture is on the release), until the WM
    says the menu is in the state asked -- start_menu_test.py's way."""
    for _ in range(tries):
        if bool(dbg.menu().get("open")) == want:
            return True
        dbg.key(KEY_SUPER)
        dbg.key(KEY_SUPER, mods="up")
        dbg.settle()
    return bool(dbg.menu().get("open")) == want


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "gpl_switch_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("the GPL switch: GPL-licensed apps out of the Start menu and off the desktop")
    try:
        dbg.send("sh config set software.show_gpl on")
        dbg.send(f"sh cp /usr/wm/applications/doom.desktop {LAUNCHER}")
        on = poll(lambda: (lambda m: m if "DOOM" in m else None)(menu_apps(dbg)), 15, 0.5) or {}
        res.check("with the switch on, the Start menu lists DOOM and the Web Browser",
                  "DOOM" in on and "Web Browser" in on, str(sorted(on)))
        res.check("...and DOOM's launcher is on the desktop",
                  poll(lambda: "DOOM" in icons(dbg) or None, 15, 0.5), str(sorted(icons(dbg))))

        out = dbg.send("sh config set software.show_gpl off")
        off = poll(lambda: (lambda m: m if m and "DOOM" not in m else None)(menu_apps(dbg)), 15, 0.5) or {}
        res.check("turned off, DOOM leaves the Start menu", off and "DOOM" not in off,
                  f"{out.strip()[-80:]} | {sorted(menu_apps(dbg))}")
        res.check("...the Web Browser (MIT) stays", "Web Browser" in off, str(sorted(off)))
        res.check("...and DOOM leaves the desktop",
                  poll(lambda: "DOOM" not in icons(dbg) or None, 15, 0.5), str(sorted(icons(dbg))))

        # The rows that remain launch what they say.
        games = [a for a, cat in off.items() if cat == off.get("Minesweeper")]
        res.check("the Games folder is Minesweeper alone", games == ["Minesweeper"], str(games))
        if dbg.window("Minesweeper"):
            dbg.close_window("Minesweeper")
        open_menu(dbg)
        try:
            x, y = dbg.menu_app_row("Minesweeper")
            dbg.click(x, y)
            res.check("...and its row opens Minesweeper", dbg.wait_window("Minesweeper", 20) is not None)
        except KeyError as e:
            res.check("...and its row opens Minesweeper", False, str(e))
        open_menu(dbg, False)
        if dbg.window("Minesweeper"):
            dbg.close_window("Minesweeper")
        res.check("DOOM's files are still there", "doom" in dbg.send("sh ls /bin/wm/apps").split())
        n_off = taskmgr_entries(dbg)
        dbg.send("sh config set software.show_gpl on")
        n_on = taskmgr_entries(dbg)
        dbg.send("sh config set software.show_gpl off")
        res.check("...and Task Manager, naming running programs, reads every entry whatever the switch",
                  n_on and n_off == n_on, f"off {n_off}, on {n_on}")

        dbg.send("sh config set software.show_gpl on")
        back = poll(lambda: (lambda m: m if "DOOM" in m else None)(menu_apps(dbg)), 15, 0.5) or {}
        res.check("turned back on, DOOM returns to the Start menu", "DOOM" in back, str(sorted(back)))
        res.check("...and to the desktop", poll(lambda: "DOOM" in icons(dbg) or None, 15, 0.5))
    finally:
        dbg.send("sh config set software.show_gpl on")
        dbg.send(f"sh rm {LAUNCHER}")
        dbg.close()
    return res.finish("gpl_switch_test")


if __name__ == "__main__":
    sys.exit(main())
