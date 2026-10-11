#!/usr/bin/env python3
"""tools/folder_options_test.py -- the desktop follows Folder Options, and
the double-click speed is one setting.

WHAT THIS IS
------------
The File Manager's options in /etc/files.conf (lib/ufolderopts.h) apply
to the desktop too, and `system.double_click` (lib/udclick.h) is the one
double-click time every widget asks:

  1. Extensions: off, a known type's caption loses it ("notes"), a folder
     keeps its name; on, it is back. Read from `gui icons --json`, the
     names the draw loop paints.
  2. Hidden files: a dot-file is not on the desktop until `hidden=on`.
  3. Thumbnails: off, a picture is its type icon; on with a 1 MB cap, a
     video over 1 MB is not a thumbnail while a small picture is (the
     control); at 2 MB it is.
  4. Asking before a delete: off, Delete moves the file to the bin with no
     dialog; on, the dialog opens (the control).
  5. Single click: one plain click on a folder opens a window and the
     pointer over an icon is the link hand; with double click, the same
     click opens nothing (the control).
  6. Double-click speed: two presses ~450 ms apart open the folder at
     "Very slow" (700 ms) and not at "Very fast" (250 ms).
  7. System Settings' Test folder, under the slider: a double click opens
     it ("settings: dclick test opened").

    python3 tools/vm.py start && python3 tools/folder_options_test.py
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402

DESK = "/home/Desktop"
CONF = "/etc/files.conf"
CURSOR_HAND = 7   # wm_internal.h's WM_CURSOR_HAND
_res = Results()
check = _res.check


def icons(dbg):
    return {i["name"]: i for i in dbg.json("gui icons --json").get("icons", [])}


def centre(i):
    return i["x"] + i["w"] // 2, i["y"] + i["h"] // 3


def wait_for(fn, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.3)
    return fn()


def options(dbg, **kv):
    """/etc/files.conf holding exactly these keys (the rest default)."""
    lines = [f"{k}={v}" for k, v in kv.items()] or ["# defaults"]
    dbg.send(f"sh write {CONF} {lines[0]}")
    for line in lines[1:]:
        dbg.send(f"sh append {CONF} {line}")
    time.sleep(1.0)
    dbg.settle()


def close_new(dbg, before):
    for w in dbg.windows():
        if w["title"] not in before:
            dbg.close_window(w["title"])


def clicks(dbg, qmp, name, gap):
    """Two real clicks on the icon `name`, `gap` seconds between the first
    release and the second press (each click holds 100 ms)."""
    x, y = centre(icons(dbg)[name])
    dbg.warp_cursor(qmp, x, y)
    qmp.click()
    if gap is not None:
        time.sleep(gap)
        qmp.click()


def run(dbg, qmp):
    for c in (f"mkdir {DESK}/Box", f"write {DESK}/notes.txt hello", f"write {DESK}/.secret.txt psst",
              f"cp /usr/share/icons/about.qoi {DESK}/pic.qoi",
              f"cp /usr/share/videos/orbit.avi {DESK}/orbit.avi"):
        dbg.send(f"sh {c}")
    options(dbg)
    want = {"Box", "notes.txt", "pic.qoi", "orbit.avi"}
    ic = wait_for(lambda: (lambda d: d if want <= set(d) else None)(icons(dbg)))
    if not check("the fixtures are on the desktop", bool(ic), f"have {sorted(icons(dbg))}"):
        return

    # 1. Extensions.
    options(dbg, extensions="off")
    names = set(wait_for(lambda: (lambda d: d if "notes" in d else None)(icons(dbg))) or icons(dbg))
    check("extensions off: a known type loses its extension", {"notes", "pic", "orbit"} <= names, str(sorted(names)))
    check("...and a folder keeps its name", "Box" in names)
    options(dbg, extensions="on")
    check("extensions on: they are back", bool(wait_for(lambda: "notes.txt" in icons(dbg))))

    # 2. Hidden files.
    check("a dot-file is not on the desktop by default", ".secret.txt" not in icons(dbg))
    options(dbg, hidden="on")
    check("hidden on: it is", bool(wait_for(lambda: ".secret.txt" in icons(dbg))), str(sorted(icons(dbg))))
    options(dbg)
    check("...and off again: gone", bool(wait_for(lambda: ".secret.txt" not in icons(dbg))))

    # 3. Thumbnails, and their size cap.
    thumbs = wait_for(lambda: (lambda d: d if d["pic.qoi"].get("thumb") and d["orbit.avi"].get("thumb") else None)(icons(dbg)),
                      timeout=40)
    check("by default both the picture and the video are thumbnails", bool(thumbs),
          str({n: icons(dbg)[n].get("thumb") for n in ("pic.qoi", "orbit.avi")}))
    options(dbg, thumbs="off")
    off = wait_for(lambda: not any(icons(dbg)[n].get("thumb") for n in ("pic.qoi", "orbit.avi")))
    check("thumbnails off: neither is", bool(off))
    options(dbg, thumb_max_mb="1")
    capped = wait_for(lambda: (lambda d: d if d["pic.qoi"].get("thumb") and not d["orbit.avi"].get("thumb") else None)(icons(dbg)),
                      timeout=20)
    check("a 1 MB cap: the small picture is a thumbnail and the 1.03 MB video is not", bool(capped),
          str({n: icons(dbg)[n].get("thumb") for n in ("pic.qoi", "orbit.avi")}))
    options(dbg, thumb_max_mb="2")
    check("...and at 2 MB the video is too", bool(wait_for(lambda: icons(dbg)["orbit.avi"].get("thumb"), timeout=20)))

    # 4. Asking before a delete. The control first: it asks.
    def select(name):
        x, y = centre(icons(dbg)[name])
        dbg.click(x, y)
    options(dbg)
    select("notes.txt")
    dbg.key("0xf788")   # Delete
    asked = wait_for(lambda: dbg.json("gui dialog --json").get("open"), timeout=4)
    check("by default Delete asks", bool(asked))
    btns = dbg.json("gui dialog --json").get("buttons", [])
    if len(btns) == 2:
        dbg.click(btns[1]["cx"], btns[1]["cy"])   # Cancel
    wait_for(lambda: not dbg.json("gui dialog --json").get("open"), timeout=4)
    options(dbg, confirm_delete="off")
    select("notes.txt")
    dbg.key("0xf788")
    gone = wait_for(lambda: "notes.txt" in dbg.send("sh trash list"))
    check("confirm_delete off: Delete moves it to the bin at once", bool(gone))
    check("...with no dialog", not dbg.json("gui dialog --json").get("open"))

    # 5. Single click. The control first: one click opens nothing.
    options(dbg)
    before = {w["title"] for w in dbg.windows()}
    clicks(dbg, qmp, "Box", None)
    time.sleep(2.0)
    check("with double click, one click opens nothing", {w["title"] for w in dbg.windows()} == before)
    dbg.click(700, 300)   # bare desktop: no selection, no double with what follows
    options(dbg, click="single")
    x, y = centre(icons(dbg)["Box"])
    dbg.warp_cursor(qmp, x, y)
    time.sleep(0.5)
    check("single click: the pointer over an icon is the link hand", dbg.cursor_shape() == CURSOR_HAND,
          f"shape {dbg.cursor_shape()}")
    qmp.click()
    opened = wait_for(lambda: {w["title"] for w in dbg.windows()} - before)
    check("...and one click on a folder opens it", bool(opened))
    close_new(dbg, before)
    options(dbg)

    # 6. Double-click speed: the same pair of presses ~450 ms apart, two settings.
    dbg.send("sh config set system.double_click veryfast")
    time.sleep(1.0)
    clicks(dbg, qmp, "Box", 0.35)
    time.sleep(2.0)
    check("at Very fast, presses 450 ms apart are two clicks", {w["title"] for w in dbg.windows()} == before)
    dbg.click(700, 300)
    dbg.send("sh config set system.double_click veryslow")
    time.sleep(1.0)
    clicks(dbg, qmp, "Box", 0.35)
    opened = wait_for(lambda: {w["title"] for w in dbg.windows()} - before)
    check("at Very slow, they are a double click", bool(opened))
    close_new(dbg, before)
    dbg.send("sh config set system.double_click normal")

    # 7. System Settings' Test folder.
    w = dbg.spawn("/bin/wm/system/settings system.double_click", title="System Settings")
    if not check("System Settings opens on the double-click setting", w is not None):
        return
    dbg.settle()
    # It opens on the Mouse page, scrolled to the top: wheel the Test card
    # into the window before clicking it.
    dbg.warp_cursor(qmp, w["x"] + w["w"] * 2 // 3, w["y"] + w["h"] // 3)   # the wheel goes where the REAL pointer is
    for _ in range(12):
        t = dbg.widgets("System Settings").get("dclick_test")
        if t and t["screen"]["y"] + t["h"] < w["y"] + w["h"] - 80:
            break
        dbg.wheel(-3)
    cx, cy = dbg.widget_center("dclick_test", title="System Settings")
    dbg.logs()   # clears
    dbg.warp_cursor(qmp, cx, cy)
    qmp.click()
    time.sleep(0.1)
    qmp.click()
    opened = wait_for(lambda: "settings: dclick test opened" in "\n".join(dbg.logs("dclick", clear=False)), timeout=5)
    check("a double click opens the Test folder", bool(opened), str(dbg.logs("settings:", clear=False)[-4:]))
    dbg.close_window("System Settings")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "folder_options_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("folder options and the double-click speed")
    run(dbg, qmp)
    print(f"\nfolder_options_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
