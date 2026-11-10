#!/usr/bin/env python3
"""A file name longer than 63 bytes in the File Manager (SYS_GETDENTS,
base/ulistdir.h): the view lists it and opens it by its WHOLE name.

1. **`ls` prints the 209-byte name** -- not an independent witness: ls
   lists through base/ulistdir.h too, so a listing that cut names fails
   here first.
2. **The File Manager opens it by that name**: End selects its row (the
   first is ".."), Enter opens it, and the app's own `files: open <path>` line carries
   the full path. A listing that cut names at 63 bytes (the old
   struct sys_dirent) would open a path that does not exist.

The name is built inside the guest by dash from short pieces: the debug
console refuses a command line that long.
"""

RUNNER = {"name": "fmlongname", "covers": "a 209-byte file name listed and opened whole by the File Manager"}

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/lng"
NAME = "Long_" + "abcdefghij" * 20 + ".txt"   # 209 bytes
K_END, K_ENTER = "0xf787", "0x0d"             # kernel/include/api/keyboard.h


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh(f"rm -r {ROOT}")
    dbg.write_lines("/var/tmp/fmlong.sh", [
        "a=abcdefghij", "b=$a$a$a$a$a",
        # touch, not `echo >`: write_lines() writes each line with a
        # redirection of its own, which a `>` in the line would break.
        f"mkdir {ROOT}", f"touch {ROOT}/Long_$b$b$b$b.txt"])
    sh("sh /var/tmp/fmlong.sh")
    listed, end = "", time.time() + 10
    while NAME not in listed and time.time() < end:
        listed = sh(f"ls {ROOT}")
        if NAME not in listed:
            time.sleep(0.3)  # sleep-ok: polls ls for the file it asserts
    res.check("ls prints the 209-byte name whole", NAME in listed, listed[-120:])

    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the folder", win is not None, "no window")
    if not win:
        return
    dbg.logs("files:", clear=True)
    dbg.key(K_END)     # the last row: the first is ".."
    dbg.key(K_ENTER)
    want = f"files: open {ROOT}/{NAME}"
    seen = []
    end = time.time() + 15
    while time.time() < end:
        seen += dbg.logs("files: open", clear=True)
        if any(want in line for line in seen):
            break
        time.sleep(0.3)  # sleep-ok: polls the app's log for the line it asserts
    res.check("Enter opens it by its whole name", any(want in line for line in seen),
              f"saw {[line[:100] for line in seen]}")
    sh(f"rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_longname_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_longname_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
