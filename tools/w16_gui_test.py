#!/usr/bin/env python3
"""w16 running Win16 programs in toywm (docs/w16-design.md), checked by pixel.

The programs are the project's own NASM samples (userland/w16/samples/,
built by tools/gen_ne.py), so the test ships with the tree; no Microsoft
file is involved.

1. **gdi.exe draws everything where it put it** (its header lists the
   places): a string-table string, a bitmap resource put with
   SetDIBitsToDevice and through a memory DC with BitBlt -- SRCCOPY the
   same colours, NOTSRCCOPY every one inverted, and the DIB's rows the
   right way up -- a pen line that stops short of its end point, a brush
   rectangle inside a pen edge, text copied through the global and local
   heaps, and a square that appears only once its timer has fired three
   times.
2. **The window controls**: Alt+Space opens w16's own control menu (a
   popup), X maximizes -- the window takes the work area and the frame
   loses its border, so the caption reaches the top-left pixel -- and
   Alt+Space, Enter restores the size it had.
3. **A fault is told**: fault.exe's click is a general protection fault;
   the Application Error box opens, Enter closes it, the program ends,
   and its crash report's message names the fault and where.

Every colour is compared with a tolerance of 0: the client buffer is
32-bit and nothing between it and the screendump blends.
"""
import argparse
import sys
import time

from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

GDI_TITLE = "GDI test (Win16)"
FAULT_TITLE = "Fault (Win16)"
FRAME, CAPTION = 4, 20     # userland/w16/w16_internal.h's W16_FRAME, W16_CAPTION

RED, GREEN, BLUE, WHITE = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)
YELLOW, CYAN, MAGENTA, BLACK = (255, 255, 0), (0, 255, 255), (255, 0, 255), (0, 0, 0)
NAVY = (0, 0, 128)


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path)
    with Image.open(path) as im:
        return im.convert("RGB")


def inv(c):
    return tuple(255 - v for v in c)


def gdi_checks(dbg, qmp, res, tmp):
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    res.check("gdi.exe opens its window", win is not None, "no window")
    if not win:
        return None
    ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION

    # The timer's square, or 5 s of waiting for it.
    im = None
    end = time.time() + 5
    while time.time() < end:
        im = shot(qmp, f"{tmp}/w16_gdi.png")
        if im.getpixel((ox + 103, oy + 53)) == GREEN:
            break
        time.sleep(0.3)
    px = lambda x, y: im.getpixel((ox + x, oy + y))
    res.check("the timer fired three times: the square is green", px(103, 53) == GREEN,
              f"got {px(103, 53)}")

    # The bitmap: its top half red, green, blue, white in 4-pixel bands,
    # its bottom half yellow, cyan, magenta, black.
    top, bottom = [RED, GREEN, BLUE, WHITE], [YELLOW, CYAN, MAGENTA, BLACK]
    def bitmap_at(x0, invert):
        got = [px(x0 + 4 * i + 1, 31) for i in range(4)] + [px(x0 + 4 * i + 1, 35) for i in range(4)]
        want = [inv(c) if invert else c for c in top + bottom]
        return got == want, f"got {got} want {want}"
    ok, why = bitmap_at(16, False)
    res.check("SetDIBitsToDevice: the bitmap's colours, top row first", ok, why)
    ok, why = bitmap_at(40, False)
    res.check("CreateDIBitmap + memory DC + BitBlt SRCCOPY: the same colours", ok, why)
    ok, why = bitmap_at(64, True)
    res.check("BitBlt NOTSRCCOPY: every colour inverted", ok, why)

    res.check("the red pen line runs x 16..79 and stops short of 80",
              px(16, 50) == RED and px(79, 50) == RED and px(80, 50) == WHITE,
              f"16:{px(16, 50)} 79:{px(79, 50)} 80:{px(80, 50)}")
    res.check("Rectangle: a black edge, a blue fill, nothing past its right edge",
              px(16, 56) == BLACK and px(47, 71) == BLACK and px(30, 64) == BLUE
              and px(48, 64) == WHITE,
              f"edge {px(16, 56)}/{px(47, 71)} fill {px(30, 64)} outside {px(48, 64)}")
    dark = lambda y: sum(1 for x in range(4, 120) if sum(px(x, y + 0)) < 200)
    res.check("the string-table string and the heaps' text are drawn",
              any(dark(y) for y in range(4, 16)) and any(dark(y) for y in range(80, 92)),
              "a text row has no dark pixel")
    return win


