#!/usr/bin/env python3
"""Which commit broke it? `git bisect run` in a SCRATCH WORKTREE, each
commit built clean and the test run N times.

    python3 tools/dev/bisect_build.py GOOD BAD -- python3 tools/tests/gui/fm_back_test.py --instance 9
    python3 tools/dev/bisect_build.py --runs 5 v0.9.0 HEAD -- make test
    python3 tools/dev/bisect_build.py --build "make all" a1b2c3 HEAD -- python3 tools/tests/sweep/x.py

WHY A WORKTREE: the main checkout is often held by a running test
(tree_lock.py refuses `make` under it) or carries work in progress, and
a bisect checks out a dozen commits. The worktree has its own build,
its own disk.img and its own test lock, so the main tree is never
touched and a preflight can run beside it. It lives under
~/.cache/saunaos/bisect/, not /tmp: /tmp is RAM, and a build is
hundreds of megabytes.

EACH STEP BUILDS FROM CLEAN -- `make clean` and `make clean-disk`
before `--build` -- because `seed/sync/` keeps a file a later commit
deleted, so an incremental bisect over a data change convicts the wrong
commit (it did, 2026-11-13: a bisect named 53b40ef5 for a failure it
could not have caused). `--incremental` skips the cleaning when the
range touches only code.

A FLAKE MAKES A ONE-RUN BISECT LIE: an intermittent failure that happens
to pass at a bad commit marks it good, and the bisect then walks off
the right answer with no sign of it. `--runs N` calls a commit good
only when all N runs pass; a 1-in-k flake still slips through a step
with probability (1 - 1/k)^N, which the summary prints for `--rate k`.

BOTH ENDS ARE TESTED FIRST, because `git bisect run` takes them on trust:
a BAD end that passes would otherwise "find" a first bad commit in a
range where nothing is bad. `--trust-ends` skips the two builds.

A commit whose build fails, or where the test's own script does not
exist yet, is SKIPPED (exit 125), never judged. A test exiting 77
(automake's SKIP, harness.Results with every check skipped) is a skip
too.

build.local.conf, if the main tree has one, is copied in -- the
worktree builds what this checkout builds; `--no-local-conf` stops it.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

REPO = toolpath.REPO
SELF = os.path.abspath(__file__)
CACHE = os.path.expanduser("~/.cache/saunaos/bisect")
SKIP, GOOD, BAD = 125, 0, 1


def git(cwd, *args, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"bisect: git {' '.join(args)} failed:\n{r.stderr.strip()}")
    return r.stdout.strip()


def _log(log, text):
    with open(log, "a") as f:
        f.write(text if text.endswith("\n") else text + "\n")


def _sh(cmd, cwd, log):
    _log(log, f"$ {cmd}")
    r = subprocess.run(cmd, shell=True, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    _log(log, r.stdout)
    return r.returncode, r.stdout


def _missing_script(test, cwd):
    """The test's own script, when the command names one in the tree that
    this commit does not have yet."""
    for word in shlex.split(test):
        if word.endswith((".py", ".sh")) and not os.path.isabs(word) and "/" in word:
            if not os.path.exists(os.path.join(cwd, word)):
                return word
    return None


def step(args):
    """One `git bisect run` call: build this commit, run the test N times."""
    cwd, log = os.getcwd(), args.log
    sha = git(cwd, "rev-parse", "--short", "HEAD")
    subject = git(cwd, "log", "-1", "--format=%s")
    _log(log, f"\n=== {sha} {subject}")
    missing = _missing_script(args.test, cwd)
    if missing:
        print(f"bisect: {sha} SKIP  ({missing} does not exist at this commit)  {subject[:60]}", flush=True)
        return SKIP
    t0 = time.time()
    if not args.incremental:
        for clean in ("make clean", "make clean-disk"):
            _sh(clean, cwd, log)
    rc, _ = _sh(args.build, cwd, log)
    if rc != 0:
        print(f"bisect: {sha} SKIP  (the build failed; {log})  {subject[:60]}", flush=True)
        return SKIP
    built = time.time() - t0
    fails = skips = 0
    for i in range(args.runs):
        rc, _ = _sh(args.test, cwd, log)
        if rc == 77:
            skips += 1
        elif rc != 0:
            fails += 1
            break                       # one failure decides BAD; the rest would only cost time
    ran = i + 1
    if fails:
        verdict, code = "BAD ", BAD
    elif skips == ran:
        verdict, code = "SKIP", SKIP
    else:
        verdict, code = "good", GOOD
    print(f"bisect: {sha} {verdict}  ({fails} failed in {ran} run(s); built in {built:.0f} s)  "
          f"{subject[:60]}", flush=True)
    return code


def main():
    ap = argparse.ArgumentParser(
        description="git bisect run in a scratch worktree: each commit built clean, the test run N times",
        usage="bisect_build.py [options] GOOD BAD -- TEST...")
    ap.add_argument("good")
    ap.add_argument("bad")
    ap.add_argument("test", nargs=argparse.REMAINDER, help="after --: the test command, run in the worktree")
    ap.add_argument("--runs", type=int, default=1, help="runs per commit; good only when all pass (default 1)")
    ap.add_argument("--rate", type=int, metavar="K", help="the flake is about 1 in K: print the chance a step missed it")
    ap.add_argument("--build", default=f"make -j{os.cpu_count() or 4} iso", help="the build (default: make -jN iso)")
    ap.add_argument("--incremental", action="store_true", help="skip make clean / clean-disk between steps")
    ap.add_argument("--repo", default=REPO, help=argparse.SUPPRESS)       # the host check's toy repo
    ap.add_argument("--dir", help=f"the worktree (default under {CACHE})")
    ap.add_argument("--trust-ends", action="store_true", help="do not run the test at GOOD and BAD first")
    ap.add_argument("--keep", action="store_true", help="leave the worktree and its build behind")
    ap.add_argument("--no-local-conf", action="store_true", help="do not copy build.local.conf in")
    ap.add_argument("--step", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--log", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.test and args.test[0] == "--":
        args.test = args.test[1:]
    if not args.test:
        ap.error("the test command goes after --")
    args.test = " ".join(shlex.quote(w) for w in args.test) if len(args.test) > 1 else args.test[0]
    if args.runs < 1:
        ap.error("--runs must be at least 1")

    if args.step:
        sys.exit(step(args))

    repo = os.path.abspath(args.repo)
    good = git(repo, "rev-parse", "--verify", args.good + "^{commit}")
    bad = git(repo, "rev-parse", "--verify", args.bad + "^{commit}")
    if subprocess.run(["git", "merge-base", "--is-ancestor", good, bad], cwd=repo).returncode:
        sys.exit(f"bisect: {args.good} is not an ancestor of {args.bad}")
    wt = os.path.abspath(args.dir or os.path.join(CACHE, time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(os.path.dirname(wt), exist_ok=True)
    git(repo, "worktree", "add", "--detach", wt, bad)
    log = os.path.join(os.path.dirname(wt), os.path.basename(wt) + ".log")
    n = int(git(repo, "rev-list", "--count", f"{good}..{bad}"))
    print(f"bisect: {n} commit(s) in {args.good}..{args.bad}, about {max(1, n.bit_length())} step(s); "
          f"worktree {wt}\nbisect: full log {log}", flush=True)
    local = os.path.join(repo, "build.local.conf")
    if os.path.exists(local) and not args.no_local_conf:
        shutil.copy2(local, os.path.join(wt, "build.local.conf"))
        print("bisect: build.local.conf copied in (--no-local-conf to build build.conf alone)")
    first_bad = None
    cmd = [sys.executable, SELF, "--step", "--log", log, "--runs", str(args.runs),
           "--build", args.build] + (["--incremental"] if args.incremental else []) + \
          [good, bad, "--", args.test]
    try:
        # THE ENDS ARE JUDGED FIRST: `git bisect run` takes them on trust, so
        # a BAD end that passes "finds" a first bad commit in a range where
        # nothing is bad.
        if not args.trust_ends:
            for sha, want, what in ((bad, BAD, "BAD"), (good, GOOD, "GOOD")):
                git(wt, "checkout", "-q", "--detach", sha)
                got = subprocess.run(cmd, cwd=wt).returncode
                if got != want:
                    print(f"bisect: the {what} end {args.bad if want == BAD else args.good} "
                          f"{'passed' if got == GOOD else 'could not be judged' if got == SKIP else 'failed'}"
                          f" -- nothing to bisect (--trust-ends skips this check)")
                    return 1
        git(wt, "bisect", "start", bad, good)
        p = subprocess.Popen(["git", "bisect", "run", *cmd], cwd=wt, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True,
                             env={**os.environ, "LC_ALL": "C"})     # its verdict line is parsed
        lines = []
        for line in p.stdout:                   # live: a step is minutes
            lines.append(line.rstrip("\n"))
            if line.startswith("bisect: "):
                print(line, end="", flush=True)
        p.wait()
        out = "\n".join(lines)
        _log(log, out)
        for line in lines:
            m = re.match(r"^([0-9a-f]{40}) is the first '?bad'? commit", line)   # git >= 2.46 quotes the term
            if m:
                first_bad = m.group(1)
        at = next((i for i, ln in enumerate(lines) if "only 'skip'ped commits left" in ln), None)
        if not first_bad and at is not None:
            # A skipped commit sits next to the answer: git names every candidate.
            print("bisect: no single answer -- a commit that could not be judged is next to it; "
                  "the first bad commit is one of:")
            for line in lines[at + 1:]:
                if re.match(r"^[0-9a-f]{7,}", line):
                    print("  " + git(repo, "log", "-1", "--format=%h %s", line.split()[0]))
            return 2
    finally:
        git(wt, "bisect", "reset", check=False)
        if not args.keep:
            git(repo, "worktree", "remove", "--force", wt, check=False)
            shutil.rmtree(wt, ignore_errors=True)
        else:
            print(f"bisect: worktree kept at {wt} (git worktree remove {wt})")
    if not first_bad:
        print("bisect: no verdict -- read the log")
        return 1
    print(f"\nbisect: FIRST BAD COMMIT: {git(repo, 'log', '-1', '--format=%h %s', first_bad)}")
    if args.rate:
        miss = (1 - 1 / args.rate) ** args.runs
        print(f"bisect: a 1-in-{args.rate} flake passes all {args.runs} run(s) of a bad commit with "
              f"probability {miss:.0%} at each step -- re-run the named commit and its parent with "
              f"more runs before believing it")
    elif args.runs == 1:
        print("bisect: one run per commit -- for an intermittent failure, confirm with --runs N")
    return 0


if __name__ == "__main__":
    sys.exit(main())
