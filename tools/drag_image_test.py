#!/usr/bin/env python3
"""The compositor's drag image: one image wherever the pointer is, saying
what a release would do (userland/wm/wm_dnd.c, WIN_REQ_DRAG_FEEDBACK).

A file dragged out of the File Manager is held over four places, and
`gui dnd --json` is asked what the image says there:

  1. OVER A FOLDER IN ITS OWN WINDOW: "Move to sub" -- the source's own
     answer. The compositor drew nothing inside the source before.
  2. OVER THE DESKTOP'S BACKGROUND: "Move to Desktop", and with Ctrl
     held "Copy to Desktop" -- Ctrl is the copy/move switch everywhere.
  3. OVER A WINDOW THAT TAKES NO FILES (Notepad): "Cannot drop here",
     Notepad's own answer. A chip that named every window would pass 1
     and 2 and fail this.
  4. THE IMAGE IS DRAWN: the chip's box holds text-coloured pixels, read
     from a real frame -- "it responds" is not "it is drawn".

Then the release over the source's own folder refuses (its own
directory), the file stays, and `gui dnd` says no drag is held.

    python3 tools/vm.py start && python3 tools/drag_image_test.py
"""
import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import filemanager_test as fm                          # noqa: E402
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
from snap_test import arrow_codes                      # noqa: E402
import port_guard                                      # noqa: E402
from harness import Results                            # noqa: E402

DIR = "/home/dragimg"
NOTEPAD = "/bin/wm/apps/notepad"


def dnd(dbg):
    try:
        return dbg.json("gui dnd --json") or {}
    except Exception:   # noqa: BLE001 -- a torn line reads as "not yet"
        return {}


def wait_chip(dbg, want, timeout=6.0):
    end = time.time() + timeout
    st = {}
    while time.time() < end:
        st = dnd(dbg)
        if st.get("chip") == want:
            return st
        dbg.settle(0.1)
    return st


def glide(dbg, qmp, x0, y0, x1, y1, steps=6):
    for i in range(1, steps + 1):
        dbg.warp_cursor(qmp, x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)


def ink_in(png, rect, text_rgb=(20, 20, 20), tol=60):
    """Pixels near the theme's text colour inside rect: a drawn label."""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    x, y, w, h = rect
    n = 0
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            p = im.getpixel((xx, yy))
            if all(abs(p[i] - text_rgb[i]) <= tol for i in range(3)):
                n += 1
    return n


def run(dbg, qmp, res, tmp):
    dbg.send(f"sh mkdir {DIR}")
    dbg.send(f"sh mkdir {DIR}/sub")
    dbg.write_lines(f"{DIR}/a.txt", ["drag me"])
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    arrow = arrow_codes()

    note = dbg.spawn(NOTEPAD, "untitled")   # Notepad titles itself by its document
    dbg.key(hex(arrow["right"]), mods="super")
    dbg.settle()
    win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
    res.check("the File Manager and Notepad open", win is not None and note is not None,
              f"fm={win} notepad={note}")
    if not win or not note:
        return
    dbg.key(hex(arrow["left"]), mods="super")
    dbg.settle()
    # Half its height, so the desktop's background shows below it.
    scr = dbg.state()["screen"]
    tb_y = dbg.json("gui taskbar --json").get("y", scr["h"])
    fw = dbg.window(fm.TITLE) or {}
    dbg.send(f"gui resize {max(400, fw.get('w', 600) - 16)} {max(300, tb_y // 2 - 40)}")
    dbg.settle()
    win = dbg.window(fm.TITLE) or win   # moved and resized: its content origin with it
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.rows.get(0) == 3)
    if not lay or lay.rowh is None or 0 not in lay.rowy:
        res.check("the File Manager lists .., sub and a.txt", False, f"layout {lay and lay.rows}")
        return

    def row_pt(r):
        px, py, pw, ph = lay.pane[0]
        return lay.ox + px + 40, lay.oy + lay.rowy[0] + lay.rowh * r + lay.rowh // 2

    fx, fy = row_pt(2)     # a.txt
    sx, sy = row_pt(1)     # sub
    nw = dbg.window("untitled") or {}
    nx, ny = nw.get("x", 0) + nw.get("w", 0) // 2, nw.get("y", 0) + nw.get("h", 0) // 2
    fw = dbg.window(fm.TITLE) or {}

    dbg.warp_cursor(qmp, fx, fy)
    qmp.mouse_down()
    try:
        glide(dbg, qmp, fx, fy, sx, sy)
        st = wait_chip(dbg, "Move to sub")
        mid = fm.layout_now(dbg, win, tries=5)
        res.check("a drag is held, with the file's icon", st.get("active") and st.get("count") == 1
                  and st.get("icon") == "yes",
                  f"{st}; the File Manager's drag {mid and mid.drag}, pressed at {fx},{fy}")
        res.check("over a folder in its own window: Move to sub", st.get("chip") == "Move to sub", str(st))

        if st.get("chip") and st.get("chip_rect"):
            png = os.path.join(tmp, "over_sub.png")
            qmp.screenshot(png, stable=False)
            r = st["chip_rect"]
            ink = ink_in(png, r)
            res.check("...and the image is drawn there, its words in ink", ink > 10, f"{ink} text pixels in {r}")

        glide(dbg, qmp, sx, sy, nx, ny)
        st = wait_chip(dbg, "Cannot drop here")
        res.check("over Notepad, which takes no files: Cannot drop here",
                  st.get("chip") == "Cannot drop here", str(st))

        # The desktop: below the File Manager's half-height window, in the
        # left half -- background, past the icon column.
        dx = fw.get("x", 0) + fw.get("w", 400) * 2 // 3
        dy = min(tb_y - 30, fw.get("y", 0) + fw.get("h", 300) + 60)
        top = dbg.json("gui windows --json") or []
        glide(dbg, qmp, nx, ny, dx, dy)
        st = wait_chip(dbg, "Move to Desktop")
        res.check("over the desktop's background: Move to Desktop",
                  st.get("chip") == "Move to Desktop", f"{st} at {dx},{dy}; windows {top}")
        qmp.key_down("ctrl")
        dbg.warp_cursor(qmp, dx + 3, dy)
        st = wait_chip(dbg, "Copy to Desktop")
        res.check("...and with Ctrl held: Copy to Desktop", st.get("chip") == "Copy to Desktop", str(st))
        qmp.key_up("ctrl")

        glide(dbg, qmp, dx, dy, fx, fy)
    finally:
        qmp.mouse_up()
    dbg.settle()
    st = dnd(dbg)
    res.check("the release ends the drag", not st.get("active"), str(st))
    ls = dbg.send(f"sh ls {DIR}")
    res.check("...and, dropped on its own folder, the file stays", "a.txt" in ls, ls[-200:])
    dbg.close_window("untitled")
    dbg.close_window(fm.TITLE)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "drag_image_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    tmp = tempfile.mkdtemp(prefix="dragimg-")
    try:
        run(dbg, qmp, res, tmp)
    finally:
        dbg.close()
    print(f"\ndrag_image_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
