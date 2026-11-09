#!/usr/bin/env python3
"""Help and System Update with no Web page engine: their pages drawn as Markdown, not a dead web view.

docs/software-design.md, stage 3. The engine (/bin/webcontent, NetSurf) is
an optional component a machine may remove, and Help and System Update's
notes show their Markdown through it when it is there. Without it each
must decide, AT START, to draw the page with uui_markdown -- a web view
with no engine draws only its "the page stopped" card.

WHAT IT CHECKS, and what a broken version would still pass:

  - with /bin/webcontent moved away, Help SAYS its pages are uui_markdown's
    (`help: pages drawn by uui_markdown`), starts NO webcontent process,
    reports the overview's links the way uui_markdown reports them
    (`help: link kernel ...`, not the content process's `doc.link`), and
    the page is DRAWN -- ink in the doc rect of a settled frame;
  - System Update says the same of its notes, and its notes rect is drawn;
  - THE CONTROL, in the same run: with the engine put back, a new Help
    says the opposite (`the web page engine`) -- so the line is not a
    constant, and the first half saw a real difference.

The engine is moved with `mv` and put back in a `finally`: removing the
component would need an update server to add it back.

    python3 tools/tests/gui/engineless_test.py --instance 8
"""

RUNNER = {"covers": "Help and System Update draw their Markdown with no Web page engine"}

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

ENGINE = "/bin/webcontent"
HELP, HELP_TITLE = "/bin/wm/apps/help", "Help"
UPD, UPD_TITLE = "/bin/wm/system/sysupdate", "System Update"


def ink(qmp, tmp, rect):
    from PIL import Image
    p = os.path.join(tmp, f"engineless_{os.getpid()}.png")
    qmp.screenshot(p)
    im = Image.open(p).convert("RGB")
    x, y, w, h = rect
    crop = im.crop((x, y, x + w, y + h))
    return sum(1 for px in crop.getdata() if sum(px) < 300)


def screen_rect(w):
    return w["screen"]["x"], w["screen"]["y"], w["w"], w["h"]


def close(dbg, title):
    if dbg.window(title):
        dbg.close_window(title)


def present(dbg, name):
    """Is /bin/<name> there, by /bin's own listing?"""
    return re.search(rf"(^|\s){re.escape(name)}(\s|$)", dbg.send("sh ls /bin")) is not None


def drawn_by(dbg, prefix, timeout=10):
    """The app's own line saying which widget draws its pages."""
    def got():
        for ln in dbg.logs(prefix, clear=False):
            m = re.search(r"drawn by (the web page engine|uui_markdown)", ln)
            if m:
                return m.group(1)
        return None
    return poll(got, timeout, 0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "engineless_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Help and System Update with no Web page engine")
    moved = False
    try:
        close(dbg, HELP_TITLE)
        close(dbg, UPD_TITLE)
        out = dbg.send(f"sh mv {ENGINE} {ENGINE}.off")
        moved = present(dbg, "webcontent.off")
        res.check("the engine can be moved away", moved and not present(dbg, "webcontent"), out)

        # --- Help
        dbg.logs("help:", clear=True)
        win = dbg.spawn(HELP, HELP_TITLE)
        res.check("Help opens", win is not None)
        res.check("Help says its pages are uui_markdown's", drawn_by(dbg, "help:") == "uui_markdown",
                  str(drawn_by(dbg, "help:", 1)))
        links = poll(lambda: [ln for ln in dbg.logs("help: link ", clear=False)], 10, 0.3)
        res.check("...reports the overview's links as uui_markdown does",
                  links and any(" kernel " in ln for ln in links)
                  and not dbg.logs("help: layout doc.link", clear=False), str((links or [])[:2]))
        res.check("...starts no webcontent process", not dbg.processes_named("webcontent"),
                  str(dbg.processes_named("webcontent")))
        doc = poll(lambda: dbg.widgets(HELP_TITLE).get("doc"), 10, 0.3)
        dbg.settle()
        res.check("...and the page is drawn", doc is not None and ink(qmp, args.tmp, screen_rect(doc)) > 500)
        close(dbg, HELP_TITLE)

        # --- System Update's notes
        dbg.logs("sysupdate:", clear=True)
        win = dbg.spawn(UPD, UPD_TITLE)
        res.check("System Update opens", win is not None)
        res.check("System Update says its notes are uui_markdown's",
                  drawn_by(dbg, "sysupdate:") == "uui_markdown", str(drawn_by(dbg, "sysupdate:", 1)))
        notes = poll(lambda: dbg.widgets(UPD_TITLE).get("notes"), 10, 0.3)
        # The check starts on open and writes its first line into the notes.
        drawn = notes and poll(lambda: ink(qmp, args.tmp, screen_rect(notes)) > 40 or None, 10, 0.5)
        res.check("...and its notes are drawn", bool(drawn))
        res.check("...with no webcontent process", not dbg.processes_named("webcontent"))
        close(dbg, UPD_TITLE)
    finally:
        if moved:
            dbg.send(f"sh mv {ENGINE}.off {ENGINE}")

    # --- the control: the engine back, a new Help says the opposite
    res.check("the engine is back", present(dbg, "webcontent"))
    dbg.logs("help:", clear=True)
    win = dbg.spawn(HELP, HELP_TITLE)
    res.check("with the engine, Help says the web page engine draws its pages",
              win is not None and drawn_by(dbg, "help:") == "the web page engine",
              str(drawn_by(dbg, "help:", 1)))
    close(dbg, HELP_TITLE)
    dbg.close()
    return res.finish("engineless_test")


if __name__ == "__main__":
    sys.exit(main())
