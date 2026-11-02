#!/usr/bin/env python3
"""Run the static checks: every tools/checks/ rule, in parallel, one verdict.

Each rule is a standalone script in tools/checks/ -- found by being
there, so a new rule is in preflight the moment it is saved -- that
exits non-zero naming what it found. Each keeps its own named waiver
(`widget-ops-ok:`, `dispatch-ok:`...; CLAUDE.md's table), because the
waiver names the mechanism being excused, not just the rule.

    python3 tools/runners/checks.py                 # every rule
    python3 tools/runners/checks.py -k widget -k key_routing
    python3 tools/runners/checks.py --list

A rule that needs the build (check_deps, check_layout) runs after
`make all && make iso`, which is where preflight.sh calls this.
"""
import argparse
import ast
import concurrent.futures as cf
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402

CHECKS = os.path.join(toolpath.TOOLS, "checks")


def rules():
    return sorted(os.path.join(CHECKS, f) for f in os.listdir(CHECKS) if f.endswith(".py"))


def summary(path):
    """The first line of a rule's docstring: what it fails on."""
    try:
        doc = ast.get_docstring(ast.parse(open(path, encoding="utf-8").read())) or ""
    except SyntaxError:
        return "(does not parse)"
    return doc.strip().splitlines()[0] if doc.strip() else ""


def run(path, timeout):
    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, path], cwd=toolpath.REPO, capture_output=True,
                           text=True, timeout=timeout)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        out, rc = f"[timed out after {timeout}s]", 124
    return path, rc, time.time() - t0, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("-k", action="append", default=[], metavar="NAME",
                    help="only rules whose file name contains this (repeatable)")
    ap.add_argument("--list", action="store_true", help="print the rules")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    chosen = [p for p in rules() if not args.k or any(k in os.path.basename(p) for k in args.k)]
    if not chosen:
        sys.exit(f"checks: nothing matches {args.k}")
    if args.list:
        for p in chosen:
            print(f"  {os.path.basename(p)[:-3]:<22} {summary(p)[:76]}")
        return 0

    failed = []
    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for path, rc, secs, out in ex.map(lambda p: run(p, args.timeout), chosen):
            name = os.path.basename(path)[:-3]
            last = next((ln.strip() for ln in reversed(out.splitlines()) if ln.strip()), f"exit {rc}")
            print(f"  {'PASS' if rc == 0 else 'FAIL':<5} {name:<22} {secs:4.1f}s  {last[:80]}", flush=True)
            if rc != 0:
                failed.append((name, out))
    for name, out in failed:
        print(f"\nFAILED: {name}\n" + "\n".join("    " + ln for ln in out.rstrip().splitlines()))
    print(f"\nchecks: {len(chosen) - len(failed)} pass, {len(failed)} fail")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
