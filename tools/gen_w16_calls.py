#!/usr/bin/env python3
"""tools/gen_w16_calls.py -- the pages that say what each Windows call w16
answers does, generated from the comment above its handler.

w16 answers a Windows 3.x program's calls with C handlers listed in
userland/w16/'s API tables (`{ ordinal, api_Name }`). The comment block
directly above each handler is its documentation, Wine's c2man shape:

    // IsIconic(hwnd) -> BOOL
    // Windows: Whether the window is minimized ...
    // w16: Always FALSE ...
    static int api_IsIconic(...)

A SIGNATURE line names a call (its name as w16_names.c has it for that
ordinal, then its arguments); signature lines in a row share the fields
after them, and a new signature after fields starts the next group -- so
one handler serving several calls documents each. Fields: `Windows:`
(what Windows 3.1 does) and `w16:` (what w16 does), both required; a
STAND-IN (task16.c's PARTLY) adds `Why:`, `Notice:` and `Whole:` (why only
that, what a program may notice, what would make it whole). A field runs
until the next field or signature; a bare `//` starts a new paragraph.
Comment text above the first signature is the code's own and not shown.
KERNEL's constants (module16.c's KERNEL_EQUATES) are documented the same
way in the block above that table, with `// __AHSHIFT = 3` signatures.

A call w16 does NOT answer may have a block in userland/w16/missing16.c,
which holds no code: `// MODULE.ORD Name(args) -> T`, then `Windows:`,
`Why:` (why it is not there) and `By:` (which of Windows 3.11's own
programs and DLLs import it). A `Why:` naming another missing call ("As
OpenComm.") is shown with that call's reason after it, so a section read
alone still says why. A block for a call that has since been implemented
is refused: its text moves onto the new handler.

    python3 tools/gen_w16_calls.py --check        # every call documented? (check_docs.py runs it)
    python3 tools/gen_w16_calls.py -o DIR         # write DIR/calls.md, stand-ins.md and missing.md
    python3 tools/gen_w16_calls.py --missing      # the calls still without a block, by file

The pages ship in /usr/share/doc/windows/: Help and `doc` read them whole,
Retro Launcher's About and Why? and `w16 --why` show one call's section.
"""
import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W16 = os.path.join(REPO, "userland", "w16")
FIELDS = {"Windows": "What Windows 3.1 does", "w16": "What w16 does", "Why": "Why only that",
          "Notice": "What a program may notice", "Whole": "What would make it whole"}
STANDIN_FIELDS = ("Why", "Notice", "Whole")
SIG = re.compile(r"^(__\w+|[A-Za-z_]\w*)(\(.*\)( -> .*)?| = .*)$")
FIELD = re.compile(r"^(Windows|w16|Why|Notice|Whole): ?(.*)$")
MISSING_FILE = "userland/w16/missing16.c"
MISSING_SIG = re.compile(r"^([A-Z0-9_]+)\.(\d+) ((__\w+|[A-Za-z_]\w*)(\(.*\)( -> .*)?| = .*))$")
MISSING_FIELD = re.compile(r"^(Windows|Why|By): ?(.*)$")


def read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as f:
        return f.read()


def names():
    """(MODULE, ordinal) -> name, from w16_names.c."""
    src = read("userland/w16/w16_names.c")
    var_mod = dict((v, m) for m, v in re.findall(r'\{ "(\w+)", (names_\w+),', src))
    out = {}
    for var, body in re.findall(r"static const struct w16_name (names_\w+)\[\] = \{(.*?)\n\};", src, re.S):
        for ordn, name in re.findall(r'\{ (\d+), -?\d+, \d+, "([^"]+)" \}', body):
            out[(var_mod[var], int(ordn))] = name
    return out


def table_modules():
    """g_*_api -> MODULE, from task16.c's w16_has()."""
    src = read("userland/w16/task16.c")
    body = re.search(r"int w16_has\(.*?\n\}", src, re.S).group(0)
    out = {}
    for mod, chunk in re.findall(r'!strcmp\(module, "(\w+)"\)(.*?)(?=!strcmp\(module|\Z)', body, re.S):
        for t in re.findall(r"in_table\((g_\w+_api),", chunk):
            out.setdefault(t, "KERNEL" if mod == "KRNL386" else mod)
    return out


def partly():
    src = read("userland/w16/task16.c")
    table = re.search(r"PARTLY\[\] = \{(.*?)\n\};", src, re.S).group(1)
    return {(m, int(o)) for m, o in re.findall(r'\{\s*"(\w+)",\s*(\d+),', table)}


