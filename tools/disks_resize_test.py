#!/usr/bin/env python3
"""tools/disks_resize_test.py -- Disks' Resize dialog (R1) on a guest of its own.

The guest boots the ordinary system disk (pinned with `root=ata0p3`) and a
second virtio disk the HOST partitioned with sfdisk: a 64 MiB partition
the guest formats tfs3 and part-fills, free space after it, a 32 MiB
FAT32 one further on with room after it, and a 32 MiB exFAT one. Then
Disks is driven:

  * Resize... opens the dialog, and its parts -- the bar, its caption,
    the two size fields, the buttons -- all lie INSIDE the dialog box and
    do not overlap (the maintainer's overflow rule, checked from the
    app's own layout log);
  * the fields start at the partition's size and the free space after it;
  * PIXELS: the used part of the bar is hatched (two tints alternating),
    the handle is the accent, the room after it is white -- and a pixel
    of the scrim outside the box does not change across the drag;
  * dragging the handle moves it (its old place is no longer the accent,
    its new one is) and the fields follow, size + room after = the most;
  * a size typed and Resize: the step QUEUES and the disk is untouched
    (lsblk), then Apply runs it: `lsblk` shows the new length, and the
    volume mounts, checks clean and every file verifies;
  * on the FAT32 partition, Resize... opens the bar with the note that
    FAT32 keeps no journal, Apply's confirm says it again, and a typed
    64 MiB grows it (lsblk, fsck);
  * on the exFAT partition, Resize... says why it cannot, not a bar.

On demand (ondemand_sweep.py), not in gui_regress: it boots its own guest
with a second disk.

    python3 tools/disks_resize_test.py [--instance N] [--kvm] [--keep DIR]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from disks_test import LOG, chk, poll, said, wait           # noqa: E402
from gui_debug import DebugConsole, enter_gui                # noqa: E402
from harness import Results, copy_disk                       # noqa: E402
from qmp_test import QMPSession                              # noqa: E402
import install_grub                                          # noqa: E402
import port_guard                                            # noqa: E402

MIB = 2048
BLANK_BYTES = 512 * 1024 * 1024
SFDISK_SCRIPT = """label: gpt
start=2048, size=131072, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="photos"
start=614400, size=65536, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="esd"
start=819200, size=65536, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="xfer"
"""
P1_MIB, ROOM_MIB = 64, 300 - 1 - 64    # p1 ends at 65 MiB, p2 starts at 300 MiB
FAT_MIB = 32                            # p2, FAT32, with 68 MiB after it; p3 exFAT at 400 MiB
TB_RESIZE = 6
TB_APPLY = 10


def inside(inner, outer):
    x, y, w, h = inner
    ox, oy, ow, oh = outer
    return x >= ox and y >= oy and x + w <= ox + ow and y + h <= oy + oh


def overlaps(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def px(img, x, y):
    return img.getpixel((x, y))[:3]


def bluish(c):
    return c[2] > c[0] + 20


def white(c):
    return min(c) >= 240


def shot(qmp, keep, name):
    from PIL import Image
    path = os.path.join(keep, name)
    qmp.screenshot(path)
    return Image.open(path).convert("RGB")


def run(dbg, qmp, res, keep):
    dbg.send("gui spawn /bin/wm/apps/disks")
    win = None
    for _ in range(50):
        win = dbg.window("Disks")
        if win:
            break
        time.sleep(0.3)
    if not chk(res, win is not None, "the window opens"):
        return
    c = win["content"]
    lay = wait(dbg, c, lambda lay: lay.state["segs"] > 0)

    def press(key):
        dbg.click(*lay.centre(key))
        dbg.settle()

    def apply_all():
        """Apply and its confirm; True when the one step was done."""
        nonlocal lay
        mark = len(LOG)
        lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r and "pending" in lay.r)
        press(f"toolbar.button {TB_APPLY}")
        lay = wait(dbg, c, lambda lay: said("ask Apply", mark) and "dialog.button 0" in lay.r)
        press("dialog.button 0")
        lay = wait(dbg, c, lambda lay: said(" done in ", mark), timeout=120)
        return said("apply 1 of 1 done", mark)

    # The second drive; its first segment, the tfs3 partition, is selected.
    x, y, w, h = lay.r["drives.first_cell"]
    dbg.click(lay.cx + x + w // 2, lay.cy + y + h + h // 2)
    lay = wait(dbg, c, lambda lay: lay.state["drive"] == 1 and lay.state["part"] == 1)
    if not chk(res, lay.state["part"] == 1, "the second disk's first partition is selected"):
        return
    mark = len(LOG)
    press(f"toolbar.button {TB_RESIZE}")
    lay = wait(dbg, c, lambda lay: "rzbar" in lay.r and "rzmib" in lay.r)
    if not chk(res, "rzbar" in lay.r, "Resize... opens the dialog with its bar"):
        print("    disks said:", [ln for ln in LOG[mark:] if "layout" not in ln])
        return
    chk(res, said("why=Shrinking a tfs3 volume is not built yet", mark), "the kernel says why it cannot shrink")

    # Layout: everything inside the box, nothing on top of anything else.
    box = lay.r["rzbox"]
    parts = ["rzbar", "rzcaption", "rzlsize", "rzsize", "rzlafter", "rzafter", "dialog.button 0", "dialog.button 1"]
    out = [p for p in parts if p not in lay.r or not inside(lay.r[p], box)]
    chk(res, not out, f"every part of the dialog lies inside its box {box} (outside: {[(p, lay.r.get(p)) for p in out]})")
    stack = ["rzbar", "rzcaption", "rzsize", "rzafter"]
    clash = [(a, b) for i, a in enumerate(stack) for b in stack[i + 1:] if a in lay.r and b in lay.r
             and overlaps(lay.r[a], lay.r[b])]
    chk(res, not clash, f"the bar, caption and size rows do not overlap ({clash})")
    chk(res, lay.r.get("rzmib") == P1_MIB and lay.r.get("rzaftermib") == ROOM_MIB,
        f"the fields start at {P1_MIB} MiB and {ROOM_MIB} MiB after it "
        f"({lay.r.get('rzmib')}, {lay.r.get('rzaftermib')})")

    # Pixels, before the drag.
    bx, by, bw, bh = lay.r["rzbar"]
    hx, hy, hw, hh = lay.r["rzhandle"]
    ox, oy = lay.cx, lay.cy
    mid = oy + by + bh // 2
    img = shot(qmp, keep, "resize_dialog.png")
    # Its top rows: the label is drawn across the middle of it.
    hatch = {px(img, ox + bx + 6 + i, oy + by + 4) for i in range(16)}
    chk(res, len(hatch) >= 2 and all(bluish(p) for p in hatch),
        f"the used part is hatched: alternating blue tints ({sorted(hatch)})")
    handle0 = px(img, ox + hx + 2, oy + hy + hh // 3)
    chk(res, bluish(handle0), f"the handle is the accent ({handle0})")
    free0 = px(img, ox + hx + hw + 12, mid)
    chk(res, white(free0), f"the room after it is white ({free0})")
    ctrl = (ox + box[0] - 12, oy + box[1] + box[3] // 2)   # the scrim, left of the box
    ctrl0 = px(img, *ctrl)

    # Drag the handle 30% of the bar to the right.
    x0, y0 = ox + hx + hw // 2, oy + hy + hh // 2
    dbg.drag_real(qmp, x0, y0, x0 + bw * 3 // 10, y0, steps=6)
    # The pointer's sprite would sit on the handle it dragged: park it on
    # the box's title row, where nothing hovers.
    dbg.warp_cursor(qmp, ox + box[0] + box[2] - 8, oy + box[1] + 6)
    lay = wait(dbg, c, lambda lay: lay.r.get("rzend", P1_MIB) > P1_MIB)
    end = lay.r.get("rzend")
    chk(res, end and end > P1_MIB, f"dragging the handle grows the size ({P1_MIB} -> {end} MiB)")
    chk(res, lay.r.get("rzmib") == end and lay.r.get("rzmib", 0) + lay.r.get("rzaftermib", 0) == P1_MIB + ROOM_MIB,
        f"the fields follow: {lay.r.get('rzmib')} + {lay.r.get('rzaftermib')} = {P1_MIB + ROOM_MIB} MiB")
    hx1 = lay.r["rzhandle"][0]
    img = shot(qmp, keep, "resize_dragged.png")
    # Beside its middle column, where a focused handle draws its grip line.
    moved_to = px(img, ox + hx1 + 2, oy + hy + hh // 3)
    left = px(img, ox + hx + 2, oy + hy + hh // 3)
    chk(res, hx1 > hx and bluish(moved_to) and left != handle0,
        f"the handle is drawn where it went ({hx} -> {hx1}: {moved_to}; its old place now {left})")
    chk(res, px(img, *ctrl) == ctrl0, f"a pixel outside the box did not change ({ctrl0})")

    # Type a size, and Resize.
    press("rzsizenum")
    dbg.key("0x01")                          # select all
    for ch in "128m":
        dbg.key(ord(ch), settle=False)
    mark = len(LOG)
    dbg.key("0x0a")                          # Enter: the field takes it, then Resize
    lay = wait(dbg, c, lambda lay: said("queued Resize", mark))
    chk(res, any("queued Resize virtio0p1" in ln and "to 128.0 MiB" in ln for ln in LOG[mark:]),
        "Resize queues the typed 128 MiB")
    chk(res, "128.0M" not in dbg.send("sh lsblk"), "...and the disk is untouched until Apply (lsblk)")
    chk(res, apply_all() and said("apply step 1 -> 0", mark), "Apply runs it, and it succeeds")
    out = dbg.send("sh lsblk")
    chk(res, "virtio0p1" in out and "128.0M" in out, "lsblk shows virtio0p1 at 128 MiB")
    out = dbg.send("sh mount virtio0p1 /mnt/r") + dbg.send("sh fsck /mnt/r") + \
        dbg.send("sh mkfiles --verify /mnt/r/a 40 400000-500000") + dbg.send("sh df") + dbg.send("sh umount /mnt/r")
    chk(res, "fsck: clean." in out, "it checks clean")
    chk(res, "verified 40 file(s)" in out, "every file verifies")

    # The FAT32 partition: a bar, the warning that it has no journal, and a grow.
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    press("map.seg 2")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 2)
    mark = len(LOG)
    press(f"toolbar.button {TB_RESIZE}")
    lay = wait(dbg, c, lambda lay: "rzbar" in lay.r and "rzmib" in lay.r)
    chk(res, "rzbar" in lay.r and lay.r.get("rzmib") == FAT_MIB, f"on FAT32, Resize... opens the bar at {FAT_MIB} MiB")
    chk(res, said("FAT32 has no journal", mark), "...with the note that FAT32 keeps no journal")
    out = [p for p in ("rzbar", "rzsize", "dialog.button 0") if p not in lay.r or not inside(lay.r[p], lay.r["rzbox"])]
    chk(res, not out, f"...and its parts inside the box (outside: {out})")
    shot(qmp, keep, "resize_fat32.png")
    press("rzsizenum")
    dbg.key("0x01")
    for ch in "64m":
        dbg.key(ord(ch), settle=False)
    mark = len(LOG)
    dbg.key("0x0a")
    lay = wait(dbg, c, lambda lay: said("queued Resize", mark))
    chk(res, apply_all() and said("apply note FAT32 has no journal", mark),
        "Apply's confirm repeats that FAT32 keeps no journal, and the grow runs")
    out = dbg.send("sh lsblk") + dbg.send("sh mount virtio0p2 /mnt/r") + dbg.send("sh fsck /mnt/r") + \
        dbg.send("sh df") + dbg.send("sh umount /mnt/r")
    chk(res, "64.0M" in out and "fsck: clean." in out, "lsblk shows it at 64 MiB and it checks clean")

    # The exFAT partition: a reason, not a bar.
    lay = wait(dbg, c, lambda lay: "dialog.button 0" not in lay.r)
    press("map.seg 4")
    lay = wait(dbg, c, lambda lay: lay.state["seg"] == 4)
    mark = len(LOG)
    press(f"toolbar.button {TB_RESIZE}")
    lay = wait(dbg, c, lambda lay: "dialog.button 0" in lay.r)
    chk(res, said("exFAT volumes cannot be resized yet", mark) and "rzbar" not in lay.r,
        "on exFAT, Resize... says why it cannot, with no bar")
    shot(qmp, keep, "resize_exfat.png")
    press("dialog.button 0")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--keep", metavar="DIR", help="keep the images and screenshots here")
    args = ap.parse_args()
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    if not shutil.which("sfdisk"):
        print("disks_resize_test: needs sfdisk (util-linux) on the host")
        return 1
    tmp = args.keep or tempfile.mkdtemp(prefix="disksresize-")
    os.makedirs(tmp, exist_ok=True)
    try:
        return run_in(args, tmp)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)


def run_in(args, tmp):
    root = os.path.join(tmp, "root.img")
    blank = os.path.join(tmp, "blank.img")
    copy_disk("disk.img", root, cwd=REPO)
    res = Results()
    ok, why = install_grub.add_boot_word(root, "root=ata0p3")
    if not chk(res, ok, f"the system disk is pinned as the root {why}"):
        return 1
    with open(blank, "wb") as f:
        f.truncate(BLANK_BYTES)
    subprocess.run(["sfdisk", "--quiet", blank], input=SFDISK_SCRIPT.encode(), check=True)

    def vm(*argv):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(args.instance),
                            "--disk", root, "--extra-disk", blank, "--extra-disk-kind", "virtio",
                            *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=600)
        return r.stdout + r.stderr

    vm("stop")
    if not chk(res, "ready" in vm("start"), "the guest came up"):
        return 1
    try:
        out = vm("exec", "mkfs virtio0p1 confirm", "mkfs -t fat32 virtio0p2 confirm",
                 "mkfs -t exfat virtio0p3 confirm", "mkdir /mnt/r",
                 "mount virtio0p1 /mnt/r", "mkdir /mnt/r/a", "mkfiles /mnt/r/a 40 400000-500000", "umount /mnt/r")
        if not chk(res, "created 40 file(s)" in out, "a 64 MiB tfs3 volume holds 40 files, ~18 MiB"):
            print(out)
            return 1
        qmp = QMPSession(port=port_guard.instance_qmp(args.instance))
        sock = port_guard.instance_sock(args.instance)
        enter_gui(qmp, sock)
        dbg = DebugConsole(sock)
        try:
            run(dbg, qmp, res, tmp)
        finally:
            dbg.close()
    finally:
        vm("stop")
    return res.finish("disks_resize_test")


if __name__ == "__main__":
    sys.exit(main())
