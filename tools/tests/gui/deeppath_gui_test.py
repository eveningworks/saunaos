#!/usr/bin/env python3
"""A path past 255 bytes on the desktop and in the File Manager
(fixed-tables round 4, track A; the shell and ls half is
userland/tests/deeppath_test.c).

1. **The desktop acts on the whole path.** A file whose name makes its
   desktop path 264 bytes: Delete moves it to the Recycle Bin and `trash
   restore` puts it back (round 4 B: the bin held 256-byte paths), then
   Shift+Delete -> Delete removes it (it leaves the folder and the icon
   goes). The desktop's old 80-byte path budget cut it, so `rm` was
   handed a path that did not exist and it stayed.
2. **The File Manager walks down to a 340-byte folder and opens a file
   there** by its whole path (`files: open <path>`), from a short folder,
   one level at a time -- as a person would. Its old 256-byte budget
   refused the folder.

Paths are built inside the guest by dash from short pieces: the debug
console refuses a command line that long.
"""

RUNNER = {"name": "deeppath", "covers": "a path past 255 bytes acted on whole by the desktop and the File Manager"}

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../sweep"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import filemanager_test as fm                          # noqa: E402
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results, poll  # noqa: E402

DESK = "/home/user/Desktop"
DESK_NAME = "Deep_" + "abcdefghij" * 24              # 245 bytes
ROOT = "/dpg"
PART = "deep_0123456789abcdefghijklmnopqrstuvw"     # + "_N", 40 bytes a level
LEVELS = 8
DEEP = ROOT + "".join(f"/{PART}_{i}" for i in range(LEVELS))
K_DELETE, K_END, K_ENTER = "0xf788", "0xf787", "0x0d"   # kernel/include/api/keyboard.h


def icons(dbg):
    return (dbg.json("gui icons --json") or {}).get("icons", [])


def icon(dbg, name):
    return next((i for i in icons(dbg) if i["name"] == name), None)


def desk_listing(dbg):
    return dbg.send(f"sh ls {DESK}") or ""


def check_desktop(dbg, res):
    path_len = len(f"{DESK}/{DESK_NAME}")
    dbg.write_lines("/var/tmp/deepdesk.sh", [
        "b=abcdefghij", "c=$b$b$b$b$b$b",
        f"touch {DESK}/Deep_$c$c$c$c"])
    dbg.send("sh sh /var/tmp/deepdesk.sh")
    ic = poll(lambda: icon(dbg, DESK_NAME), 30)
    res.check(f"the {path_len}-byte desktop path is an icon", ic is not None, "no icon")
    if not ic:
        return
    dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
    dbg.settle()
    # Delete: into the Recycle Bin by its whole path (base/utrash.h held
    # 256 bytes and refused it), asked first when Folder Options say so.
    dbg.key(K_DELETE)
    dbg.settle()
    btns = (dbg.json("gui dialog --json") or {}).get("buttons", [])
    if btns:
        dbg.send(f"gui click {btns[0]['cx']} {btns[0]['cy']}")
        dbg.settle()
    binned = poll(lambda: DESK_NAME not in desk_listing(dbg), 30)
    res.check("Delete moves it to the Recycle Bin", bool(binned), "still in the folder")
    if not binned:
        return
    # ...and `trash restore` puts it back, named by where it was deleted from.
    dbg.write_lines("/var/tmp/deepdesk.sh", [
        "b=abcdefghij", "c=$b$b$b$b$b$b",
        f"trash restore {DESK}/Deep_$c$c$c$c"])
    out = dbg.as_user("sh /var/tmp/deepdesk.sh") or ""
    ic = poll(lambda: icon(dbg, DESK_NAME), 30)
    res.check("`trash restore` brings it back by its whole path", ic is not None, out.strip()[-200:])
    if not ic:
        return
    dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
    dbg.settle()
    # Shift+Delete: `/bin/rm` with the path the desktop built.
    dbg.key(K_DELETE, mods="shift")
    dbg.settle()
    btns = (dbg.json("gui dialog --json") or {}).get("buttons", [])
    res.check("Shift+Delete asks first", len(btns) == 2 and btns[0].get("label") == "Delete",
              str([b.get("label") for b in btns]))
    if len(btns) != 2:
        return
    dbg.send(f"gui click {btns[0]['cx']} {btns[0]['cy']}")
    dbg.settle()
    gone = poll(lambda: DESK_NAME not in desk_listing(dbg), 30)
    res.check("it is deleted by its whole path", bool(gone), "still in the folder")
    res.check("...and its icon goes", poll(lambda: icon(dbg, DESK_NAME) is None, 15) is True,
              "icon still shown")
    if not gone:
        dbg.write_lines("/var/tmp/deepdesk.sh", [
            "b=abcdefghij", "c=$b$b$b$b$b$b",
            f"rm -f {DESK}/Deep_$c$c$c$c"])
        dbg.send("sh sh /var/tmp/deepdesk.sh")


def check_files(dbg, res):
    dbg.send(f"sh rm -r {ROOT}")
    lines = [f"p={PART}", f"d={ROOT}", "mkdir $d"]
    for i in range(LEVELS):
        lines += [f"d=$d/${{p}}_{i}", "mkdir $d"]
    lines.append("touch $d/inside.txt")
    dbg.write_lines("/var/tmp/deepfm.sh", lines)
    dbg.send("sh sh /var/tmp/deepfm.sh")
    res.check(f"the {len(DEEP)}-byte folder exists",
              poll(lambda: "inside.txt" in (dbg.send(f"sh ls -R {ROOT}") or ""), 15), "not made")

    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the short folder", win is not None, "no window")
    if not win:
        return
    # Down one level at a time: End is the last row (the first is ".."),
    # and each folder holds one entry.
    for _ in range(LEVELS):
        dbg.key(K_END)
        dbg.key(K_ENTER)
        dbg.settle()
    dbg.logs("files:", clear=True)
    dbg.key(K_END)
    dbg.key(K_ENTER)
    want = f"files: open {DEEP}/inside.txt"
    seen = []

    def opened():
        seen.extend(dbg.logs("files: open", clear=True))
        return any(want in line for line in seen)
    res.check(f"it opens {len(want) - 12} bytes of path whole", poll(opened, 15),
              f"saw {[line[:120] for line in seen]}")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--only", choices=["desktop", "files"], default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "deeppath_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        if args.only != "files":
            check_desktop(dbg, res)
        if args.only != "desktop":
            check_files(dbg, res)
    finally:
        dbg.close()
    print(f"\ndeeppath_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