def comment_above(lines, i):
    """The `//` lines directly above line i, top first, without the slashes."""
    out = []
    j = i - 1
    while j >= 0 and lines[j].lstrip().startswith("//"):
        out.insert(0, lines[j].strip()[2:].strip())
        j -= 1
    return out


def parse_block(block):
    """[(signatures, {field: [paragraphs]})] from a comment block."""
    groups, cur, field = [], None, None
    for text in block:
        f = FIELD.match(text)
        if not f and SIG.match(text):
            if cur is None or cur[1]:
                cur = ([], {})
                groups.append(cur)
            cur[0].append(text)
            field = None
            continue
        if cur is None:
            continue                             # the code's own text, above the first signature
        if f:
            field = f.group(1)
            cur[1].setdefault(field, [[]])
            if f.group(2):
                cur[1][field][-1].append(f.group(2))
            continue
        if field is None:
            continue
        if not text:
            if cur[1][field][-1]:
                cur[1][field].append([])
            continue
        cur[1][field][-1].append(text)
    return groups


def handlers():
    """api_X -> (file, line, comment block) over every userland/w16/*.c."""
    out = {}
    for path in sorted(glob.glob(os.path.join(W16, "*.c"))):
        lines = open(path, encoding="utf-8").read().split("\n")
        for i, ln in enumerate(lines):
            m = re.match(r"static int (api_\w+)\(", ln)
            if m:
                out[m.group(1)] = (os.path.relpath(path, REPO), i + 1, comment_above(lines, i))
    return out


def entries():
    """[(MODULE, ordinal, api_X)] for every API table entry."""
    mods = table_modules()
    out = []
    for path in sorted(glob.glob(os.path.join(W16, "*.c"))):
        src = open(path, encoding="utf-8").read()
        for var, body in re.findall(r"const struct w16_api (g_\w+_api)\[\] = \{(.*?)\n\};", src, re.S):
            if var not in mods:
                continue
            for ordn, fn in re.findall(r"\{ *(\d+), *(api_\w+) *\}", body):
                out.append((mods[var], int(ordn), fn))
    return out


def unsorted_tables():
    """Problems for any API table whose ordinals are not ascending.

    w16_has() and w16_api_call() BINARY-SEARCH a table, so one entry out of
    order is a call that is "not implemented" while its handler sits in the
    file -- on 2026-10-21 thirty-six new calls went in at the head of a table
    and BitBlt, SelectObject and SetMessageQueue stopped being found.
    """
    out = []
    for path in sorted(glob.glob(os.path.join(W16, "*.c"))):
        src = open(path, encoding="utf-8").read()
        for var, body in re.findall(r"const struct w16_api (g_\w+_api)\[\] = \{(.*?)\n\};", src, re.S):
            last = -1
            for ordn, fn in re.findall(r"\{ *(\d+), *(api_\w+) *\}", body):
                if int(ordn) < last:
                    out.append(f"{os.path.relpath(path, REPO)}: {var} is not in ordinal order at {ordn} {fn} "
                               "-- the table is searched by bisection; keep it ascending")
                    break
                last = int(ordn)
    return out


def equates():
    """[(ordinal, value)] and the comment block above KERNEL_EQUATES."""
    src = read("userland/w16/module16.c")
    lines = src.split("\n")
    i = next(k for k, ln in enumerate(lines) if "KERNEL_EQUATES[] = {" in ln)
    body = re.search(r"KERNEL_EQUATES\[\] = \{(.*?)\n\};", src, re.S).group(1)
    return [(int(o), v) for o, v in re.findall(r"\{ (\d+), (\w+) \}", body)], comment_above(lines, i), i + 1


