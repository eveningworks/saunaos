#!/usr/bin/env python3
"""The panic store: a kernel panic survives the warm reset that follows it.

kernel/panic_store.h keeps the kernel log in a fixed range of RAM when the
kernel panics; panic_finish() then counts down and resets; the next boot
checks the record and logd appends it to the DEAD boot's log file. Only a
warm reset carries RAM across, which is why this needs a whole guest
rather than a KTEST (the record format has KTESTs of its own).

WHAT IT ASSERTS, in one guest that is allowed to reboot (`vm.py --reboot`):
  - a COLD boot recovers nothing -- RAM that never held a record fails
    the check rather than being read as one;
  - `config set kernel.crash gp-fault` panics the machine, and the
    countdown restarts it after about panic='s 10 s rather than at once;
  - the next boot says the previous one panicked, logd appends the
    kernel's last lines -- the deliberate-crash announcement and the
    #GP report -- to the end of THAT boot's file, and clears it;
  - `crashlog --panics` lists that boot and `crashlog --panics BOOT`
    prints its recovered lines;
  - a further warm reboot recovers nothing: a cleared record stays cleared.

`--kind ap-gp-fault --cpus 3` has the #GP raised on an AP that is not
the caller's core, inside
a cross-core call (kernel/core/crashtest.c), and adds the AP's own checks:
the report names that core, and the panic stopped every other one first.
`--boot-word smp=park` aims it at a PARKED core, which before b954b91c
halted with no report; a scheduling one reported, but the others ran on.

`--trace` arms the kernel event trace (`trace start`) before the crash,
and checks the PANIC TAIL: the filed record ends with each core's newest
events, decoded (kernel/include/kernel/ktrace.h) -- among them the
`setting` syscall that asked for the crash. Without `--trace` the record
must hold no tail at all: a trace that was not running costs it nothing.

THE POSITIVE CONTROL (--positive-control) replaces the warm reset with a
cold one -- QEMU is stopped and started again after the panic -- so the
RAM is gone and every recovery check must go red. A run where they stay
green is measuring something other than the store.

The image is a COPY with `faultinject` added to its GRUB line: kernel
faults are disarmed on an ordinary boot, on purpose (kernel/core/crashtest.c).

ON DEMAND, not in the gate: two or three boots and a 10 s countdown, and a
suite that reboots its own VMs is a suite whose failures are hard to read.

    python3 tools/tests/sweep/panic_store_test.py [--instance 3] [--positive-control]
        [--kind ap-gp-fault --cpus 3]
"""

# PANICS AND REBOOTS ITS GUEST, on purpose: a warm reset is the only
# thing that carries the RAM store across, so it cannot be a KTEST.
RUNNER = [
    {"covers": "a kernel panic survives the warm reset, into the dead boot's log"},
    {"covers": "a kernel fault on another core panics like the boot core's: reported, "
               "the others stopped, the record kept",
     "name": "panic_store_ap", "args": ["--kind", "ap-gp-fault", "--cpus", "3"]},
    {"covers": "...and on a PARKED core (smp=park), which used to halt with no report",
     "name": "panic_store_ap_parked",
     "args": ["--kind", "ap-gp-fault", "--cpus", "3", "--boot-word", "smp=park"]},
    {"covers": "...with the kernel event trace armed, the record ends with each core's newest events",
     "name": "panic_store_trace", "args": ["--trace", "--cpus", "2"]},
]


import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
TOOLS = toolpath.TOOLS
REPO = os.path.dirname(TOOLS)
import install_grub  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

VM = [sys.executable, toolpath.tool("vm.py")]
BOOT_MARK = "\nidt: syscall entry"        # a boot's raw line; dmesg's echo has a [stamp]
PANIC_MARK = "panic: restarting in"


Result = Results


def vm(inst, disk, log, *args, timeout=240):
    cmd = VM + ["--instance", str(inst), "--disk", disk, "--serial-log", log] + list(args)
    try:
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ""
    return (r.stdout or "") + (r.stderr or "")


