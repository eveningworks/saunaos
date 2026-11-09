#!/usr/bin/env python3
"""Stage the optional components into /usr/share/components, checked, with their sizes.

    python3 tools/build/stage_components.py seed/sync

The Makefile's `seed` runs this LAST, after the strip, so each staged
definition carries the size of what the image really holds: the
data/components/<id>.component text, then `Size=<bytes>` and
`Files=<n>` summed over that component's files in the tree -- the
numbers the Software app shows are the build's, never a figure someone
keeps true (docs/software-design.md, "The data").

A component marked `Built=if-present` with none of its paths in the tree
(the compiler, from a checkout with no toolchain/native) is NOT staged,
and any earlier copy is removed: that build has no such component --
nor, with --nogpl (NOGPL=1), a GPL component or one that needs it. Every
other problem tools/lib/components.py finds fails the build, the same
list tools/checks/check_optional_components.py reports.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import components  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("staging", help="the seeded tree (seed/sync)")
    ap.add_argument("--nogpl", action="store_true", help="a NOGPL=1 build: no GPL component")
    ap.add_argument("--defs", default=components.SOURCE_DIR, help="the definitions (data/components)")
    args = ap.parse_args()
    staging, nogpl = args.staging, args.nogpl
    problems = []
    defs = components.load(args.defs, problems)
    problems += components.check(defs, staging, nogpl)
    if problems:
        for p in problems:
            print(f"seed: components: {p}", file=sys.stderr)
        sys.exit(f"seed: {len(problems)} problem(s) with data/components -- see "
                 f"data/components/README.md")
    out_dir = os.path.join(staging, components.STAGED_DIR)
    os.makedirs(out_dir, exist_ok=True)
    wanted = set()
    size = components.sizes(defs, staging)
    for cid, c in defs.items():
        dest = os.path.join(out_dir, cid + components.SUFFIX)
        if not components.staged(staging, c, defs, nogpl):
            print(f"seed: component {cid} is not in this build"
                  + (" (NOGPL=1)" if nogpl and components.present(staging, c) else ""))
            continue
        wanted.add(os.path.basename(dest))
        with open(c.file, encoding="utf-8") as fh:
            text = fh.read()
        if not text.endswith("\n"):
            text += "\n"
        nbytes, nfiles = size[cid]
        text += f"Size={nbytes}\nFiles={nfiles}\n"
        old = open(dest, encoding="utf-8").read() if os.path.isfile(dest) else None
        if old != text:          # unchanged bytes keep the image's copy as it is
            with open(dest, "w", encoding="utf-8") as fh:
                fh.write(text)
    for name in os.listdir(out_dir):
        if name.endswith(components.SUFFIX) and name not in wanted:
            os.remove(os.path.join(out_dir, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
