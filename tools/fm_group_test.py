#!/usr/bin/env python3
"""The File Manager's View > Group by: the details view in bands under
captions, asserted from the captions the app reports in its layout.

1. **Group by type** (from /etc/files.conf, as a saved choice comes back)
   bands the fixture as Folders, Pictures, Music and sounds, Documents --
   in that order, the order a person reads them in.
2. **`..` is left out while grouped**: the rows are the fixture's alone.
3. **The View menu's Group by > Size** puts the empty file under Empty
   and the folder under Folders.
4. **Group by > Name** regroups by first letter, and the choice is
   written to /etc/files.conf.
5. **Group by > None** takes the captions away and brings `..` back -- the
   negative control: a view that drew captions regardless would fail here.

Shares filemanager_test.py's layout reader; the fixture is /grp.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/grp"
# name -> what makes it: a folder, a picture, a sound, two texts (one empty).
FILES = ["Albums/", "swatches.gif", "first-boot.mid", "notes.txt", "9lives.txt"]
GROUP_ROW = fm.menu_rows("group_items")
VIEW_GROUP_ROW = fm.menu_rows("view_items")["Group by"]


def setup(dbg):
    dbg.send(f"sh rm -r {ROOT}")
    dbg.send(f"sh mkdir {ROOT}")
    dbg.send(f"sh mkdir {ROOT}/Albums")
    dbg.send(f"sh cp /usr/share/pictures/swatches.gif {ROOT}/swatches.gif")
    dbg.send(f"sh cp /usr/share/music/first-boot.mid {ROOT}/first-boot.mid")
    dbg.write_lines(f"{ROOT}/notes.txt", ["a few words"])
    dbg.send(f"sh touch {ROOT}/9lives.txt")


def settled_click(dbg, qmp, x, y):
    """Warp, let the hover land, then click: a click on the very event
    after a warp can reach the bar before the pointer does, and the menu
    never opens (filemanager_test's drop-downs are red the same way)."""
    dbg.warp_cursor(qmp, x, y)
    time.sleep(0.4)
    qmp.click()


def pick_group(dbg, qmp, win, lay, name, res):
    """View > Group by > `name`, each step clicked where the app reports it."""
    r = fm.btn_rect(lay, "view") if lay else None
    if not r:
        res.check(f"Group by > {name}: the View button is reported", False, f"lay={lay}")
        return
    # ONE RETRY of the click that opens View: under a loaded guest the
    # first can land while the window is still settling (the menu never
    # opens, and nothing else is wrong -- it opens on the second).
    for _ in range(2):
        settled_click(dbg, qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
        pop = fm.wait_layout(dbg, win, lambda l: VIEW_GROUP_ROW in l.popitems, timeout=6.0)
        if pop:
            break
    if not pop:
        res.check(f"Group by > {name}: View opens", False, f"popup={fm._popup_state(dbg, win)}")
        return
    x, y, w, h = pop.popitems[VIEW_GROUP_ROW]
    sub = None
    for _ in range(2):
        settled_click(dbg, qmp, pop.ox + x + w // 2, pop.oy + y + h // 2)
        sub = fm.wait_layout(dbg, win, lambda l: GROUP_ROW[name] in l.popitems1, timeout=6.0)
        if sub:
            break
    res.check(f"Group by opens its submenu ({name})", sub is not None,
              f"level1={sub and sorted(sub.popitems1)}")
    if sub:
        x, y, w, h = sub.popitems1[GROUP_ROW[name]]
        settled_click(dbg, qmp, sub.ox + x + w // 2, sub.oy + y + h // 2)


def groups_are(want):
    return lambda l: l.dir.get(0) == ROOT and l.groups is not None and l.groups[1] == want


def run(dbg, qmp, res):
    setup(dbg)
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details",
                                    "details_pane=0", "group=type"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return

    # 1.
    want = ["Folders", "Pictures", "Music and sounds", "Documents"]
    lay = fm.wait_layout(dbg, win, groups_are(want)) or fm.last_layout()
    res.check("group=type bands the folder as Folders, Pictures, Music and sounds, Documents",
              lay is not None and lay.groups is not None and lay.groups[1] == want,
              f"groups={lay and lay.groups}")
    # 2.
    res.check("'..' is left out while grouped",
              lay is not None and lay.rows.get(0) == len(FILES),
              f"rows={lay and lay.rows.get(0)} want {len(FILES)}")

    # 3. Size, through the menu: the first click in a fresh window waits
    # for the window to settle, as a person's would.
    time.sleep(1.0)
    pick_group(dbg, qmp, win, lay, "Size", res)
    want = ["Folders", "Empty", "Tiny (under 16 KB)"]
    lay = fm.wait_layout(dbg, win, groups_are(want)) or fm.last_layout()
    res.check("Group by > Size puts the folder under Folders and the empty file under Empty",
              lay is not None and lay.groups is not None and lay.groups[1] == want,
              f"groups={lay and lay.groups}")
    if not lay:
        return

    # 4. View > Group by > Name, through the menu.
    pick_group(dbg, qmp, win, lay, "Name", res)
    want = ["0 - 9", "A - H", "I - P", "Q - Z"]
    lay = fm.wait_layout(dbg, win, groups_are(want)) or fm.last_layout()
    res.check("Group by > Name bands by first letter",
              lay is not None and lay.groups is not None and lay.groups[1] == want,
              f"groups={lay and lay.groups}")
    conf = dbg.send(f"sh cat {fm.FILES_CONF}") or ""
    res.check("...and the choice is saved", "group=name" in conf, repr(conf))

    # 5. None: the control.
    pick_group(dbg, qmp, win, lay, "None", res)
    lay = fm.wait_layout(dbg, win, groups_are([])) or fm.last_layout()
    res.check("Group by > None takes every caption away",
              lay is not None and lay.groups is not None and lay.groups[1] == [],
              f"groups={lay and lay.groups}")
    res.check("...and '..' comes back",
              lay is not None and lay.rows.get(0) == len(FILES) + 1,
              f"rows={lay and lay.rows.get(0)}")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_group_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_group_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
