#!/usr/bin/env python3
"""Run a small guest out of memory: the BIGGEST process is ended, not the one that asked.

Two `/tests/memtest` processes on one desktop. The VICTIM takes nearly
all free memory and sits on it; the ASKER then wants a little more than
is left. Without the OOM killer (kernel/oom.h) the asker's page fault
found no frame and the asker died -- whichever process faulted next,
which on a real machine is as likely to be the compositor. With it:

  * the kernel log names the VICTIM as the process ended, for the asker;
  * the asker finishes and verifies its own pattern (PASSED);
  * the victim never reports, and is gone;
  * the compositor and init are the same pids as before;
  * `meminfo --audit` balances, and the memory comes back.

THE CONTROL, same boot: `kernel.oom_kill_allocating_task on` -- Linux's
sysctl, which ends the process that faulted instead. The same two
processes must then go the OTHER way (the asker ended, the victim
PASSED). A test whose first half passed because the asker happened to
fit would fail here, so the pair is what makes the first half evidence.

    python3 tools/tests/sweep/oom_test.py                 # 384 MiB guest
    python3 tools/tests/sweep/oom_test.py --mem 512 --instance 3
"""

RUNNER = {"covers": "out of memory: the OOM killer ends the biggest process, the desktop survives"}

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole  # noqa: E402
import vm as vm_mod  # noqa: E402 -- started_ok()
import port_guard  # noqa: E402
from harness import copy_disk, poll  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
VM = toolpath.tool("vm.py")
MIB = 1 << 20

# What the asker wants on top of what the victim leaves. The gap is
# smaller than the ask by this much, so the ask cannot fit.
ASK_MIB = 48
LEAVE_MIB = 16
HOLD_S = 60

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok)))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def vm_run(disk, instance, mem, *argv):
    cmd = [sys.executable, VM, "--disk", disk, "--mem", str(mem)]
    if instance:
        cmd += ["--instance", str(instance)]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


def fact(con, name):
    """A kernel fact as an int, or None (a dropped first command answers '')."""
    def ask():   # a list, so that a 0 is a truthy answer to poll()
        for line in (con.send("sh config get " + name) or "").splitlines():
            if re.fullmatch(r"\d+", line.strip()):
                return [int(line.strip())]
        return None
    v = poll(ask, 5, step=1.0)
    return v[0] if v else None


def dmesg(con):
    return poll(lambda: con.send("sh dmesg") or "", 5, step=0.5)


def wait_log(con, pattern, timeout, after=0):
    """The match of `pattern` in dmesg past the first `after` of them, within
    `timeout` s, or None. `after` is what an earlier round left in the ring."""
    def ask():
        found = list(re.finditer(pattern, dmesg(con)))
        return found[after] if len(found) > after else None
    return poll(ask, timeout, step=1.0)


def spared_pids(con):
    """init and the compositor -- the processes whose loss is the machine's."""
    by = {p["name"]: p["pid"] for p in con.processes() if p["state"] != "zombie"}
    return {n: pid for n, pid in by.items() if n in ("init", "toywm")}


