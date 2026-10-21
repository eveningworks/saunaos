#!/usr/bin/env python3
"""The File Manager lists a folder of any size: 600 files, not the first 256.

WHAT IT CHECKS, and what a broken version would still pass:

  - a folder of 600 files (`mkfiles`, from inside the guest) shows 600 rows -- the listing is
    paged (lib/ulistdir.h) and the view grows to it. A view with a fixed array shows 256 and
    says "(more)", which is how a copied Windows folder once looked as if it had lost most
    of its files; the rows count is the app's own report, not a count of pixels;
  - the LAST file can be chosen and MARKED -- the mark bitmap grew with the listing (a bitmap
    sized for 256 rows would put the mark past its end);
  - the status line counts every file ("600 items").

Positive control (mutate.py): capping the listing at 256 in uui_fileview.c's reload reddens the
row count and the status line (End then lands on row 256, which marks fine -- the mark check is
for a bitmap that did not grow, a different break).
"""
import argparse
import sys

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                      # noqa: E402

N = 600
DIR = "/home/big"
END = "0xf787"        # KEY_END (api/keyboard.h): the row after Home's 0xf786


def run(dbg, res):
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
    sh(f"rm -r {DIR}")
    dbg.close_window(fm.TITLE)


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
        run(dbg, res)
    finally:
        dbg.close()
    return res.finish("fm_bigdir_test")


if __name__ == "__main__":
    sys.exit(main())