def window_controls(dbg, qmp, res, win):
    w0, h0 = win["w"], win["h"]
    dbg.key("0x20", mods="alt")
    time.sleep(0.5)
    popups = [w for w in dbg.windows() if w["popup"]]
    res.check("Alt+Space opens w16's control menu", len(popups) == 1, f"popups={len(popups)}")
    dbg.key("x")
    end = time.time() + 5
    cur = win
    while time.time() < end:
        cur = dbg.window(GDI_TITLE)
        if cur and cur["state"] == "maximized" and cur["w"] > w0:
            break
        time.sleep(0.2)
    res.check("X maximizes the window", cur and cur["state"] == "maximized" and cur["w"] > w0,
              f"{cur and cur['state']} {cur and cur['w']}x{cur and cur['h']}")
    dbg.settle()
    im = shot(qmp, f"{TMP}/w16_max.png")
    res.check("maximized, the frame has no border: the caption reaches the top-left pixel",
              im.getpixel((cur["x"] + 2, cur["y"] + 2)) == (192, 192, 192)
              and im.getpixel((cur["x"] + 40, cur["y"] + 4)) == NAVY,
              f"corner {im.getpixel((cur['x'] + 2, cur['y'] + 2))} "
              f"caption {im.getpixel((cur['x'] + 40, cur['y'] + 4))}")
    dbg.key("0x20", mods="alt")
    time.sleep(0.5)
    dbg.key("0x0a")
    end = time.time() + 5
    while time.time() < end:
        cur = dbg.window(GDI_TITLE)
        if cur and cur["state"] == "normal" and (cur["w"], cur["h"]) == (w0, h0):
            break
        time.sleep(0.2)
    res.check("Alt+Space, Enter restores the size it had",
              cur and cur["state"] == "normal" and (cur["w"], cur["h"]) == (w0, h0),
              f"{cur and cur['state']} {cur and (cur['w'], cur['h'])} want {(w0, h0)}")
    dbg.close_window(GDI_TITLE)


def fault(dbg, res):
    win = dbg.spawn("/bin/w16 /usr/share/w16/fault.exe", FAULT_TITLE)
    res.check("fault.exe opens its window", win is not None, "no window")
    if not win:
        return
    before = set((dbg.send("sh ls /var/crash") or "").split())
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    end = time.time() + 5
    while time.time() < end and not any(w["popup"] for w in dbg.windows()):
        time.sleep(0.2)
    res.check("a click faults, and the Application Error box opens",
              any(w["popup"] for w in dbg.windows()), "no popup")
    dbg.key("0x0a")
    end = time.time() + 10
    while time.time() < end and dbg.window(FAULT_TITLE):
        time.sleep(0.3)
    res.check("Enter closes the box and the program ends", dbg.window(FAULT_TITLE) is None,
              "the window is still there")
    new = [f for f in (dbg.send("sh ls /var/crash") or "").split()
           if f.startswith("w16-") and f.endswith(".crash") and f not in before]
    out = dbg.send(f"sh crashlog {new[-1]}") if new else ""
    res.check("its crash report's message names the fault and where",
              "fault.exe: general protection fault at 0001:" in (out or ""),
              f"new reports {new}; crashlog said {(out or '')[:200]!r}")


TMP = "/tmp"


def main():
    global TMP
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--tmp", default="/tmp", help="where screendumps go")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "w16_gui_test")
    TMP = args.tmp
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        win = gdi_checks(dbg, qmp, res, TMP)
        if win:
            window_controls(dbg, qmp, res, win)
        fault(dbg, res)
    finally:
        dbg.close()
    print(f"\nw16_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
