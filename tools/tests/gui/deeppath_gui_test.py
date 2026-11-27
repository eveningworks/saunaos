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
3. **Notepad, Paint, Image Viewer, Audio Player and Video Player open a
   file by a 350-byte path** (track A2), named on the command line as the
   File Manager names it, and Notepad and Paint SAVE back to it. Each
   kept a 256-byte path of its own and refused the file.

Paths are built inside the guest by dash from short pieces: the debug
console refuses a command line that long.
"""

RUNNER = {"name": "deeppath", "covers": "a path past 255 bytes acted on whole by the desktop, the File Manager and five apps"}

import argparse
import os
import re
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
# The apps' folder is the desktop account's own: Notepad and Paint save there.
AROOT = "/home/user/dpa"
ADEEP = AROOT + "".join(f"/{PART}_{i}" for i in range(LEVELS))
APPS = "/bin/wm/apps"
K_DELETE, K_END, K_ENTER, K_CTRL_S = "0xf788", "0xf787", "0x0d", "0x13"   # kernel/include/api/keyboard.h


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


def run_deep(dbg, command):
    """`command` run as the desktop's account with $d the deep folder."""
    lines = [f"p={PART}", f"d={AROOT}"] + [f"d=$d/${{p}}_{i}" for i in range(LEVELS)] + [command]
    dbg.write_lines("/var/tmp/deepapp.sh", lines)
    return dbg.as_user("sh /var/tmp/deepapp.sh") or ""


def logged(dbg, prog, want, timeout=30):
    """True once `prog`'s own log holds `want`; else its last lines. AN APP
    STARTED FROM A SHELL LOGS TO THAT SHELL'S stderr, not the kernel log,
    so start() sends it to a file."""
    last = [""]

    def hit():
        last[0] = dbg.tsh(f"cat /var/tmp/deep_{prog}.log") or ""
        return want in last[0].replace("\r", "")
    if poll(hit, timeout):
        return True, ""
    lines = [ln for ln in last[0].splitlines() if " layout " not in ln]
    return False, f"saw {[ln[-160:] for ln in lines[-4:]]}"


def check_apps(dbg, res):
    dbg.as_user(f"rm -r {AROOT}")
    lines = [f"p={PART}", f"d={AROOT}", "mkdir $d"]
    for i in range(LEVELS):
        lines += [f"d=$d/${{p}}_{i}", "mkdir $d"]
    lines += ["cp /tests/web/photo.jpg $d/photo.jpg", "cp /tests/web/bands.png $d/bands.png",
              "cp /usr/share/music/first-boot.mp3 $d/song.mp3",
              "cp /usr/share/videos/orbit.avi $d/clip.avi", "echo hello > $d/note.txt"]
    dbg.write_lines("/var/tmp/deepapp.sh", lines)
    dbg.as_user("sh /var/tmp/deepapp.sh")
    listing = poll(lambda: "note.txt" in run_deep(dbg, "ls $d") and run_deep(dbg, "ls $d"), 20) or ""
    res.check(f"the {len(ADEEP)}-byte folder holds the five files",
              all(n in listing for n in ("photo.jpg", "bands.png", "song.mp3", "clip.avi", "note.txt")),
              listing[-200:])
    dbg.prompt_policy("allow")

    def start(prog, name, want):
        run_deep(dbg, f"{APPS}/{prog} $d/{name} 2>/var/tmp/deep_{prog}.log &")
        ok, why = logged(dbg, prog, want)
        res.check(f"{prog} opens {len(ADEEP) + 1 + len(name)} bytes of path whole", ok, why)
        dbg.settle()
        return ok

    # The folder is listed by its whole path, and the file named is the
    # one decoded -- a cut path listed nothing.
    if start("imgview", "photo.jpg", f"imgview: listing {ADEEP} -- 2 image(s)"):
        ok, why = logged(dbg, "imgview", "imgview: shown photo.jpg")
        res.check("...and shows the picture named", ok, why)
    # A file named to the Audio Player is selected, not played: its
    # folder listed by its whole path, with tracks in it, is the proof.
    if start("player", "song.mp3", f"player: listing {ADEEP} -- "):
        log = dbg.tsh("cat /var/tmp/deep_player.log") or ""
        m = re.search(re.escape(f"listing {ADEEP} -- ") + r"(\d+) track", log)
        res.check("...and finds the track there", bool(m) and int(m.group(1)) >= 1, log[-200:])
    start("video", "clip.avi", f"video: playing {ADEEP}/clip.avi")

    # Paint saves BACK by the whole path: Ctrl+S on a picture it opened.
    if start("paint", "bands.png", f"paint: opened {ADEEP}/bands.png"):
        dbg.key(K_CTRL_S, mods="ctrl")
        ok, why = logged(dbg, "paint", f"paint: saved {ADEEP}/bands.png as png")
        res.check("Paint saves to it by its whole path", ok, why)

    # Notepad logs no open, so the round trip is the proof: the file's
    # own text with a typed letter in front, read back from the disk.
    run_deep(dbg, f"{APPS}/notepad $d/note.txt &")
    win = poll(lambda: next((w for w in dbg.windows() if "note.txt" in w.get("title", "")), None), 30)
    res.check("Notepad opens the deep file (its title names it)", win is not None,
              str([w.get("title") for w in dbg.windows()]))
    if win:
        dbg.settle()
        dbg.key("0x5a")                    # Z, at the start of the text
        dbg.key(K_CTRL_S, mods="ctrl")
        saved = poll(lambda: "Zhello" in run_deep(dbg, "cat $d/note.txt"), 20)
        res.check("Notepad saves to it by its whole path", bool(saved),
                  run_deep(dbg, "cat $d/note.txt")[-120:])
    for title in ("Image Viewer", "Audio Player", "Video Player", "Paint", "note.txt"):
        for w in dbg.windows():
            if title in w.get("title", ""):
                dbg.close_window(w["title"])
    dbg.as_user(f"rm -r {AROOT}")
    dbg.tsh("rm -f /var/tmp/deep_*.log /var/tmp/deepapp.sh")
    # Notepad remembered the deep file; a later start must not look for it.
    dbg.as_user("rm -f /home/user/.local/state/notepad/recent /home/user/.local/state/notepad/session")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--only", choices=["desktop", "files", "apps"], default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "deeppath_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        if args.only in (None, "desktop"):
            check_desktop(dbg, res)
        if args.only in (None, "files"):
            check_files(dbg, res)
        if args.only in (None, "apps"):
            check_apps(dbg, res)
    finally:
        dbg.close()
    print(f"\ndeeppath_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
