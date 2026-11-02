#!/usr/bin/env python3
"""Run the tools nothing else runs, and report which ones have rotted.

WHY THIS EXISTS
---------------
`preflight.sh` and `gui_regress.py` between them cover most of tools/.
About twenty test tools are covered by NEITHER: they need a particular
boot target, absent hardware, a second QEMU, an IWAD nobody fetched, or
simply take too long for a per-change gate. They run when somebody types
them, which is to say almost never.

**Two of them were found red by accident in one session**, having failed
for an unknown period: `fs_switch_test.py` (10 of its checks, because
`df` and `stat` became /bin programs and it still parsed the ring-0
output) and `init_test.py` (16 of 31, same class). Neither failure was a
regression anybody introduced; both were rot, and the repo had no way to
notice. That is the gap.

WHAT IT IS NOT. Not a gate, and it must never become one -- several of
these need hardware or a fetched IWAD, and a check that cannot pass on a
clean checkout is a check people learn to ignore. It is the thing you
run before a release, or when you want to know whether the on-demand
half of this directory still works.

A SKIP IS A RESULT, NOT A PASS. A tool that cannot run here says so and
is counted separately, because "19 passed" over a table with eight skips
in it is a number that means something different from what it looks
like.

    python3 tools/runners/ondemand_sweep.py               # everything runnable
    python3 tools/runners/ondemand_sweep.py --only init   # one, by name fragment
    python3 tools/runners/ondemand_sweep.py --list        # what it would run
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from private_tmp import private_tmp  # noqa: E402

HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)

import runner_meta  # noqa: E402

# EVERY tools/tests/sweep/ tool and every hostchecks/ tool that is not a
# preflight `gate` one, from each tool's own RUNNER header
# (lib/runner_meta.py). The fields that say how it must be run:
#
# `parallel` absent means it needs the one shared VM slot / the physical
# console / its own boot, and cannot share the machine with another run.
# Those are run one at a time even under -j; the rest fan out.
#
# `needs` is a precondition the sweep checks BEFORE running, so a
# missing IWAD reports as a skip with a reason rather than as a failure.
#
# `shared_vm` means the tool ATTACHES to an already-running `vm.py` guest
# rather than launching its own QEMU. Those need one started for them,
# and the sweep does it -- otherwise they fail with "could not connect
# to QMP", which is the sweep's fault and not theirs.
#
# THE HAZARD THIS FIXES IS WORSE THAN A FAILURE. Tools that launch
# their own guest sometimes leave it running, so an attaching tool
# further down the list can find somebody else's VM and PASS on it.
# That is a green line that means nothing -- which is why run_one()
# stops whatever guest is up before every tool, whatever the order.
_ENTRIES = (runner_meta.entries("tests/sweep")
            + [e for e in runner_meta.entries("hostchecks") if not e.gate and not e.manual])
TOOLS = [(e.name, " ".join([e.script] + e.args), e.covers, not e.parallel, e.needs, e.shared_vm)
         for e in _ENTRIES]

# TOOLS THAT LEAVE disk.img CHANGED, run LAST for that reason.
#
# `console_bleed` deletes /etc/services.d/toywm and cannot put it back
# -- there is nothing in the guest to copy it from -- so the image has
# no autostarted desktop until the next `make iso`. Any tool running
# after it boots into a machine it did not configure, which is this
# file's own "green line that means nothing" hazard pointed at itself.
# The sweep says so at the end rather than running `make iso` on
# somebody's behalf: re-seeding the image is not a thing a test runner
# should do unasked.
DIRTIES_IMAGE = {"console_bleed"}

# NOT here: tests/manual/, where each tool's `manual` says why no runner
# runs it (a bare-metal reboot loop, Docker, minutes of KVM), and the
# measurements in bench/, which have numbers and no verdict.


from preconditions import precondition_met  # noqa: E402,F401


VERDICT = re.compile(r"^\s*\w[\w /-]*:\s*(PASS|FAIL|FAILED|all checks passed|"
                     r"\d+/\d+ checks?|SKIP)", re.M)


def summarise(out, rc):
    """One line from the tool's own output, preferring its verdict."""
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    for ln in reversed(lines):
        if re.search(r"\b(PASS|FAIL|FAILED|SKIP|passed|checks)\b", ln):
            return ln[:96]
    return (lines[-1][:96] if lines else f"exit {rc}")


def vm(*args):
    subprocess.run([sys.executable, toolpath.tool("vm.py"), *args],
                   cwd=REPO, capture_output=True, text=True)


def shared_guest_has_desktop():
    """Boot the shared guest once and ask whether its desktop service exists."""
    vm("stop")
    vm("start")
    r = subprocess.run([sys.executable, toolpath.tool("vm.py"), "exec",
                        "ls /etc/services.d"], cwd=REPO, capture_output=True, text=True)
    vm("stop")
    return "toywm" in r.stdout


