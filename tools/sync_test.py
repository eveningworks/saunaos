#!/usr/bin/env python3
"""`gui sync` and settle(): after a click and settle(), the frame the click caused is ALREADY on the screen.

WHY THIS EXISTS. settle() used to wait for the window manager to DELIVER injected input, then sleep a fixed grace
and hope the client had drawn by then -- the commonest way a GUI test here went red one run in five: under load
the guess was short. Now it asks (`gui sync`, abi/win_proto.h's WIN_EV_SYNC): the clients holding the keyboard
and the pointer answer once everything queued before the sync is handled and presented, and the window manager
calls it done once that frame is composited.

THE CHECK, which an early answer cannot pass: the counter demo's label reads "Clicked N times". Twenty times:
click Add, settle(0) -- the sync and NO grace, which would hide an early answer -- and dump the screen at once (no
settled-frame wait): the label's pixels must differ from the last dump every time. A sync answered before the
frame (or no sync at all) shows the old label now and then.

  python3 tools/sync_test.py [--instance N] [--positive-control]

--positive-control settles the OLD way with no grace (delivered, not drawn) and must see a stale label at least
once; a control that stays green means the check cannot see the race on this host.
"""
import argparse
import os
import statistics
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

CLICKS = 20


def label_pixels(qmp, path, rect):
    from PIL import Image
    qmp.screenshot(path, settle=0, stable=False)
    im = Image.open(path).convert("RGB")
    x, y, w, h = rect
    return im.crop((x, y, x + w, y + h)).tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="settle the old way with no grace; must see a stale frame")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sync_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    tmp = tempfile.mkdtemp(prefix="synctest-")
    shot = os.path.join(tmp, "label.png")
    control = args.positive_control
    print("sync: a click's frame is on the screen when settle() returns" + (" (POSITIVE CONTROL)" if control else ""))
    try:
        win = dbg.spawn("/bin/wm/demos/counter", "Counter")
        if not res.check("the counter opens", win is not None, "no window"):
            return res.finish("sync_test")
        ws = poll(lambda: dbg.widgets("Counter") if "label" in dbg.widgets("Counter") else None, 10, 0.3)
        if not res.check("it names its Add button and label", ws is not None and "add" in ws, f"{ws and list(ws)}"):
            return res.finish("sync_test")
        add, lab = ws["add"], ws["label"]
        ax, ay = add["screen"]["x"] + add["w"] // 2, add["screen"]["y"] + add["h"] // 2
        rect = (lab["screen"]["x"], lab["screen"]["y"], lab["w"], lab["h"])
        dbg.settle()
        before = label_pixels(qmp, shot, rect)
        stale, waits = 0, []
        for _ in range(CLICKS):
            t0 = time.time()
            dbg.click(ax, ay, settle=False)
            # NO GRACE either way: the grace would hide a sync that answered early.
            dbg.settle(0, sync=not control)
            waits.append(time.time() - t0)
            now = label_pixels(qmp, shot, rect)
            stale += now == before
            before = now
        print(f"  settle took {statistics.median(waits) * 1000:.0f} ms median, {max(waits) * 1000:.0f} ms at most")
        if control:
            res.check(f"POSITIVE CONTROL: the old wait shows a stale label at least once ({stale} of {CLICKS})",
                      stale > 0, "the check cannot see the race on this host")
        else:
            res.check(f"every one of {CLICKS} clicks' labels is on the screen when settle() returns",
                      stale == 0, f"{stale} stale")
            res.check("the sync answered (not the fallback grace)", dbg.sync(), "no answer from the counter")
        dbg.close_window("Counter")
    finally:
        dbg.close()
    return res.finish("sync_test")


if __name__ == "__main__":
    sys.exit(main())
