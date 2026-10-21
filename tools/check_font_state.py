#!/usr/bin/env python3
"""Refuse a function that SELECTS a font and does not put back the one it found.

WHY THIS EXISTS. The selected font (`ugfx_set_font()`) is the PROCESS's,
not a widget's: whatever a widget leaves selected is what the next widget
draws and measures with. On 2026-10-21 Help Viewer's topic widget selected
its zoomed face to draw and returned without restoring, and the magnifier
zoomed the toolbar, the tree and the status bar with it -- a bug that
reads as "the whole window zoomed", nowhere near the line that caused it.

THE RULE, per function in userland/: if the body calls `ugfx_set_font()`
it must restore. Either

  * save what was there -- `ugfx_font_current()`, or the call's own return
    value (`was = ugfx_set_font(f)`) -- and set it back before returning; a
    helper that RETURNS the call (`return ugfx_set_font(f)`), the push half
    of a push/pop pair, hands the old font to its caller, who restores it; or
  * end on `ugfx_set_font(NULL)`, the session's regular face, which is what
    the process is on between widgets.

Setting a variable back counts only under the toolkit's own names for a saved
font -- was, saved, prev, prior, old, orig, restore -- because `f = font_for(...)`
then `ugfx_set_font(f)` is the SELECTION, which is the bug.

THE WAIVER, in the function: a comment `font-ok: <why>`. Say whose job
the restore is -- a helper that its one caller brackets, say. A waiver
with no reason is a failure of review, not of this script.

It reads the SOURCE with comments and strings removed and finds functions
by their braces, so it can be fooled by a macro that hides a call; it
cannot be fooled by a call it can see, which is the case that shipped.

  python3 tools/check_font_state.py
  python3 tools/check_font_state.py --list               # every function that sets a font, and how it restores
  python3 tools/check_font_state.py --positive-control   # remove the restores from the widget that shipped the bug
Exit status 1 on any function that does not restore.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {"ports", "tests"}
SET_RE = re.compile(r"\bugfx_set_font\s*\(")
FUNC_RE = re.compile(r"(?m)^[A-Za-z_][^;{}()]*\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:\n\s*)?\{")


def strip(text):
    """The text with comments and string/char literals blanked, line structure kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(raw):
    """(name, start line, body, raw body) for each top-level function."""
    code = strip(raw)
    for m in FUNC_RE.finditer(code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            depth += (code[i] == "{") - (code[i] == "}")
            i += 1
        yield m.group(1), code.count("\n", 0, m.start()) + 1, code[m.end():i], raw[m.end():i]


def last_set_arg(body):
    last = None
    for m in SET_RE.finditer(body):
        depth, i, start = 1, m.end(), m.end()
        while i < len(body) and depth:
            depth += (body[i] == "(") - (body[i] == ")")
            i += 1
        last = body[start:i - 1].strip()
    return last


SAVED_NAME_RE = re.compile(r"(?:was|saved|prev|prior|old|orig|restore)\w*")


def restores(body):
    """Does this function put the font back? (How, for --list.)"""
    if re.search(r"\breturn\s+ugfx_set_font\s*\(", body):
        return "returns the old one"
    arg = last_set_arg(body)
    if arg in ("NULL", "0"):
        return "ends on the default"
    # A variable that holds the PREVIOUS font: assigned, in a statement, from a call that returns it --
    # ugfx_set_font() itself, even under a condition -- or, from a push helper (`was = push_font(l)`),
    # under one of the toolkit's own names for a saved font. `f = font_for(...)` is neither: setting
    # THAT is the selection, which is the bug.
    for m in re.finditer(r"\b(\w+)\s*=\s*([^;=][^;]*);", body):
        name, rhs = m.group(1), m.group(2)
        from_set = re.search(r"\bugfx_(?:set_font|font_current)\s*\(", rhs)
        from_push = SAVED_NAME_RE.fullmatch(name) and re.search(r"\w*font\w*\s*\(", rhs)
        if (from_set or from_push) and re.search(r"\bugfx_set_font\s*\(\s*%s\s*\)" % re.escape(name), body):
            return "sets back " + name
    return None


def check_text(rel, raw, listing, problems):
    for name, line, body, raw_body in functions(raw):
        if not SET_RE.search(body) or "font-ok:" in raw_body:
            continue
        how = restores(body)
        listing.append((rel, line, name, how))
        if not how:
            problems.append(f"{rel}:{line}: {name}() selects a font with ugfx_set_font() and never puts back "
                            "the one it found -- save it (was = ugfx_font_current(), or the call's return "
                            "value) and ugfx_set_font(was) before every return, or end on "
                            "ugfx_set_font(NULL); a helper whose caller brackets it says why in a "
                            "`font-ok:` comment")


def check_file(path, listing, problems):
    with open(path, encoding="utf-8", errors="replace") as fh:
        raw = fh.read()
    if "ugfx_set_font" in raw:
        check_text(os.path.relpath(path, ROOT), raw, listing, problems)


def scan():
    problems, listing = [], []
    for top, dirs, files in os.walk(os.path.join(ROOT, "userland")):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in sorted(files):
            if f.endswith(".c"):
                check_file(os.path.join(top, f), listing, problems)
    return problems, listing


def positive_control():
    """Take the restores out of the widget that shipped the bug: this must go red."""
    path = os.path.join(ROOT, "userland", "ui", "uui_helpview.c")
    with open(path, encoding="utf-8") as fh:
        raw = fh.read()
    broken = re.sub(r"[ \t]*ugfx_set_font\(saved\);\n", "", raw)
    if broken == raw:
        sys.exit("check_font_state: the positive control finds no `ugfx_set_font(saved);` to remove "
                 "-- the widget moved, fix the control")
    problems = []
    check_text("userland/ui/uui_helpview.c", broken, [], problems)
    if not problems:
        sys.exit("check_font_state: POSITIVE CONTROL FAILED -- the restores were removed and nothing complained")
    print(f"check_font_state: positive control ok -- without its restores {len(problems)} function(s) fail, "
          f"the first {problems[0].split(':')[2].strip().split('(')[0]}()")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="remove the restores from uui_helpview.c and check that it is refused")
    a = ap.parse_args()
    if a.positive_control:
        return positive_control()
    problems, listing = scan()
    if a.list:
        for rel, line, name, how in listing:
            print(f"{rel}:{line} {name}: {how}")
    if problems:
        print(f"check_font_state: {len(problems)} function(s) leave a font selected\n")
        for p in problems:
            print("  " + p)
        return 1
    print(f"check_font_state: ok -- {len(listing)} functions select a font, each puts it back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
