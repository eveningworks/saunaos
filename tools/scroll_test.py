#!/usr/bin/env python3
"""Scrolling, as a person does it: the wheel goes the right way, the scrollbar thumb follows the view, a click in
the track pages and a drag of the thumb moves the view WITH it.

WHY THIS EXISTS. The Retro Launcher's grid drew its scrollbar thumb MIRRORED once a folder held a few hundred
programs (the thumb sat at the foot with the view at the top, and the track and the drag ran backwards): the painter
counts a vertical bar's offset from the BOTTOM, as a terminal's scrollback does, and three widgets passed it their
position from the TOP. Nothing looked at it, because every widget with room for all its rows has no bar to look at.
**A NEW SCROLLING VIEW IS NOT DONE UNTIL THIS KIND OF CHECK HAS RUN ON IT** (userland/CLAUDE.md, rule 10).

THE INVARIANT, asked of each view, with the pointer parked off the bar:
  - the view at its two ends (Home and End where a view takes keys; where it does not, the view as it OPENS, at the
    top, and a long wheel run to the other end) has the THUMB, read from pixels at the strip's edge, at the top and
    at the foot;
  - from the top, the wheel DOWN moves the thumb down and the wheel UP moves it back;
  - a click in the track BELOW the thumb moves it down, and ABOVE moves it up;
  - dragging the thumb to the middle puts it there, and the view (checked by End/Home's thumb) was not left at an end.

  python3 tools/scroll_test.py
Disks' pending list (ui/uui_steplist, a uui_table) runs check_view() from tools/disks_test.py, which has the second
disk a queue needs. Positive control (mutate.py): drop one uui_scrollbar_flip() from uui_grid.c's draw -- the Launcher's thumb check
reddens. The Launcher's Details table is a uui_table, whose bar is uui_sbar's.
"""
import argparse
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

K_HOME, K_END = "0xf786", "0xf787"
PROGRAMS = 60
FOLDER = "/home/w16many"


def thumb(im, wx, wy, ww, wh):
    """(top, bottom) of the scrollbar thumb at the right edge of the widget's rect, or None. The column's commonest
    colour is the ground; the thumb is the longest run of anything clearly different."""
    x = min(wx + ww - 2, im.width - 1)
    col = [im.getpixel((x, y)) for y in range(wy, min(wy + wh, im.height))]
    ground = Counter(col).most_common(1)[0][0]
    best = None
    start = None
    for i, px in enumerate(col + [ground]):
        far = sum(abs(a - b) for a, b in zip(px, ground)) > 60
        if far and start is None:
            start = i
        elif not far and start is not None:
            if best is None or i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    if not best or best[1] - best[0] < 12:
        return None
    return best


def where(t, wh):
    """The thumb's centre as a fraction of the track, 0 top .. 1 foot."""
    return ((t[0] + t[1]) / 2) / wh