def collect():
    """{(MODULE, ordinal): doc} and the problems found."""
    nm, part, hs = names(), partly(), handlers()
    docs, problems = {}, []
    for mod, ordn, fn in entries():
        name = nm.get((mod, ordn), "?")
        where = f"{mod}.{ordn} {name}"
        if fn not in hs:
            problems.append(f"{where}: its handler {fn} is not a `static int api_...(` in userland/w16/")
            continue
        path, line, block = hs[fn]
        doc = None
        for sigs, fields in parse_block(block):
            for s in sigs:
                # without case: Wine's spec names OLE 1.0's exports as the DLL does (OLEDELETE)
                if s.split("(")[0].split(" =")[0].lower() == name.lower() and (doc is None or fields):
                    doc = (s, fields)
        if not doc:
            problems.append(f"{path}:{line}: {fn} has no `// {name}(...)` signature line for {where}")
            continue
        sig, fields = doc
        need = ("Windows", "w16") + (STANDIN_FIELDS if (mod, ordn) in part else ())
        for f in need:
            if not any(p for p in fields.get(f, [])):
                problems.append(f"{path}:{line}: {where} has no `{f}:` field")
        for f in STANDIN_FIELDS:
            if f in fields and (mod, ordn) not in part:
                problems.append(f"{path}:{line}: {where} has `{f}:` but is no stand-in (task16.c's PARTLY)")
        docs[(mod, ordn)] = (name, sig, fields, path, fn)
    eqs, block, line = equates()
    groups = parse_block(block)
    for ordn, value in eqs:
        name = nm.get(("KERNEL", ordn), "?")
        doc = next(((s, f) for sigs, f in groups for s in sigs if s.split(" =")[0] == name), None)
        if not doc or not all(any(p for p in doc[1].get(f, [])) for f in ("Windows", "w16")):
            problems.append(f"userland/w16/module16.c:{line}: KERNEL.{ordn} {name} needs `// {name} = ...` "
                            "with Windows: and w16: above KERNEL_EQUATES")
            continue
        docs[("KERNEL", ordn)] = (name, doc[0], doc[1], "userland/w16/module16.c", "KERNEL_EQUATES")
    tabled = {(m, o) for m, o, _ in entries()}
    for key in sorted(part - tabled):
        problems.append(f"task16.c's PARTLY lists {key[0]}.{key[1]}, which no API table has")
    return docs, part, problems


def missing_blocks():
    """{(MODULE, ordinal): (name, sig, {field: text}, line)} from missing16.c, and its problems."""
    nm = names()
    tabled = {(m, o) for m, o, _ in entries()} | {("KERNEL", o) for o, _ in equates()[0]}
    out, problems, cur, field = {}, [], None, None
    if not os.path.exists(os.path.join(REPO, MISSING_FILE)):
        return out, problems
    for i, raw in enumerate(read(MISSING_FILE).split("\n"), 1):
        t = raw.strip()
        if not t.startswith("//"):
            cur = field = None
            continue
        t = t[2:].strip()
        m = MISSING_SIG.match(t)
        if m:
            key = (m.group(1), int(m.group(2)))
            name = m.group(4)
            where = f"{MISSING_FILE}:{i}: {key[0]}.{key[1]}"
            if key in out:
                problems.append(f"{where} has two blocks")
            elif nm.get(key, "").lower() != name.lower():
                problems.append(f"{where} is {nm.get(key, 'no export')} in w16_names.c, not {name}")
            elif key in tabled:
                problems.append(f"{where} {name} is answered now: move its block onto its handler")
            cur = key
            out[key] = (nm.get(key, name), m.group(3), {}, i)
            field = None
            continue
        if cur is None:
            continue
        f = MISSING_FIELD.match(t)
        if f:
            field = f.group(1)
            out[cur][2][field] = f.group(2)
        elif field and t:
            out[cur][2][field] += " " + t
    for key, (name, _, fields, line) in out.items():
        for f in ("Windows", "Why", "By"):
            if not fields.get(f):
                problems.append(f"{MISSING_FILE}:{line}: {key[0]}.{key[1]} {name} has no `{f}:` field")
    return out, problems


def missing_section(key, blocks):
    name, sig, fields, _ = blocks[key]
    why = fields["Why"]
    paras = [why]
    # "As OpenComm." -- the named call's own reason, so the section stands alone
    by_name = {v[0].lower(): k for k, v in blocks.items() if k[0] == key[0]}
    m = re.search(r"\b[Aa]s (\w+)", why)
    ref = by_name.get(m.group(1).lower()) if m else None
    if ref and ref != key:
        paras.append(f"**{blocks[ref][0]}:** {blocks[ref][2]['Why']}")
    out = [f"## {key[0]}.{key[1]} {name}", "", "```", sig, "```", "",
           "**Not there yet:** a program that makes this call stops with Application Error, naming it.", "",
           "### What Windows 3.1 does", "", fields["Windows"], "", "### Why it is not there", ""]
    for p in paras:
        out += [p, ""]
    out += ["### Who calls it", "", f"Windows 3.11's own programs and DLLs that import it: {fields['By']}.", ""]
    return out


