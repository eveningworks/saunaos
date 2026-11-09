#!/usr/bin/env python3
"""The optional components in data/components/ are well-formed, disjoint and GPL-complete.

WHAT IT CHECKS (tools/lib/components.py does the work, shared with the
seed step that stages them and the update server that tags the manifest
with them -- docs/software-design.md):

  1. Every <id>.component has Name, Summary, Category, License and
     Paths, no unknown key, and no Size=/Files= (the seed step writes
     those from the files).
  2. No path is in two components, and none is the machine's own
     (/etc, /home, /var) or a component's definition, which stays in
     the base so a removed component is still listed.
  3. Every Needs= names a component, one level deep.
  4. THE GPL SWITCH CANNOT MISS ONE: every program built from a
     GPL-only port (PORT_PROGRAMS_<port>) lies in a component whose
     License= is GPL, and every program useless without one (NEEDS_PORT)
     in a component that is GPL or needs one -- what NOGPL=1 drops.
  5. With a staging tree (seed/sync, after `make iso`): every path is
     there, a directory's path ends with /, and a Built=if-present
     component has all of its paths or none.

    python3 tools/checks/check_optional_components.py
    python3 tools/checks/check_optional_components.py --staging DIR
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402
import components  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--staging", default=os.path.join(toolpath.REPO, "seed", "sync"),
                    help="the seeded tree to check the paths against (skipped when absent)")
    ap.add_argument("--dir", default=components.SOURCE_DIR, help="the definitions")
    args = ap.parse_args()
    problems = []
    defs = components.load(args.dir, problems)
    staging = args.staging if os.path.isdir(args.staging) else None
    problems += components.check(defs, staging)
    for p in problems:
        print(f"check_optional_components: {p}")
    if problems:
        print(f"check_optional_components: {len(problems)} problem(s) -- data/components/README.md")
        return 1
    print(f"check_optional_components: {len(defs)} components OK"
          + ("" if staging else " (no staging tree: paths not checked)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
