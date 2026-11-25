"""tools/tests/gui/sdl_stage3_test.py -- what an SDL port reaches for beyond
a window and sound (docs/sdl-design.md, stage 3), over `/tests/sdlmisc_test`.

Asserted:

  * SDL_GetPrefPath is /home/user/.local/share/<org>/<app>/, made if missing,
    and a file written there reads back;
  * SDL_SetClipboardText puts text on the system clipboard and
    SDL_GetClipboardText reads it back; another SDL program hears
    SDL_EVENT_CLIPBOARD_UPDATE and reads the same text;
  * SDL_OpenURL opens a file: URL (percent-escapes decoded) in the app
    that opens the file -- Notepad for a .txt -- and an http: URL in the
    Web Browser; a scheme nothing opens is refused;
  * SDL_ShowMessageBox is a window of its own, MODAL over the SDL window
    that asked (owned by it), with its buttons and its message; an error
    box's badge is the error colour, its footer the panel colour (pixels).
    Ctrl+C on it copies the whole message; a click answers that button's
    id; Esc answers the escape button's; Return the return button's; the
    title bar's close on a box with no escape button answers -1;
  * a message box before SDL_Init(VIDEO) is an unowned window that answers;
  * relative mode HOLDS the pointer: motion arrives as SDL relative motion
    summing to what the mouse moved, the pointer stays put, a click still
    arrives. Another window taking the focus SUSPENDS it (the pointer moves
    again, no relative motion), the focus coming back resumes it, and R
    again ends it (absolute motion returns).

    python3 tools/tests/gui/sdl_stage3_test.py [--instance N] [--in-gui]
"""

