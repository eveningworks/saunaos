#!/usr/bin/env python3
"""Disc images on the desktop: an .iso mounted as a CD drive from the File
Manager, its row under This computer, the details pane, and the tray's
drives flyout.

userland/apps/apps/files/fm_disc.c, fm_places.c and fm_details.c, ui/uui_places.c,
ui/uui_fileinfo.c and ui/uui_volinfo.c (the facts), apps/system/toywm/drives_popup.c
and ui/uui_drive.c (the flyout), and /bin/automountd doing the mounting
(base/udrive.h). The kernel's reading of the disc is iso9660_test.py's.

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. "Mount as a CD drive" is in a selected .iso's right-click menu, the
     row right after Open, and absent from a text file's; in the See more
     drop-down it is DRAWN greyed for the text file and live for the .iso
     (the label's darkest ink, at the rect the app reports; the Properties
     row is the control), and a click on the greyed row asks for nothing.
  2. The details pane describes the .iso as a disc (its label) with a
     Mount button.
  3. A DOUBLE-CLICK on the .iso mounts it -- the mount table has it at
     /media/TESTDISC as iso9660 -- and the pane goes there and lists the
     disc's files.
  4. Under This computer a row for it appears, DRAWN (its rect changed),
     and entered from there the details pane shows the drive with Eject.
  5. Opening the .iso again while mounted opens the same drive: no second
     mount.
  6. The row's own Eject button unmounts it: the mount and /media/TESTDISC
     go, the row leaves the tree, and the File Manager logged the ask.
  7. A file named .iso that is no disc image is refused in the status
     line, and nothing is mounted.
  8. Mounted again, the tray's drives flyout lists it as an image row with
     Open and Eject, and did NOT open itself (an image is not a plug-in);
     opened by its icon it is DRAWN (its rect changes when it closes), and
     its Eject unmounts the image.

    python3 tools/tests/sweep/disc_desktop_test.py --instance 5
"""

# The desktop's half: an .iso mounted from the File Manager, its CD
# row and Eject, the details pane, the drives flyout. Its own guest.
RUNNER = {"covers": "disc images on the desktop: FM mount, Eject, flyout"}


import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
from harness import Results, copy_disk, poll  # noqa: E402
from port_guard import instance_sock  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
import filemanager_test as fm  # noqa: E402
import mkpart_test  # noqa: E402

TOOL = "disc_desktop_test"
WRITER = toolpath.tool("tfs3_writer.py")
DIR = "/home/disc"
ISO = f"{DIR}/test.iso"
LABEL = "TESTDISC"
POINT = "/media/" + LABEL
K_ESC = "0x1b"
MOUNT_ROW = "Mount as a CD drive"


def build_iso(work):
    tree = os.path.join(work, "tree")
    os.makedirs(os.path.join(tree, "DOCS"))
    with open(os.path.join(tree, "HELLO.TXT"), "w") as f:
        f.write("hello from a disc\n")
    with open(os.path.join(tree, "DOCS", "README.TXT"), "w") as f:
        f.write("read me\n")
    iso = os.path.join(work, "test.iso")
    r = subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "--norock", "-J", "-V", LABEL,
                        "-o", iso, tree], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{TOOL}: xorriso could not build the image:\n{r.stderr}")
    return iso


