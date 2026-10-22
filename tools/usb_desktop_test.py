#!/usr/bin/env python3
"""USB sticks on the desktop: the tray's USB drives flyout, the File
Manager's Removable group, Eject, and what the person is told.

userland/wm/drives_popup.c, ui/uui_drive.c, the File Manager's tree
(fm_tree.c, ui/uui_tree.c's row action) and /bin/automountd, on QEMU's
usb-storage plugged into a running desktop (docs/usb-storage-design.md,
stage 5).

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. With nothing plugged in the tray item is hidden.
  2. A stick plugged in is MOUNTED by automountd at /media/<label>, the
     tray item shows, and the flyout OPENS ITSELF listing it -- read from
     `gui drives --json`, and DRAWN: the flyout's rect differs from the
     same rect once it has closed (a flyout that is open in state only
     passes every log check).
  3. Untouched, it closes itself again.
  4. Its Eject, pressed where `gui drives` says it is, ejects the stick:
     the row goes, /media/<label> is removed, the kernel logs it ejected,
     and the notice says "<label> can be pulled out".
  5. Plugged in again, the File Manager lists it under a "Removable"
     heading, and the row's own Eject button (aimed from the tree's
     layout log) ejects it -- the row leaves the tree.
  6. Plugged in a third time and PULLED without Eject: the notice says it
     "was pulled out", and the mount is tidied away.

`--positive-control` pulls the stick at step 4 instead of pressing Eject:
the "can be pulled out" check must go red.

    python3 tools/usb_desktop_test.py
    python3 tools/usb_desktop_test.py --instance 9 --positive-control
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import Results, copy_disk, poll  # noqa: E402
from port_guard import instance_sock  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
import filemanager_test as fm  # noqa: E402
from usb_storage_test import fat_stick  # noqa: E402

TOOL = "usb_desktop_test"
LABEL = "DESKSTICK"
POINT = "/media/" + LABEL


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=9)
    ap.add_argument("--positive-control", action="store_true",
                    help="pull the stick instead of pressing Eject: the eject notice check must fail")
    a = ap.parse_args()
    for t in ("sfdisk", "mkfs.fat", "mcopy"):
        if not shutil.which(t):
            print(f"{TOOL}: needs {t} on the host (dosfstools, mtools)")
            return 1
    res = Results()
    tmp = tempfile.mkdtemp(prefix="usbdesk.")
    base = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(a.instance)]
    started = False
    dbg = None
    try:
        disk = os.path.join(tmp, "disk.img")
        copy_disk(os.path.join(REPO, "disk.img"), disk, cwd=REPO)
        hello = os.path.join(tmp, "hello.txt")
        with open(hello, "w") as f:
            f.write("hello from the desktop test\n")
        sticks = []
        for i in range(3):
            p = os.path.join(tmp, f"stick{i}.img")
            fat_stick(p, LABEL, {"HELLO.TXT": hello})
            sticks.append(p)

        r = subprocess.run(base + ["--disk", disk, "--mem", "1024", "--usb", "xhci", "start"],
                           cwd=REPO, capture_output=True, text=True)
        started = r.returncode == 0
        if not res.check("the guest boots with an xHCI controller", started,
                         (r.stdout + r.stderr).strip()[-300:]):
            return res.finish(TOOL)
        q = QMPSession(port=4445 + a.instance)
        sock = instance_sock(a.instance)
        enter_gui(q, sock)
        dbg = DebugConsole(sock)

        def drives():
            return dbg.json("gui drives --json")

        def notice():
            return (dbg.json("gui state --json").get("notice") or {})

        def sh(cmd):
            return dbg.send(f"sh {cmd}") or ""

        plugged = [0]

        def plug():
            n = plugged[0]
            plugged[0] += 1
            q.hmp(f"drive_add 0 if=none,id=dsk{n},file={sticks[n]},format=raw")
            q.hmp(f"device_add usb-storage,id=dstick{n},drive=dsk{n},bus=xhci.0")
            return n

        def pull(n):
            q.hmp(f"device_del dstick{n}")

        # 1. nothing plugged in
        d = drives()
        res.check("with no stick the tray item is hidden", d["tray_hidden"] and not d["rows"], str(d))

        # 2. plugged in: mounted, shown, and the flyout opens itself
        n0 = plug()
        mounted = poll(lambda: any(r["point"] == POINT for r in drives()["rows"]), 20)
        d = drives()
        res.check("a stick plugged in is mounted at /media/<label> and listed",
                  mounted and d["rows"] and d["rows"][0]["title"] == LABEL, str(d)[:400])
        res.check("the tray item shows", not d["tray_hidden"], str(d)[:200])
        res.check("the flyout opened itself", d["open"] and d["itself"], str(d)[:200])
        ls = sh(f"cat {POINT}/HELLO.TXT")
        res.check("the stick reads at its mount point", "hello from the desktop test" in ls, ls[-200:])
        shot_open = os.path.join(tmp, "open.png")
        q.screenshot(shot_open)
        box = (d["x"], d["y"], d["x"] + d["w"], d["y"] + d["h"])

        # 3. it closes itself
        closed = poll(lambda: not drives()["open"], 12)
        res.check("untouched, it closes itself", closed, str(drives())[:200])
        shot_closed = os.path.join(tmp, "closed.png")
        q.screenshot(shot_closed)
        a_img = Image.open(shot_open).convert("RGB").crop(box)
        b_img = Image.open(shot_closed).convert("RGB").crop(box)
        diff = ImageChops.difference(a_img, b_img).convert("L").point(lambda v: 255 if v > 24 else 0)
        changed = diff.histogram()[255] / max(1, box[2] - box[0]) / max(1, box[3] - box[1])
        res.check("the flyout was DRAWN where it said it was (its rect changes when it closes)",
                  changed > 0.3, f"{changed:.2f} of the rect changed")
        # The control: a corner far from the tray is the same in both.
        ca = Image.open(shot_open).convert("RGB").crop((0, 0, 64, 64))
        cb = Image.open(shot_closed).convert("RGB").crop((0, 0, 64, 64))
        res.check("a corner away from it did not change", ImageChops.difference(ca, cb).getbbox() is None)

        # 4. Eject from the flyout
        dbg.click(d["tray"]["cx"], d["tray"]["cy"])
        d = drives()
        res.check("a click on the tray icon opens it, not by itself", d["open"] and not d["itself"], str(d)[:200])
        dbg.logs("")   # drop what came before
        if a.positive_control:
            pull(n0)
        else:
            ej = d["rows"][0].get("eject") if d["rows"] else None
            res.check("its row has an Eject button", ej is not None, str(d)[:300])
            if ej:
                dbg.click(ej["cx"], ej["cy"])
        gone = poll(lambda: not drives()["rows"], 15)
        res.check("the row goes", gone, str(drives())[:300])
        told = poll(lambda: notice().get("title") == f"{LABEL} can be pulled out", 5)
        res.check("the notice says it can be pulled out", told, str(notice())[:300])
        media, dm = sh("ls /media"), sh("dmesg")
        res.check("its /media folder is removed and the kernel logged the eject",
                  LABEL not in media and "block: usb0 ejected" in dm,
                  media.strip()[-100:] + " | " + " | ".join(l for l in dm.splitlines() if "usb0" in l)[-200:])
        if not a.positive_control:
            pull(n0)   # the ejected stick, unplugged for real

        # 5. the File Manager's Removable group
        plug()
        poll(lambda: any(r["point"] == POINT for r in drives()["rows"]), 20)
        dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details"])
        win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
        res.check("the File Manager opens", win is not None, "no window")
        if win:
            lay = fm.wait_layout(dbg, win, lambda l: l.treebox and POINT in l.treerow.values())
            rows = sorted(lay.treerow.items()) if lay else []
            at = [r for r, p in rows if p == POINT]
            res.check("the stick is a row in the tree", bool(at), str(rows))
            # The heading over it: a "-" row right above (a heading has no path).
            res.check("under a heading of its own",
                      bool(at) and lay.treerow.get(at[0] - 1) == "-", str(rows))
            if at:
                x, y, w, h, rh, _ = lay.treebox
                # ui/uui_tree.c: the action's square is row_h - 6, at the
                # row's right edge less the pad (4) and the rounded inset (6).
                cx = lay.ox + x + w - 4 - 6 - (rh - 6) // 2
                cy = lay.oy + y + at[0] * rh + rh // 2
                dbg.logs("")
                dbg.click(cx, cy)
                def tree_rows():
                    lay = fm.layout_now(dbg, win)
                    return dict(lay.treerow) if lay else None

                left = poll(lambda: (tree_rows() or {POINT: 1}).get(POINT) is None
                            and POINT not in (tree_rows() or {}).values(), 15)
                res.check("its Eject button in the tree ejects it, and the row leaves",
                          left and not drives()["rows"], str(tree_rows())[:300])
                logs = sh("dmesg")
                res.check("the File Manager asked for the eject", "files: eject " + POINT in logs,
                          " | ".join(l for l in logs.splitlines() if "eject" in l)[-300:])
        pull(1)

        # 6. pulled without Eject
        n2 = plug()
        poll(lambda: any(r["point"] == POINT for r in drives()["rows"]), 20)
        pull(n2)
        told = poll(lambda: notice().get("title") == f"{LABEL} was pulled out", 10)
        res.check("pulled without Eject, the notice says so", told, str(notice())[:300])
        tidy = poll(lambda: POINT not in sh("mount"), 10)
        res.check("and its mount is tidied away", tidy, sh("mount")[-300:])
    finally:
        if dbg:
            dbg.close()
        if started:
            subprocess.run(base + ["stop"], cwd=REPO, capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)
    return res.finish(TOOL)


if __name__ == "__main__":
    sys.exit(main())
