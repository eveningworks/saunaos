#!/usr/bin/env python3
"""Help Viewer (/bin/wm/apps/hlpview, lib/uhlp, ui/uui_helpview) on a GENERATED help file.

The file is `tools/hlpgen.py`'s sample() put into the guest with `vm.py put`, so the answer
is known and no Microsoft data is needed. The reader itself is `uhlp_hostcheck.py`'s; this
is what only a running window can show.

WHAT IT CHECKS, and what a broken version would still pass:

  - the file opens under its own title and the viewer reports the topic it landed on;
  - links are DRAWN in the link colour, and nowhere else is: pixel samples inside the
    reported link rects against the rest of the topic (a topic that drew its links as
    body text would still "respond");
  - the pointer is the HAND over a link, the TEXT cursor over text that is not one, and
    the arrow over empty space (the shape the compositor would draw, not the app's say-so);
  - a click on a jump link opens its topic, Alt+Left and Alt+Right walk the history, and a
    pop-up link opens a pop-up that Esc closes;
  - zoom stops at its limits (a press at the limit changes nothing) and Ctrl+0 resets it;
  - Ctrl+A, Ctrl+C puts the topic's TEXT on the clipboard: pasted into Notepad, saved and
    read back from disk, with a phrase-compressed paragraph intact;
  - Find looks through topic text (the word is inside a phrase the file stores as a code),
    and Enter opens the first hit;
  - a Win16 program's WinHelp() (winhelp.exe, a NASM sample) opens the viewer at the topic its
    context number names, and HELP_QUIT closes it;
  - the right-click menu opens with its rows, and greys Copy (no selection) and Back (no earlier
    page) but not Select All, Forward or Contents -- by the row's own pixels, not the app's say-so.

Positive controls (mutate.py): removing the zoom clamp reddens the zoom check; drawing
links in the text colour reddens the colour check.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
import hlpgen                                   # noqa: E402
from harness import Results, poll               # noqa: E402

SAMPLE = "/tmp/sample.hlp"
WIN_SAMPLE = "/home/Win16/WINDOWS/SAMPLE.HLP"     # where winhelp.exe asks for it as C:\\WINDOWS\\SAMPLE.HLP
CLIP = "/tmp/hlpclip.txt"
K_ENTER, K_ESC = "0x0d", "0x1b"
K_LEFT, K_RIGHT = "0xf784", "0xf785"
K_CTRL_F = "0x06"
CTRL_V, CTRL_S = "0x16", "0x13"      # Notepad takes the control codes, as clipboard_test does


def put_sample(instance):
    """Before the console is ours: `vm.py put` shares the debug socket, and a second client
    on it while the test holds it loses the file without a word."""
    with tempfile.NamedTemporaryFile(suffix=".hlp", delete=False) as f:
        f.write(hlpgen.sample().build())
        host = f.name
    vm = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance)]
    subprocess.run(vm + ["exec", "mkdir /home/Win16", "mkdir /home/Win16/WINDOWS"], capture_output=True)
    ok = all(subprocess.run(vm + ["put", host, dst], capture_output=True).returncode == 0
             for dst in (SAMPLE, WIN_SAMPLE))
    os.unlink(host)
    return ok


class Viewer:
    """What the viewer says it shows: its newest layout block, parsed."""

    def __init__(self, dbg):
        self.dbg = dbg
        self.lines = []
        self.origin = (0, 0)       # the view's own place in window coordinates

    def pump(self):
        self.dbg.settle()
        self.lines += [ln for ln in self.dbg.logs("hlpview:", clear=True)]
        self.lines = self.lines[-400:]

    def here(self):
        """The newest `here` line as a dict, with the links the same frame's widget walk
        reported (`view.link`, in window coordinates: made relative to the view)."""
        idxs = [i for i, ln in enumerate(self.lines) if "hlpview: here" in ln]
        if not idxs:
            return None
        m = re.search(r'here "(.*)" topic (-?\d+) file (-?\d+) back (\d+) fwd (\d+) popup (\d+) zoom (-?\d+)',
                      self.lines[idxs[-1]])
        if not m:
            return None
        h = dict(title=m.group(1), topic=int(m.group(2)), back=int(m.group(4)), fwd=int(m.group(5)),
                 popup=int(m.group(6)), zoom=int(m.group(7)), links={})
        lo = idxs[-2] + 1 if len(idxs) > 1 else 0
        for ln in self.lines[lo:idxs[-1]]:
            k = re.search(r"hlpview: layout view\.link (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
            if k:
                h["links"][int(k.group(1))] = tuple(int(k.group(i)) - (self.origin[i - 2] if i < 4 else 0)
                                                    for i in range(2, 6))
        return h

    def wait(self, pred, timeout=8.0):
        def look():
            self.pump()
            h = self.here()
            return h if h and pred(h) else None
        return poll(look, timeout, step=0.3)

    def view(self, title):
        w = self.dbg.widgets(title).get("view")
        return (w["screen"]["x"], w["screen"]["y"], w["w"], w["h"]) if w else None


def saturated(px):
    return max(px) - min(px) > 60


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "hlpview_test")
    res = Results()
    check = res.check

    put_ok = put_sample(args.instance if args.instance is not None else 0)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Help Viewer on a generated help file")
    if not check("the sample help file reached the guest", put_ok and "sample.hlp" in (dbg.send("sh ls /tmp") or "")):
        return res.finish("hlpview_test")

    from PIL import Image
    v = Viewer(dbg)
    dbg.logs("hlpview:", clear=True)
    win = dbg.spawn(f"/bin/wm/apps/hlpview {SAMPLE}", "Sample Help")
    if not check("the viewer opens under the file's own title", win is not None,
                 f"windows {[w.get('title') for w in dbg.windows()]}"):
        return res.finish("hlpview_test")
    title = "Sample Help"
    h = v.wait(lambda h: h["links"], 10)
    w0 = dbg.widgets(title).get("view")
    if w0:
        v.origin = (w0["x"], w0["y"])
        h = v.wait(lambda h: h["links"], 5)
    if not check("it reports the topic it landed on, with its two links (a jump, a pop-up)", h and h["title"] == "Contents"
                 and h["topic"] == 0 and h["back"] == 0 and h["fwd"] == 0 and len(h["links"]) == 2, h):
        return res.finish("hlpview_test")

    vx, vy, vw, vh = v.view(title)

    def shot():
        p = os.path.join(args.tmp, f"hlpview_{os.getpid()}.png")
        dbg.settle()
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    # --- links are drawn in colour and nothing else is ----------------------------------
    qmp_park = (vx + vw - 6, vy + vh - 6)
    dbg.warp_cursor(qmp, *qmp_park)
    im = shot()
    rects = [(vx + x, vy + y, w, hh) for (x, y, w, hh) in h["links"].values()]

    def inside(px, py, pad=2):
        return any(x - pad <= px < x + w + pad and y - pad <= py < y + hh + pad for x, y, w, hh in rects)

    link_ink = [sum(1 for yy in range(y, y + hh) for xx in range(x, x + w) if saturated(im.getpixel((xx, yy))))
                for x, y, w, hh in rects]
    other = sum(1 for yy in range(vy, vy + vh - 12, 2) for xx in range(vx, vx + vw - 20, 2)
                if not inside(xx, yy) and saturated(im.getpixel((xx, yy))))
    check("each link is drawn in the link colour", all(n > 15 for n in link_ink), f"coloured pixels {link_ink}")
    check("...and nothing else in the topic is", other == 0, f"{other} coloured samples outside the links")

    # --- the pointer ----------------------------------------------------------------------
    (lx, ly, lw, lh) = h["links"][0]
    dbg.warp_cursor(qmp, vx + lx + lw // 2, vy + ly + lh // 2)
    check("the pointer is the hand over a link", dbg.cursor_shape() == dbg.CURSOR_HAND, dbg.cursor_shape())
    dbg.warp_cursor(qmp, vx + lx - 3, vy + ly + lh // 2)
    check("...the text cursor over the text beside it", dbg.cursor_shape() == dbg.CURSOR_TEXT, dbg.cursor_shape())
    dbg.warp_cursor(qmp, *qmp_park)
    check("...and the arrow over empty space", dbg.cursor_shape() == dbg.CURSOR_NORMAL, dbg.cursor_shape())

    # --- following links, the history ----------------------------------------------------------
    dbg.click(vx + lx + lw // 2, vy + ly + lh // 2)
    h = v.wait(lambda h: h["topic"] == 1)
    check("a click on a jump link opens its topic", h and h["title"] == "Details" and h["back"] == 1, h)
    dbg.key(K_LEFT, mods="alt")
    h = v.wait(lambda h: h["topic"] == 0)
    check("Alt+Left goes back", h and h["fwd"] == 1 and h["back"] == 0, h)
    dbg.key(K_RIGHT, mods="alt")
    h = v.wait(lambda h: h["topic"] == 1)
    check("Alt+Right goes forward", h and h["back"] == 1, h)
    dbg.key(K_LEFT, mods="alt")
    h = v.wait(lambda h: h["topic"] == 0 and h["links"])

    # --- a pop-up -----------------------------------------------------------------------------------
    if h and 1 in h["links"]:
        (px, py, pw, ph) = h["links"][1]
        dbg.click(vx + px + pw // 2, vy + py + ph // 2)
        h = v.wait(lambda h: h["popup"] == 1)
        check("a pop-up link opens a pop-up", h and h["topic"] == 0 and h["popup"] == 1, h)
        dbg.key(K_ESC)
        h = v.wait(lambda h: h["popup"] == 0)
        check("Esc closes it", h and h["popup"] == 0, h)
    else:
        check("a pop-up link opens a pop-up", False, "no pop-up link reported")

    # --- zoom -----------------------------------------------------------------------------------------
    for _ in range(7):
        dbg.key("+", mods="ctrl", settle=False)
    h = v.wait(lambda h: h["zoom"] == 4)
    top = h["zoom"] if h else None
    dbg.key("+", mods="ctrl")
    v.pump()
    h = v.here()
    check("zoom in stops at its limit, and a press there changes nothing", top == 4 and h and h["zoom"] == 4,
          f"{top} then {h and h['zoom']}")
    dbg.key("0", mods="ctrl")
    h = v.wait(lambda h: h["zoom"] == 0)
    check("Ctrl+0 resets it", h and h["zoom"] == 0, h)
    for _ in range(5):
        dbg.key("-", mods="ctrl", settle=False)
    h = v.wait(lambda h: h["zoom"] == -2)
    dbg.key("-", mods="ctrl")
    v.pump()
    h2 = v.here()
    check("zoom out stops at its limit", h and h["zoom"] == -2 and h2 and h2["zoom"] == -2,
          f"{h and h['zoom']} then {h2 and h2['zoom']}")
    dbg.key("0", mods="ctrl")
    v.wait(lambda h: h["zoom"] == 0)

    # --- the right-click menu: drawn by the app, rows reported in the layout log ---------------------------
    dbg.logs("hlpview:", clear=True)
    dbg.rclick(vx + 40, vy + vh - 40)
    time.sleep(0.5)
    rows = {}
    for ln in dbg.logs("hlpview:", clear=True):
        r = re.search(r"layout ctxmenu\.item 0 (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
        if r:
            rows[int(r.group(1))] = tuple(int(g) for g in r.groups()[1:])
    # Separators are the short rows; the rest are Copy, Select All, Back, Forward, Contents.
    full = sorted((v2 for v2 in rows.values() if v2[3] > 20), key=lambda t: t[1])
    if check("the right-click menu opens with its five rows", len(full) == 5, f"{len(rows)} items, rows {full}"):
        cx, cy = win["content"]["x"], win["content"]["y"]
        im = shot()

        def strength(r):
            """How far the row's darkest-or-lightest pixel is from its background (the median)."""
            px = [sum(im.getpixel((xx, yy))) // 3 for yy in range(cy + r[1], cy + r[1] + r[3], 2)
                  for xx in range(cx + r[0], cx + r[0] + 120, 2)]
            bg = sorted(px)[len(px) // 2]
            return max(abs(p - bg) for p in px)

        st = [strength(r) for r in full]      # Copy, Select All, Back, Forward, Contents
        # Copy has no selection and Back no earlier page; Forward has the topic Alt+Left came from.
        check("...Copy and Back are greyed (no selection, no earlier page); Select All, Forward, Contents are not",
              max(st[0], st[2]) * 1.25 < min(st[1], st[3], st[4]), f"text contrast {st}")
    dbg.key(K_ESC)

    # --- copy, to the clipboard and out through Notepad -------------------------------------------------
    dbg.send(f"sh rm {CLIP}")
    dbg.send(f"sh touch {CLIP}")
    dbg.key("a", mods="ctrl")
    dbg.key("c", mods="ctrl")
    dbg.settle()
    dbg.spawn(f"/bin/wm/apps/notepad {CLIP}", None)
    npw = poll(lambda: next((w for w in dbg.windows() if "hlpclip" in w.get("title", "")), None), 10, 0.3)
    if check("Notepad opens for the paste", npw is not None):
        time.sleep(1)
        dbg.key(CTRL_V)
        dbg.key(CTRL_S)
        time.sleep(1.5)
        out = dbg.send(f"sh cat {CLIP}") or ""
        check("Ctrl+A, Ctrl+C copies the topic's text", "Welcome. See Details or the glossary." in out, repr(out[:200]))
        check("...with the phrase-compressed paragraph expanded", "the quick brown fox jumps over the lazy dog" in out,
              out[:300])
        dbg.send(f"gui close {npw['z']}")
        time.sleep(1)

    # --- Find -----------------------------------------------------------------------------------------------
    dbg.key(K_RIGHT, mods="alt")
    v.wait(lambda h: h["topic"] == 1)
    dbg.key(K_CTRL_F)
    for ch in "brown":
        dbg.key(ch)
    dbg.key(K_ENTER)
    h = v.wait(lambda h: h["topic"] == 0)
    check("Find searches the topics' text and Enter opens the first hit", h and h["title"] == "Contents", h)

    dbg.send(f"sh rm {CLIP}")
    for w in dbg.windows():
        if w.get("title") == title:
            dbg.send(f"gui close {w['z']}")
    poll(lambda: dbg.window(title) is None, 5, 0.3)
    winhelp_call(dbg, res, Viewer(dbg))
    return res.finish("hlpview_test")


def winhelp_call(dbg, res, v):
    """A Win16 program's WinHelp(): winhelp.exe has no window, asks for context number 100 of
    C:\\WINDOWS\\SAMPLE.HLP, waits eight seconds and quits the help. The number is the file's
    |CTXOMAP entry for topic 1, so the viewer must land on "Details" -- through w16, the help
    broker (the one unsandboxed process that may start it) and the viewer's own `-c`."""
    dbg.logs("hlpview:", clear=True)
    dbg.send("sh spawn /bin/w16 /usr/share/w16/winhelp.exe")
    win = poll(lambda: dbg.window("Sample Help"), 12, 0.3)
    res.check("WinHelp() opens the viewer on the help file", win is not None,
              f"windows {[w.get('title') for w in dbg.windows()]}")
    if not win:
        return
    h = v.wait(lambda h: h["topic"] == 1, 10)
    res.check("...at the topic its context number names (HELP_CONTEXT 100 -> Details)",
              h and h["title"] == "Details", h)
    gone = poll(lambda: dbg.window("Sample Help") is None, 15, 0.5)
    res.check("HELP_QUIT closes it when the program is done", gone, f"windows {[w.get('title') for w in dbg.windows()]}")
    for w in dbg.windows():
        if w.get("title") == "Sample Help":
            dbg.send(f"gui close {w['z']}")


if __name__ == "__main__":
    sys.exit(main())
