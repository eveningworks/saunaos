#!/usr/bin/env python3
"""Fail the build on a BYTE of a string measured or drawn as a CHARACTER.

Every string SaunaOS keeps is UTF-8, decoded where it is drawn or
edited (docs/utf8-design.md). `ugfx_char_advance()` and
`ugfx_draw_char()` take ONE BYTE and treat it as a Latin-1 character --
right for a literal ('n', '0', '-'), wrong for `s[i]` in a loop, which
measures and draws `é` (C3 A9) as the two letters `Ã©`. A loop like
that compiles, looks right on every ASCII string anyone tests with, and
reintroduces mojibake the day a Polish file name reaches it. Step 1
converted every one; this keeps them converted.

WHAT IS FLAGGED, outside comments:

  - `ugfx_char_advance(X)` / `ugfx_draw_char(.., X, ..)` where X is not a
    character literal -- a variable of type `char` is a byte of some
    string (the literal case stays: it is a layout unit, `'n' * 20`);
  - `ugfx_cp_advance(..)` / `ugfx_draw_cp(..)` / `ugfx_kern(..)` whose
    argument SUBSCRIPTS a string (`s[i]`) -- a byte passed as a codepoint
    is the same bug with a cast on it;
  - `ugfx_text_width_n(X, 1)` -- one byte measured as one character.

The fix is to step with `ugfx_text_decode()` (or `ugfx_text_next()`/
`_prev()`) and measure or draw the codepoint -- see ui/ugfx.h, "characters,
not bytes".

Waive with a `text-loop-ok: <reason>` comment on the line or in the
comment block directly above it, the shape `check_text_measure.py` uses.
The legitimate reason is a CODE PAGE: w16 draws Windows-1252, where one
byte genuinely is one character (ugfx_font_codepage()).

`--positive-control` runs the matcher over planted lines of each shape
and fails unless every one is caught and a literal is not.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["userland"]
SKIP = ("userland/ports/",)
WAIVER = "text-loop-ok:"

DECL = re.compile(r"^\s*(?:const\s+)?(?:struct\s+\w+\s*\*\s*\w+\s*$|(?:unsigned\s+)?(?:char|int|uint32_t)\s+\w+\s*$)")
CHAR_LIT = re.compile(r"^\s*(?:\(\s*(?:unsigned\s+)?char\s*\)\s*)?'(?:\\.|[^'\\])'\s*$")


def split_args(s, start):
    """The comma-separated arguments of the call whose '(' is at
    s[start], and whether its ')' was found on this line."""
    depth, cur, out = 0, [], []
    i = start
    while i < len(s):
        ch = s[i]
        if ch == "(":
            depth += 1
            if depth > 1:
                cur.append(ch)
        elif ch == ")":
            depth -= 1
            if depth == 0:
                out.append("".join(cur))
                return out, True
            cur.append(ch)
        elif ch == "," and depth == 1:
            out.append("".join(cur))
            cur = []
        elif ch == "'":
            j = i + 1
            while j < len(s) and s[j] != "'":
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j
        else:
            cur.append(ch)
        i += 1
    out.append("".join(cur))
    return out, False


def subscripts(arg):
    """A subscript of a STRING -- not a member of an array's element:
    `row[c].ch` is a terminal cell's codepoint (ui/uvterm.h), a field of a
    struct, and no byte of anything."""
    if "[" not in arg:
        return False
    return not re.search(r"\]\s*(?:\.|->)\s*\w+\s*\)*\s*$", arg)


# (function, index of the argument that is a character, test)
BYTE_FNS = {"ugfx_char_advance": 0, "ugfx_draw_char": 3}
CP_FNS = {"ugfx_cp_advance": 0, "ugfx_draw_cp": 3, "ugfx_kern": None}


def check_line(code):
    """A reason string when `code` (one line, comments stripped) has the
    shape, else None."""
    for m in re.finditer(r"\b(ugfx_\w+)\s*\(", code):
        fn = m.group(1)
        if fn not in BYTE_FNS and fn not in CP_FNS and fn != "ugfx_text_width_n":
            continue
        args, closed = split_args(code, m.end() - 1)
        # A DECLARATION or definition names types, not values.
        if any(DECL.match(a) for a in args):
            continue
        if fn in BYTE_FNS:
            k = BYTE_FNS[fn]
            if k < len(args) and args[k].strip() and not CHAR_LIT.match(args[k]):
                return "%s() given `%s`, a byte of a string, not a literal" % (fn, args[k].strip())
        elif fn in CP_FNS:
            k = CP_FNS[fn]
            pick = args if k is None else args[k:k + 1]
            for a in pick:
                if subscripts(a):
                    return "%s() given `%s`, a byte passed as a codepoint" % (fn, a.strip())
        elif fn == "ugfx_text_width_n":
            if len(args) == 2 and args[1].strip() == "1":
                return "ugfx_text_width_n(.., 1) measures one BYTE as a character"
    return None


def waived(lines, i):
    if WAIVER in lines[i]:
        return True
    j = i - 1
    while j >= 0 and lines[j].lstrip().startswith("//"):
        if WAIVER in lines[j]:
            return True
        j -= 1
    return False


def strip_comment(line):
    out, i, q = [], 0, None
    while i < len(line):
        ch = line[i]
        if q:
            out.append(ch)
            if ch == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if ch == q:
                q = None
        elif ch in "'\"":
            q = ch
            out.append(ch)
        elif line.startswith("//", i):
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def scan(path):
    text = path.read_text(errors="replace")
    if "ugfx_" not in text:
        return []
    lines = text.splitlines()
    found, in_block = [], False
    for i, line in enumerate(lines):
        code = line
        if in_block:
            if "*/" not in code:
                continue
            code = code.split("*/", 1)[1]
            in_block = False
        if "/*" in code and "*/" not in code.split("/*", 1)[1]:
            code = code.split("/*", 1)[0]
            in_block = True
        code = strip_comment(code)
        if not code.strip():
            continue
        why = check_line(code)
        if why and not waived(lines, i):
            found.append((i + 1, line.strip(), why))
    return found


PLANTED = [
    ("    for (int i = 0; s[i]; i++) w += ugfx_char_advance(s[i]);", True),
    ("        ugfx_draw_char(s, x, y, txt[i], fg, bg);", True),
    ("    int a = ugfx_char_advance(c);", True),
    ("    x += ugfx_cp_advance((unsigned char)str[n]);", True),
    ("    gx += ugfx_kern(prev, (unsigned char)str[n]);", True),
    ("    ugfx_draw_cp(s, x, y, (uint32_t)(unsigned char)buf[k], fg, bg);", True),
    ("    int w1 = ugfx_text_width_n(line + mark, 1);", True),
    ("    int pad = ugfx_char_advance('n') * 2;", False),
    ("    ugfx_draw_char(s, x, y, '-', fg, bg);", False),
    ("    x += ugfx_cp_advance(cp);", False),
    ("    gx += ugfx_kern(prev, (int)cp);", False),
    ("    ugfx_draw_cp(s, x, y, row[q].ch, fg, bg);", False),
    ("    ugfx_draw_cp(s, x, y, (uint32_t)row[q].name[0], fg, bg);", True),
]


def main():
    if "--positive-control" in sys.argv[1:]:
        bad = 0
        for line, want in PLANTED:
            got = check_line(strip_comment(line)) is not None
            if got != want:
                print("positive control FAILED: %s %s" % ("missed" if want else "flagged", line.strip()))
                bad += 1
        if bad:
            return 1
        print("positive control ok: all %d planted byte loops caught, %d literals passed"
              % (sum(1 for _, w in PLANTED if w), sum(1 for _, w in PLANTED if not w)))
        return 0

    files = []
    for d in SCAN_DIRS:
        files += sorted((ROOT / d).rglob("*.c"))
        files += sorted((ROOT / d).rglob("*.h"))
    files = [f for f in files if not str(f.relative_to(ROOT)).startswith(SKIP)]

    bad = []
    for f in files:
        for line_no, line, why in scan(f):
            bad.append((f.relative_to(ROOT), line_no, line, why))

    if bad:
        print("A BYTE IS NOT A CHARACTER -- %d site(s):\n" % len(bad))
        for path, line_no, line, why in bad:
            print("  %s:%d" % (path, line_no))
            print("      %s" % line)
            print("      %s\n" % why)
        print("Step with ugfx_text_decode() / ugfx_text_next() and measure or draw")
        print("the codepoint (ugfx_cp_advance(), ugfx_draw_cp()). A code page,")
        print("where one byte IS one character, is waived with `%s <reason>`." % WAIVER)
        return 1

    print("text loops ok (%d files scanned)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
