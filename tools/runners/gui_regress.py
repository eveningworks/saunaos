#!/usr/bin/env python3
"""tools/runners/gui_regress.py -- run every GUI test tool, each on a fresh VM.

WHAT THIS IS
------------
The GUI test tools (`uidemo_test.py`, `gfxdemo_test.py`,
`notepad_client_test.py`, ...) each drive one app and assert on its log.
Running them is the standard check after touching `userland/ui/`,
`userland/`, or anything the window manager draws -- and doing that by
hand means retyping the same six commands, each with its own image copy
and VM lifecycle. This is that sequence, once, with a summary table.

    python3 tools/runners/gui_regress.py                 # all of them, 4 at a time
    python3 tools/runners/gui_regress.py -j1             # one at a time (the old behaviour)
    python3 tools/runners/gui_regress.py -k uidemo -k gfxdemo    # a subset
    python3 tools/runners/gui_regress.py --list
    echo $?                                      # 0 = every tool passed

WHY IT RUNS THEM IN PARALLEL
----------------------------
The tools are independent by construction (see below) and each spends
almost all of its wall clock waiting on an emulated machine, so running
them one after another wasted most of the host. Each concurrent tool
gets a VM SLOT -- `vm.py --instance N`, which derives that VM's
pidfile, serial socket, QMP port and VNC display from N, and which the
tool is pointed at with `--sock`/`--qmp-port`. Nothing is shared, so
the isolation the per-tool image and per-tool VM already provided is
unchanged; only the scheduling is.

Tools are STARTED longest-first (measured costs, `pick_order` below), because
a parallel run cannot finish before its slowest member does and
`forcequit` (71s of genuinely waiting out ping timeouts) previously sat
eleventh of fourteen and finished alone after everything else had
drained. The summary table is still printed in the declared dependency
order -- only the start order changed.

WHY EACH TOOL GETS ITS OWN IMAGE AND ITS OWN VM
-----------------------------------------------
This is the part that is easy to get wrong and expensive to debug.

  * **A fresh disk copy per tool.** Several of these write files
    (Notepad saves, the Terminal's shell creates things). A tool
    inheriting the previous one's filesystem hits "file already exists"
    paths its assertions never anticipated. The copy also keeps the
    real `disk.img` untouched, which matters if the user has their own
    QEMU open -- see CLAUDE.md on the write lock.
  * **A fresh VM per tool.** Each tool enters GUI mode itself and
    expects an empty desktop. Handing one a desktop with three windows
    already open fails in ways that look exactly like real widget bugs
    -- a click lands on the wrong window, and the log line that proves
    it is in a scrollback nobody reads.

`cp --reflink=auto` makes the copies nearly free on btrfs/XFS, and the
image is sparse besides (~3 MB actually allocated against a 9 GB
apparent size), so the per-tool copy costs far less than it looks.

WHAT THIS DOES NOT COVER
------------------------
`tools/tests/manual/damage_sweep.py` is deliberately NOT in the list: it takes a
different kind of run (`gui damage verify on` makes every frame render
twice, so the whole thing is several times slower) and it has its own
`--positive-control` protocol. Run it separately after touching
anything that draws, damages, focuses or changes window chrome.

Nor does this build anything. Run `make all && make iso` first -- these
tools drive whatever is already on the image.
"""

import argparse
import atexit
import concurrent.futures as cf
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
import fresh_disk  # noqa: E402
from harness import copy_disk  # noqa: E402
from private_tmp import private_tmp  # noqa: E402
import test_history  # noqa: E402