INTRO_MISSING = """# Calls w16 does not answer yet

**Category:** Windows programs

## Description

`w16` runs a Windows 3.x program by answering the Windows calls it
makes. A program that makes a call w16 does not answer yet stops with
Windows' Application Error, naming the call. The sections below are the
missing calls that Windows 3.11's own programs and DLLs import: what
Windows 3.1 did, why w16 does not do it yet, and who calls it. Many are
Windows' own internals -- calls USER, GDI and the drivers make of each
other, which a program never makes. Retro Launcher's **What?** shows one
section, and so does `w16 --why CALL`; a missing call with no section
here is shown with what is known of it.
"""


def section(key, doc, part):
    name, sig, fields, path, fn = doc
    mod, ordn = key
    out = [f"## {mod}.{ordn} {name}", "", "```", sig, "```", ""]
    if key in part:
        out += ["**Partly there:** w16 answers this without doing all Windows did.", ""]
    for f, title in FIELDS.items():
        paras = [" ".join(p) for p in fields.get(f, []) if p]
        if not paras:
            continue
        out += [f"### {title}", ""]
        for p in paras:
            out += [p, ""]
    where = f"`{os.path.basename(path)}`, `{fn}`" if fn != "KERNEL_EQUATES" else "`module16.c`, `KERNEL_EQUATES`"
    out += [f"**In w16's source:** {where}.", ""]
    return out


INTRO_CALLS = """# Windows calls w16 answers

**Category:** Windows programs

## Description

`w16` runs a Windows 3.x program by answering the Windows calls it
makes. Each section below is one call w16 answers: what Windows 3.1 did,
and what w16 does. A call w16 answers only partly says so, and why. A
call that is not here stops a program that makes it, naming the call.

This page is generated from the comments in w16's source, beside the
code they describe. Retro Launcher's **About** and **Why?** show one
section, and so does `w16 --why CALL`.
"""

INTRO_STANDINS = """# Calls w16 answers only partly

**Category:** Windows programs

## Description

`w16` runs a Windows 3.x program by answering the Windows calls it
makes. Most are answered in full. A few are **stand-ins**: w16 answers
them the way Windows did when the thing asked for was not there -- no
printer, no other program, no Help viewer -- so the program carries on
rather than stopping. Retro Launcher marks these **Partly**, and its
**Why?** opens the section below for the call.

Every program runs in a `w16` of its own. Where Windows 3.1 had one
system that all programs shared, each program here sees a Windows with
nothing else running in it -- which is why several of these calls find
no one.

Every call w16 answers, whole or not, is on the page *Windows calls w16
answers* (`doc calls`).
"""


def write(out_dir, docs, part, missing):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "missing.md"), "w", encoding="utf-8") as f:
        f.write(INTRO_MISSING)
        for k in sorted(missing):
            f.write("\n" + "\n".join(missing_section(k, missing)))
    keys = sorted(docs)
    with open(os.path.join(out_dir, "calls.md"), "w", encoding="utf-8") as f:
        f.write(INTRO_CALLS)
        for k in keys:
            f.write("\n" + "\n".join(section(k, docs[k], part)))
    with open(os.path.join(out_dir, "stand-ins.md"), "w", encoding="utf-8") as f:
        f.write(INTRO_STANDINS)
        for k in keys:
            if k in part:
                f.write("\n" + "\n".join(section(k, docs[k], part)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", help="write calls.md, stand-ins.md and missing.md here")
    ap.add_argument("--check", action="store_true", help="fail on a call without its block")
    ap.add_argument("--missing", action="store_true", help="list the calls without a block, by file")
    args = ap.parse_args()
    docs, part, problems = collect()
    missing, mproblems = missing_blocks()
    problems = problems + unsorted_tables() + mproblems
    if args.missing:
        for p in sorted(problems):
            print(p)
        print(f"{len(docs)} documented, {len(problems)} problem(s)")
        return 0
    if problems:
        for p in problems:
            print(f"gen_w16_calls: {p}", file=sys.stderr)
        return 1
    if args.out:
        write(args.out, docs, part, missing)
        print(f"gen_w16_calls: {len(docs)} calls, {len(part)} of them stand-ins, {len(missing)} missing "
              f"explained -> {args.out}")
    elif args.check:
        print(f"gen_w16_calls: ok -- {len(docs)} calls documented, {len(part)} stand-ins explained, "
              f"{len(missing)} missing calls explained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
