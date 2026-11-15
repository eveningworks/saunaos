#!/usr/bin/env python3
"""The hang kit: the lockup detector reports each kind of hang, and the
hardware watchdog resets a machine nothing feeds.

One q35 guest (QEMU's ICH9 carries the Intel TCO timer kernel/drivers/
watchdog/itco.c drives), on a disk COPY whose GRUB line adds `faultinject
watchdog=40 hung_task=15`, allowed to reboot (`vm.py --reboot`):

  - boot: the TCO binds, init opens /dev/watchdog and feeds it;
  - `kernel.crash hung-task` parks the shell in the kernel for good: the
    hung-task check names it within its 15 s, and the machine stays up;
  - `kernel.crash soft-lockup` spins with preemption off and interrupts
    on: the soft check reports it at ~20 s and panics, the panic restarts
    the machine WARM, and the next boot recovers the record;
  - `kernel.crash hard-lockup` spins with interrupts off. Under TCG there
    is no cycle counter for the NMI, so nothing reports it and the TCO
    resets the machine at ~40 s -- the next boot says the watchdog did it.
    Under --kvm (-cpu host has a counter) the hard check reports it first.

THE POSITIVE CONTROL (--positive-control) boots with `lockup=off
watchdog=off hung_task=off` instead: every report and every reset must
then go missing, so every detection check goes red.

ON DEMAND, not in the gate: three boots and some 3-4 minutes of deliberate
hangs.

    python3 tools/tests/sweep/lockup_test.py [--instance 3] [--kvm] [--positive-control]
"""

# HANGS AND RESETS ITS GUEST, on purpose, three times.
RUNNER = {"covers": "the lockup detector and the hardware watchdog catch a hung machine"}

import argparse
import os
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
BOOT_MARK = "\nidt: syscall entry"   # a boot's raw line; dmesg's echo has a [stamp]