def physical_cores():
    """Cores (not SMT threads) among the CPUs this process may run on."""
    cpus = os.sched_getaffinity(0)
    cores = set()
    for cpu in cpus:
        topo = f"/sys/devices/system/cpu/cpu{cpu}/topology"
        try:
            with open(f"{topo}/physical_package_id") as f:
                pkg = f.read().strip()
            with open(f"{topo}/core_id") as f:
                cores.add((pkg, f.read().strip()))
        except OSError:
            return max(1, len(cpus) // 2)
    return len(cores)


# How many tools run at once by default: ONE GUEST PER PHYSICAL CORE,
# capped at 16. `-j1` restores the original serial behaviour exactly.
#
# WHETHER THE SMT SIBLING HELPS DEPENDS ON THE HOST'S ALL-CORE CLOCK.
# Measured 2026-10-09 on the first 8-core/16-thread host (fe39489f):
# 8 jobs 825s, 12 jobs 625-644s, 16 jobs 554-594s, all green -- the suite
# waits in settles and polls, so a waiting guest leaves its thread to
# another. On its replacement (an i7-11700, 2026-10-24, 4dbec864) 16 jobs
# took 653s with 15 tools red, every one a window or the desktop not up
# within its wait; the same 15 passed at 8, and a full run at 8 took 979s
# with 3 red (fmprogress, doomdata, w16). `-j 16` is still the faster run
# on a host where it is green.
#
# THE FLOOR IS THE SLOWEST TOOL: ~5,950 tool-seconds over 16 jobs is
# ~370s, and screensaver_test alone is ~260s, so past 16 the run cannot
# get much shorter -- the cap is where measuring stopped, not a guess.
# A new tool that waits on a fixed sleep rather than on what the app
# says is what turns more jobs into flakes; COST_S orders the longest
# first so the tail is short.
DEFAULT_JOBS = min(16, max(2, physical_cores()))

import runner_meta  # noqa: E402

# EVERY tools/tests/gui/ tool, from its own RUNNER header (lib/runner_meta.py):
# a tool is in this suite by being in that folder, and says what it covers
# and any guest flags it needs (`vm`) itself.
_ENTRIES = runner_meta.entries("tests/gui")

# Reported first: the widget toolkit, so a toolkit regression reads before
# the apps built on it start failing for what looks like their own reasons.
# Everything else is reported by name.
FIRST = ("uidemo", "uiclient", "winclient", "gfxdemo")
_ENTRIES.sort(key=lambda e: (FIRST.index(e.name) if e.name in FIRST else len(FIRST), e.name))
TOOLS = [(e.name, e.script, e.covers) for e in _ENTRIES]
EXTRA_VM_ARGS = {e.name: e.vm for e in _ENTRIES if e.vm}


def _measured_costs(rows=None):
    """Each tool's median duration over its last ten PASSING runs, from the
    local run history (test_history.py) -- measured, never hand-kept. Used
    ONLY for the start order (pick_order), so a stale or missing number
    costs scheduling and never a result."""
    by = {}
    for r in rows if rows is not None else test_history.load():
        if r.get("verdict") == "PASS" and r.get("duration_s"):
            by.setdefault(r.get("tool"), []).append(r["duration_s"])
    out = {}
    for e in _ENTRIES:
        d = by.get(os.path.splitext(e.script)[0], [])[-10:]
        if d:
            out[e.name] = sorted(d)[len(d) // 2]
    return out


# A tool never measured here is assumed SLOW rather than fast: started
# early, a quick tool finishes early, while a slow one assumed quick
# becomes the straggler everything else waits behind.
COST_UNKNOWN_S = 90
COST_S = _measured_costs()


def pick_order(picked):
    """Longest job first -- the standard fix for a parallel makespan.

    Order matters here for one reason: with `-j4`, the run cannot finish
    before its slowest tool does, so a slow tool that STARTS late adds
    its whole duration to the tail. `forcequit` (71s) sat eleventh of
    fourteen in dependency order and finished alone, well after the
    other thirteen had drained -- 339s of tool-time took 116s of wall
    clock when the theoretical floor was ~85s.

    This is LPT scheduling (Graham 1969), which is within 4/3 of optimal
    and costs one sort. Deliberately applied to the START order ONLY:
    the summary table is still rebuilt in the declared dependency order,
    so a toolkit regression still reads before the apps built on it.

    Ties keep declaration order (`sorted` is stable), so the ordering is
    deterministic and a run is reproducible.
    """
    return sorted(picked, key=lambda t: -COST_S.get(t[0], COST_UNKNOWN_S))


def run_one(name, script, disk_src, timeout, keep_logs, slot, kvm=False):
    """Run one tool against its own image, in VM slot `slot`.

    Everything that could collide between two concurrently running
    tools is derived from `slot`: vm.py's pidfile/serial socket/QMP
    port/VNC display (`--instance`), and the `--sock`/`--qmp-port` the
    tool itself connects with. The disk copy is already per-tool.

    Note the `stop` below is also slot-scoped -- it used to be a bare
    `vm.py stop`, which under parallelism would have killed a sibling's
    VM rather than a leftover of its own.
    """
    tool = toolpath.tool(script)
    if not os.path.exists(tool):
        return ("SKIP", 0.0, f"no such tool: {script}")

    img = os.path.join(tempfile.gettempdir(), f"gui_regress_{name}.img")
    vm = toolpath.tool("vm.py")
    inst = ["--instance", str(slot)]

    subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                   capture_output=True)
    # --reflink=auto: a copy-on-write clone where the filesystem
    # supports it, a plain copy where it doesn't. Never --reflink=always,
    # which fails outright on ext4. --sparse=always because disk.img is
    # ~4 MB of data in a 9 GB sparse file, and the destination is
    # usually /tmp on a tmpfs -- filling the holes in would cost 9 GB of
    # RAM per tool (`cp` defaults to --sparse=auto and gets this right
    # already; stating it means a future edit cannot quietly lose it,
    # which is exactly how damage_hunt.py's shutil.copyfile did).
    copy_disk(disk_src, img, cwd=REPO)

    # KVM is a per-run choice, passed straight to vm.py. Opt-in, never
    # the default -- see launch_qemu_cmd()'s comment on why a gate
    # quietly switched to KVM stops covering the emulated path.
    kvm_args = ["--kvm"] if kvm else []

    started = time.time()
    try:
        # CHECK vm.py's exit code. It used to be ignored, so a guest
        # that never started -- a refused port, a stale ISO, a QEMU
        # that died -- showed up only as the TOOL failing with
        # "could not connect to QMP", which says nothing about why and
        # points at the tool rather than at the launch. Eighteen tools
        # reported that at once before this existed, and the actual
        # reason was one line on vm.py's stderr that nobody read.
        boot = subprocess.run([sys.executable, vm] + inst + kvm_args +
                              EXTRA_VM_ARGS.get(name, []) +
                              ["--disk", img, "start"],
                              cwd=REPO, capture_output=True, text=True,
                              timeout=120)
        if boot.returncode != 0:
            why = (boot.stderr or boot.stdout or "").strip().splitlines()
            return ("FAIL", time.time() - started,
                    "the guest never started: "
                    + (why[0] if why else f"vm.py exited {boot.returncode}"))
        # -u, because the partial output below is the whole diagnosis
        # and without it there is none. A captured child's stdout is a
        # pipe, so Python block-buffers it: a tool killed by the guard
        # has flushed nothing, and the timeout branch reports "NO
        # output at all" for a tool that had printed thirty checks.
        # A PRIVATE TMPDIR, removed after: tools leave disk copies and
        # screenshots under tempfile, and /tmp is a tmpfs (private_tmp.py).
        with private_tmp(name, _child_env()) as env:
            r = subprocess.run([sys.executable, "-u", tool,
                                "--instance", str(slot)],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=timeout, env=env)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        # KEEP WHAT IT MANAGED TO SAY. A timed-out tool used to report
        # the bare word TIMEOUT, so a run that hit the guard showed zero
        # checks and no hint of WHERE -- indistinguishable from a tool
        # that crashed on line one, and the reason a `files` that merely
        # grew past the guard read as a hang. The partial output names
        # the last check that passed, which is the whole diagnosis.
        partial = ""
        for chunk in (e.stdout, e.stderr):
            if not chunk:
                continue
            partial += chunk if isinstance(chunk, str) else chunk.decode(
                "utf-8", errors="replace")
        tail = partial.strip().splitlines()[-25:]
        out = (f"TIMEOUT after {timeout}s -- raise --timeout if the tool "
               f"merely grew; the last checks it printed were:\n"
               + "\n".join(tail) if tail else f"TIMEOUT after {timeout}s "
               f"with NO output at all (it hung before its first check)")
        rc = 124
    finally:
        subprocess.run([sys.executable, vm] + inst + ["stop"], cwd=REPO,
                       capture_output=True)
        # The per-tool image has served its purpose once the VM is
        # stopped. Left behind, a full run leaves thirteen of them in
        # /tmp -- sparse, so cheap on disk, but that is RAM on a tmpfs
        # and they are stale the moment `make iso` reseeds disk.img.
        try:
            os.unlink(img)
        except OSError:
            pass

    if keep_logs:
        # The slot goes in the LOG, not only on the console. An
        # intermittent failure is diagnosed after the fact from these
        # files, and "which slot was it on" is the first question asked
        # of one -- it was unanswerable once already, which is how a
        # slot correlation ended up recorded in docs/roadmap.md on no
        # evidence and had to be withdrawn.
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
            f.write(f"# tool={name} slot={slot} qmp={4445 + slot} "
                    f"serial={'.vm.serial' if slot == 0 else f'.vm.{slot}.serial'}\n")
            f.write(out)

    # The tools all print a "<name>: N passed, M failed" summary line;
    # pull it out for the table, but fall back to the last line rather
    # than to nothing if a tool died before printing one.
    summary = ""
    for line in out.splitlines():
        if "passed," in line and "failed" in line:
            summary = line.strip()
    if not summary:
        tail = [l for l in out.splitlines() if l.strip()]
        summary = tail[-1].strip() if tail else "(no output)"

    if rc == EXIT_SKIP:
        summary = "nothing judged -- " + summary
    _record_tool(script, rc, time.time() - started, summary, out, f"slot {slot}")
    return (status_of(rc), time.time() - started, summary)


def _child_env(base=None):
    """A tool's environment: this run records its row (test_history.py)."""
    env = dict(base if base is not None else os.environ)
    env[test_history.BY_PARENT_ENV] = "1"
    return env


def _record_tool(script, rc, secs, summary, out, target):
    """One history row per tool, under its script's name -- the name
    harness.Results records it by when run by hand."""
    fails = [ln.strip()[4:].strip() for ln in out.splitlines() if ln.lstrip().startswith("FAIL  ")]
    if rc == 124:
        fails.append("TIMEOUT")
    elif rc not in (0, EXIT_SKIP) and not fails:
        fails.append(summary[:200])
    test_history.record(os.path.splitext(script)[0], status_of(rc), rc=rc, duration=secs,
                        failures=fails, summary=summary, log_text=out, argv=[],
                        target=target, hints=False)


# automake's SKIP convention, which tools/lib/harness.py's finish() follows:
# a run that judged NOTHING (every check skipped) is neither a pass nor
# a fail, and is counted on its own line.
EXIT_SKIP = 77


def status_of(rc):
    return "PASS" if rc == 0 else "SKIP" if rc == EXIT_SKIP else "FAIL"


def report_skips(results):
    skipped = [r[0] for r in results if r[1] == "SKIP"]
    if skipped:
        print(f"\ngui_regress: {len(skipped)} tool(s) skipped, judging nothing: "
              f"{', '.join(skipped)}")


def all_clear(results, where=""):
    """Never a bare "all clear" while anything skipped: the skips are named."""
    skipped = [r for r in results if r[1] == "SKIP"]
    line = "gui_regress: all clear" + where
    if skipped:
        line += f", {len(skipped)} skipped: " + ", ".join(
            f"{r[0]} ({r[3].split(' -- ')[0]})" for r in skipped)
    return line


def run_remote_suite(picked, args):
    """The whole suite against one bare-metal machine, SERIALLY.

    There is one laptop: no slots, no parallelism, and nothing to copy.
    That also means this can never be a gate -- it is a spot-check tier
    like `qemu_matrix.py`, run when a change is hardware-shaped or
    before a release.

    ASK THE MACHINE FIRST, and say which one did not answer: the Lenovo
    is not always switched on (local_info.txt), and silence from it is a
    machine that is off rather than fifty broken tools.
    """
    import remote_gui

    host = args.host
    if not remote_gui.machine_is_up(host):
        print(f"gui_regress: {host} is not answering on telnet -- is it "
              f"powered on and booted? (local_info.txt lists the machines)")
        return 2
    con = remote_gui.RemoteConsole(host=host, quiet=True)
    try:
        if not remote_gui.desktop_is_up(con):
            print(f"gui_regress: {host} answers, but no desktop is running "
                  f"-- `guictl state` says nothing. It may be at a shell "
                  f"prompt or mid-boot.")
            return 2
    finally:
        # THE SESSION, not just this object: inetd serves four at a time
        # and the parent holding one leaves the children three. The
        # check above is the only thing this process does itself.
        remote_gui.close_session()

    print(f"gui_regress: {len(picked)} tool(s) against {host} (bare metal), "
          f"one at a time\n")
    results = []
    for name, script, what in picked:
        print(f"=== {name}: {what}  [{host}]")
        status, secs, summary = run_one_remote(name, script, host,
                                               args.timeout, args.logs)
        print(f"    {status}  ({secs:.0f}s)  {summary}\n", end="\n")
        results.append((name, status, secs, summary))
    _RAN.append(results)

    print("gui_regress: summary")
    for name, status, secs, summary in results:
        print(f"  {status:<5} {name:<12} {secs:5.0f}s  {summary}")

    report_skips(results)
    na = [r[0] for r in results if r[1] == "N/A"]
    if na:
        print(f"\ngui_regress: {len(na)} tool(s) cannot run on hardware: "
              f"{', '.join(na)}")
    failed = [r[0] for r in results if r[1] == "FAIL"]
    if failed:
        print(f"\ngui_regress: FAILED on {host} -- {', '.join(failed)}")
        if not args.logs:
            print("  re-run with --logs DIR to keep each tool's full output")
        return 1
    print("\n" + all_clear(results, f" on {host}"))
    return 0


def run_one_remote(name, script, host, timeout, keep_logs):
    """Run one tool against the BARE-METAL machine.

    No guest to launch, no disk to copy, no slot: there is ONE machine
    and it is already running. `SAUNAOS_REMOTE_HOST` is what redirects the
    tool -- see tools/lib/remote_gui.py for why that lands in the two
    constructors rather than in a flag each tool has to grow.

    A tool that asks for something hardware cannot do raises
    `RemoteUnsupported`, and that is reported as **N/A with the reason**
    rather than as a failure: "this check needs an emulator" and "the
    desktop is broken" are different answers, and a run that conflates
    them teaches a reader to ignore red.
    """
    tool = toolpath.tool(script)
    if not os.path.exists(tool):
        return ("SKIP", 0.0, f"no such tool: {script}")

    env = _child_env()
    env["SAUNAOS_REMOTE_HOST"] = host
    started = time.time()
    try:
        with private_tmp(name, env) as env:
            r = subprocess.run([sys.executable, "-u", tool],
                               cwd=REPO, capture_output=True, text=True,
                               timeout=timeout, env=env)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        partial = ""
        for chunk in (e.stdout, e.stderr):
            if not chunk:
                continue
            partial += chunk if isinstance(chunk, str) else chunk.decode(
                "utf-8", errors="replace")
        tail = partial.strip().splitlines()[-25:]
        out = (f"TIMEOUT after {timeout}s on {host}; last output:\n"
               + "\n".join(tail)) if tail else f"TIMEOUT after {timeout}s with no output"
        rc = 124

    if keep_logs:
        with open(os.path.join(keep_logs, f"{name}.log"), "w") as f:
            f.write(f"# tool={name} host={host} (bare metal)\n")
            f.write(out)

    unsupported = [ln.strip() for ln in out.splitlines()
                   if "RemoteUnsupported" in ln]
    summary = ""
    for line in out.splitlines():
        if "passed," in line and "failed" in line:
            summary = line.strip()
    if not summary:
        tail = [ln for ln in out.splitlines() if ln.strip()]
        summary = tail[-1].strip() if tail else "(no output)"
    if rc != 0 and unsupported and not summary.startswith(name):
        return ("N/A", time.time() - started, unsupported[-1][:160])
    if rc == EXIT_SKIP:
        summary = "nothing judged -- " + summary
    _record_tool(script, rc, time.time() - started, summary, out, host)
    return (status_of(rc), time.time() - started, summary)


def acquire_run_lock():
    """Refuse a SECOND concurrent suite run. Returns (handle, holder).

    Every tool here takes a VM SLOT, and the slots start at 0 for every
    run -- so two suites started at once fight over the same pidfiles,
    serial sockets and QMP ports. It does not fail as a port clash:
    `port_guard` refuses some of the second run's launches, and the rest
    surface MINUTES later as `BrokenPipeError` or a screenshot that was
    never written, in whichever tool happened to be mid-command. Seven
    tools "failed" that way in one run here, none of them at fault.

    An flock, so a run killed with -9 leaves nothing to clean up: the
    lock dies with the process that held it. The pid in the file is for
    the message only.
    """
    import fcntl
    path = os.path.join(REPO, "build", ".gui_regress.lock")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fh = open(path, "a+")
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fh.seek(0)
        holder = fh.read().strip() or "unknown pid"
        fh.close()
        return None, holder
    fh.seek(0)
    fh.truncate()
    fh.write(f"{os.getpid()}\n")
    fh.flush()
    return fh, None


# The results of a run that ran tools, for its history row.
_RAN = []


def main():
    run_id = test_history.new_id("gui_regress")
    os.environ[test_history.PARENT_ENV] = run_id     # each tool's row names this run
    started = time.time()
    with test_history.capture() as printed:
        rc = run()
    if _RAN:
        failed = [f"{r[0]}: {r[3]}" for r in _RAN[-1] if r[1] == "FAIL"]
        os.environ.pop(test_history.PARENT_ENV)
        test_history.record("gui_regress", "PASS" if rc == 0 else "FAIL", rc=rc,
                            duration=time.time() - started, failures=failed,
                            summary=f"{len(_RAN[-1])} tools, {len(failed)} failed",
                            log_text="".join(printed), run_id=run_id, hints=False)
    return rc


def run():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--only", action="append", metavar="NAME",
                    help="run only these tools (repeatable); matches on the short name")
    ap.add_argument("--disk", default=None,
                    help="image to copy for each run (default: a FRESH image of this "
                         "build, made by tools/lib/fresh_disk.py -- not disk.img)")
    ap.add_argument("--no-fresh", action="store_true",
                    help="copy disk.img as it is instead of a fresh image: what a "
                         "test or `make run` left on it reaches every tool")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-tool timeout in seconds (default: 600). This is a "
                         "HANG GUARD, not a budget: it wants a wide margin over "
                         "the slowest real tool, which is `files` at ~5min (it "
                         "drives real file operations through spawned children "
                         "and waits on each). At 360 it began failing HEALTHY "
                         "runs as that tool grew -- and a timed-out tool prints "
                         "NOTHING, so the symptom was zero checks and no clue "
                         "which one hung, the worst possible failure for a "
                         "guard. Re-check the margin whenever `files` grows. "
                         "It was 180 while the ceiling was "
                         "~41s; a guard with no margin fails healthy runs, which "
                         "is worse than no guard.")
    ap.add_argument("--logs", metavar="DIR",
                    help="write each tool's full output to DIR/<name>.log")
    ap.add_argument("--host", metavar="IP",
                    help="run against the BARE-METAL machine at this address "
                         "instead of QEMU guests (serial, one machine; see "
                         "tools/lib/remote_gui.py). local_info.txt has the addresses")
    ap.add_argument("--list", action="store_true", help="list the tools and exit")
    ap.add_argument("--kvm", action="store_true",
                    help="run the guests under KVM instead of TCG -- much "
                         "faster, and deliberately NOT the default: every "
                         "automated test here runs TCG, and the difference is "
                         "load-bearing (see tools/tests/manual/kvm_soak.py). Use it to "
                         "iterate, not to judge a release.")
    ap.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS, metavar="N",
                    help=f"run N tools concurrently, each in its own VM slot "
                         f"(default: {DEFAULT_JOBS}). -j1 is the old serial "
                         f"behaviour, one VM at a time in slot 0.")
    ap.add_argument("--first-slot", type=int, default=0, metavar="N",
                    help="lease VM slots from N upwards instead of 0 -- for a run "
                         "beside another checkout's tests, whose slots start at 0 "
                         "(the run lock is per checkout, the ports are not)")
    args = ap.parse_args()

    if args.list:
        for name, script, what in TOOLS:
            print(f"  {name:<12} {script:<28} {what}")
        return 0

    picked = [t for t in TOOLS
              if not args.only or any(k in t[0] for k in args.only)]
    if not picked:
        print(f"gui_regress: nothing matched {args.only}")
        return 2

    if args.logs:
        os.makedirs(args.logs, exist_ok=True)
    import tree_lock
    tree_lock.hold()   # the fresh disk and every tool read this build

    if args.host:
        return run_remote_suite(picked, args)

    # A FRESH IMAGE BY DEFAULT. `make iso` syncs disk.img rather than
    # reformatting it, so remembered window positions, a changed setting
    # or a stray launcher left by an earlier test or a `make run` reach
    # every copy -- four tools failed that way on 2026-09-30, and
    # predates.py agreed they were pre-existing because it boots the same
    # image. Twenty seconds buys a fixture that is the build and nothing
    # else (tools/lib/fresh_disk.py).
    fresh_dir = None
    if args.disk is None and not args.no_fresh:
        fresh_dir = tempfile.mkdtemp(prefix="gui_regress_fresh.")
        atexit.register(shutil.rmtree, fresh_dir, True)   # every return path, not just the last
        disk = os.path.join(fresh_dir, "fresh.img")
        print("gui_regress: making a fresh image of this build (tools/lib/fresh_disk.py)")
        fresh_disk.make_fresh(disk)
    else:
        disk = args.disk or "disk.img"
        disk = disk if os.path.isabs(disk) else os.path.join(REPO, disk)
        if not os.path.exists(disk):
            print(f"gui_regress: no {disk} -- run `make iso` first")
            return 2
        found = fresh_disk.drift(disk)
        if found:
            print("gui_regress: " + "\ngui_regress: ".join(fresh_disk.describe(disk, found)) + "\n")

    if shutil.which("qemu-system-x86_64") is None:
        print("gui_regress: qemu-system-x86_64 not on PATH")
        return 2

    lock, holder = acquire_run_lock()
    if lock is None:
        print(f"gui_regress: another run is already going (pid {holder}).\n"
              f"gui_regress: two suites share VM slot 0 upwards and would "
              f"kill each other's guests -- wait for it, or `kill {holder}`.")
        return 2

    jobs = max(1, min(args.jobs, len(picked)))
    print(f"gui_regress: {len(picked)} tool(s), each on its own copy of "
          f"{'a fresh image' if fresh_dir else os.path.basename(disk)}, {jobs} at a time\n")

    # Each concurrent tool gets a VM slot, and the slot is what keeps
    # two of them from sharing a pidfile, a serial socket or a QMP port
    # (see vm.py's _apply_instance and run_one above).
    #
    # Slots are LEASED from a pool, not derived from the tool's position
    # in the list. Position looks equivalent and is not: with `-j4` and
    # seven tools, task 4 also maps to slot 0, but it starts as soon as
    # ANY worker frees up -- which is routinely while task 0 is still
    # running on slot 0. Written that way first, and the symptom was
    # ugly and misleading: the fifth tool's `vm.py --instance 0 stop`
    # killed the first tool's VM out from under it, so the FIRST tool
    # died on a broken pipe and the fifth died on a screenshot that was
    # never written. Neither traceback pointed anywhere near the
    # scheduling. A lease is held for exactly as long as the VM exists.
    free_slots = queue.Queue()
    for s in range(jobs):
        free_slots.put(args.first_slot + s)

    def work(name, script, what):
        slot = free_slots.get()
        try:
            status, secs, summary = run_one(name, script, disk, args.timeout,
                                            args.logs, slot, args.kvm)
        finally:
            free_slots.put(slot)
        return (name, what, slot, status, secs, summary)

    done = {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        # Submitted longest-first (pick_order), not in declaration
        # order: a ThreadPoolExecutor starts tasks in submission order,
        # so this is what keeps the slowest tool from being the tail.
        futures = [pool.submit(work, *t) for t in pick_order(picked)]
        # as_completed, not map: results print the moment each tool
        # finishes rather than in submission order, so one slow tool
        # doesn't hold up everything behind it. The summary table below
        # is rebuilt in the original (dependency) order regardless.
        for fut in cf.as_completed(futures):
            name, what, slot, status, secs, summary = fut.result()
            print(f"=== {name}: {what}  [slot {slot}]\n"
                  f"    {status}  ({secs:.0f}s)  {summary}\n", end="\n")
            done[name] = (status, secs, summary)

    results = [(name, *done[name]) for name, _s, _w in picked if name in done]
    _RAN.append(results)

    print("gui_regress: summary")
    for name, status, secs, summary in results:
        print(f"  {status:<5} {name:<12} {secs:5.0f}s  {summary}")

    report_skips(results)
    failed = [r[0] for r in results if r[1] == "FAIL"]
    if failed:
        print(f"\ngui_regress: FAILED -- {', '.join(failed)}")
        if not args.logs:
            print("  re-run with --logs DIR to keep each tool's full output")
        return 1
    print("\n" + all_clear(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
