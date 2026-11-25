#!/usr/bin/env python3
"""Configuration Editor (/bin/wm/system/confedit) on the real desktop: edit a person's value, read it back
through `config get`, reset it, and scroll both panes.

WHAT IT CHECKS, and what a broken version would still pass:
  - Find narrows the table to a name from any branch, and Enter goes from the field to what it found;
  - an EDIT of a person's setting (desktop.clock_seconds, Scope=user) is read back through `config get`
    run as the desktop's account -- an independent path, the registry and the person's file -- and
    `config --machine get` still says what it said before: an edit that wrote /etc would pass the first
    read and fail the second. The row's From says You;
  - Reset to machine's on the command bar removes the person's value: `config get` and `config --machine
    get` agree again, and From says Machine (or Default when /etc has none);
  - the TABLE and the TREE scroll with more rows than fit (scroll_test.check_view(): the thumb at the top,
    the wheel both ways, a trough click, a thumb drag) -- the desktop branch's settings, and every branch
    opened by View > Expand all.
The person's own value is put back if they had one; the app is closed at the end.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/confedit_test.py
"""

RUNNER = {
    "covers": "Configuration Editor end to end: find, edit a person's value read back through config get, reset, both panes scrolled",
}

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402
from scroll_test import check_view              # noqa: E402

TITLE = "Configuration Editor"
APP = "/bin/wm/system/confedit"
KEY = "desktop.clock_seconds"
K_ENTER, K_ESC = "0x0d", "0x1b"


class View:
    """The app's `confedit:` lines: its layout, its tree rows and its state line."""

    def __init__(self, dbg):
        self.dbg = dbg
        self.lines = []

    def pump(self):
        self.lines += self.dbg.logs("confedit:", clear=True)
        self.lines = self.lines[-4000:]

    def state(self):
        self.pump()
        for ln in reversed(self.lines):
            m = re.search(r'confedit: branch (\S+) find "(.*)" rows (\d+) selected (\S+) from (\S+) '
                          r'edit (\d) reset (\d)', ln)
            if m:
                return dict(branch=m.group(1), find=m.group(2), rows=int(m.group(3)), selected=m.group(4),
                            frm=m.group(5), edit=m.group(6) == "1", reset=m.group(7) == "1")
        return None

    def wait(self, pred, timeout=10):
        return poll(lambda: (lambda s: s if s and pred(s) else None)(self.state()), timeout, 0.3)

    def rect(self, key):
        self.pump()
        for ln in reversed(self.lines):
            m = re.search(r"confedit: layout " + re.escape(key) + r" (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
            if m:
                return tuple(int(g) for g in m.groups())
        return None

    def num(self, key):
        self.pump()
        for ln in reversed(self.lines):
            m = re.search(r"confedit: layout " + re.escape(key) + r" (-?\d+)$", ln)
            if m:
                return int(m.group(1))
        return None

    def tree_row(self, name):
        self.pump()
        for ln in reversed(self.lines):
            m = re.search(r'confedit: tree row (\d+) "(.*)"$', ln)
            if m and m.group(2) == name:
                return int(m.group(1))
        return None


def config(dbg, line):
    """`config LINE` as the desktop's account: its first line, or None on a failure."""
    out = dbg.as_user(f"config {line}") or ""
    if "[exit" in out:
        return None
    return out.strip().splitlines()[0].strip() if out.strip() else ""


def type_text(dbg, text):
    """A few keys a `gui input` line: the WM takes a bounded number of steps at once."""
    keys = [f"key 0x{ord(c):02x}" for c in text]
    return all(dbg.input(keys[i:i + 8]) for i in range(0, len(keys), 8))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=None, help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "confedit_test")
    res = Results()
    tmp = args.tmp or tempfile.mkdtemp(prefix="confedit-")
    os.makedirs(tmp, exist_ok=True)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Configuration Editor: edit, read back, reset, scroll")
    restore = None
    try:
        restore = run(dbg, qmp, tmp, res)
    finally:
        if restore is not None:
            config(dbg, f"set {KEY} {restore}")
        if dbg.window(TITLE):
            dbg.close_window(TITLE)
        dbg.close()
    return res.finish("confedit_test")


