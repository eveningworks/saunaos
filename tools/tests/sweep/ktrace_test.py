#!/usr/bin/env python3
"""The kernel event trace end to end: armed, a known workload, dumped, decoded.

One guest on a COPY of disk.img, two cores. `trace run` arms switches,
wakeups, syscalls, page faults and interrupts around one run of
`/tests/ktrace_load 1000` -- a thousand getpids, fifty one-byte round
trips with a forked partner over two pipes, five spawns of /bin/hello --
and stops as it exits; `trace dump` writes the records, they come out
over TFTP (`vm.py get`), and tools/dev/trace_view.py decodes them. What
the workload MUST have caused is then asserted:

  - its thread made >= 1000 getpids, and each getpid's exit returned
    that thread's own pid (a record's tid and its argument agree);
  - it was switched in and out, and it and its partner handed the core
    to each other across the pipe, each woken at least once;
  - it spawned its five programs, and something took a page fault;
  - interrupts were recorded, the Chrome JSON has a track per CPU and
    slices for the workload's thread;
  - `trace show` in the guest decodes: syscalls by name, switches.

THE POSITIVE CONTROL (--positive-control) arms only `irq,fault`: every
syscall, switch and wakeup check must then go red, so a pass is the
trace's and not something the checks would find in any dump.

ON DEMAND, not in the gate: one boot and about a minute of TCG.

    python3 tools/tests/sweep/ktrace_test.py [--instance 12] [--keep DIR] [--positive-control]
"""

# Boots a guest, arms `kernel.trace` around /tests/ktrace_load and decodes
# the dump on the host. Carries a --positive-control that must go red.
RUNNER = {"covers": "the kernel event trace records a known workload's switches and syscalls, "
                    "dumped and decoded by trace_view.py"}

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk, poll, scratch_dir  # noqa: E402

REPO = toolpath.REPO
VM = [sys.executable, toolpath.tool("vm.py")]
sys.path.insert(0, os.path.join(REPO, "tools", "dev"))
import trace_view  # noqa: E402

GROUPS = "switch,wakeup,syscall,fault,irq"
CALLS = 1000
SPAWNS = 5   # ktrace_load.c's