def stage(disk, files):
    """{guest path: host file} into the image's filesystem volume, as
    iso9660_test.py's stage() does."""
    vol = mkpart_test.volume_of(disk)
    at = ["--at-lba", str(vol[0]), "--sectors", str(vol[1])] if vol[0] else []
    subprocess.run([sys.executable, WRITER, "mkdir", disk, DIR, *at], cwd=REPO,
                   capture_output=True, text=True)
    for guest, host in files.items():
        r = subprocess.run([sys.executable, WRITER, "write", disk, host, guest, *at], cwd=REPO,
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{TOOL}: staging {guest} failed:\n{r.stdout}{r.stderr}")


def ink(png, box):
    """The darkest luminance in `box`: a label's ink (greyed is lighter)."""
    return Image.open(png).convert("L").crop(box).getextrema()[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=9)
    a = ap.parse_args()
    if not shutil.which("xorriso"):
        print(f"{TOOL}: needs xorriso on the host -- skipped")
        return 0
    res = Results()
    tmp = tempfile.mkdtemp(prefix="discdesk.")
    base = [sys.executable, toolpath.tool("vm.py"), "--instance", str(a.instance)]
    started = False
    dbg = None
    try:
        iso = build_iso(tmp)
        notes = os.path.join(tmp, "notes.txt")
        with open(notes, "w") as f:
            f.write("not a disc\n")
        disk = os.path.join(tmp, "disk.img")
        copy_disk(os.path.join(REPO, "disk.img"), disk, cwd=REPO)
        stage(disk, {ISO: iso, f"{DIR}/notes.txt": notes, f"{DIR}/fake.iso": notes})

        r = subprocess.run(base + ["--disk", disk, "--mem", "1024", "start"],
                           cwd=REPO, capture_output=True, text=True)
        started = r.returncode == 0
        if not res.check("the guest boots", started, (r.stdout + r.stderr).strip()[-300:]):
            return res.finish(TOOL)
        q = QMPSession(port=4445 + a.instance)
        sock = instance_sock(a.instance)
        enter_gui(q, sock)
        dbg = DebugConsole(sock)

        def sh(cmd):
            return dbg.send(f"sh {cmd}") or ""

        def mounted():
            return [ln for ln in sh("mount").splitlines() if POINT in ln]

        def drives():
            return dbg.json("gui drives --json")

        def flog(match):
            """Log lines holding `match`: the File Manager's own transcript
            (filemanager_test's layout waits sweep every `files:` line into
            it) and what the console still holds."""
            return [ln for ln in fm._ALL_BUF + dbg.logs(match, clear=False) if match in ln]

        def shot(name):
            p = os.path.join(tmp, name + ".png")
            q.screenshot(p)
            return p

        dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=1"])
        win = dbg.spawn(f"{fm.SPAWN_PATH} {DIR}", fm.TITLE)
        if not res.check("the File Manager opens on the disc's folder", win is not None, "no window"):
            return res.finish(TOOL)
        names = sorted(n.rstrip("/") for n in fm.listing(dbg, DIR))
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR and l.rowy.get(0) is not None
                             and l.rowh and 0 in l.pane)
        if not res.check("its pane reports its rows", lay is not None, str(fm.last_layout())[:200]):
            return res.finish(TOOL)
        px, py, pw, ph = lay.pane[0]

        def row_pt(name):
            v = 1 + names.index(name)   # row 0 is ".."
            return (lay.ox + px + pw // 2, lay.oy + lay.rowy[0] + lay.rowh * v + lay.rowh // 2)

        def select(name):
            dbg.key(fm.K_HOME)
            for ch in name:
                dbg.key(ch)
            return fm.wait_layout(dbg, win, lambda l: l.selected == name)

        # 1. the menus
        def ctx_of(name):
            select(name)
            mark = fm.ctx_mark(dbg)
            fm.sure_rclick(dbg, q, *row_pt(name))
            rows = fm.ctx_rows(dbg, mark) or []
            dbg.key(K_ESC)
            fm.wait_layout(dbg, win, lambda l: l.ctx == 0)
            return rows

        rows = ctx_of("test.iso")
        res.check("the .iso's menu has Mount as a CD drive right after Open",
                  "Open" in rows and MOUNT_ROW in rows and rows.index(MOUNT_ROW) == rows.index("Open") + 1,
                  "|".join(rows))
        rows = ctx_of("notes.txt")
        res.check("a text file's menu has no Mount row", rows and MOUNT_ROW not in rows, "|".join(rows))

        # The See more drop-down: its rows' rects are the app's own report
        # (filemanager_test's Layout.popitems), and greyed is judged by
        # PIXELS -- the row's darkest ink -- with Properties as the control.
        more = fm.menu_rows("more_items")
        mrow, prow = more[MOUNT_ROW], more["Properties"]

        def more_menu(name):
            """(the Mount row's ink, the Properties row's ink) with `name`
            selected and See more open; Esc closes it. None when it did
            not open."""
            got = select(name) or fm.layout_now(dbg, win)
            fm.toolbar_click(dbg, q, win, got, "more", res, f"See more with {name}")
            pop = fm.wait_layout(dbg, win, lambda l: l.ctx == 1 and mrow in l.popitems and prow in l.popitems)
            out = None
            if pop:
                png = shot("more-" + name)

                def box(i):
                    x, y, w, h = pop.popitems[i]
                    return (pop.ox + x + 2, pop.oy + y + 2, pop.ox + x + w - 2, pop.oy + y + h - 2)
                out = (ink(png, box(mrow)), ink(png, box(prow)), pop)
            return out

        txt = more_menu("notes.txt")
        if txt:
            # Clicking the greyed row does nothing: no mount is asked for.
            x, y, w, h = txt[2].popitems[mrow]
            fm.sure_click(dbg, q, txt[2].ox + x + w // 2, txt[2].oy + y + h // 2)
            dbg.settle()
            res.check("See more: a click on the greyed Mount row asks for nothing",
                      not any("mount asked" in ln for ln in flog("files: disc")) and not mounted(),
                      " | ".join(flog("files: disc")[-2:]))
        dbg.key(K_ESC)
        fm.wait_layout(dbg, win, lambda l: l.ctx == 0)
        iso_m = more_menu("test.iso")
        dbg.key(K_ESC)
        fm.wait_layout(dbg, win, lambda l: l.ctx == 0)
        if res.check("See more opened with each selection", txt and iso_m, f"txt={txt} iso={iso_m}"):
            res.check("See more: Mount is DRAWN greyed for a text file and live for an .iso "
                      "(the greyed label's ink is lighter)", txt[0] > iso_m[0] + 40,
                      f"greyed {txt[0]}, live {iso_m[0]}")
            res.check("(control: the Properties row is drawn alike both times)",
                      abs(txt[1] - iso_m[1]) <= 16, f"{txt[1]} vs {iso_m[1]}")

        # 2. the details pane, on the .iso
        select("test.iso")
        said = poll(lambda: any(f"details disc {ISO} label {LABEL} button Mount" in ln
                                for ln in flog("files: details disc")), 6)
        res.check("the details pane describes the .iso by its label, with Mount", said,
                  " | ".join(flog("files: details")[-3:]))

        # 3. double-click mounts and opens
        tree_before = shot("tree-before")
        dbg.logs("")
        x, y = row_pt("test.iso")
        dbg.send(f"gui click {x} {y}")
        # sleep-ok: the gap between a double click's two presses IS the gesture
        time.sleep(0.1)
        dbg.send(f"gui click {x} {y}")
        up = poll(lambda: mounted(), 20)
        res.check("a double-click mounts it at /media/TESTDISC as iso9660",
                  up and "iso9660" in up[0], sh("mount")[-300:])
        there = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == POINT, timeout=15)
        res.check("...and the pane goes there", there is not None, str(getattr(fm.last_layout(), "dir", None)))
        listed = sorted(n.rstrip("/") for n in fm.listing(dbg, POINT))
        res.check("...where the disc's files are", listed == ["DOCS", "HELLO.TXT"], str(listed))
        res.check("the File Manager asked automountd and heard back",
                  any(f"disc {ISO} mount rc 0 at {POINT}" in ln for ln in flog("files: disc")),
                  " | ".join(flog("files: disc")[-3:]))

        # 4. the row under This computer
        lay = fm.wait_layout(dbg, win, lambda l: l.treebox and POINT in l.treerow.values())
        at = [r for r, p in sorted(lay.treerow.items()) if p == POINT] if lay else []
        seen = lay or fm.last_layout()
        res.check("a row for it under This computer", bool(at),
                  str(sorted(seen.treerow.items())) if seen else "no layout")
        if at:
            tx, ty, tw, th, rh, _ = lay.treebox
            rbox = (lay.ox + tx, lay.oy + ty + at[0] * rh, lay.ox + tx + tw, lay.oy + ty + (at[0] + 1) * rh)
            after = shot("tree-after")
            d = ImageChops.difference(Image.open(tree_before).convert("RGB").crop(rbox),
                                      Image.open(after).convert("RGB").crop(rbox))
            res.check("...DRAWN (its rect changed when it appeared)", d.getbbox() is not None, str(rbox))
            # Entered from the tree, the details pane describes the drive.
            dbg.logs("")
            fm.sure_click(dbg, q, lay.ox + tx + tw // 3, rbox[1] + rh // 2)
            said = poll(lambda: any(f"details volume {POINT} button Eject" in ln
                                    for ln in flog("files: details volume")), 8)
            res.check("the drive's details pane offers Eject", said, " | ".join(flog("files: details")[-3:]))

        # 5. opened again while mounted: the same drive
        dbg.settle()
        fm.toolbar_click(dbg, q, win, fm.layout_now(dbg, win), "back", res, "Back to the folder")
        fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR, timeout=10)
        dbg.logs("")
        select("test.iso")
        dbg.key(fm.K_ENTER)
        again = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == POINT, timeout=10)
        res.check("Enter on it again opens the same drive, with no second mount",
                  again is not None and len(mounted()) == 1 and
                  any(f"disc {ISO} open at {POINT}" in ln for ln in flog("files: disc")),
                  f"mounts={mounted()} log={flog('files: disc')[-2:]}")

        # 6. Eject from its row
        lay = fm.wait_layout(dbg, win, lambda l: l.treebox and POINT in l.treerow.values())
        at = [r for r, p in sorted(lay.treerow.items()) if p == POINT] if lay else []
        if at:
            x, y, w, h, rh, _ = lay.treebox
            # ui/uui_tree.c: the action's square is row_h - 6, at the row's
            # right edge less the pad (4) and the rounded inset (6).
            dbg.logs("")
            dbg.click(lay.ox + x + w - 4 - 6 - (rh - 6) // 2, lay.oy + y + at[0] * rh + rh // 2)

            def tree_has():
                l = fm.layout_now(dbg, win)
                return POINT in l.treerow.values() if l else True
            gone = poll(lambda: not mounted() and not tree_has(), 15)
            res.check("its row's Eject unmounts it and the row leaves", gone, sh("mount")[-200:])
            res.check("...and /media/TESTDISC is removed", LABEL not in sh("ls /media"), sh("ls /media"))
            res.check("the File Manager asked for it",
                      any(f"eject image {POINT}" in ln for ln in flog("files: eject image")),
                      " | ".join(flog("files: ")[-4:]))
        else:
            res.check("its row's Eject unmounts it and the row leaves", False, "no row to press")

        # 7. a refusal
        fm.wait_layout(dbg, win, lambda l: l.dir.get(0) != POINT, timeout=8)
        now = fm.layout_now(dbg, win) or fm.last_layout()
        if now is None or now.dir.get(0) != DIR:
            fm.toolbar_click(dbg, q, win, fm.layout_now(dbg, win), "back", res, "Back again")
            fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == DIR, timeout=10)
        select("fake.iso")
        dbg.key(fm.K_ENTER)
        said = fm.wait_layout(dbg, win, lambda l: l.note and "not a disc image" in l.note, timeout=8)
        res.check("a file named .iso that is no disc image is refused in the status line",
                  said is not None and not mounted(), str(getattr(fm.last_layout(), "note", None)))

        # 8. the tray's flyout
        select("test.iso")
        dbg.key(fm.K_ENTER)
        poll(lambda: mounted(), 20)
        rows = poll(lambda: [r for r in drives()["rows"] if r.get("image")], 10) or []
        d = drives()
        res.check("the flyout lists the mounted image as its own row, with Open and Eject",
                  len(rows) == 1 and rows[0]["title"] == LABEL and "open" in rows[0] and "eject" in rows[0]
                  and rows[0]["point"] == POINT, str(d)[:400])
        res.check("...and the tray item shows, but the flyout did not open itself",
                  not d["tray_hidden"] and not d["open"] and d.get("self_opens") == 0, str(d)[:200])
        dbg.click(d["tray"]["cx"], d["tray"]["cy"])
        d = drives()
        if res.check("a click on the tray icon opens it", d["open"], str(d)[:200]):
            box = (d["x"], d["y"], d["x"] + d["w"], d["y"] + d["h"])
            open_png = shot("flyout-open")
            row = next((r for r in d["rows"] if r.get("image")), None)
            dbg.logs("")
            if row and "eject" in row:
                dbg.click(row["eject"]["cx"], row["eject"]["cy"])
            gone = poll(lambda: not mounted() and not [r for r in drives()["rows"] if r.get("image")], 15)
            res.check("its Eject unmounts the image and the row goes", gone,
                      sh("mount")[-200:] + " | " + str(drives()["rows"])[:200])
            res.check("the compositor asked automountd for it",
                      any(f"eject image {POINT} asked" in ln for ln in flog("wm: drives:")),
                      " | ".join(flog("wm: drives")[-3:]))
            if drives()["open"]:
                dbg.click(d["tray"]["cx"], d["tray"]["cy"])
            poll(lambda: not drives()["open"], 6)
            closed_png = shot("flyout-closed")
            a_img = Image.open(open_png).convert("RGB").crop(box)
            b_img = Image.open(closed_png).convert("RGB").crop(box)
            diff = ImageChops.difference(a_img, b_img).convert("L").point(lambda v: 255 if v > 24 else 0)
            changed = diff.histogram()[255] / max(1, box[2] - box[0]) / max(1, box[3] - box[1])
            res.check("the flyout was DRAWN where it said it was (its rect changes when it closes)",
                      changed > 0.3, f"{changed:.2f} of the rect changed")
    finally:
        if dbg:
            dbg.close()
        if started:
            subprocess.run(base + ["stop"], cwd=REPO, capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)
    return res.finish(TOOL)


if __name__ == "__main__":
    sys.exit(main())
