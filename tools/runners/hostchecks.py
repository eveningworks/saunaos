#!/usr/bin/env python3
"""Run the host checks: SaunaOS code built with the HOST's gcc, judged by
an implementation that shares nothing with it.

Every tools/hostchecks/ tool, found by its folder and described by its own
RUNNER header (lib/runner_meta.py). Each stays a standalone script --
`python3 tools/hostchecks/uhlp_hostcheck.py` still works -- and this is
the one command that runs all of them, or the `gate` ones preflight.sh
needs, in parallel: they are host-only, so nothing here boots a guest.

A missing oracle (ffmpeg, Pillow, mtools...) is a SKIP with its reason,
from the tool's `needs`; a tool whose RUNNER says `manual` is listed and
never run.

    python3 tools/runners/hostchecks.py                  # every runnable one
    python3 tools/runners/hostchecks.py --gate           # what preflight runs
    python3 tools/runners/hostchecks.py -k uhlp -k uimg  # by name fragment
    python3 tools/runners/hostchecks.py --positive-control   # each must FAIL
    python3 tools/runners/hostchecks.py --list
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import runner_meta  # noqa: E402
from preconditions import precondition_met  # noqa: E402
from private_tmp import private_tmp  # noqa: E402

EXIT_SKIP = 77


def has_control(e):
    return "--positive-control" in open(e.path, encoding="utf-8").read()


def run_one(e, control, timeout, logdir):
    argv = [sys.executable, e.path, *e.args] + (["--positive-control"] if control else [])
    t0 = time.time()
    try:
        with private_tmp(e.name) as env:
            r = subprocess.run(argv, cwd=toolpath.REPO, capture_output=True, text=True,
                               timeout=timeout, env=env)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired as ex:
        out, rc = (ex.stdout or "") if isinstance(ex.stdout, str) else "", 124
        out += f"\n[timed out after {timeout}s]"
    if logdir:
        os.makedirs(logdir, exist_ok=True)
        with open(os.path.join(logdir, e.name + ".log"), "w") as f:
            f.write(out)
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    if control:
        # A control PASSES when the check it controls went red.
        verdict = "pass" if rc not in (0, EXIT_SKIP) else ("skip" if rc == EXIT_SKIP else "fail")
    else:
        verdict = "skip" if rc == EXIT_SKIP else "pass" if rc == 0 else "fail"
    return e, verdict, time.time() - t0, (lines[-1][:90] if lines else f"exit {rc}"), out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--gate", action="store_true", help="only the ones preflight.sh runs")
    ap.add_argument("-k", action="append", default=[], metavar="NAME",
                    help="only names containing this (repeatable)")
    ap.add_argument("--positive-control", action="store_true",
                    help="break each check's source copy: every one must FAIL")
    ap.add_argument("--list", action="store_true", help="print what would run")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=900.0, help="per check (default 900s)")
    ap.add_argument("--logs", metavar="DIR", help="write each check's output here")
    ap.add_argument("-v", "--verbose", action="store_true", help="print a failing check's whole output")
    args = ap.parse_args()

    chosen = [e for e in runner_meta.entries("hostchecks")
              if (not args.gate or e.gate) and (not args.k or any(k in e.name for k in args.k))]
    if not chosen:
        sys.exit(f"hostchecks: nothing matches {args.k}")

    runnable, results = [], []
    for e in chosen:
        ok, why = precondition_met(e.needs)
        if e.manual:
            results.append((e, "manual", 0.0, e.manual, ""))
        elif not ok:
            results.append((e, "skip", 0.0, why, ""))
        elif args.positive_control and not has_control(e):
            results.append((e, "skip", 0.0, "has no --positive-control", ""))
        else:
            runnable.append(e)

    if args.list:
        for e in chosen:
            why = next((r[3] for r in results if r[0] is e), "")
            flag = "gate " if e.gate else "     "
            print(f"  {flag}{e.name:<16} {e.covers[:62]}" + (f"   [{why}]" if why else ""))
        return 0

    print(f"hostchecks: {len(runnable)} to run, {len(results)} not runnable here"
          + (" -- POSITIVE CONTROL: each must fail" if args.positive_control else ""), flush=True)
    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [ex.submit(run_one, e, args.positive_control, args.timeout, args.logs) for e in runnable]
        for f in cf.as_completed(futs):
            r = f.result()
            results.append(r)
            print(f"  {r[1].upper():<6} {r[0].name:<16} {r[2]:5.0f}s  {r[3]}", flush=True)

    fails = [r for r in results if r[1] == "fail"]
    n = {k: sum(1 for r in results if r[1] == k) for k in ("pass", "fail", "skip", "manual")}
    print(f"\nhostchecks: {n['pass']} pass, {n['fail']} fail, {n['skip']} skip, {n['manual']} by hand")
    for e, _, _, summary, out in fails:
        print(f"\nFAILED: {e.name} ({e.rel})")
        tail = out.splitlines() if args.verbose else out.splitlines()[-25:]
        print("\n".join("    " + ln for ln in tail))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
