"""tools/tests/gui/sdl_window_test.py -- an SDL3 program's window on the
desktop: drawn, scaled, driven, closed (docs/sdl-design.md, stage 1).

`/tests/sdlwin_test` opens a 160x100 SDL window, paints red | blue |
green bands in POINTS, and logs every event SDL hands it. Asserted, at
display.scale 100% and then 125%:

  * the window's content is the SDL size times the scale (160x100,
    then 200x125);
  * the bands are on screen. At 125% the picture is scaled SHARP
    BILINEAR: the red|blue edge lands on a pixel edge (80 -> 100) and
    stays unblended, the blue|green edge lands inside a pixel (121 ->
    151.25) and blends one column. Plain bilinear would blend both,
    nearest neither -- so each scaler that is not the chosen one fails
    a different check;
  * a click arrives as an SDL button event at the point in SDL's
    coordinates (points: 40,20 at 125% for a click at pixel 50,25);
    the secondary button is SDL's right; the wheel is SDL's wheel;
  * a REAL key press (QMP send-key, the keyboard's own path) arrives as
    its SDL scancode, down and up, and as SDL text;
  * the window is SaunaOS's own SDL video driver, "saunaos";
  * with the left button HELD, the right button's press and release are
    SDL's right only -- the left comes up once, when it is let go;
  * the pointer leaving the window is SDL's mouse-leave;
  * F (the program's fullscreen key) fills the screen and SDL hears the
    screen's size in points; F again gives the FIXED window its own size
    back, and SDL hears that;
  * the title bar's close is SDL_EVENT_WINDOW_CLOSE_REQUESTED, and the
    program, which exits on it, is gone.

A CHORD IS ONE PRESS (docs/conventions/gui.md), checked at 100%:

  * left held, the MIDDLE's press and release are SDL's middle only --
    the left is not released early (toywm sent the middle's up as 0);
  * left held in this window, the right pressed over ANOTHER client's
    window goes to THIS one (the implicit grab), and the left's release
    still arrives; the other window hears nothing;
  * a left drag past the window's left edge is not a mouse-leave (a
    negative position is real while a button is held);
  * an event watcher that repaints, from inside the driver's pump, never
    runs inside itself (the driver does not pump from within its pump);
  * SDL_Init leaves the program's locale "C" with a decimal-comma
    region chosen (locale.region fi);
  * a BORDERLESS window's hit-test strip moves it (a client-started
    move), and its next click is an ordinary one: no "a press of button
    0x1, already down" from uapp (toywm's release named the left as still
    held).

    python3 tools/tests/gui/sdl_window_test.py [--instance N] [--in-gui]
"""

RUNNER = {
    "name": "sdlwindow",
    "covers": "an SDL3 window: scaled sharp bilinear at 125%, input in points and scancodes, close",
}

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui, _hold_setting  # noqa: E402
from qmp_test import QMPSession                                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402
from PIL import Image  # noqa: E402

TITLE = "SDL window test"
RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)