def serial(log):
    try:
        with open(log, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_serial(log, pred, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        if pred(serial(log)):
            return True
        time.sleep(0.25)
    return False


def wait_for_guest(g, deadline_s=180):
    end = time.time() + deadline_s
    while time.time() < end:
        if re.search(r"up [0-9]", g("uptime")):
            return True
        time.sleep(3)
    return False


def run(inst, disk, log, res, cold, kind="gp-fault", cpus=0, trace=False):
    g = lambda *c, timeout=60: vm(inst, disk, log, "exec", *c, timeout=timeout)  # noqa: E731
    vm(inst, disk, log, "--reboot", *(["--cpus", str(cpus)] if cpus else []), "start",
       timeout=300)
    if not wait_for_guest(g):
        res.check("the guest boots", False, serial(log)[-600:])
        return
    boot1 = g("dmesg")
    res.check("a cold boot recovers nothing",
              "nothing recovered" in boot1 and "PREVIOUS BOOT PANICKED" not in boot1,
              "\n".join(l for l in boot1.splitlines() if "panic_store" in l)[-400:])

    if trace:
        armed = g("trace start switch,syscall,irq")
        res.check("the kernel event trace arms", "trace: recording" in armed, armed[-300:])

    boots_before = serial(log).count(BOOT_MARK)
    # IN THE BACKGROUND: a panicked guest never answers, so waiting on the
    # exec would start the countdown's clock a whole timeout late.
    trigger = subprocess.Popen(
        VM + ["--instance", str(inst), "exec", f"config set kernel.crash {kind}"],
        cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t_panic = None
    if wait_serial(log, lambda s: PANIC_MARK in s, 30):
        t_panic = time.time()
    try:
        trigger.wait(timeout=30)
    except subprocess.TimeoutExpired:
        trigger.kill()
    res.check("config set kernel.crash panics the machine", t_panic is not None,
              serial(log)[-600:])
    if t_panic is None:
        return
    if kind.startswith("ap-"):
        tail = serial(log)
        tail = tail[tail.rfind(f'deliberately triggering "{kind}"'):]
        hand = re.search(r"crash: core (\d+) hands the fault to core (\d+)", tail)
        on = re.search(r"on core (\d+), ", tail)
        res.check("the fault is reported by the AP it happened on",
                  hand is not None and on is not None and on.group(1) == hand.group(2)
                  and hand.group(2) != "0",
                  tail[:900])
        res.check("...as the boot core's is: the #GP named, with its function",
                  "PANIC: General protection fault" in tail and "in ap_fault_cb" in tail,
                  tail[:900])
        stop = re.search(r"smp: core (\d+) stopped (\d+) other core\(s\) of (\d+)", tail)
        res.check("...and every other core was stopped before the report",
                  stop is not None and int(stop.group(2)) == cpus - 1
                  and int(stop.group(3)) == cpus - 1, stop.group(0) if stop else tail[:900])

    if cold:
        # THE CONTROL: RAM does not survive this, so nothing may be recovered.
        vm(inst, disk, log, "stop")
        vm(inst, disk, log, "--reboot", "start", timeout=300)
    else:
        rebooted = wait_serial(log, lambda s: s.count(BOOT_MARK) > boots_before, 60)
        waited = time.time() - t_panic
        res.check("the countdown restarts the machine after ~10 s, not at once",
                  rebooted and 8.0 <= waited <= 40.0, f"rebooted={rebooted} after {waited:.1f}s")
    if not wait_for_guest(g):
        res.check("the guest comes back", False, serial(log)[-600:])
        return

    boot2 = g("dmesg")
    res.check("the next boot says the previous one PANICKED",
              "PREVIOUS BOOT PANICKED" in boot2,
              "\n".join(l for l in boot2.splitlines() if "panic_store" in l)[-400:])

    # logd files it within a poll or two of starting.
    filed = ""
    end = time.time() + 30
    while time.time() < end:
        filed = g("log -p 1 -n 200", timeout=60)
        if "THIS BOOT PANICKED" in filed:
            break
        time.sleep(2)
    marker = filed.find("THIS BOOT PANICKED")
    after = filed[marker:] if marker >= 0 else ""
    res.check("logd appends the record to the dead boot's log", marker >= 0, filed[-600:])
    res.check("...with the kernel's last lines: the crash it was asked for, and the fault",
              f'deliberately triggering "{kind}"' in after
              and "PANIC: General protection fault" in after,
              after[-800:])
    tail = after[after.find("ktrace: the last"):] if "ktrace: the last" in after else ""
    if trace:
        res.check("...and, the trace armed, ends with each core's newest events",
                  bool(tail) and "ktrace: core 0," in tail
                  and (cpus < 2 or "ktrace: core 1," in tail), after[-800:])
        res.check("...decoded: the syscall that asked for the crash, by name",
                  "sys_enter nr=setting" in tail, tail[-600:])
    else:
        res.check("...and, the trace not armed, holds no trace tail", not tail, tail[:300])
    res.check("...and the record is cleared once filed",
              "panic_store: record cleared" in g("dmesg"))
    listed = g("crashlog --panics")
    m = re.search(r"boot (\d+)\s+PANIC: General protection fault", listed)
    res.check("`crashlog --panics` lists the boot with its PANIC line", m is not None, listed[-400:])
    shown = g(f"crashlog --panics {m.group(1)}") if m else ""
    res.check("...and `crashlog --panics BOOT` prints its recovered lines",
              "THIS BOOT PANICKED" in shown and "PANIC: General protection fault" in shown,
              shown[-400:])

    boots_before = serial(log).count(BOOT_MARK)
    g("reboot", timeout=10)
    wait_serial(log, lambda s: s.count(BOOT_MARK) > boots_before, 60)
    if not wait_for_guest(g):
        res.check("the guest comes back from `reboot`", False, serial(log)[-600:])
        return
    boot3 = g("dmesg")
    res.check("a cleared record is not reported again on the next warm boot",
              "nothing recovered" in boot3 and "PREVIOUS BOOT PANICKED" not in boot3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3,
                    help="VM slot; this tool PANICS and REBOOTS it")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--positive-control", action="store_true",
                    help="cold-restart after the panic; the recovery checks must then fail")
    ap.add_argument("--keep", help="keep the serial log at this path")
    ap.add_argument("--kind", default="gp-fault",
                    help="the kernel.crash kind (ap-gp-fault: on another core)")
    ap.add_argument("--boot-word", action="append", default=[],
                    help="another word for the copy's GRUB line (smp=park)")
    ap.add_argument("--cpus", type=int, default=0, help="guest cores (ap-* kinds want 3: an AP that is not the caller's)")
    ap.add_argument("--trace", action="store_true",
                    help="arm the kernel event trace first, and check the record's trace tail")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="panic_store_test.")
    disk = os.path.join(work, "disk.img")
    log = os.path.join(work, "serial.log")
    copy_disk(args.disk, disk)
    for word in ["faultinject"] + args.boot_word:
        ok, why = install_grub.add_boot_word(disk, word)
        if not ok:
            break
    if not ok:
        print(f"panic_store_test: cannot arm the copy: {why}")
        return 2

    res = Result()
    try:
        run(args.instance, disk, log, res, args.positive_control, args.kind, args.cpus, args.trace)
    finally:
        vm(args.instance, disk, log, "stop")
        if args.keep:
            shutil.copy(log, args.keep)
        shutil.rmtree(work, ignore_errors=True)

    if args.positive_control:
        if res.fails:
            print(f"\npanic_store_test: positive control FAILED as it must "
                  f"({len(res.fails)} finding(s))")
            return 0
        print("\npanic_store_test: positive control PASSED -- a cold restart still "
              "reported a recovered panic, so this is not checking the store",
              file=sys.stderr)
        return 1

    print(f"\npanic_store_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
