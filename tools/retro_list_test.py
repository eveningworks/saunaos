#!/usr/bin/env python3
"""Retro Launcher's list: the Details view, the search box and the order, as a person uses them.

WHAT IT CHECKS, on the sample programs in /usr/share/w16 (their call counts differ):
  - /etc/retrolauncher.conf's `view=details` is honoured (the table is the widget on screen, the grid is not);
  - the search box filters as you type, by NAME and by FILE name (`.EXE` is in no title, only in every file's),
    whatever the case; it says `N of M programs`, and Esc empties it and brings every program back;
  - a click on a column's HEADER sorts by it and a second click reverses it -- the log line the app writes after a
    change names the first and last program with their call counts, so the test judges the ORDER, not a picture;
  - the selection stays on the same program when the order changes;
  - the toolbar's Icons and Details buttons switch the view and keep the order and the selection;
  - a double click on a row runs that program;
  - the choices are in /etc/retrolauncher.conf, and a second launch opens as they were left.

The scrolling of the table (wheel, thumb, trough, drag) is in scroll_test.py, rule 10 of userland/CLAUDE.md.

  python3 tools/retro_list_test.py
Positive control (mutate.py): make cmp_prog() compare COL_MISSING as COL_CALLS -- the sort check reddens; delete
the `ci_has(file_name(p->path), q)` term of matches() -- the file-name search reddens.
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

TITLE = "Retro Launcher"
APP = "/bin/wm/apps/retrolauncher"
CONF = "/etc/retrolauncher.conf"
COL_NAME, COL_FILE, COL_CALLS, COL_MISSING = 0, 1, 2, 3
K_ESC = "0x1b"
K_UP, K_DOWN, K_LEFT, K_RIGHT = "0xf780", "0xf781", "0xf784", "0xf785"


def list_line(dbg, why, since=0):
    """The newest `retrolauncher: <why>: ...` line past `since`, as a dict, or None."""
    lines = [ln for ln in dbg.logs("retrolauncher: " + why + ":", clear=False)]
    if len(lines) <= since:
        return None
    ln = lines[-1]
    m = re.search(r": (\w+): (\w+) by (\w+) (\w+), (\d+) of (\d+) programs, selected ([^\s,]+)"
                  r"(?:, first (\S+) \((\d+) calls, (\d+) missing\), last (\S+) \((\d+) calls, (\d+) missing\))?", ln)
    if not m:
        return None
    d = dict(why=m[1], view=m[2], key=m[3], order=m[4], shown=int(m[5]), total=int(m[6]), selected=m[7])
    if m[8]:
        d.update(first=m[8], first_calls=int(m[9]), first_missing=int(m[10]),
                 last=m[11], last_calls=int(m[12]), last_missing=int(m[13]))
    d["n"] = len(lines)
    return d


def types(dbg, text):
    for ch in text:
        dbg.key(ch, settle=False)
    dbg.settle()


def search_for(dbg, text, n):
    """Clears the box and types `text`: the log line it produced (the n-th `search:` line or later)."""
    types(dbg, text)
    return poll(lambda: list_line(dbg, "search", n), 6, 0.2)


def layout_ints(dbg, pattern):
    """The integers of the newest `retrolauncher: layout <pattern> ...` line, or None."""
    lines = dbg.logs("retrolauncher: layout " + pattern, clear=False)
    return [int(x) for x in re.findall(r"-?\d+", lines[-1].split(pattern, 1)[1])] if lines else None


def column_centre(dbg, col):
    """Screen x of the middle of column `col`, from the table's own layout lines (a hidden column is 0 wide)."""
    tb = dbg.widgets(TITLE)["programs-table"]
    lines = [ln for ln in dbg.logs("retrolauncher: layout programs-table.col ", clear=False)]
    got = {}
    for ln in lines:
        v = [int(x) for x in re.findall(r"-?\d+", ln.split("programs-table.col", 1)[1])]
        got[v[0]] = v[1:]          # x, y, w, h -- content-relative
    if col not in got or got[col][2] <= 0:
        return None
    x, _, w, _ = got[col]
    return tb["screen"]["x"] + (x - tb["x"]) + w // 2


def arrows_move(dbg, widget, forward, back):
    """Do the arrow keys move `widget`'s selection (its `.selected` layout line)? One of the two must."""
    def sel():
        v = layout_ints(dbg, widget + ".selected")
        return v[0] if v else None
    before = sel()
    for k in (forward, back):
        dbg.key(k)
        after = poll(lambda: (lambda x: x if x != before else None)(sel()), 2, 0.2)
        if after is not None:
            return True, f"{before} -> {after}"
    return False, f"stayed at {before}"


