#!/usr/bin/env python3
"""The test lock's PreToolUse hook refuses every way a source file can be
changed while a test reads the tree -- and nothing else.

    python3 tools/hostchecks/tree_lock_hostcheck.py
    python3 tools/hostchecks/tree_lock_hostcheck.py --positive-control   # must FAIL

Holds the lock (`tree_lock.hold()`), then feeds `tree_lock.py hook` the
tool calls below and compares its verdicts. The DENY rows are each a way
a source file changes: the Edit tool, an in-place sed/perl, a redirection,
tee, a heredoc script's open() -- the shell forms are why this exists: on
2026-11-04 a `sed -i` went through during a preflight, because the hook
only judged Edit/Write and `make`/`git`. The allow rows are what a session
does all the time while a test runs and must not be refused: reading, a
write to a scratch file, docs/, tools/, build/, `2>&1`.

`--positive-control` asks the hook about the DENY rows WITHOUT holding the
lock, so they must come back allowed and the run must fail -- proof the
verdicts depend on the lock, not on the command text alone.
"""

RUNNER = {
    "covers": "the test lock refuses every shell way to write a source file, and nothing else",
    "gate": True,
}

import argparse  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import tree_lock  # noqa: E402

REPO = toolpath.REPO
HOOK = os.path.join(REPO, "tools", "lib", "tree_lock.py")

BASH = [
    ("deny", "sed -i 's/a/b/' userland/libs/ui/ugfx.h"),
    ("deny", "perl -pi -e 's/a/b/' kernel/core/kernel.c"),
    ("deny", "echo x > userland/libs/base/ufile.c"),
    ("deny", "printf x | tee -a Makefile"),
    ("deny", "python3 - <<'X'\nopen('userland/libs/ui/ugfx.h','a').write('x')\nX"),
    ("deny", "cd kernel && echo x >> core/kernel.c"),
    ("deny", "make all"),
    ("deny", "git stash"),
    ("allow", "sed -n 1,5p userland/libs/ui/ugfx.h"),
    ("allow", "grep -n foo kernel/core/kernel.c 2>&1 | head"),
    ("allow", "echo x > /tmp/tree_lock_probe.txt"),
    ("allow", "echo x >> docs/roadmap.md"),
    ("allow", "make -n all > build/dry.txt 2>&1"),
    ("allow", "cat > $SCRATCH/x.txt <<'X'\nhi\nX"),
    ("allow", "python3 - <<'X'\nprint(open('Makefile').read()[:10])\nX"),
    ("allow", "sed -i 's/a/b/' tools/lib/vm.py"),
]
EDITS = [
    ("deny", os.path.join(REPO, "userland", "libs", "ui", "ugfx.h")),
    ("allow", os.path.join(REPO, "docs", "roadmap.md")),
    ("allow", os.path.join(REPO, "tools", "lib", "vm.py")),
]


def verdict(call):
    env = {k: v for k, v in os.environ.items()
           if k not in (tree_lock.HELD_ENV, tree_lock.BYPASS_ENV)}
    out = subprocess.run([sys.executable, HOOK, "hook"], input=json.dumps(call),
                         capture_output=True, text=True, env=env).stdout
    return "deny" if '"deny"' in out else "allow"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true")
    a = ap.parse_args()
    if not a.positive_control:
        tree_lock.hold("tree_lock_hostcheck")
    calls = [(w, {"tool_name": "Bash", "tool_input": {"command": c}, "cwd": REPO}, c.splitlines()[0])
             for w, c in BASH]
    calls += [(w, {"tool_name": "Edit", "tool_input": {"file_path": p}, "cwd": REPO},
               "Edit " + os.path.relpath(p, REPO)) for w, p in EDITS]
    bad = 0
    for want, call, label in calls:
        got = verdict(call)
        ok = got == want
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'}  {got:5}  {label}")
    if bad:
        print(f"tree_lock_hostcheck: FAILED -- {bad} of {len(calls)} verdicts wrong")
        return 1
    print(f"tree_lock_hostcheck: ok -- {len(calls)} verdicts as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
