#!/usr/bin/env python3
"""The File Manager's left pane menus (userland/fm/fm_places.c) and the
volume card they and the details pane open (ui/uui_volinfo.h).

1. **Every kind of row has its own menu** -- a named place, Recent, the
   Recycle Bin, a pin at each end of the list, a disk volume, a volume
   in memory, a heading -- read from the app's `placemenu` log line, and
   the popup is OPEN (its rows are laid out), not just logged.
2. **Move down / Rename pin / Unpin** change /etc/places.conf, read back
   with `cat`, and the tree follows.
3. **Hide from Places** writes files.conf's hidden_places and the row
   goes; **Show hidden places** on a heading brings it back.
4. **Properties on a volume opens the volume page**: the window is
   titled "System Properties", logs the volume's rows, and DRAWS its
   used/free ring -- track grey or the CREATE green all round the
   middle of the ring's own box (a broken ring passes every log check).
5. **The details pane describes a volume's root as the volume** -- its
   card, with "Open in Disks" for its button.

`--positive-control` aims step 4 at a FOLDER that is not a mount point:
its Properties is the file page, so the ring check must go red.
"""
import argparse
import math
import os
import re
import sys
import tempfile
import time

from PIL import Image

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

PA, PB = "/home/pin-a", "/home/pin-b"
GREEN = (44, 122, 61)        # utheme's UTHEME_ACT_CREATE: a disk volume's meter
TRACK = (205, 205, 210)      # utheme's separator: the ring's unused part


