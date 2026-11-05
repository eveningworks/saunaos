#!/usr/bin/env python3
"""Refuse an app whose menu bar or toolbar never says when a command cannot act.

WHY THIS EXISTS. A control that is offered and does nothing is the commonest
small lie in a GUI. On 2026-10-21 Help Viewer's magnifier kept offering
"larger text" after the text could grow no more -- a click that did nothing --
and the fix (grey it at the limit) was the right one everywhere: Back with no
history, Save with nothing changed, Copy with nothing selected. Windows' menu
items and Qt's QAction::setEnabled are the shape; here it is the toolbar's and
the menu bar's `item_flags` callback, which is asked on every draw.

THE RULE, per app source under userland/: every `uui_toolbar_init()` and
`uui_menubar_init()` that is given its items must be matched by an
`item_flags` assignment in the same file -- one per bar. The callback is where
"can this act right now?" is answered; a bar with none has decided that every
command is always possible, and that is rarely true.

A BAR DECLARED IN A .ui FILE (userland/libs/ui/uui_builder.h) is counted for
the program's directory: its `toolbar`/`menubar` lines, plus the C's inits,
must be matched by the `item_flags` assignments in that directory's C.

A bar the app builds its rows for later (`uui_menubar_init(&ctx, 0, 0)`, a
context menu) is not counted.

THE WAIVER: a comment `flags-ok: <why every command always applies>` anywhere
in the file. The reason is the point -- "the list is always shown, so Copy and
Save as always have something to act on" -- not the comment.

THE OTHER HALF IS NOT CHECKABLE and is in docs/gui-guidelines.md, "A control
that cannot act is greyed": that the callback answers for EVERY command that
can fail to apply, which only a person looking at the commands can say.

  python3 tools/checks/check_control_states.py
  python3 tools/checks/check_control_states.py --positive-control
Exit status 1 on a bar with no item_flags and no waiver.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
ROOT = toolpath.REPO
INIT_RE = re.compile(r"\buui_(?:toolbar|menubar)_init\s*\(\s*&?[\w\[\].>-]+\s*,\s*([^,)]+)")
ASSIGN_RE = re.compile(r"(?:\.|->)item_flags\s*=")
UI_BAR_RE = re.compile(r"^\s*(?:toolbar|menubar)\b", re.M)


def inits(raw):
    """Bars given their items: the init calls whose second argument is not a literal 0."""
    return [m for m in INIT_RE.finditer(raw) if m.group(1).strip() != "0"]


def check_text(rel, raw, problems):
    n = len(inits(raw))
    if not n or "flags-ok:" in raw:
        return
    assigned = len(ASSIGN_RE.findall(raw))
    if assigned < n:
        problems.append(f"{rel}: {n} menu bar/toolbar(s) given items but {assigned} `item_flags` assignment(s) -- "
                        "say when each command cannot act (grey it: UUI_MI_DISABLED), or `flags-ok: <why every "
                        "command always applies>`")


def check_ui(rel, ui_raw, c_raw, problems):
    """A program directory with .ui files: its bars there and in its C, against its C's assignments."""
    bars = len(UI_BAR_RE.findall(ui_raw)) + len(inits(c_raw))
    if not bars or "flags-ok:" in c_raw:
        return
    assigned = len(ASSIGN_RE.findall(c_raw))
    if assigned < bars:
        problems.append(f"{rel}: {bars} menu bar/toolbar(s), counting its .ui files, but {assigned} `item_flags` "
                        "assignment(s) in its C -- set each built bar's item_flags after uui_builder_widget(), "
                        "or `flags-ok: <why>`")


def scan():
    problems = []
    for top, dirs, files in os.walk(os.path.join(ROOT, "userland")):
        dirs[:] = [d for d in dirs if d not in ("upstream", "tests")]  # third_party/<port>/upstream
        for f in sorted(files):
            # the widgets themselves (uui_toolbar.c ...) are what the rule is ABOUT, not subject to it
            if not f.endswith(".c") or f.startswith("uui_"):
                continue
            path = os.path.join(top, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                check_text(os.path.relpath(path, ROOT), fh.read(), problems)
        uis = [f for f in files if f.endswith(".ui")]
        if uis:
            def read(names):
                out = ""
                for n in sorted(names):
                    with open(os.path.join(top, n), encoding="utf-8", errors="replace") as fh:
                        out += fh.read() + "\n"
                return out
            check_ui(os.path.relpath(top, ROOT), read(uis), read(f for f in files if f.endswith(".c")), problems)
    return problems


def positive_control():
    """Take every item_flags assignment out of Help Viewer: it must be refused."""
    path = os.path.join(ROOT, "userland", "apps", "apps", "hlpview", "hlpview.c")
    with open(path, encoding="utf-8") as fh:
        raw = fh.read()
    # EVERY assignment: its menu bar's would otherwise still cover the toolbar's count.
    broken = re.sub(r"\b\w+(?:\.|->)item_flags\s*=\s*\w+;", "", raw)
    if broken == raw:
        sys.exit("check_control_states: the positive control finds no `x.item_flags = ...;` to remove "
                 "-- the app moved, fix the control")
    problems = []
    check_text("userland/apps/apps/hlpview/hlpview.c", broken, problems)
    if not problems:
        sys.exit("check_control_states: POSITIVE CONTROL FAILED -- item_flags removed and nothing complained")
    # ...and a bar declared in a .ui file: Crash Reports without its assignments.
    d = os.path.join(ROOT, "userland", "apps", "apps", "crashreports")
    with open(os.path.join(d, "crashreports.ui"), encoding="utf-8") as fh:
        ui = fh.read()
    with open(os.path.join(d, "crashreports.c"), encoding="utf-8") as fh:
        c = fh.read()
    stripped = ASSIGN_RE.sub("->unflagged = ", c)
    if stripped == c or not UI_BAR_RE.search(ui):
        sys.exit("check_control_states: the positive control finds no .ui bar or assignment in crashreports -- fix the control")
    problems = []
    check_ui("userland/apps/apps/crashreports", ui, stripped, problems)
    if not problems:
        sys.exit("check_control_states: POSITIVE CONTROL FAILED -- a .ui toolbar with no item_flags passed")
    print("check_control_states: positive control ok -- without its item_flags, hlpview.c and crashreports' .ui bars are refused")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="remove Help Viewer's item_flags and check that it is refused")
    a = ap.parse_args()
    if a.positive_control:
        return positive_control()
    problems = scan()
    if problems:
        print(f"check_control_states: {len(problems)} app(s) never grey a command\n")
        for p in problems:
            print("  " + p)
        return 1
    print("check_control_states: ok -- every menu bar and toolbar says when a command cannot act, or says why it always can")
    return 0


if __name__ == "__main__":
    sys.exit(main())
