#!/usr/bin/env python3
"""bisect_build.py names the first bad commit in a toy repository -- past a
commit that does not build, and through a flake when given --runs.

    python3 tools/hostchecks/bisect_hostcheck.py
    python3 tools/hostchecks/bisect_hostcheck.py --positive-control   # must FAIL

The toy history: c1..c8 on a good c0; c5 breaks the test; c6 -- the second
midpoint -- alone has a build that fails (bisect_build.py must SKIP it, never judge it). Two tests:
a steady one, and a FLAKE that fails only every third run at a bad
commit, which `--runs 3` must still pin on c5. And a range where nothing
fails must be refused: git takes the BAD end on trust. The main checkout is
never touched: bisect_build.py is pointed at the toy repo with `--repo`.

`--positive-control` bisects the flake with ONE run per commit -- the
mistake --runs exists for -- so a bad commit passes, the bisect walks
off the answer, and the run must fail.
"""

RUNNER = {
    "covers": "bisect_build.py names the first bad commit, skips one that does not build, and survives a flake with --runs",
    "gate": True,
}

import argparse  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import tempfile  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

BISECT = toolpath.tool("bisect_build.py")


def sh(cwd, *cmd):
    subprocess.run(cmd, cwd=cwd, check=True, capture_output=True)


def toy_repo(d):
    sh(d, "git", "init", "-q", "-b", "main")
    sh(d, "git", "config", "user.name", "hostcheck")
    sh(d, "git", "config", "user.email", "hostcheck@saunaos.local")
    shas = []
    for i in range(9):
        with open(os.path.join(d, "state"), "w") as f:
            f.write("broken\n" if i >= 5 else "ok\n")
        with open(os.path.join(d, "n"), "w") as f:
            f.write(f"{i}\n")
        bb = os.path.join(d, "broken_build")
        if i == 6:
            open(bb, "w").close()
        elif os.path.exists(bb):
            os.unlink(bb)
        sh(d, "git", "add", "-A")
        sh(d, "git", "commit", "-q", "-m", f"c{i}")
        shas.append(subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=d,
                                   capture_output=True, text=True).stdout.strip())
    return shas


def bisect(repo, wt, test, runs):
    r = subprocess.run([sys.executable, BISECT, "--repo", repo, "--dir", wt, "--incremental",
                        "--build", "test ! -e broken_build", "--runs", str(runs),
                        "HEAD~8", "HEAD", "--", "sh", test],
                       capture_output=True, text=True, timeout=120)
    m = re.search(r"FIRST BAD COMMIT: \S+ (c\d)", r.stdout)
    return (m.group(1) if m else None), r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    fails = []

    def ok(name, cond, detail=""):
        print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"   {detail}" if detail and not cond else ""))
        if not cond:
            fails.append(name)

    tmp = tempfile.mkdtemp(prefix="bisect-hostcheck-")
    try:
        repo = os.path.join(tmp, "repo")
        os.makedirs(repo)
        toy_repo(repo)
        steady = os.path.join(tmp, "steady.sh")
        with open(steady, "w") as f:
            f.write("grep -qx ok state\n")
        flake = os.path.join(tmp, "flake.sh")
        count = os.path.join(tmp, "count")
        with open(flake, "w") as f:
            f.write(f"n=$(cat {count} 2>/dev/null || echo 0); n=$((n + 1)); echo $n > {count}\n"
                    f"grep -qx ok state && exit 0\n[ $((n % 3)) -eq 0 ] && exit 1\nexit 0\n")

        found, out = bisect(repo, os.path.join(tmp, "wt1"), steady, 1)
        ok("a steady failure is pinned on c5", found == "c5", out[-600:])
        ok("the commit that does not build is skipped, never judged",
           re.search(r"SKIP\s+\(the build failed.*c6$", out, re.M) is not None, out[-600:])
        ok("the worktree is removed afterwards", not os.path.exists(os.path.join(tmp, "wt1")))
        listed = subprocess.run(["git", "worktree", "list"], cwd=repo, capture_output=True, text=True).stdout
        ok("and unregistered from the repository", "wt1" not in listed, listed)

        good_test = os.path.join(tmp, "good.sh")
        with open(good_test, "w") as f:
            f.write("exit 0\n")
        found, out = bisect(repo, os.path.join(tmp, "wt3"), good_test, 1)
        ok("a range where nothing fails is refused, not given a culprit",
           found is None and "BAD end HEAD passed" in out, out[-400:])

        if os.path.exists(count):
            os.unlink(count)
        found, out = bisect(repo, os.path.join(tmp, "wt2"), flake, 1 if args.positive_control else 3)
        ok("a 1-in-3 flake is pinned on c5 with --runs 3", found == "c5", out[-600:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nbisect_hostcheck: {6 - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
