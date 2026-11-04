#!/usr/bin/env python3
"""Move source paths and rewrite every reference to them, from a mapping table.

    tools/dev/tree_move.py tools/dev/tree_moves/1-third-party.tsv
    tools/dev/tree_move.py --dry-run MAP     # print the moves and the files to rewrite
    tools/dev/tree_move.py --check MAP       # only list old paths still referenced

docs/tree-design.md's stages are each one run of this over one table. A
table line is `FROM<TAB>TO` (a tracked file or directory, relative to the
repo root); `#` starts a comment, and a line `old-root<TAB>PREFIX` names
a prefix that must not survive anywhere once the stage is done.

What it does, in order:
  1. refuses unless every FROM is tracked and no TO exists;
  2. `git mv`s each pair, the most specific FROM first, so a subdirectory
     with its own row (`backends/x/patches`) leaves before its parent;
  3. rewrites every tracked text file, LONGEST FROM FIRST, matching a
     FROM only at a path boundary (`ports/doom` never matches
     `ports/doomx`);
  4. lists every remaining line naming an `old-root` -- the generic
     references (`userland/ports/<x>`, `$(wildcard userland/ports/*)`)
     no table row can rewrite, which a person fixes by hand -- and exits
     1 while there are any.

`git mv` keeps `git log --follow` and `blame` working across the move;
nothing here rewrites history.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def git(*args, capture=True):
    r = subprocess.run(["git", *args], cwd=ROOT, text=True,
                       capture_output=capture, check=False)
    if r.returncode not in (0, 1):
        sys.exit(f"tree_move: git {' '.join(args)} failed:\n{r.stderr}")
    return r.stdout if capture else ""


def read_map(path):
    moves, roots = [], []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].rstrip()
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 2:
                sys.exit(f"tree_move: {path}:{n}: want FROM<TAB>TO, got {line!r}")
            a, b = (p.strip().rstrip("/") for p in parts)
            (roots.append(b) if a == "old-root" else moves.append((a, b)))
    return moves, roots


def tracked(path):
    return bool(git("ls-files", "--", path).strip())


def boundary_re(froms):
    # Longest first, so `userland/backends/x/patches` wins over
    # `userland/backends/x`; the lookarounds are the path boundary, and a
    # `\t`/`\n` escape (a string literal) or a `-I`/`-L` flag before a path
    # still counts as one.
    alts = "|".join(re.escape(f) for f in sorted(froms, key=len, reverse=True))
    return re.compile(rf"(?:(?<![\w.-])|(?<=\\[tn])|(?<=-[IL]))({alts})(?![\w.-])")


# The record of the moves is never rewritten: the tables name the old
# paths on purpose, and so does the design doc's mapping.
KEEP = (":!tools/dev/tree_moves", ":!docs/tree-design.md")


def text_files():
    out = git("grep", "-lI", "-e", ".", "--", ".", *KEEP)
    return [p for p in out.splitlines() if p]


def leftovers(roots):
    hits = []
    for r in roots:
        out = git("grep", "-nI", "-e", r, "--", ".", *KEEP)
        hits += [line for line in out.splitlines()
                 if re.search(rf"(?:(?<![\w.-])|(?<=\\[tn])|(?<=-[IL])){re.escape(r)}(?![\w.-])",
                              line.split(":", 2)[-1])]
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("map", help="the mapping table (FROM<TAB>TO lines)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--rewrite-only", action="store_true",
                    help="rewrite references to the table's paths without moving anything")
    ap.add_argument("--check", action="store_true",
                    help="only list old-root references still present")
    a = ap.parse_args()
    moves, roots = read_map(a.map)

    if a.check:
        hits = leftovers(roots)
        print("\n".join(hits) if hits else "tree_move: no old paths left")
        return 1 if hits else 0

    rx = boundary_re([s for s, _ in moves])
    dest = dict(moves)
    if a.rewrite_only:
        return finish(rx, dest, roots, 0)

    bad = [f"  not tracked: {s}" for s, _ in moves if not tracked(s)]
    bad += [f"  exists: {d}" for _, d in moves if os.path.exists(os.path.join(ROOT, d))]
    if bad:
        sys.exit("tree_move: refusing:\n" + "\n".join(bad))

    order = sorted(moves, key=lambda m: m[0].count("/"), reverse=True)
    if a.dry_run:
        for s, d in order:
            print(f"mv  {s} -> {d}")
        for p in text_files():
            body = open(os.path.join(ROOT, p), encoding="utf-8", errors="surrogateescape").read()
            n = len(rx.findall(body))
            if n:
                print(f"fix {p} ({n})")
        return 0

    for s, d in order:
        if not tracked(s):      # emptied by a more specific row before it
            continue
        os.makedirs(os.path.dirname(os.path.join(ROOT, d)), exist_ok=True)
        git("mv", s, d, capture=False)

    return finish(rx, dest, roots, len(moves))


def finish(rx, dest, roots, moved):
    changed = 0
    for p in text_files():
        full = os.path.join(ROOT, p)
        if os.path.islink(full):
            continue
        body = open(full, encoding="utf-8", errors="surrogateescape").read()
        new = rx.sub(lambda m: dest[m.group(1)], body)
        if new != body:
            with open(full, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(new)
            changed += 1
    print(f"tree_move: {moved} moved, {changed} files rewritten")

    hits = leftovers(roots)
    if hits:
        print(f"tree_move: {len(hits)} references to an old root remain -- fix by hand:")
        print("\n".join(hits))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