RUNNER = {
    "name": "sdlstage3",
    "covers": "SDL stage 3: pref path, clipboard, OpenURL, message boxes, relative mode",
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

PROG = "/tests/sdlmisc_test"
PREF = "/home/user/.local/share/SaunaOS Tests/sdlmisc/"
MESSAGE = "No game data was found in /home/user/Games/sky. Copy the game's files into that folder and try again."
ERROR_RED = (178, 58, 50)   # utheme_default()'s severity[ERROR]
PANEL = (236, 236, 236)     # ...and its panel_bg


class Log:
    """The program's lines ("sdlmisc[ tag]: ...") since the last mark()."""
    def __init__(self, dbg):
        self.dbg = dbg
        self.seen = 0

    def mark(self):
        self.seen = len(self.dbg.logs("sdlmisc", clear=False))

    def lines(self):
        return self.dbg.logs("sdlmisc", clear=False)[self.seen:]

    def wait(self, text, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self.lines():
                if text in ln:
                    return ln
            time.sleep(0.2)   # sleep-ok: polls the guest log for a line, bounded
        return None

    def tail(self):
        return self.lines()[-8:]


def run(dbg, log, args):
    log.mark()
    dbg.send(f"gui spawn {PROG} {args}")


def check_pref(dbg, log, res):
    run(dbg, log, "pref")
    ln = log.wait("pref ")
    res.check("SDL_GetPrefPath is under /home/user/.local/share, made and writable",
              ln is not None and f"pref {PREF} wrote 1 read 'level 3'" in ln, f"log: {log.tail()}")


def check_clipboard(dbg, log, res):
    run(dbg, log, "clipwait tag=A")
    if not log.wait("A: ready"):
        res.check("the clipboard listener opens", False, f"log: {log.tail()}")
        return
    run(dbg, log, "clipset Sauna-clip-42 tag=B")
    ln = log.wait("B: clipset")
    res.check("SDL_SetClipboardText then SDL_GetClipboardText reads it back",
              ln is not None and "has 1 got 'Sauna-clip-42'" in ln, f"log: {log.tail()}")
    res.check("another SDL program hears SDL_EVENT_CLIPBOARD_UPDATE with the text",
              log.wait("A: clip update Sauna-clip-42") is not None, f"log: {log.tail()}")
    kill(dbg)


def check_url(dbg, log, res):
    # The pref check's file, its space escaped: Notepad opens it.
    before = {p["pid"] for p in dbg.processes_named("notepad")}
    run(dbg, log, "url file:///home/user/.local/share/SaunaOS%20Tests/sdlmisc/save.txt")
    ln = log.wait("url file:")
    res.check("SDL_OpenURL accepts a file: URL", ln is not None and " ok 1 " in ln, f"log: {log.tail()}")
    note = None
    deadline = time.time() + 15
    while time.time() < deadline and note is None:
        pids = {p["pid"] for p in dbg.processes_named("notepad")} - before
        note = next((w for w in dbg.windows() if w.get("client_pid") in pids), None)
        time.sleep(0.3)   # sleep-ok: waits for the opened app's window, bounded
    res.check("...and the file opens in its app (a Notepad window)", note is not None,
              f"windows: {[(w['title'], w.get('client_pid')) for w in dbg.windows()]}; "
              f"notepads: {dbg.processes_named('notepad')}")
    if note:
        dbg.send(f"sh kill {note['client_pid']}")
    before = {p["pid"] for p in dbg.processes_named("browser")}
    run(dbg, log, "url http://127.0.0.1:9/")
    ln = log.wait("url http:")
    res.check("SDL_OpenURL accepts an http: URL", ln is not None and " ok 1 " in ln, f"log: {log.tail()}")
    browser = None
    deadline = time.time() + 15
    while time.time() < deadline and browser is None:
        browser = next((p for p in dbg.processes_named("browser") if p["pid"] not in before), None)
        time.sleep(0.3)   # sleep-ok: waits for the browser process, bounded
    res.check("...and the Web Browser starts for it", browser is not None,
              f"processes: {dbg.processes_named('browser')}")
    if browser:
        dbg.send(f"sh kill {browser['pid']}")
    run(dbg, log, "url gopher://example.invalid/")
    ln = log.wait("url gopher:")
    res.check("a scheme nothing opens is refused", ln is not None and " ok 0 " in ln, f"log: {log.tail()}")


def box(dbg, title, timeout=10):
    return dbg.wait_window(title, timeout=timeout)


def box_rects(dbg, win, names, timeout=10.0):
    """{name: screen rect} from the box's layout log ("msgbox: layout <name>
    x y w h", content-relative) -- a dialog window sends no widget map."""
    c = win["content"]
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = {}
        for ln in dbg.logs("msgbox: layout ", clear=False):
            parts = ln.split("msgbox: layout ", 1)[1].split()
            if len(parts) >= 5 and parts[0] in names:
                x, y, w, h = (int(v) for v in parts[1:5])
                got[parts[0]] = {"x": c["x"] + x, "y": c["y"] + y, "w": w, "h": h}
        if all(n in got for n in names):
            return got
        time.sleep(0.2)   # sleep-ok: polls the guest log for the box's layout, bounded
    return got


def check_msgbox(dbg, qmp, log, res, shot):
    answered = len(dbg.logs("msgbox: answered", clear=False))
    run(dbg, log, "msgbox")
    if not log.wait("ready"):
        res.check("the message-box program opens its window", False, f"log: {log.tail()}")
        return
    owner = dbg.window("SDL misc box")
    b = box(dbg, "Game data")
    res.check("SDL_ShowMessageBox opens a window titled as asked", b is not None,
              f"windows: {[w['title'] for w in dbg.windows()]}")
    if not b or not owner:
        return
    res.check("...a dialog owned by the SDL window that asked, and modal",
              b.get("dialog") and b.get("modal") and b.get("owner") not in (-1, 0xFFFF),
              f"box: dialog={b.get('dialog')} owner={b.get('owner')} modal={b.get('modal')}")
    names = ("Retry", "Quit", "Help", "message", "badge")
    wid = box_rects(dbg, b, names)
    res.check("...with its three buttons, the message and a badge",
              all(n in wid for n in names), f"layout: {sorted(wid)}")
    if "badge" in wid and "Retry" in wid:
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        bx = wid["badge"]
        # The disc's rim, not its centre: the glyph is drawn there.
        rim = im.getpixel((bx["x"] + bx["w"] // 2, bx["y"] + 3))
        r = wid["Retry"]
        foot = im.getpixel((r["x"] - 6, r["y"] + r["h"] // 2))
        res.check("...an error badge in the error colour", rim == ERROR_RED, f"rim {rim}")
        res.check("...and the buttons on the panel-coloured footer", foot == PANEL, f"footer {foot}")
    qmp.combo(["ctrl", "c"])
    dbg.settle()
    h = wid.get("Help")
    if h:
        dbg.click(h["x"] + h["w"] // 2, h["y"] + h["h"] // 2)
    ln = log.wait("answer ")
    res.check("a click answers that button's id, and Ctrl+C copied the whole message",
              ln is not None and f"answer 33 ok 1 clip '{MESSAGE}'" in ln, f"log: {log.tail()}")
    log.mark()
    if box(dbg, "Second"):
        dbg.settle()
        qmp.send_key("esc")
    ln = log.wait("answer ")
    res.check("Esc answers the escape button's id", ln is not None and "answer 22 ok 1" in ln,
              f"log: {log.tail()}")
    log.mark()
    if box(dbg, "Third"):
        dbg.close_window("Third")
    ln = log.wait("answer ")
    res.check("closing a box with no escape button answers -1", ln is not None and "answer -1 ok 1" in ln,
              f"log: {log.tail()}")
    log.mark()
    if box(dbg, "Fourth"):
        dbg.settle()
        qmp.send_key("ret")
    ln = log.wait("answer ")
    res.check("Return answers the return button's id", ln is not None and "answer 11 ok 1" in ln,
              f"log: {log.tail()}")
    # ONE answer per box: the focused button's Enter is not answered again
    # by the window's own key handler (a second answer could reach a box
    # opened from the first one's reply).
    lines = dbg.logs("msgbox: answered", clear=False)[answered:]
    res.check("each of the four boxes answered once", len(lines) == 4, f"answers: {lines}")

    run(dbg, log, "alone")
    a = box(dbg, "Before video")
    res.check("a message box before SDL_Init(VIDEO) opens, unowned",
              a is not None and a.get("owner") == 0xFFFF, f"box: {a}")
    if a:
        dbg.settle()
        qmp.send_key("ret")
    ln = log.wait("alone answer")
    res.check("...and answers", ln is not None and "alone answer 7 ok 1" in ln, f"log: {log.tail()}")


def rel_sum(log):
    """The relative motion the program logged since the mark, summed."""
    dx = dy = 0
    for ln in log.lines():
        parts = ln.split(": rel ")
        if len(parts) == 2:
            a, b = parts[1].split()[:2]
            dx, dy = dx + int(a), dy + int(b)
    return dx, dy


def push(qmp, dx, n=4):
    for _ in range(n):
        qmp.move_rel(dx, 0)
        time.sleep(0.05)   # sleep-ok: paces the device reports like a hand would


def check_rel(dbg, qmp, log, res):
    run(dbg, log, "rel")
    if not log.wait("ready"):
        res.check("the relative-mode program opens its window", False, f"log: {log.tail()}")
        return
    w = dbg.wait_window("SDL misc rel")
    c = w["content"]
    dbg.warp_cursor(qmp, c["x"] + 60, c["y"] + 40)
    log.mark()
    qmp.send_key("r")
    res.check("SDL_SetWindowRelativeMouseMode is granted",
              log.wait("relative on ok 1") is not None, f"log: {log.tail()}")
    dbg.settle()
    at = dbg.cursor()
    log.mark()
    push(qmp, 20)
    log.wait("rel ", 3)
    time.sleep(0.5)   # sleep-ok: the program sums a run until 300 ms of quiet
    dx, dy = rel_sum(log)
    res.check("motion arrives as relative motion summing to what the mouse moved (80, 0)",
              (dx, dy) == (80, 0), f"summed ({dx}, {dy}); log: {log.tail()}")
    res.check("...and the pointer is held where it was", dbg.cursor() == at,
              f"before {at}, after {dbg.cursor()}")
    log.mark()
    qmp.mouse_down("left")
    qmp.mouse_up("left")
    res.check("a click still arrives while held", log.wait("button 1 down") is not None,
              f"log: {log.tail()}")

    # The Start menu (Super) is the compositor's own, not a window: it too
    # suspends the lock, and closing it resumes it.
    qmp.send_key("meta_l")
    dbg.settle()
    at = dbg.cursor()
    push(qmp, 15)
    dbg.settle()
    res.check("Super's Start menu suspends it: the pointer moves", dbg.cursor()[0] > at[0],
              f"before {at}, after {dbg.cursor()}")
    qmp.send_key("esc")
    dbg.settle()
    at = dbg.cursor()
    push(qmp, 15)
    dbg.settle()
    res.check("...and closing it resumes it: held again", dbg.cursor() == at,
              f"before {at}, after {dbg.cursor()}")

    # Another window takes the focus: the lock is suspended.
    log.mark()
    dbg.send(f"gui spawn {PROG} clipwait tag=F")
    other = dbg.wait_window("SDL misc clip")
    dbg.settle()
    at = dbg.cursor()
    push(qmp, 15)
    dbg.settle()
    moved = dbg.cursor()
    res.check("another window focused: the pointer moves again", other is not None and moved[0] > at[0],
              f"before {at}, after {moved}")
    res.check("...and no relative motion reaches the locked window", rel_sum(log) == (0, 0),
              f"log: {log.tail()}")
    if other:
        dbg.close_window("SDL misc clip")
        dbg.settle()
    # SDL re-enables relative mode when its window has the focus again.
    at = dbg.cursor()
    log.mark()
    push(qmp, 10)
    time.sleep(0.6)   # sleep-ok: the program sums a run until 300 ms of quiet
    res.check("its focus back: held again, motion relative",
              dbg.cursor() == at and rel_sum(log) == (40, 0),
              f"cursor {at} -> {dbg.cursor()}, summed {rel_sum(log)}; log: {log.tail()}")
    log.mark()
    qmp.send_key("r")
    log.wait("relative off ok 1")
    at = dbg.cursor()
    push(qmp, -10)
    dbg.settle()
    res.check("R again ends it: the pointer moves and SDL hears absolute motion",
              dbg.cursor()[0] < at[0] and log.wait("abs ", 3) is not None,
              f"cursor {at} -> {dbg.cursor()}; log: {log.tail()}")


def kill(dbg):
    for p in dbg.processes_named("sdlmisc_test"):
        dbg.send(f"sh kill {p['pid']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--shot", default=os.path.join(tempfile.gettempdir(), "sdl_stage3_test.png"),
                    help="where the capture goes")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sdl_stage3_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    log = Log(dbg)
    restore = _hold_setting(dbg, "display.scale", "100")
    print("sdl_stage3_test: checks")
    try:
        check_pref(dbg, log, res)
        check_clipboard(dbg, log, res)
        check_url(dbg, log, res)
        check_msgbox(dbg, qmp, log, res, args.shot)
        kill(dbg)
        check_rel(dbg, qmp, log, res)
    finally:
        kill(dbg)
        dbg.close()
        qmp.close()
        if restore:
            restore()
    print(f"\nsdl_stage3_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