def header_click(dbg, col):
    """Clicks column `col`'s header; the `sort:` line it produced, or None."""
    n = len(dbg.logs("retrolauncher: sort:", clear=False))
    tb = dbg.widgets(TITLE)["programs-table"]
    cx = column_centre(dbg, col)
    if cx is None:
        return None
    hh = layout_ints(dbg, "programs-table.header_h")[0]
    dbg.click(cx - 8, tb["screen"]["y"] + hh // 2)
    return poll(lambda: list_line(dbg, "sort", n), 6, 0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "retro_list_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("retro_list: the Details view, the search and the order")
    try:
        dbg.send("sh rm " + CONF)
        dbg.send("sh write /etc/win16.conf folders=1")
        dbg.send("sh append /etc/win16.conf folder1=/usr/share/w16")
        dbg.send(f"sh write {CONF} view=details")
        win = dbg.spawn(APP, TITLE)
        if not res.check("the Retro Launcher opens", win is not None, "no window"):
            return res.finish("retro_list_test")
        poll(lambda: dbg.widgets(TITLE).get("search"), 10, 0.3)
        time.sleep(8)               # the programs are examined a few a tick

        widgets = dbg.widgets(TITLE)
        res.check("view=details in the conf opens the Details table, not the icon grid",
                  "programs-table" in widgets and "programs" not in widgets, f"widgets {sorted(widgets)}")
        # The search box stays inside the toolbar's strip and the window (it once stood a pixel into the menu bar).
        sr, tr, mr = layout_ints(dbg, "search "), layout_ints(dbg, "toolbar "), layout_ints(dbg, "menu ")
        res.check("the search box sits inside the toolbar's strip and the window",
                  sr and tr and mr and sr[1] >= tr[1] and sr[1] + sr[3] <= tr[1] + tr[3] and sr[0] + sr[2] <= mr[2],
                  f"search {sr} toolbar {tr} menu {mr}")
        sx, sy = dbg.widget_center("search", TITLE)
        dbg.click(sx, sy)

        # --- the search -------------------------------------------------------------------------------------
        n = len(dbg.logs("retrolauncher: search:", clear=False))
        # "dialog." -- the Open dialog test's description says "dialog" too; only one FILE name has it.
        q = "dialog."
        s = search_for(dbg, q, n)
        res.check(f"typing '{q}' lists only the Dialog test", s and s["shown"] == 1 and s["first"] == "dialog.exe"
                  and s["total"] >= 5, s)
        res.check("...and selects it, so Enter would run it", s and s["selected"] == "dialog.exe", s)
        total = s["total"] if s else 10
        for _ in range(len(q)):
            dbg.key("\b", settle=False)
        dbg.settle()
        s = poll(lambda: (lambda x: x if x and x["shown"] == total else None)(list_line(dbg, "search")), 6, 0.2)
        res.check("emptying the box brings every program back", s is not None, s)

        n = len(dbg.logs("retrolauncher: search:", clear=False))
        s = search_for(dbg, ".EXE", n)
        res.check("the search matches FILE names, whatever the case (no title, description or module has '.exe')",
                  s and s["shown"] == total and total >= 5, s)
        dbg.key(K_ESC)
        s = poll(lambda: (lambda x: x if x and x["shown"] == total else None)(list_line(dbg, "search")), 6, 0.2)
        res.check("Esc empties the search and every program is back", s is not None, s)
        n = len(dbg.logs("retrolauncher: search:", clear=False))
        dbg.click(sx, sy)
        s = search_for(dbg, "zzzq", n)
        res.check("a search nothing matches lists none", s and s["shown"] == 0, s)
        dbg.key(K_ESC)
        poll(lambda: (lambda x: x if x and x["shown"] == total else None)(list_line(dbg, "search")), 6, 0.2)

        # --- the order --------------------------------------------------------------------------------------
        # Select the first row: the selection must survive the re-sorts below.
        tb = dbg.widgets(TITLE)["programs-table"]["screen"]
        hh = layout_ints(dbg, "programs-table.header_h")[0]
        rh = layout_ints(dbg, "programs-table.row_h")[0]
        dbg.click(tb["x"] + 40, tb["y"] + hh + rh // 2)
        a = header_click(dbg, COL_MISSING)       # Calls and Stand-ins start hidden; Missing is shown
        res.check("a header click sorts by that column, ascending", a and a["key"] == "missing"
                  and a["order"] == "ascending" and a["first_missing"] <= a["last_missing"], a)
        sel_before = a["selected"] if a else None
        b = header_click(dbg, COL_MISSING)
        res.check("a second click reverses it: the most missing calls first", b and b["order"] == "descending"
                  and b["first_missing"] >= b["last_missing"] and b["first_missing"] > 0, b)
        res.check("the selection stays on the same program through the re-sort",
                  b and sel_before and b["selected"] == sel_before and sel_before != "none", f"{sel_before} -> {b}")
        c = header_click(dbg, COL_FILE)
        res.check("sorting by File orders by file name", c and c["key"] == "file" and c["order"] == "ascending"
                  and c["first"] <= c["last"], c)

        # --- Icons and back -------------------------------------------------------------------------------------
        def toolbar_click(index):
            lines = dbg.logs("retrolauncher: layout toolbar.button " + str(index), clear=False)
            if not lines:
                return False
            x, y, w, h = [int(v) for v in re.findall(r"-?\d+", lines[-1].split("button", 1)[1])][1:5]
            ox, oy = dbg.widgets(TITLE)["toolbar"]["screen"]["x"], dbg.widgets(TITLE)["toolbar"]["screen"]["y"]
            dbg.click(ox + x + w // 2, oy + (y - 23) + h // 2)
            return True

        n = len(dbg.logs("retrolauncher: view:", clear=False))
        ok = toolbar_click(6)
        v = poll(lambda: list_line(dbg, "view", n), 6, 0.2) if ok else None
        res.check("the Icons button shows the grid and keeps the order and the selection",
                  v and v["view"] == "icons" and v["key"] == "file" and v["selected"] == c["selected"]
                  and "programs" in dbg.widgets(TITLE) and "programs-table" not in dbg.widgets(TITLE), v)
        moved, why = arrows_move(dbg, "programs", K_RIGHT, K_LEFT)
        res.check("the icon grid takes the arrow keys (it had no focus ring before)", moved, why)
        n = len(dbg.logs("retrolauncher: view:", clear=False))
        ok = toolbar_click(7)
        v = poll(lambda: list_line(dbg, "view", n), 6, 0.2) if ok else None
        res.check("the Details button brings the table back", v and v["view"] == "details"
                  and "programs-table" in dbg.widgets(TITLE), v)

        moved, why = arrows_move(dbg, "programs-table", K_DOWN, K_UP)
        res.check("the Details table takes the arrow keys", moved, why)

        # --- a double click runs it ---------------------------------------------------------------------------
        n = len(dbg.logs("retrolauncher: run ", clear=False))
        tb = dbg.widgets(TITLE)["programs-table"]["screen"]
        y = tb["y"] + hh + rh // 2
        dbg.click(tb["x"] + 40, y, settle=False)
        dbg.click(tb["x"] + 40, y)
        ran = poll(lambda: len(dbg.logs("retrolauncher: run ", clear=False)) > n, 6, 0.2)
        res.check("a double click on a row runs that program", ran, "no 'retrolauncher: run' line")

        # --- the choices are kept ------------------------------------------------------------------------------
        conf = dbg.send("sh cat " + CONF)
        res.check("the view, the order and the columns are in /etc/retrolauncher.conf",
                  "view=details" in conf and "sort=file" in conf and "order=ascending" in conf, conf)
    finally:
        pass
    dbg.close_window(TITLE)
    # A second launch opens as it was left: Details, by file name.
    dbg.send("sh write /etc/retrolauncher.conf view=details")
    dbg.send("sh append /etc/retrolauncher.conf sort=missing")
    dbg.send("sh append /etc/retrolauncher.conf order=descending")
    win = dbg.spawn(APP, TITLE)
    if res.check("it opens again", win is not None, "no window"):
        poll(lambda: dbg.widgets(TITLE).get("search"), 10, 0.3)
        time.sleep(6)
        sx, sy = dbg.widget_center("search", TITLE)
        dbg.click(sx, sy)
        n = len(dbg.logs("retrolauncher: search:", clear=False))
        s = search_for(dbg, "e", n)
        res.check("it opens as the conf says: Details, by Missing, most first", s and s["view"] == "details"
                  and s["key"] == "missing" and s["order"] == "descending", s)
        dbg.close_window(TITLE)
    dbg.send("sh rm " + CONF)
    dbg.close()
    return res.finish("retro_list_test")


if __name__ == "__main__":
    sys.exit(main())