def vm(inst, *args, timeout=240):
    try:
        r = subprocess.run(VM + ["--instance", str(inst), *args], cwd=REPO,
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, ""
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def g(inst, cmd, timeout=120):
    return vm(inst, "exec", cmd, timeout=timeout)[1]


def run(inst, disk, work, res, groups):
    rc, out = vm(inst, "--disk", disk, "--cpus", "2", "start", timeout=300)
    if rc != 0:
        res.check("the guest boots", False, out[-600:])
        return
    # `trace run` brackets the workload: armed just before it, stopped as
    # it exits. SPAWNED: the workload forks, and the console's bare `run`
    # loader has no slot to fork from. Its stderr lands in dmesg.
    g(inst, f"spawn /bin/trace -s 32768 run {groups} /tests/ktrace_load {CALLS}")
    seen = {"log": "", "status": ""}

    def finished():
        # What the checks below need: the workload's last line AND the
        # trace stopped behind it.
        seen["log"] = g(inst, "dmesg -n 60")
        if "ktrace_load: done" not in seen["log"]:
            return False
        seen["status"] = g(inst, "trace")
        return "kernel.trace=off" in seen["status"]

    poll(finished, 300, step=2)
    log, status = seen["log"], seen["status"]
    done = re.search(r"ktrace_load: done (\d+)", log)
    partner = re.search(r"ktrace_load: partner (\d+)", log)
    res.check("the workload ran to the end", done is not None and partner is not None, log[-300:])
    res.check("...and the trace stopped as it exited", "kernel.trace=off" in status, status[-300:])
    if not done or not partner:
        return
    me, mate = int(done.group(1)), int(partner.group(1))
    shown = g(inst, "trace -n 60 show")
    res.check("trace show decodes in the guest: syscalls by name, switches",
              re.search(r"sys_exit nr=[a-z]", shown) is not None and "switch prev=" in shown,
              shown[-300:])
    dumped = g(inst, "trace dump /tmp/k.ktrace")
    res.check("trace dump writes the records", "records from" in dumped, dumped[-300:])
    local = os.path.join(work, "k.ktrace")
    rc, out = vm(inst, "get", "/tmp/k.ktrace", local, timeout=180)
    if not res.check("the dump comes out of the guest", rc == 0 and os.path.exists(local), out[-300:]):
        return
    check_dump(local, work, res, me, mate)


def check_dump(path, work, res, me, mate):
    try:
        d = trace_view.load(path)
    except (OSError, trace_view.DumpError) as e:
        res.check("trace_view.py reads the dump", False, str(e))
        return
    lost = sum(c["lost"] for c in d["cpus"])
    res.check("trace_view.py reads the dump", True,
              f"{len(d['recs'])} records, {len(d['cpus'])} cores, {lost} lost")
    nr = {n: i for i, n in d["syscalls"].items()}
    ev = {e["name"]: i for i, e in d["events"].items()}
    recs = d["recs"]

    def of(name, **want):
        out = []
        for r in recs:
            if r["event"] != ev.get(name, -1):
                continue
            if all(r[k] == v if k in ("tid", "cpu") else r["a"][int(k[1])] == v for k, v in want.items()):
                out.append(r)
        return out

    getpids = of("sys_enter", tid=me, a0=nr.get("getpid"))
    res.check(f"the workload's thread made >= {CALLS} getpids", len(getpids) >= CALLS,
              f"pid {me}: {len(getpids)}")
    own = of("sys_exit", tid=me, a0=nr.get("getpid"), a1=me)
    res.check("...and each getpid returned its own pid", len(own) >= CALLS, f"{len(own)} returned {me}")
    into, out_of = of("switch", a2=me), of("switch", a0=me)
    res.check("it was switched in and out", into and out_of, f"in {len(into)}, out {len(out_of)}")
    # Two cores: the pair may run side by side, each switching with its
    # core's idle rather than with the other, so what is asserted is that
    # each PARKED (prev_state S) waiting for the other at least once.
    parked = [r for r in out_of if r["a"][1] == 4]
    mate_parked = [r for r in of("switch", a0=mate) if r["a"][1] == 4]
    res.check("it and its partner each parked waiting on the pipe",
              parked and mate_parked and of("switch", a2=mate),
              f"{len(parked)} and {len(mate_parked)} parks, partner switched in {len(of('switch', a2=mate))}")
    rw = {nr.get("read"), nr.get("write")}
    mate_io = [r for r in of("sys_enter", tid=mate) if r["a"][0] in rw]
    res.check("...the partner read and wrote the pipe", len(mate_io) >= 50, f"{len(mate_io)}")
    res.check("...and each was woken", of("wakeup", a0=me) and of("wakeup", a0=mate),
              f"{len(of('wakeup', a0=me))} and {len(of('wakeup', a0=mate))}")
    spawns = of("sys_enter", tid=me, a0=nr.get("spawn"))
    res.check(f"it spawned its {SPAWNS} programs", len(spawns) >= SPAWNS, f"{len(spawns)}")
    faults = of("fault")
    res.check("something took a page fault", faults, f"{len(faults)}")
    irqs = {r["cpu"] for r in of("irq")}
    res.check("interrupts are recorded", irqs, f"on cores {sorted(irqs)}")

    doc = trace_view.chrome(d)
    tracks = {e["args"]["name"] for e in doc["traceEvents"] if e.get("name") == "thread_name"}
    res.check("the Chrome JSON has a track per CPU", {"CPU 0", "CPU 1"} <= tracks, str(sorted(tracks)))
    slices = [e for e in doc["traceEvents"] if e["ph"] == "X" and e["args"].get("tid") == me]
    res.check("...and slices for the workload's thread", slices, f"{len(slices)}")
    with open(os.path.join(work, "k.json"), "w") as f:
        json.dump(doc, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=12, help="VM slot")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--keep", help="keep the dump and its JSON in this directory")
    ap.add_argument("--positive-control", action="store_true",
                    help="arm only irq,fault; the syscall and switch checks must then fail")
    args = ap.parse_args()
    work = scratch_dir("ktrace_test.")
    disk = os.path.join(work, "disk.img")
    copy_disk(args.disk, disk)
    res = Results()
    try:
        run(args.instance, disk, work, res, "irq,fault" if args.positive_control else GROUPS)
    finally:
        vm(args.instance, "stop")
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            for f in ("k.ktrace", "k.json"):
                if os.path.exists(os.path.join(work, f)):
                    shutil.copy(os.path.join(work, f), args.keep)

    if args.positive_control:
        if res.fails:
            print(f"\nktrace_test: positive control FAILED as it must ({len(res.fails)} finding(s))")
            return 0
        print("\nktrace_test: positive control PASSED -- the checks find a workload the trace "
              "did not record", file=sys.stderr)
        return 1
    print(f"\nktrace_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