def run(dbg, qmp, res, positive_control):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    def menu_log(timeout=8.0):
        end = time.time() + timeout
        seen = ""
        while time.time() < end:
            seen += "\n".join(dbg.logs("files: placemenu"))
            m = re.findall(r"files: placemenu (\S+) (\S+) rows (.*)", seen)
            if m:
                return m[-1]
            time.sleep(0.3)
        return None

    for p in (PA, PB):
        sh(f"rm -r {p}")
        sh(f"mkdir {p}")
    dbg.write_lines("/etc/places.conf", [PA, PB])
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=1"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home" and l.treebox and l.treerow) \
        or fm.layout_now(dbg, win)

    def row_of(l, path):
        rows = [r for r, p in sorted(l.treerow.items()) if p == path]
        return rows[0] if rows else None

    def rclick_row(l, r):
        tx, ty, tw, th, trh, _ = l.treebox
        dbg.logs("files: placemenu")       # drop older lines
        fm.sure_rclick(dbg, qmp, l.ox + tx + tw // 3, l.oy + ty + r * trh + trh // 2)
        got = menu_log()
        pop = fm.wait_layout(dbg, win, lambda x: x.ctx == 1 and x.popitems, timeout=6.0)
        return got, pop

    def pick(pop, rows, label):
        """Click the row labelled `label` of the open menu."""
        labels = rows.split("|")
        if not pop or label not in labels:
            return False
        i = labels.index(label)
        if i not in pop.popitems:
            return False
        x, y, w, h = pop.popitems[i]
        fm.sure_click(dbg, qmp, pop.ox + x + w // 2, pop.oy + y + h // 2)
        return True

    def close_menu():
        dbg.key(fm.K_ESC)
        fm.wait_layout(dbg, win, lambda l: l.ctx == 0, timeout=4.0)

    # 1. a menu per kind
    want = {
        "/home": ("place", "Open|Open in new tab|-|Hide from Places|-|Copy path|Open in Terminal|-|Properties"),
        "recent:/": ("recent", "Open|Open in new tab|-|Hide from Places|Clear list"),
        "trash:/": ("bin", "Open|Open in new tab|-|Restore all|Empty Recycle Bin|-|Hide from Places|"
                           "Recycle Bin settings..."),
        PA: ("pin", "Open|Open in new tab|-|Rename pin...|Move down|Unpin from Places|-|Copy path|"
                    "Open in Terminal|-|Properties"),
        PB: ("pin", "Open|Open in new tab|-|Rename pin...|Move up|Unpin from Places|-|Copy path|"
                    "Open in Terminal|-|Properties"),
        "/": ("volume", "Open|Open in new tab|-|Copy path|Open in Terminal|-|Open in Disks|Check...|-|Properties"),
        "/tmp": ("volume", "Open|Open in new tab|-|Copy path|Open in Terminal|-|Properties"),
    }
    for path, (kind, rows) in want.items():
        lay = fm.layout_now(dbg, win) or lay
        r = row_of(lay, path)
        if r is None:
            res.check(f"the {path} row's menu", False, f"no row: {lay.treerow}")
            continue
        got, pop = rclick_row(lay, r)
        ok = got is not None and got[0] == kind and got[1] == path and got[2] == rows
        res.check(f"the {kind} row {path} has its own menu, open", ok and pop is not None,
                  f"got={got} popup={pop is not None}")
        close_menu()

    lay = fm.layout_now(dbg, win) or lay
    pins = [r for r, p in sorted(lay.treerow.items()) if p in (PA, PB)]
    head = min(pins) - 1 if pins else None
    got = None
    if head is not None and lay.treerow.get(head) == "-":
        got, pop = rclick_row(lay, head)
        close_menu()
    res.check("a heading opens the pane's own menu", got is not None and got[0] == "pane"
              and got[2] == "Folder tree", f"got={got}")

    def conf():
        return [ln.strip() for ln in sh("cat /etc/places.conf").splitlines() if ln.startswith("/")]

    def wait_conf(pred, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            c = conf()
            if pred(c):
                return c
            time.sleep(0.3)
        return conf()

    # 2. Move down, Rename pin, Unpin
    lay = fm.layout_now(dbg, win) or lay
    got, pop = rclick_row(lay, row_of(lay, PA))
    pick(pop, got[2] if got else "", "Move down")
    c = wait_conf(lambda c: c[:2] == [PB, PA])
    lay = fm.wait_layout(dbg, win, lambda l: (row_of(l, PB) or 99) < (row_of(l, PA) or -1)) or fm.layout_now(dbg, win)
    res.check("Move down swaps the pins in the file and the tree", c[:2] == [PB, PA]
              and (row_of(lay, PB) or 99) < (row_of(lay, PA) or -1), f"conf={c} tree={lay.treerow}")

    got, pop = rclick_row(lay, row_of(lay, PA))
    if pick(pop, got[2] if got else "", "Rename pin..."):
        fm.wait_layout(dbg, win, lambda l: l.modal == 1, timeout=6.0)
        for ch in "Alpha":
            dbg.key(ch, settle=False)
        dbg.key(fm.K_ENTER)
    c = wait_conf(lambda c: f"{PA}\tAlpha" in c)
    res.check("Rename pin writes its name beside the path", f"{PA}\tAlpha" in c, f"conf={c}")

    lay = fm.layout_now(dbg, win) or lay
    got, pop = rclick_row(lay, row_of(lay, PB))
    pick(pop, got[2] if got else "", "Unpin from Places")
    c = wait_conf(lambda c: not any(x.startswith(PB) for x in c))
    lay = fm.wait_layout(dbg, win, lambda l: row_of(l, PB) is None) or fm.layout_now(dbg, win)
    res.check("Unpin takes it out of the file and the tree",
              not any(x.startswith(PB) for x in c) and row_of(lay, PB) is None, f"conf={c}")

    # 3. Hide and Show
    got, pop = rclick_row(lay, row_of(lay, "/home/Music"))
    pick(pop, got[2] if got else "", "Hide from Places")
    lay = fm.wait_layout(dbg, win, lambda l: row_of(l, "/home/Music") is None) or fm.layout_now(dbg, win)
    hidden = sh(f"cat {fm.FILES_CONF}")
    res.check("Hide from Places writes hidden_places and the row goes",
              "hidden_places=music" in hidden and row_of(lay, "/home/Music") is None,
              f"conf={hidden[-200:]!r}")
    pins = [r for r, p in sorted(lay.treerow.items()) if p == PA]
    got, pop = rclick_row(lay, pins[0] - 1) if pins else (None, None)
    pick(pop, got[2] if got else "", "Show hidden places (Music)")
    lay = fm.wait_layout(dbg, win, lambda l: row_of(l, "/home/Music") is not None) or fm.layout_now(dbg, win)
    res.check("Show hidden places brings it back",
              got is not None and "Show hidden places (Music)" in got[2]
              and row_of(lay, "/home/Music") is not None
              and "hidden_places" not in sh(f"cat {fm.FILES_CONF}"), f"got={got}")

    # 4. Properties of a volume: the volume page, ring drawn
    target = "/home" if positive_control else "/"
    title = "home Properties" if positive_control else "System Properties"
    dbg.logs("properties:")
    got, pop = rclick_row(lay, row_of(lay, target))
    pick(pop, got[2] if got else "", "Properties")
    pw = None
    for _ in range(30):
        pw = dbg.window_settled(title) if hasattr(dbg, "window_settled") else dbg.window(title)
        if pw:
            break
        time.sleep(0.5)
    logs = "\n".join(dbg.logs("properties:"))
    ring = re.findall(r"properties: layout ring (\d+) (\d+) (\d+)", logs)
    shot = os.path.join(tempfile.mkdtemp(prefix="fmplaces."), "props.png")
    inked = total = 0
    if pw and ring:
        qmp.screenshot(shot)
        x, y, d = (int(v) for v in ring[-1])
        # The app's layout log is content-relative.
        box = (pw["content"]["x"] + x, pw["content"]["y"] + y, d, d)
        # THE ANNULUS ITSELF is track grey or the used colour: sample its
        # middle circle, where a drawn ring cannot be anything else.
        im = Image.open(shot).convert("RGB")
        cx, cy, rm = box[0] + d / 2, box[1] + d / 2, d / 2 * 0.8
        for k in range(72):
            px = im.getpixel((int(cx + rm * math.cos(k * math.pi / 36)), int(cy + rm * math.sin(k * math.pi / 36))))
            total += 1
            if any(all(abs(px[i] - c[i]) <= 12 for i in range(3)) for c in (GREEN, TRACK)):
                inked += 1
    res.check("Properties on System is the volume page, its ring drawn",
              pw is not None and "properties: volume / local" in logs
              and "properties: row Filesystem=" in logs and total and inked >= total * 9 // 10,
              f"window={pw is not None} ring={ring[-1:] if ring else None} inked={inked}/{total} "
              f"shot={shot} logs={logs[-200:]!r}")
    if pw:
        dbg.close_window(title)

    # 5. the details pane at a volume's root
    dbg.logs("files:")
    lay = fm.layout_now(dbg, win) or lay
    r = row_of(lay, "/")
    if r is not None:
        tx, ty, tw, th, trh, _ = lay.treebox
        fm.sure_click(dbg, qmp, lay.ox + tx + tw // 3, lay.oy + ty + r * trh + trh // 2)
    seen = ""
    end = time.time() + 10
    while time.time() < end and "files: details volume / " not in seen:
        seen += "\n".join(dbg.logs("files: details"))
        time.sleep(0.3)
    res.check("the details pane shows the volume card at /, Open in Disks its button",
              "files: details volume / button Open in Disks" in seen, f"logs={seen[-200:]!r}")

    for p in (PA, PB):
        sh(f"rm -r {p}")
    sh("rm /etc/places.conf")
    dbg.close_window(fm.TITLE)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--positive-control", action="store_true",
                    help="aim Properties at a folder: the ring check must fail")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_places_menu_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res, args.positive_control)
    finally:
        dbg.close()
    print(f"\nfm_places_menu_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
