#!/usr/bin/env python3
"""The File Manager lists a folder of any size: 600 files, not the first 256.

WHAT IT CHECKS, and what a broken version would still pass:

  - a folder of 600 files (`mkfiles`, from inside the guest) shows 600 rows -- the listing is
    paged (lib/ulistdir.h) and the view grows to it. A view with a fixed array shows 256 and
    says "(more)", which is how a copied Windows folder once looked as if it had lost most
    of its files; the rows count is the app's own report, not a count of pixels;
  - the LAST file can be chosen and MARKED -- the mark bitmap grew with the listing (a bitmap
    sized for 256 rows would put the mark past its end);
  - the status line counts every file ("600 items");
  - the ICONS view wheeled to the end and the window made WIDER still draws its icons: more columns
    mean fewer rows, the range shrank under the scroll offset, and the grid sat past its end, empty,
    until a wheel step. The ink threshold sits between the broken state (~300 samples: the bar and
    borders) and the working one (~2000).

Positive control (mutate.py): capping the listing at 256 in uui_fileview.c's reload reddens the
row count and the status line (End then lands on row 256, which marks fine -- the mark check is
for a bitmap that did not grow, a different break).
"""
import argparse
import os
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                      # noqa: E402

N = 600
DIR = "/home/big"
END = "0xf787"        # KEY_END (api/keyboard.h): the row after Home's 0xf786


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh(f"mkdir {DIR}")
    out = sh(f"mkfiles {DIR} {N}")
    if not res.check(f"mkfiles made {N} files", "created" in out or str(N) in out, out.strip()[-120:]):
        return
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
    if not res.check("the File Manager opens on the folder", win is not None, "no window"):
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.rows.get(0, 0) >= 256, timeout=20)
    rows = lay.rows.get(0) if lay else None
    # one more than the files when a ".." row leads them
    res.check(f"it lists all {N} files, not the first 256", rows in (N, N + 1), f"rows {rows}")

    dbg.key(END)
    dbg.key(fm.K_INSERT)
    lay = fm.wait_layout(dbg, win, lambda l: l.marked[0] >= 1, timeout=8) or fm.last_layout()
    res.check("the last file can be marked", lay is not None and lay.marked[0] == 1,
              f"marked {lay and lay.marked}, selected {lay and lay.selected!r}")

    status = lay.status if lay else None
    res.check(f"the status line counts every file ({N} items)", bool(status) and f"{N} items" in status[0],
              f"status {status}")
    dbg.close_window(fm.TITLE)

    # The icons view wheeled to the end, then the window made WIDER: more columns, fewer rows, so the
    # range shrinks under the scroll offset, and the grid must follow it, not sit past its end drawing nothing.
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=icons", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.pane.get(0) is not None, timeout=20) if win else None
    if res.check("the icons view opens on the folder", lay is not None, "no layout"):
        # The WHEEL to the end, not End: End selects the last file and a resize then reveals the
        # selection, which hides the bug (a scroll-bar or wheel user has no selection to follow).
        c0 = win["content"]
        dbg.move(c0["x"] + 400, c0["y"] + 300)
        for _ in range(12):
            dbg.wheel(-40, settle=False)
        dbg.settle()
        before = (fm.layout_now(dbg, win) or fm.last_layout()).scroll.get(0)
        dbg.send("gui resize 1200 640")
        dbg.settle()
        time.sleep(1.0)
        win = dbg.window(fm.TITLE)
        lay = fm.layout_now(dbg, win) or fm.last_layout()
        px, py, pw, ph = lay.pane[0]
        c = win["content"]
        from PIL import Image
        qmp.screenshot(os.path.join("/tmp", f"fmbig_{os.getpid()}.png"))
        im = Image.open(os.path.join("/tmp", f"fmbig_{os.getpid()}.png")).convert("RGB")
        ink = sum(1 for y in range(c["y"] + py + 8, c["y"] + py + ph - 8, 3)
                  for x in range(c["x"] + px + 8, c["x"] + px + pw - 20, 3)
                  if sum(im.getpixel((x, y))) < 600)
        res.check("wheeled to the end, then a wider window: the icons are still drawn", ink > 1000, f"{ink} non-white samples, scroll {before} -> {lay.scroll.get(0)}")
        dbg.close_window(fm.TITLE)
    sh(f"rm -r {DIR}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_bigdir_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    return res.finish("fm_bigdir_test")


if __name__ == "__main__":
    sys.exit(main())