def run_one(entry, timeout, logdir):
    name, script, _what, _serial, _needs, wants_vm = entry
    # A row may carry arguments after the script name (`audio_test.py
    # --card hda`): one tool, run twice against two cards.
    script, *extra = script.split()
    path = toolpath.tool(script)

    # ALWAYS stop first, whatever this tool needs. A guest left running
    # by the previous tool is how an attaching tool passes against the
    # wrong machine, and how a launching one hits a busy QMP port.
    vm("stop")
    if wants_vm:
        vm("start")

    t0 = time.time()
    try:
        # A PRIVATE TMPDIR, removed after (private_tmp.py): the leftovers
        # of many runs once filled /tmp and ended a sweep with ENOSPC.
        with private_tmp(name) as env:
            r = subprocess.run([sys.executable, path, *extra], cwd=REPO,
                               capture_output=True, text=True, timeout=timeout,
                               env=env)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += f"\n[timed out after {timeout}s]"
        rc = 124
    dt = time.time() - t0
    vm("stop")
    if logdir:
        os.makedirs(logdir, exist_ok=True)
        with open(os.path.join(logdir, f"{name}.log"), "w") as f:
            f.write(out)
    # A tool that reports its own SKIP is a skip, not a pass -- doom_test
    # exits 0 when there is no IWAD, and counting that as a pass is how a
    # suite reports coverage it does not have. Exit 77 is automake's SKIP,
    # which tools/lib/harness.py's finish() returns when nothing was judged.
    skipped = rc == 77 or (rc == 0 and re.search(r"\bSKIP(PING)?\b", out) is not None)
    return name, ("skip" if skipped else "pass" if rc == 0 else "fail"), dt, summarise(out, rc)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--only", help="run tools whose name contains this")
    ap.add_argument("--list", action="store_true", help="print what would run")
    ap.add_argument("--timeout", type=float, default=600.0, help="per tool (default 600s)")
    ap.add_argument("--logs", metavar="DIR", help="write each tool's output here")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="parallel jobs for the tools that allow it. DEFAULT 1: "
                         "nearly all of these take the shared VM slot or the "
                         "physical console, so this is not gui_regress and "
                         "raising it is usually wrong.")
    args = ap.parse_args()

    # AN EXACT NAME WINS over the substring match: `--only console` also
    # matched `console_bleed`, which deletes the desktop service and left
    # every later attaching tool with no desktop (2026-09-30).
    exact = [t for t in TOOLS if args.only and t[0] == args.only]
    chosen = exact or [t for t in TOOLS if not args.only or args.only in t[0]]
    if not chosen:
        sys.exit(f"ondemand_sweep: nothing matches --only {args.only!r}")

    if args.list:
        for name, script, what, serial, needs, _vm in chosen:
            ok, why = precondition_met(needs)
            flag = "" if ok else f"   [would skip: {why}]"
            print(f"  {name:<13} {script:<26} {what}{flag}")
        return 0
    import tree_lock
    tree_lock.hold()   # every tool reads this build

    # THE SHARED GUEST MUST HAVE A DESKTOP, or every attaching tool fails
    # at once with `no provider named gui`, twenty lines that each look
    # like their own bug. A tool that deletes the service and dies before
    # restoring it (stdin, ansi and console_bleed all remove it), so an image one earlier
    # sweep dirtied stays without one until `make iso`: asked here,
    # answered in one line.
    if any(e[5] for e in chosen) and not shared_guest_has_desktop():
        sys.exit("ondemand_sweep: disk.img has no /etc/services.d/toywm, so the shared guest "
                 "boots with NO desktop and every tool that attaches to it would fail.\n"
                 "  `make iso` puts it back (a tool that removes it -- console_bleed, stdin, "
                 "ansi -- must restore it; one that did not, or died first, left it gone)")

    results = []
    runnable = []
    for entry in chosen:
        ok, why = precondition_met(entry[4])
        if not ok:
            results.append((entry[0], "skip", 0.0, why))
        else:
            runnable.append(entry)

    print(f"ondemand_sweep: {len(runnable)} tool(s) to run, "
          f"{len(results)} skipped up front\n")

    # A tool that changes disk.img runs after everything else, or the
    # tools behind it boot a machine it reconfigured -- see
    # DIRTIES_IMAGE.
    runnable.sort(key=lambda e: e[0] in DIRTIES_IMAGE)
    serial = [e for e in runnable if e[3]]
    parallel = [e for e in runnable if not e[3]]

    if parallel and args.jobs > 1:
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for r in ex.map(lambda e: run_one(e, args.timeout, args.logs), parallel):
                results.append(r)
                print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}")
    else:
        serial = parallel + serial

    for entry in serial:
        r = run_one(entry, args.timeout, args.logs)
        results.append(r)
        print(f"  {r[1].upper():<5} {r[0]:<13} {r[2]:5.0f}s  {r[3]}", flush=True)

    npass = sum(1 for r in results if r[1] == "pass")
    nfail = sum(1 for r in results if r[1] == "fail")
    nskip = sum(1 for r in results if r[1] == "skip")
    print(f"\nondemand_sweep: {npass} pass, {nfail} fail, {nskip} skip")
    dirtied = sorted(r[0] for r in results if r[0] in DIRTIES_IMAGE and r[1] != "skip")
    if dirtied:
        print(f"\ndisk.img was changed by: {', '.join(dirtied)}"
              "\n  Run `make iso` before anything else boots it.")
    if nfail:
        print("\nFAILED:")
        for r in results:
            if r[1] == "fail":
                print(f"  {r[0]:<13} {r[3]}")
        print("\nBefore assuming a regression: most of these are ON DEMAND and may")
        print("have rotted rather than broken. `python3 tools/dev/predates.py` will say")
        print("which, and a dirty disk.img explains more failures than it should --")
        print("`make clean-disk && make iso` first.")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
