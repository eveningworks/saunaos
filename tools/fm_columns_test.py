#!/usr/bin/env python3
"""The File Manager's details columns, sized, chosen and saved -- uui_table's
column layout reached through uui_fileview, asserted from the layout the
pane itself reports (`left.col N`, `left.columns`).

1. **A folder's header is choosable**: the pane reports a column layout.
2. **Dragging the divider after Name resizes it**: Name narrows by about
   the drag, Size starts where Name now ends, and the width is in the
   layout.
3. **The layout is saved** in /etc/files.conf (`left_cols`) once the drag
   is done.
4. **A right-click on the header is the checklist, not the file menu**:
   Down, Enter hides Size (Name is locked, greyed and skipped).
5. **A virtual folder keeps its own columns** -- Recent reports no
   choosable layout -- and **coming back finds the folder's layout as it
   was**: Size still hidden, Name still narrowed. The control: a layout
   carried across by index would show Recent's columns hidden and sized.

Shares filemanager_test.py's layout reader.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DIR = "/usr/share/pictures"
K_CTRL_L = "0x0c"


def run(dbg, qmp, res):
    def goto(path):
        """The breadcrumb's text, as a person types it: Ctrl+L, the path, Enter."""
        dbg.key(K_CTRL_L)
        time.sleep(0.3)
        for ch in path:
            dbg.key(ch)
        dbg.key(fm.K_ENTER)

    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return

    # 1.
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.lcolumns and 1 in l.lcols) \
        or fm.last_layout()
    res.check("a folder's header reports a choosable layout",
              lay is not None and (lay.lcolumns or "").startswith("Name"),
              f"layout={lay and lay.lcolumns} cols={lay and lay.lcols}")
    if not lay or 0 not in lay.lcols:
        return
    x0, y0, w0, _ = lay.lcols[0]

    # 2. The divider is Name's right edge, in the header row.
    hy = lay.oy + y0 + 8
    dx = lay.ox + x0 + w0
    time.sleep(0.5)
    dbg.warp_cursor(qmp, dx, hy)
    time.sleep(0.3)
    qmp.mouse_down()
    for step in range(1, 9):
        dbg.warp_cursor(qmp, dx - 10 * step, hy)
    qmp.mouse_up()
    lay = fm.wait_layout(dbg, win, lambda l: 0 in l.lcols and l.lcols[0][2] < w0 - 20) or fm.last_layout()
    w1 = lay.lcols.get(0, [0, 0, 0, 0])[2]
    s1 = lay.lcols.get(1, [0, 0, 0, 0])[0]
    res.check("dragging the divider after Name narrows it by about the drag",
              60 <= w0 - w1 <= 100, f"was {w0}, now {w1}")
    res.check("...Size starts where Name ends",
              abs(s1 - (lay.lcols.get(0, [0])[0] + w1)) <= 1, f"size x={s1}")
    res.check("...and the width is in the layout",
              (lay.lcolumns or "").startswith(f"Name:{w1}"), f"layout={lay.lcolumns}")

    # 3.
    conf = ""
    for _ in range(20):
        conf = dbg.send(f"sh cat {fm.FILES_CONF}") or ""
        if "left_cols=" in conf:
            break
        time.sleep(0.5)
    res.check("the layout is saved in files.conf", f"left_cols=Name:{w1}" in conf, repr(conf))

    # 4.
    dbg.warp_cursor(qmp, lay.ox + s1 + 20, hy)
    time.sleep(0.3)
    qmp.click(button="right")
    time.sleep(1.0)
    st = fm._popup_state(dbg, win)
    res.check("a right-click on the header opens a menu", st is not None and st[0] == 1, f"popup={st}")
    for k in (fm.K_DOWN, fm.K_ENTER):
        dbg.key(k)
        time.sleep(0.3)
    lay = fm.wait_layout(dbg, win, lambda l: "-Size" in (l.lcolumns or "")) or fm.last_layout()
    res.check("Down, Enter on the checklist hides Size",
              "-Size" in (lay.lcolumns or ""), f"layout={lay.lcolumns}")

    # 5.
    goto("recent:/")
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "recent:/") or fm.last_layout()
    res.check("Recent keeps its own columns: no choosable layout",
              lay is not None and lay.dir.get(0) == "recent:/" and lay.lcolumns is None,
              f"dir={lay and lay.dir.get(0)} layout={lay and lay.lcolumns}")
    goto(DIR)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.lcolumns) or fm.last_layout()
    res.check("back in the folder, its layout is as it was",
              (lay.lcolumns or "").startswith(f"Name:{w1}") and "-Size" in (lay.lcolumns or ""),
              f"layout={lay.lcolumns}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_columns_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_columns_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