class Log:
    """The program's lines since the last mark()."""
    def __init__(self, dbg):
        self.dbg = dbg
        self.seen = 0
        self.pid = None

    def mark(self):
        self.seen = len(self.dbg.logs("sdlwin:", clear=False))

    def wait(self, text, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            lines = self.dbg.logs("sdlwin:", clear=False)[self.seen:]
            if any(text in ln for ln in lines):
                return True
            time.sleep(0.2)   # sleep-ok: polls the guest log for a line, bounded
        return False

    def tail(self):
        return self.dbg.logs("sdlwin:", clear=False)[self.seen:][-8:]


def spawn(dbg, log, args=""):
    """Start /tests/sdlwin_test; its pid goes on `log.pid` once it is up."""
    before = {p["pid"] for p in dbg.processes_named("sdlwin_test")}
    log.mark()
    dbg.send(f"gui spawn /tests/sdlwin_test {args}".rstrip())
    up = log.wait("ready")
    log.pid = next((p["pid"] for p in dbg.processes_named("sdlwin_test") if p["pid"] not in before), None)
    return up


def start(dbg, log, res, label):
    if not spawn(dbg, log):
        res.check(f"{label}: the SDL program opens its window", False, f"log: {log.tail()}")
        return None
    dbg.settle()
    win = dbg.window(TITLE)
    res.check(f"{label}: an SDL window is a desktop window with its title", win is not None,
              f"windows: {dbg.windows()}")
    res.check(f"{label}: ...opened by SaunaOS's SDL video driver", log.wait("driver saunaos", 1),
              f"log: {log.tail()}")
    return win


def fullscreen(dbg, qmp, log, res, label, scale):
    """F in, F out: the screen's size in points, then the window's own."""
    log.mark()
    qmp.send_key("f")
    ok = log.wait("resize")
    dbg.settle()
    win = dbg.window(TITLE)
    c = win["content"] if win else {"w": 0, "h": 0}
    want = f"resize {round(c['w'] / scale)} {round(c['h'] / scale)}"
    res.check(f"{label}: fullscreen fills the screen and SDL hears its size in points ({want})",
              ok and c["w"] > 160 * scale and log.wait(want, 2), f"content {c}, log: {log.tail()}")
    log.mark()
    qmp.send_key("f")
    ok = log.wait("resize 160 100")
    dbg.settle()
    win = dbg.window(TITLE)
    c = win["content"] if win else {}
    res.check(f"{label}: leaving it gives the fixed window its own size back, and SDL hears so",
              ok and (c.get("w"), c.get("h")) == (round(160 * scale), round(100 * scale)),
              f"content {c}, log: {log.tail()}")


def pixels(qmp, path, win, points):
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    c = win["content"]
    return [im.getpixel((c["x"] + x, c["y"] + y)) for x, y in points]


def close(dbg, log, res, label):
    log.mark()
    dbg.close_window(TITLE)
    res.check(f"{label}: the close button is SDL's close request", log.wait("close requested"),
              f"log: {log.tail()}")
    def alive():
        return any(p["pid"] == log.pid for p in dbg.processes_named("sdlwin_test"))
    deadline = time.time() + 5
    while alive() and time.time() < deadline:
        time.sleep(0.2)   # sleep-ok: waits for the process to exit, bounded
    res.check(f"{label}: ...and the program, which quits on it, is gone",
              log.pid is not None and not alive(), f"pid {log.pid}")


def buttons_seq(dbg, log, prefix="sdlwin:"):
    """'button N down|up' lines since the mark, without the position."""
    return [ln.split(prefix + " ", 1)[1].rsplit(" ", 2)[0] for ln in
            dbg.logs(prefix, clear=False)[log.seen:] if f"{prefix} button" in ln]


def odd_presses(dbg):
    """uapp's 'a press of button ..., already down' lines so far."""
    return [ln for ln in dbg.logs("uapp: pid", clear=False) if "already down" in ln]


def chords(dbg, qmp, log, res, c):
    """A chord is one press: the middle, another window, a drag off the edge."""
    log.mark()
    dbg.warp_cursor(qmp, c["x"] + 50, c["y"] + 50)
    for step in (("down", "left"), ("down", "middle"), ("up", "middle"), ("up", "left")):
        (qmp.mouse_down if step[0] == "down" else qmp.mouse_up)(step[1])
        dbg.settle()
    log.wait("button 1 up")
    seq = buttons_seq(dbg, log)
    res.check("left held: the middle's press and release are SDL's middle (2) only, the left "
              "comes up once at the end",
              seq == ["button 1 down", "button 2 down", "button 2 up", "button 1 up"], f"order: {seq}")

    log.mark()
    dbg.warp_cursor(qmp, c["x"] + 10, c["y"] + 50)
    qmp.mouse_down("left")
    dbg.settle()
    dbg.warp_cursor(qmp, c["x"] - 20, c["y"] + 50)
    dbg.warp_cursor(qmp, c["x"] - 30, c["y"] + 50)
    qmp.mouse_up("left")
    log.wait("button 1 up")
    lines = [ln.split("sdlwin: ", 1)[1] for ln in dbg.logs("sdlwin:", clear=False)[log.seen:]]
    up = next((i for i, ln in enumerate(lines) if ln.startswith("button 1 up")), None)
    leave = next((i for i, ln in enumerate(lines) if ln == "mouse leave"), None)
    res.check("a left drag past the window's left edge is not a mouse-leave while held",
              up is not None and (leave is None or leave > up), f"lines: {lines[-8:]}")

    # ANOTHER client window beside this one.
    blog = Log(dbg)
    dbg.send("gui spawn /tests/sdlwin_test tag=B")
    deadline = time.time() + 10
    while time.time() < deadline and not dbg.logs("sdlwin B: ready", clear=False):
        time.sleep(0.2)   # sleep-ok: polls the guest log for a line, bounded
    dbg.settle()
    try:
        other_window(dbg, qmp, log, res, blog)
    finally:
        dbg.close_window(TITLE + " B")


def other_window(dbg, qmp, log, res, blog):
    a, b = dbg.window(TITLE), dbg.window(TITLE + " B")
    if not res.check("a second SDL window opens beside the first", a is not None and b is not None,
                     f"windows: {[w['title'] for w in dbg.windows()]}"):
        return
    # Both open centred, one on the other: B goes to the right by its title bar.
    tx, ty = b["x"] + 30, b["y"] + (b["content"]["y"] - b["y"]) // 2
    dbg.drag_real(qmp, tx, ty, tx + b["w"] + 40, ty)
    dbg.settle()
    a, b = dbg.window(TITLE), dbg.window(TITLE + " B")

    def inside(w, x, y):
        return w["x"] <= x < w["x"] + w["w"] and w["y"] <= y < w["y"] + w["h"]

    def spot(mine, other):
        cc = mine["content"]
        for y in range(cc["y"] + 4, cc["y"] + cc["h"] - 4, 6):
            for x in range(cc["x"] + 4, cc["x"] + cc["w"] - 4, 6):
                if not inside(other, x, y):
                    return x, y
        return None

    pa, pb = spot(a, b), spot(b, a)
    if not res.check("...with a point of each the other does not cover", pa and pb, f"a {a}, b {b}"):
        return
    log.mark()
    blog.seen = len(dbg.logs("sdlwin B:", clear=False))
    dbg.warp_cursor(qmp, *pa)
    qmp.mouse_down("left")
    dbg.settle()
    dbg.warp_cursor(qmp, *pb)
    qmp.mouse_down("right")
    dbg.settle()
    qmp.mouse_up("right")
    dbg.settle()
    dbg.warp_cursor(qmp, *pa)
    qmp.mouse_up("left")
    log.wait("button 1 up", 3)
    seq = buttons_seq(dbg, log)
    bseq = [ln for ln in dbg.logs("sdlwin B:", clear=False)[blog.seen:] if "button" in ln]
    res.check("left held here, the right pressed over ANOTHER window is this window's, and "
              "the left still comes up",
              seq == ["button 1 down", "button 3 down", "button 3 up", "button 1 up"], f"order: {seq}")
    res.check("...and the other window hears no button at all", not bseq, f"B: {bseq}")


def watcher(dbg, qmp, log, res):
    log.mark()
    for k in ("a", "s", "d"):
        qmp.send_key(k)
    log.wait("key up sc=7 D")
    res.check("an event watcher repainting from inside the driver's pump never runs inside itself",
              not any("watch nested" in ln for ln in dbg.logs("sdlwin:", clear=False)[log.seen:]),
              f"log: {log.tail()}")


def locale_kept(dbg, qmp, log, res):
    """SDL_Init must not take the system's region for the program."""
    r = _hold_setting(dbg, "locale.region", "fi")
    try:
        ok = spawn(dbg, log) and log.wait("number ")
        line = next((ln for ln in dbg.logs("sdlwin:", clear=False)[log.seen:] if "number " in ln), "")
        res.check("with a decimal-comma region chosen, SDL_Init leaves the program in \"C\"",
                  ok and "number 1.5 locale C" in line, line)
        close(dbg, log, res, "locale")
    finally:
        if r:
            r()


def hit_test(dbg, qmp, log, res):
    """A borderless window's drag strip: a move the CLIENT starts."""
    clog = Log(dbg)
    title = TITLE + " C"
    clog.seen = len(dbg.logs("sdlwin C:", clear=False))
    dbg.send("gui spawn /tests/sdlwin_test tag=C borderless")
    deadline = time.time() + 10
    while time.time() < deadline and not dbg.logs("sdlwin C: ready", clear=False)[clog.seen:]:
        time.sleep(0.2)   # sleep-ok: polls the guest log for a line, bounded
    dbg.settle()
    w = dbg.window(title)
    if not res.check("a borderless SDL window opens", w is not None):
        return
    odd = len(odd_presses(dbg))
    x0, y0 = w["content"]["x"] + 80, w["content"]["y"] + 5
    dbg.drag_real(qmp, x0, y0, x0 + 60, y0 + 40)
    dbg.settle()
    moved = dbg.window(title)
    res.check("its hit-test strip moves it (a client-started move)",
              moved and (moved["x"] - w["x"], moved["y"] - w["y"]) == (60, 40),
              f"was {w['x']},{w['y']}, now {moved and (moved['x'], moved['y'])}")
    if moved:
        clog.seen = len(dbg.logs("sdlwin C:", clear=False))
        cc = moved["content"]
        dbg.warp_cursor(qmp, cc["x"] + 80, cc["y"] + 60)
        qmp.mouse_down("left")
        dbg.settle()
        qmp.mouse_up("left")
        dbg.settle()
        seq = [ln.split("sdlwin C: ", 1)[1].rsplit(" ", 2)[0]
               for ln in dbg.logs("sdlwin C:", clear=False)[clog.seen:] if "button" in ln]
        res.check("...and its next click is an ordinary press and release",
                  seq == ["button 1 down", "button 1 up"], f"order: {seq}")
        res.check("...which uapp heard as a NEW press: the move's release named nothing held",
                  len(odd_presses(dbg)) == odd, f"{odd_presses(dbg)[odd:]}")
    dbg.close_window(title)


def at_100(dbg, qmp, log, res, shot):
    win = start(dbg, log, res, "100%")
    if not win:
        return
    c = win["content"]
    res.check("100%: the window is the SDL size", (c["w"], c["h"]) == (160, 100), f"content {c}")
    got = pixels(qmp, shot, win, [(40, 50), (100, 50), (140, 50)])
    res.check("100%: the program's red, blue and green bands are on screen, exact",
              got == [RED, BLUE, GREEN], f"got {got}")

    log.mark()
    dbg.click(c["x"] + 40, c["y"] + 30)
    res.check("a click is an SDL button 1 press and release at its point",
              log.wait("button 1 down 40 30") and log.wait("button 1 up 40 30"), f"log: {log.tail()}")
    log.mark()
    dbg.rclick(c["x"] + 60, c["y"] + 70)
    res.check("the secondary button is SDL's right (3)", log.wait("button 3 down 60 70"),
              f"log: {log.tail()}")
    log.mark()
    dbg.send(f"gui move {c['x'] + 20} {c['y'] + 20}")
    dbg.send("gui wheel 1")
    res.check("the wheel is SDL's wheel", log.wait("wheel 1"), f"log: {log.tail()}")
    log.mark()
    dbg.warp_cursor(qmp, c["x"] + 50, c["y"] + 50)
    qmp.mouse_down("left")
    dbg.settle()
    qmp.mouse_down("right")
    dbg.settle()
    qmp.mouse_up("right")
    dbg.settle()
    qmp.mouse_up("left")
    log.wait("button 1 up")
    seq = [ln.split("sdlwin: ", 1)[1].rsplit(" ", 2)[0] for ln in
           dbg.logs("sdlwin:", clear=False)[log.seen:] if "sdlwin: button" in ln]
    res.check("left held: the right button's press and release are SDL's right, the left "
              "comes up once at the end",
              seq == ["button 1 down", "button 3 down", "button 3 up", "button 1 up"], f"order: {seq}")
    chords(dbg, qmp, log, res, c)
    log.mark()
    dbg.warp_cursor(qmp, c["x"] + 80, c["y"] + 50)
    dbg.warp_cursor(qmp, c["x"] + 400, c["y"] + 300)
    res.check("the pointer leaving the window is SDL's mouse-leave", log.wait("mouse leave"),
              f"log: {log.tail()}")
    log.mark()
    qmp.send_key("a")
    res.check("a real key press is its SDL scancode, down (A = 4)", log.wait("key down sc=4 A"),
              f"log: {log.tail()}")
    res.check("...and up", log.wait("key up sc=4 A"), f"log: {log.tail()}")
    res.check("...and the character it typed is SDL text", log.wait("text a"), f"log: {log.tail()}")
    watcher(dbg, qmp, log, res)
    fullscreen(dbg, qmp, log, res, "100%", 1.0)
    close(dbg, log, res, "100%")
    hit_test(dbg, qmp, log, res)
    locale_kept(dbg, qmp, log, res)


def at_125(dbg, qmp, log, res, shot):
    win = start(dbg, log, res, "125%")
    if not win:
        return
    c = win["content"]
    res.check("125%: the window is the SDL size times 1.25", (c["w"], c["h"]) == (200, 125),
              f"content {c}")
    got = pixels(qmp, shot, win, [(50, 62), (99, 62), (100, 62), (150, 62), (151, 62), (152, 62)])
    res.check("125%: away from the edges the bands stay pure", got[0] == RED and got[5] == GREEN,
              f"got {got}")
    res.check("125%: an edge ON a pixel edge (80 -> 100) is not blended -- plain bilinear's would be",
              got[1] == RED and got[2] == BLUE, f"columns 99,100: {got[1:3]}")
    mixed = got[4]
    res.check("125%: an edge INSIDE a pixel (121 -> 151.25) blends that one column -- nearest's would not",
              got[3] == BLUE and mixed[0] == 0 and mixed[1] > 100 and mixed[2] > 10,
              f"columns 150,151,152: {got[3:6]}")
    log.mark()
    dbg.click(c["x"] + 50, c["y"] + 25)
    res.check("125%: a click at pixel 50,25 is SDL's point 40,20", log.wait("button 1 down 40 20"),
              f"log: {log.tail()}")
    fullscreen(dbg, qmp, log, res, "125%", 1.25)
    close(dbg, log, res, "125%")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--shot", default=os.path.join(tempfile.gettempdir(), "sdl_window_test.png"),
                    help="where the captures go")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sdl_window_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    log = Log(dbg)
    restores = []
    print("sdl_window_test: checks")
    try:
        for scale, phase in (("100", at_100), ("125", at_125)):
            r = _hold_setting(dbg, "display.scale", scale)
            if r:
                restores.append(r)
            dbg.settle()
            phase(dbg, qmp, log, res, args.shot)
    finally:
        for p in dbg.processes_named("sdlwin_test"):
            dbg.send(f"sh kill {p['pid']}")
        dbg.close()
        qmp.close()
        for r in restores[:1]:
            r()
    print(f"\nsdl_window_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
