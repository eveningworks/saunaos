#!/usr/bin/env python3
"""tools/half_frame_test.py -- no half-painted window reaches the screen.

WHAT IS UNDER TEST
------------------
The buffer handoff between a client and the compositor. A client draws
into one buffer while the compositor shows another, and may draw into a
buffer only once the compositor has handed it back (WIN_EV_BUF_RELEASE,
Wayland's wl_buffer.release). Without that, a client that presents and
at once repaints the buffer a preempted composite is still reading puts
a HALF-PAINTED frame on screen -- the File Manager "flashed" while
scrolling: the toolbar and tree drawn, the icon pane bare window grey.

HOW: the File Manager on /bin, icons view, scrolled up and down with the
real wheel while the screen is dumped as fast as QMP allows. A frame is
half-painted when a large part of the icon pane is the window's plain
grey (236,236,236) -- the pane itself is white, so a finished frame has
almost none of it.

THE CONTROL is that the pane MOVED: frames are counted that differ from
the one before. A scroll that never happened would pass the main check
on a still picture. A frame can only be half-painted around a repaint,
so the check's power is the MOVED count, not the frames taken: it
captures until MOVED_WANT frames have moved, up to MAX_FRAMES. A fixed
240 gave 29-32 moved alone and 14-16 beside fifteen other guests, under
a floor of 20 -- the suite's load, not the File Manager, failed it.

POSITIVE CONTROL, verified with tools/mutate.py: surf_back()
(ui/uapp_surface.c) starting from `int pick = s->front;` -- every frame
drawn into the buffer on screen -- turns this red, 85 of 241 frames
half-painted under TCG. Making it merely ignore `busy` NO LONGER DOES
(0 of 240, before and after the moved-frame loop): with three buffers
the lowest non-front one is in practice released already.

Run by gui_regress.py, which boots the guest and passes --instance.
"""
import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image                                  # noqa: E402
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
from harness import Results                            # noqa: E402
import port_guard                                      # noqa: E402

MOVED_WANT = 40       # moved frames to observe before stopping
MOVED_MIN = 20        # the control's floor
MAX_FRAMES = 960      # the hang guard: 4x the old fixed count
GREY = (236, 236, 236)     # utheme panel_bg: what a cleared, unpainted window shows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "half_frame_test")

    qmp = QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = tempfile.mkdtemp(prefix="halfframe-")
    try:
        dbg.send("gui spawn /bin/wm/apps/files /bin")
        win = None
        for _ in range(40):
            ws = [w for w in dbg.json("gui windows --json").get("windows", [])
                  if w.get("title") == "File Manager"]
            if ws:
                win = ws[-1]
                break
            time.sleep(0.5)
        if not res.check("the File Manager opened on /bin", win is not None):
            return res.finish("half_frame_test")
        dbg.settle()
        time.sleep(2.0)   # the listing and its icons, before the first capture
        c = win["content"]
        dbg.warp_cursor(qmp, c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
        dbg.settle()

        # The icon pane: right of the places tree, below the toolbars,
        # above the status bar. Sampled small -- the test is a fraction.
        box = (c["x"] + 260, c["y"] + 60, c["x"] + c["w"] - 30, c["y"] + c["h"] - 30)
        ppm = os.path.join(tmp, "frame.ppm")
        half, moved, prev, first_half = 0, 0, None, None
        frames = 0
        for i in range(MAX_FRAMES):
            if moved >= MOVED_WANT:
                break
            frames += 1
            if i % 6 == 0:
                qmp.wheel("down" if (i // 36) % 2 == 0 else "up", notches=3, delay=0.02)
            qmp._cmd({"execute": "screendump", "arguments": {"filename": ppm}})
            im = Image.open(ppm).convert("RGB").crop(box).resize((140, 100))
            cur = im.tobytes()
            if prev is not None and cur != prev:
                moved += 1
            prev = cur
            grey = next((n for n, col in im.getcolors(140 * 100) if col == GREY), 0)
            if grey > 0.2 * 140 * 100:
                half += 1
                if first_half is None:
                    first_half = os.path.join(tmp, f"half{i}.png")
                    Image.open(ppm).save(first_half)

        res.check("the pane really scrolled (the control)", moved >= MOVED_MIN,
                  f"{moved} of {frames} frames moved")
        res.check("no half-painted frame reached the screen", half == 0,
                  f"{half} of {frames}" + (f", first saved as {first_half}" if first_half else ""))
    finally:
        dbg.close()
    return res.finish("half_frame_test")


if __name__ == "__main__":
    sys.exit(main())