def run(dbg, qmp, tmp, res):
    check = res.check
    v = View(dbg)
    dbg.logs("confedit:", clear=True)
    machine0, user0 = config(dbg, f"--machine get {KEY}"), config(dbg, f"get {KEY}")
    if not check("config reads the setting both ways", machine0 is not None and user0 is not None,
                 f"machine {machine0!r} person {user0!r}"):
        return None
    dbg.send(f"gui spawn {APP}")
    win = poll(lambda: dbg.window(TITLE), 20, 0.3)
    if not check("the window opens", win is not None, f"windows {[w.get('title') for w in dbg.windows() or []]}"):
        return None
    s = v.wait(lambda s: s["rows"] > 0)
    check("it opens on a branch with its rows", s is not None, str(s))
    dbg.settle()
    wid = dbg.widgets(TITLE)

    def centre(name):
        w = wid.get(name)
        return (w["screen"]["x"] + w["w"] // 2, w["screen"]["y"] + w["h"] // 2) if w else None

    # --- find, then Enter to the row it found --------------------------------------------
    dbg.click(*centre("find"))
    type_text(dbg, KEY)
    s = v.wait(lambda s: s["find"] == KEY)
    check("Find lists the setting from any branch", s is not None and s["selected"] == KEY and s["rows"] >= 1,
          str(s))
    # The person's own value, if they had one, is put back at the end: Reset removes it.
    restore = user0 if s is not None and s["frm"] == "You" else None
    check("a person's setting can be edited", s is not None and s["edit"], str(s))
    dbg.key(K_ENTER)

    # --- edit: the person's file, read back by config -----------------------------------------
    dbg.key(K_ENTER)
    check("Enter on the row opens the edit dialog", dbg.wait_widget(TITLE, "value", 10) is not None,
          f"widgets {sorted(dbg.widgets(TITLE))}")
    new = "off" if user0 == "on" else "on"
    type_text(dbg, new)
    dbg.key(K_ENTER)
    s = v.wait(lambda s: s["frm"] == "You")
    check("the row is From You after the edit", s is not None, str(v.state()))
    got = poll(lambda: (lambda g: g if g == new else None)(config(dbg, f"get {KEY}")), 10, 0.5)
    check(f"config get reads the edited value back ({new})", got == new, f"config get says {config(dbg, f'get {KEY}')!r}")
    check("...and the machine's value is untouched", config(dbg, f"--machine get {KEY}") == machine0,
          f"was {machine0!r}, now {config(dbg, f'--machine get {KEY}')!r}")

    # --- reset: the machine's value applies again ----------------------------------------------
    b = v.rect("toolbar.button 1")
    if check("the command bar reports Reset", b is not None, "no toolbar.button 1"):
        cx, cy = win["content"]["x"], win["content"]["y"]
        dbg.click(cx + b[0] + b[2] // 2, cy + b[1] + b[3] // 2)
    s = v.wait(lambda s: s["frm"] in ("Machine", "Default"))
    check("Reset makes the row the machine's", s is not None and not s["reset"], str(v.state()))
    check("config get and config --machine get agree again", config(dbg, f"get {KEY}") == machine0,
          f"{config(dbg, f'get {KEY}')!r} vs {machine0!r}")
    dbg.key(K_ESC)
    v.wait(lambda s: s["find"] == "")

    # --- the table scrolls: the desktop branch ----------------------------------------------------
    tree = wid.get("tree")
    row = v.tree_row("desktop")
    rh = v.num("tree.row_h")
    if check("the tree lists desktop", row is not None and rh and tree, f"row {row} row_h {rh}"):
        dbg.click(tree["screen"]["x"] + 40, tree["screen"]["y"] + row * rh + rh // 2)
        s = v.wait(lambda s: s["branch"] == "desktop")
        check("desktop has more rows than the table shows", s is not None and s["rows"] > 20, str(s))
        hh = v.num("values.header_h") or 0
        check_view(dbg, qmp, res, tmp, "values table", TITLE, "values",
                   lambda sx, sy, w, h: (sx + 40, sy + 6), lambda sx, sy, w, h: (sx + w // 3, sy + h // 2),
                   keys=True, top_inset=hh, warp=True)

    # --- the tree scrolls: View > Expand all ------------------------------------------------------------
    t = v.rect("menu.title 2")
    if check("the menu bar reports View", t is not None, "no menu.title 2"):
        cx, cy = win["content"]["x"], win["content"]["y"]
        dbg.click(cx + t[0] + t[2] // 2, cy + t[1] + t[3] // 2)
        item = poll(lambda: v.rect("menu.item 0 0"), 5, 0.3)
        if check("View opens", item is not None, "no menu.item 0 0"):
            dbg.click(cx + item[0] + item[2] // 2, cy + item[1] + item[3] // 2)
    rows = poll(lambda: (lambda n: n if n and n > 60 else None)(v.num("tree.rows")), 10, 0.3)
    check("Expand all opens every branch", rows is not None, f"tree.rows {v.num('tree.rows')}")
    check_view(dbg, qmp, res, tmp, "branch tree", TITLE, "tree",
               lambda sx, sy, w, h: (sx + 40, sy + 6), lambda sx, sy, w, h: (sx + w // 3, sy + h // 2),
               keys=True, warp=True)
    return restore


if __name__ == "__main__":
    sys.exit(main())