def shot(qmp, tmp):
    from PIL import Image
    p = os.path.join(tmp, f"scroll_{os.getpid()}.png")
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def check_view(dbg, qmp, res, tmp, label, title, widget, click_at, park, keys, top_inset=0, warp=False):
    """The invariant above for one widget. `click_at` is a point inside it that focuses it; `keys` says Home and End
    reach it (the Retro Launcher's grid takes no keys, so there the view's own starting position is the independent
    fact: it opens at the TOP, and its thumb must say so). `top_inset`: rows above the track, a table's header --
    with a few rows the thumb is long, and measured from the header its centre is never near the top. `warp` parks
    the REAL pointer (QMP): the wheel goes to the widget under it, and `gui move` lasts one compositor iteration, so a
    view that does not fill the window's middle never hears the wheel without it."""
    w = poll(lambda: dbg.widgets(title).get(widget), 10, 0.3)
    if not res.check(f"{label}: the view opens", w is not None, f"widgets {list(dbg.widgets(title))}"):
        return
    sx, sy, ww, wh = w["screen"]["x"], w["screen"]["y"] + top_inset, w["w"], w["h"] - top_inset
    cx, cy = click_at(sx, sy, ww, wh)
    dbg.click(cx, cy)
    px, py = park(sx, sy, ww, wh)

    def read(settle=0.6):
        if warp:
            dbg.warp_cursor(qmp, px, py)
        else:
            dbg.move(px, py)
        dbg.settle()
        time.sleep(settle)
        return thumb(shot(qmp, tmp), sx, sy, ww, wh)

    def go(end):
        if keys:
            dbg.key(K_END if end else K_HOME)
        else:                                  # a long wheel run: clamps at the end it is going to
            for _ in range(15):
                dbg.wheel(-10 if end else 10, settle=False)
        dbg.settle()

    # Where it opens: the beginning. (A tail-following log is the exception: Home first.)
    if keys:
        go(False)
    top = read()
    if not res.check(f"{label}: there is a scrollbar thumb to read", top is not None, "no thumb at the right edge"):
        return
    res.check(f"{label}: at the top the thumb is at the top", where(top, wh) < 0.3, f"thumb {top} of {wh}")
    go(True)
    foot = read()
    res.check(f"{label}: at the end the thumb is at the foot", foot is not None and where(foot, wh) > 0.7,
              f"thumb {foot} of {wh}")
    go(False)
    top = read()

    for _ in range(4):
        dbg.wheel(-3, settle=False)
    dbg.settle()
    down = read()
    res.check(f"{label}: the wheel down moves the thumb down", down is not None and top is not None and
              where(down, wh) > where(top, wh) + 0.02, f"{top} -> {down}")
    for _ in range(2):
        dbg.wheel(3, settle=False)
    dbg.settle()
    up = read()
    res.check(f"{label}: the wheel up moves it back", up is not None and down is not None and
              where(up, wh) < where(down, wh) - 0.01, f"{down} -> {up}")

    # a click in the track below the thumb pages DOWN; above it pages UP. In PIXELS: a long list's page is a
    # sliver of the track (the Log Viewer's, with a few thousand lines), so a fraction of it is no threshold
    go(False)
    top = read()
    if top:
        track_top = top[0]
        bx = sx + ww - 2
        dbg.click(bx, sy + min(wh - 6, top[1] + 30))
        paged = read()
        res.check(f"{label}: a click in the track below the thumb moves it down",
                  paged is not None and paged[0] > top[0] + 2, f"{top} -> {paged}")
        if paged and paged[0] - track_top > 6:
            dbg.click(bx, sy + (track_top + paged[0]) // 2)
            back = read()
            res.check(f"{label}: a click in the track above the thumb moves it up",
                      back is not None and back[0] < paged[0] - 2, f"{paged} -> {back}")

    # drag the thumb from the top to about the middle of the track
    go(False)
    top = read()
    if top:
        bx = sx + ww - 2
        y0 = sy + (top[0] + top[1]) // 2
        dbg.drag(bx, y0, bx, y0 + wh // 2, steps=6)
        mid = read()
        res.check(f"{label}: dragging the thumb down moves it down, with the pointer",
                  mid is not None and 0.3 < where(mid, wh) < 0.9, f"{top} -> {mid}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "scroll_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("scrolling: the wheel, the thumb, the track and the drag")
    try:
        # --- the Retro Launcher's program grid (ui/uui_grid) with sixty programs -----------------------------
        dbg.send(f"sh mkdir {FOLDER}")
        for i in range(PROGRAMS):
            dbg.send(f"sh cp /usr/share/w16/hello.exe {FOLDER}/p{i:02d}.exe")
        dbg.send("sh write /etc/win16.conf folders=1")
        dbg.send(f"sh append /etc/win16.conf folder1={FOLDER}")
        win = dbg.spawn("/bin/wm/apps/retrolauncher", "Retro Launcher")
        if res.check("the Retro Launcher opens", win is not None, "no window"):
            time.sleep(15)       # programs are examined a few a tick
            check_view(dbg, qmp, res, args.tmp, "Retro Launcher grid", "Retro Launcher", "programs",
                       lambda sx, sy, w, h: (sx + 60, sy + 40), lambda sx, sy, w, h: (sx + w // 2, sy - 20), keys=False)
            dbg.close_window("Retro Launcher")

        # --- the Retro Launcher's Details view (ui/uui_table), the same sixty programs -------------------------
        dbg.send("sh write /etc/retrolauncher.conf view=details")
        win = dbg.spawn("/bin/wm/apps/retrolauncher", "Retro Launcher")
        if res.check("the Retro Launcher opens in Details", win is not None, "no window"):
            time.sleep(15)
            check_view(dbg, qmp, res, args.tmp, "Retro Launcher details", "Retro Launcher", "programs-table",
                       lambda sx, sy, w, h: (sx + 60, sy + 60), lambda sx, sy, w, h: (sx + w // 2, sy - 20), keys=True)
            dbg.close_window("Retro Launcher")
        dbg.send("sh rm /etc/retrolauncher.conf")

        # --- the Log Viewer's list (ui/uui_loglist), on the kernel log -----------------------------------------
        win = dbg.spawn("/bin/wm/apps/logview", "Log Viewer")
        if res.check("the Log Viewer opens", win is not None, "no window"):
            time.sleep(3)
            check_view(dbg, qmp, res, args.tmp, "Log Viewer list", "Log Viewer", "log",
                       lambda sx, sy, w, h: (sx + 120, sy + 60), lambda sx, sy, w, h: (sx + w // 2, sy - 20), keys=True)
            dbg.close_window("Log Viewer")
    finally:
        dbg.send(f"sh rm -r {FOLDER}")
        dbg.close()
    return res.finish("scroll_test")


if __name__ == "__main__":
    sys.exit(main())
