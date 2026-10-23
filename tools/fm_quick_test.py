#!/usr/bin/env python3
"""Quick access, the File Manager's start page (fm_quick.c): the
"quick:/" virtual folder in three bands, asserted from the app's own
layout report and checked against what opening a row does.

1. **Places leads with Quick access** on a fresh image (a default pin).
2. **Clicking it shows the bands** Pinned folders and Drives, in that
   order -- the captions the app reports.
3. **A file opened elsewhere appears under Recent files**: `open` on a
   .txt records it (lib/urecent.h), and a Refresh brings the band in.
4. **Opening the first row goes where it points**: Down then Enter on
   Pinned folders' first row leaves Quick access for a real folder.
5. **Nothing is renamed here**: F2 on a row starts no rename -- the
   page's rows are other folders' things.

Shares filemanager_test.py's layout reader; the fixture is /qa.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/qa"
QUICK = "quick:/"
K_CTRL_R = "0x12"


def caps(l):
    return l.groups[1] if l is not None and l.groups is not None else None


def run(dbg, qmp, res):
    dbg.send(f"sh rm -r {ROOT}")
    dbg.send(f"sh mkdir {ROOT}")
    dbg.write_lines(f"{ROOT}/hello.txt", ["hello from Quick access"])
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return

    # 1.
    lay = fm.wait_layout(dbg, win, lambda l: QUICK in l.treerow.values()) or fm.last_layout()
    row = next((r for r, p in sorted(lay.treerow.items()) if p == QUICK), None) if lay else None
    res.check("Places leads with Quick access", row == 0,
              f"treerows={lay and sorted(lay.treerow.items())[:4]}")
    if row is None:
        return

    # 2.
    tx, ty, tw, th, trh, _ = lay.treebox
    time.sleep(0.5)
    fm.sure_click(dbg, qmp, lay.ox + tx + tw // 3, lay.oy + ty + row * trh + trh // 2)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == QUICK and caps(l)) or fm.last_layout()
    got = caps(lay)
    res.check("clicking it shows Pinned folders, then Drives",
              lay is not None and lay.dir.get(0) == QUICK and got is not None
              and got[:1] == ["Pinned folders"] and got[-1:] == ["Drives"],
              f"dir={lay and lay.dir.get(0)} groups={got}")

    # 3.
    dbg.send(f"sh open {ROOT}/hello.txt")
    time.sleep(3)
    dbg.key(K_CTRL_R, mods="ctrl")
    want = ["Pinned folders", "Recent files", "Drives"]
    lay = fm.wait_layout(dbg, win, lambda l: caps(l) == want, timeout=20) or fm.last_layout()
    res.check("a file opened elsewhere brings the Recent files band",
              caps(lay) == want, f"groups={caps(lay)}")

    # 5. (before 4, which leaves the page)
    px, py, pw, ph = lay.pane[0]
    fm.sure_click(dbg, qmp, lay.ox + px + pw // 2, lay.oy + py + ph - 8)   # empty space: focus, no row
    dbg.key(fm.K_DOWN)
    lay = fm.wait_layout(dbg, win, lambda l: l.selected not in (None, "-")) or fm.last_layout()
    first = lay.selected if lay else None
    dbg.key(fm.K_F2)
    time.sleep(1)
    lay = fm.layout_now(dbg, win) or fm.last_layout()
    res.check("F2 starts no rename on Quick access",
              lay is not None and (lay.renaming is None or lay.renaming[0] == 0),
              f"renaming={lay and lay.renaming} selected={first}")

    # 4.
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) not in (None, QUICK)) or fm.last_layout()
    res.check("Enter on the first pinned folder goes there",
              lay is not None and (lay.dir.get(0) or "").startswith("/"),
              f"selected={first} dir={lay and lay.dir.get(0)}")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_quick_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_quick_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