def round_trip(con, label, expect_victim_dies):
    """One victim + one asker. Returns (victim_pid, asker_pid) or None."""
    room = fact(con, "mem.frame.free")
    reserve = fact(con, "mem.frame.reserve.user") or 0
    if room is None:
        check(f"{label}: the guest reports its free memory", False)
        return None
    take = (room - reserve) * 4096 // MIB - LEAVE_MIB
    if not check(f"{label}: a victim size can be chosen", take > 2 * ASK_MIB,
                 f"only {take} MiB free -- raise --mem"):
        return None

    con.send(f"sh spawn /tests/memtest {take} {HOLD_S}")
    m = wait_log(con, rf"memtest: pid (\d+) holding {take * 1024} KiB", 240)
    if not check(f"{label}: the victim holds {take} MiB", m is not None,
                 "no 'holding' line -- did it die filling memory?"):
        return None
    victim = int(m.group(1))

    oom_line = r"oom: out of memory -- ended pid (\d+) \(([^,]+), \d+ KiB\) for pid (\d+)"
    seen = len(re.findall(oom_line, dmesg(con)))
    con.send(f"sh spawn /tests/memtest {ASK_MIB}")
    ended = wait_log(con, oom_line, 120, after=seen)
    if not check(f"{label}: the kernel says it ran out of memory and ended somebody",
                 ended is not None, "no 'oom: out of memory' line"):
        return None
    dead, asker = int(ended.group(1)), int(ended.group(3))
    print(f"    victim {victim}, asker {asker}, ended {dead} ({ended.group(2)})")

    if expect_victim_dies:
        check(f"{label}: the one ended is the BIGGEST, the victim", dead == victim,
              f"ended {dead}, victim {victim}")
        ok = wait_log(con, rf"memtest: PASSED -- pid {asker},", 120)
        check(f"{label}: the asker finishes and verifies its memory", ok is not None)
        log = dmesg(con)
        check(f"{label}: the victim never reports",
              not re.search(rf"memtest: (PASSED -- pid {victim},|FAIL)", log))
    else:
        check(f"{label}: the one ended is the ASKER", dead == asker,
              f"ended {dead}, asker {asker}")
        ok = wait_log(con, rf"memtest: PASSED -- pid {victim},", HOLD_S + 120)
        check(f"{label}: the victim survives its hold and verifies", ok is not None)
    alive = {p["pid"] for p in con.processes() if p["state"] != "zombie"}
    check(f"{label}: the process ended is gone", dead not in alive)
    return victim, asker


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mem", type=int, default=384, metavar="MIB", help="guest RAM (default 384)")
    ap.add_argument("--disk", default=None)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("oom_test: no free VM slot")
            return 2
        args.instance = n
    args.instance = int(args.instance)
    sock = port_guard.instance_sock(args.instance)

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("oom_test: no disk.img -- run `make iso` first")
            return 2
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        copy_disk(src, tmp.name)
        args.disk = tmp.name

    try:
        if not vm_mod.started_ok(vm_run(args.disk, args.instance, args.mem, "start")):
            print("oom_test: could not start the VM")
            return 2
        con = DebugConsole(sock, timeout=60.0)
        # init starts the desktop a little after the console is ready.
        before = poll(lambda: (lambda d: "toywm" in d and d)(spared_pids(con)), 120, step=2.0) \
            or spared_pids(con)
        if not check("the desktop is up (init and toywm running)",
                     "init" in before and "toywm" in before, f"saw {before}"):
            return report()
        free0 = fact(con, "mem.frame.free")
        kills0 = fact(con, "mem.oom.kills")
        check("no out-of-memory kills before the test", kills0 == 0, f"kills={kills0}")

        print("the killer: biggest process ends")
        if round_trip(con, "biggest", True) is None:
            return report()

        print("the control: kernel.oom_kill_allocating_task on")
        con.send("sh config set kernel.oom_kill_allocating_task on")
        r = round_trip(con, "allocating", False)
        con.send("sh config set kernel.oom_kill_allocating_task off")
        if r is None:
            return report()

        after = spared_pids(con)
        check("init and the compositor are the SAME processes afterwards", after == before,
              f"before {before}, after {after}")
        check("the kill counter says two", fact(con, "mem.oom.kills") == 2)

        audit = con.send("sh meminfo --audit") or ""
        check("meminfo --audit balances", "no dangling" in audit and "DANGLING" not in audit,
              (audit.strip().splitlines() or ["(nothing)"])[-1])
        # Back to within 2% of where it started, once both have gone.
        poll(lambda: (fact(con, "mem.frame.free") or 0) >= free0 * 98 // 100, 30, step=1.0)
        free1 = fact(con, "mem.frame.free")
        check("the memory comes back", free1 is not None and free1 >= free0 * 98 // 100,
              f"{free0} frames free before, {free1} after")
    finally:
        if not args.keep:
            vm_run(args.disk, args.instance, args.mem, "stop")
        if tmp:
            os.unlink(tmp.name)
    return report()


def report():
    bad = [n for n, ok in checks if not ok]
    print(f"\noom_test: {'FAIL' if bad else 'PASS'} ({len(checks) - len(bad)}/{len(checks)})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
