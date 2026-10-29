#!/usr/bin/env python3
"""tools/check_test_sleeps.py -- a test tool may not gain a FIXED SLEEP.

A GUI test that sleeps a guessed interval and then looks is the commonest
flake here: under load the guess is short, and the tool goes red one run
in five on a build that is fine (docs/bugs.md, "THE FULL GUI SUITE FAILS
2-4 TOOLS A RUN"). The waits that do not guess exist: DebugConsole.settle()
(input delivered, handled AND drawn -- `gui sync`), harness.poll() on the
condition the test then asserts, tools/wait_for.sh on a pid or a file.

So `time.sleep(` in tools/*_test.py is FROZEN at the counts below
(2026-10-29), a RATCHET like check_chains.py's: a file may not go above
its number, a file with none may not start, and when a file drops below
it this prints the new number to write in. A sleep that IS the test --
real time passing, a rate measured, a deadline proved -- says so on its
line (or the one above) with `sleep-ok: <why>`, and is not counted.

The baseline is tools/check_test_sleeps.baseline, `<count> <path>` per
line. Exits non-zero naming the file and the counts. Run by preflight.sh.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BASELINE = os.path.join(HERE, "check_test_sleeps.baseline")
SLEEP = re.compile(r"\btime\.sleep\s*\(")
WAIVER = "sleep-ok:"


def count(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    n = 0
    for i, line in enumerate(lines):
        code = line.split("#", 1)[0]
        if not SLEEP.search(code):
            continue
        if WAIVER in line or (i and WAIVER in lines[i - 1]):
            continue
        n += len(SLEEP.findall(code))
    return n


def load():
    base = {}
    for line in open(BASELINE, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            n, path = line.split(None, 1)
            base[path] = int(n)
    return base


def main():
    if "--write" in sys.argv:   # regenerate (only ever after a decision to)
        rows = []
        for path in sorted(glob.glob(os.path.join(HERE, "*_test.py"))):
            n = count(path)
            if n:
                rows.append(f"{n} {os.path.relpath(path, REPO)}")
        with open(BASELINE, "w", encoding="utf-8") as f:
            f.write("# check_test_sleeps.py's ratchet: fixed sleeps per test tool. Only ever goes DOWN.\n")
            f.write("\n".join(rows) + "\n")
        print(f"check_test_sleeps: wrote {len(rows)} files")
        return 0
    base = load()
    problems, lower = [], []
    for path in sorted(glob.glob(os.path.join(HERE, "*_test.py"))):
        rel = os.path.relpath(path, REPO)
        n, b = count(path), base.get(rel, 0)
        if n > b:
            problems.append(f"  {rel}: {n} fixed sleeps, the most allowed is {b}")
        elif n < b:
            lower.append(f"{n} {rel}")
    if lower:
        print("check_test_sleeps: fewer fixed sleeps than the baseline -- lower these lines in "
              "tools/check_test_sleeps.baseline:")
        for row in lower:
            print("  " + row)
    if problems:
        print("check_test_sleeps: a test tool gained a FIXED SLEEP -- wait on what you then assert instead:")
        print("\n".join(problems))
        print("  settle() (input handled and drawn), harness.poll(fn, timeout), tools/wait_for.sh; a sleep that IS")
        print("  the test (time passing, a rate) carries `# sleep-ok: <why>` on its line or the one above.")
        return 1
    print(f"check_test_sleeps: ok -- no test tool above its fixed-sleep count ({sum(base.values())} frozen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
