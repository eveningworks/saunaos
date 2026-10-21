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

A bar the app builds its rows for later (`uui_menubar_init(&ctx, 0, 0)`, a
context menu) is not counted.

THE WAIVER: a comment `flags-ok: <why every command always applies>` anywhere
in the file. The reason is the point -- "the list is always shown, so Copy and
Save as always have something to act on" -- not the comment.

THE OTHER HALF IS NOT CHECKABLE and is in docs/gui-guidelines.md, "A control
that cannot act is greyed": that the callback answers for EVERY command that
can fail to apply, which only a person looking at the commands can say.

  python3 tools/check_control_states.py
  python3 tools/check_control_states.py --positive-control
Exit status 1 on a bar with no item_flags and no waiver.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INIT_RE = re.compile(r"\buui_(?:toolbar|menubar)_init\s*\(\s*&?[\w\[\].>-]+\s*,\s*([^,)]+)")
ASSIGN_RE = re.compile(r"(?:\.|->)item_flags\s*=")


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


def scan():
    problems = []
    for top, dirs, files in os.walk(os.path.join(ROOT, "userland")):
        dirs[:] = [d for d in dirs if d not in ("ports", "tests")]
        for f in sorted(files):
            # the widgets themselves (uui_toolbar.c ...) are what the rule is ABOUT, not subject to it
            if not f.endswith(".c") or f.startswith("uui_"):
                continue
            path = os.path.join(top, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                check_text(os.path.relpath(path, ROOT), fh.read(), problems)
    return problems


def positive_control():
    """Take every item_flags assignment out of Help Viewer: it must be refused."""
    path = os.path.join(ROOT, "userland", "gui", "apps", "hlpview.c")
    with open(path, encoding="utf-8") as fh:
        raw = fh.read()
    broken = ASSIGN_RE.sub("/* removed */ = ", raw) if False else re.sub(r"g_tb\.item_flags\s*=\s*\w+;", "", raw)
    if broken == raw:
        sys.exit("check_control_states: the positive control finds no `g_tb.item_flags = ...;` to remove "
                 "-- the app moved, fix the control")
    problems = []
    check_text("userland/gui/apps/hlpview.c", broken, problems)
    if not problems:
        sys.exit("check_control_states: POSITIVE CONTROL FAILED -- item_flags removed and nothing complained")
    print("check_control_states: positive control ok -- without its item_flags, hlpview.c is refused")
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