class Guest:
    def __init__(self, inst, disk, log, kvm):
        self.inst, self.disk, self.log = inst, disk, log
        self.base = VM + ["--instance", str(inst), "--machine", "q35", "--disk", disk,
                          "--serial-log", log] + (["--kvm"] if kvm else [])

    def vm(self, *args, timeout=240):
        try:
            r = subprocess.run(self.base + list(args), cwd=REPO, capture_output=True,
                               text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return ""
        return (r.stdout or "") + (r.stderr or "")

    def exec(self, *cmds, timeout=60):
        return self.vm("exec", *cmds, timeout=timeout)

    def serial(self):
        try:
            with open(self.log, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def since(self, mark):
        return self.serial()[mark:]

    def wait(self, pred, deadline_s, since=0):
        end = time.time() + deadline_s
        while time.time() < end:
            if pred(self.since(since)):
                return True
            time.sleep(0.5)  # sleep-ok: polls the serial log, which has no readiness to wait on
        return False

    def up(self, deadline_s=180):
        end = time.time() + deadline_s
        while time.time() < end:
            if "up " in self.exec("uptime", timeout=20):
                return True
            time.sleep(3)  # sleep-ok: a guest mid-reboot answers nothing to wait on
        return False

    def trigger(self, kind):
        # IN THE BACKGROUND: a hung guest never answers the exec.
        return subprocess.Popen(self.base + ["exec", f"config set kernel.crash {kind}"],
                                cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def trigger_detached(self, kind):
        # A SPAWNED process, for a hang the machine survives: an exec whose
        # command never returns keeps the debug console, and every later
        # exec then goes unanswered.
        self.vm("spawn", "--wait", "1", "/bin/config", f"set kernel.crash {kind}", timeout=60)


def reap(p):
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


def run(g, res, control, kvm):
    g.vm("--reboot", "start", timeout=300)
    if not g.up():
        res.check("the guest boots", False, g.serial()[-600:])
        return
    boot = g.exec("dmesg")
    nmi = "cycle-counter NMI" in boot
    res.check("the TCO binds and init feeds /dev/watchdog",
              "itco: TCO v2" in boot and "watchdog: started by pid 1" in boot,
              "\n".join(l for l in boot.splitlines() if "itco" in l or "watchdog" in l))

    # --- a hung task: reported, and the machine carries on ---------------
    mark = len(g.serial())
    g.trigger_detached("hung-task")
    found = g.wait(lambda s: "LOCKUP: hung task" in s, 45, mark)
    tail = g.since(mark)
    res.check("a task parked in the kernel is reported as hung", found, tail[-500:])
    res.check("...naming the lock it waits on, with a stack scan",
              found and "on lock" in tail and "stack scan from" in tail, tail[-500:])
    res.check("...and the machine stays up", "up " in g.exec("uptime", timeout=20))

    # --- a soft lockup: reported, a panic, a warm restart, the record ----
    mark = len(g.serial())
    boots = g.serial().count(BOOT_MARK)
    p = g.trigger("soft-lockup")
    t0 = time.time()
    found = g.wait(lambda s: "LOCKUP: soft lockup" in s, 45, mark)
    waited = time.time() - t0
    reap(p)
    tail = g.since(mark)
    res.check("a spin with preemption off is reported as a soft lockup after ~20 s",
              found and 15 <= waited <= 45, f"found={found} after {waited:.0f}s\n{tail[-400:]}")
    res.check("...and panics, restarting warm", "PANIC: lockup" in tail and "warm" in tail,
              tail[-400:])
    rebooted = g.wait(lambda s: s.count(BOOT_MARK) > 0, 60, mark) if found else False
    if rebooted and g.up():
        after = g.exec("dmesg")
        res.check("the next boot recovers the lockup's record", "PREVIOUS BOOT PANICKED" in after,
                  "\n".join(l for l in after.splitlines() if "panic_store" in l))
    else:
        res.check("the next boot recovers the lockup's record", False, "no reboot")
        if control:
            g.vm("stop")
            g.vm("--reboot", "start", timeout=300)
            g.up()

    # --- a hard lockup: the NMI's report, or the watchdog's reset --------
    mark = len(g.serial())
    p = g.trigger("hard-lockup")
    if nmi:
        found = g.wait(lambda s: "LOCKUP: hard lockup" in s, 40, mark)
        res.check("a spin with interrupts off is reported by the cycle-counter NMI", found,
                  g.since(mark)[-400:])
    else:
        t0 = time.time()
        reset = g.wait(lambda s: BOOT_MARK in s, 90, mark)
        waited = time.time() - t0
        res.check("with no NMI (TCG), the TCO resets the hung machine at ~40 s",
                  reset and 30 <= waited <= 80, f"reset={reset} after {waited:.0f}s")
        if reset and g.up():
            res.check("...and the next boot says the watchdog did it",
                      "THE LAST RESET WAS THIS WATCHDOG'S" in g.exec("dmesg"))
        else:
            res.check("...and the next boot says the watchdog did it", False, "no reboot")
    reap(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3,
                    help="VM slot; this tool HANGS and RESETS it")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--kvm", action="store_true",
                    help="run under KVM (-cpu host): the hard check has its NMI")
    ap.add_argument("--positive-control", action="store_true",
                    help="boot with every detector and the watchdog off; detection must fail")
    ap.add_argument("--keep", help="keep the serial log at this path")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="lockup_test.")
    disk = os.path.join(work, "disk.img")
    log = os.path.join(work, "serial.log")
    copy_disk(args.disk, disk)
    words = "faultinject " + ("lockup=off watchdog=off hung_task=off" if args.positive_control
                              else "watchdog=40 hung_task=15")
    ok, why = install_grub.add_boot_word(disk, words)
    if not ok:
        print(f"lockup_test: cannot arm the copy: {why}")
        return 2

    res = Results()
    g = Guest(args.instance, disk, log, args.kvm)
    try:
        run(g, res, args.positive_control, args.kvm)
    finally:
        g.vm("stop")
        if args.keep:
            shutil.copy(log, args.keep)
        shutil.rmtree(work, ignore_errors=True)
    return res.finish("lockup_test")


if __name__ == "__main__":
    sys.exit(main())
