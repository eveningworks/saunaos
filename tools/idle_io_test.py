#!/usr/bin/env python3
"""tools/idle_io_test.py -- an idle machine stays off the disk.

An idle desktop should read and write almost nothing. A daemon polling a
file in a loop is invisible to every functional test -- everything still
works -- and showed up only on the laptop: soundd re-read its config 50
times a second, ~300 block reads a second, 95% of the idle disk traffic
(b93b62c9). This boots the setup that bug needed and puts a number on it.

    1. a guest with an HDA card -- so snddrv and soundd both run, as on
       the laptops -- given the `cachesim` boot word with bootcfg and
       rebooted. gui_regress.py boots that guest (EXTRA_VM_ARGS); run by
       hand, this boots its own on a copy of disk.img;
    2. waits for boot's own I/O to die down (bounded);
    3. reads `cachesim` and `cachesim -t` twice, WINDOW seconds apart, and
       fails if idle block reads or writes per second exceed the budget,
       naming the heaviest process and path in the window.

Cumulative tables read twice and diffed here, not `cachesim -i`, so no
console command blocks for the window. THE PROBE IS NOT FREE: with no
page cache, each one loads /bin/cachesim from the disk (79 block pages
measured), as pid 0 -- more than the whole idle machine. So one probe's
cost is measured back to back first and taken off every interval, and
its own binary is never named as the culprit.

    python3 tools/idle_io_test.py [--instance N] [--window S]
                                  [--max-reads R] [--max-writes W]

Run by gui_regress.py, the standard check after touching userland/.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from audio_test import wait_serial  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

# Pages (4 KiB) a second. An idle HDA guest measured ~3 reads/s and
# ~1 write/s after the fix; the soundd bug was ~300 reads/s. The budget
# sits well clear of both, so it does not flake on a busy host and a
# poller at even a tenth of that bug's rate still fails it.
MAX_READS = 15.0
MAX_WRITES = 10.0
WINDOW_S = 30
SETTLE_STEP_S = 5
SETTLE_MAX_S = 90


def totals(dbg):
    """(block pages read since boot, {pid: (name, read, written)},
    {(pid, path): (name, lookups, pages)}), or None when not measuring."""
    sizes = dbg.send("sh cachesim") or ""
    reads = None
    for line in sizes.splitlines():
        w = line.split()
        if len(w) >= 4 and w[0] == "block" and w[1] == "256":
            reads = int(w[3])
    if reads is None:
        return None
    procs, paths = {}, {}
    section = None
    for line in (dbg.send("sh cachesim -t") or "").splitlines():
        if line.startswith("Block I/O"):
            section = "proc"
            continue
        if line.startswith("Filesystem calls"):
            section = "path"
            continue
        w = line.split(None, 5)
        if not w or not w[0].isdigit():
            continue
        if section == "proc" and len(w) >= 5:
            procs[int(w[0])] = (w[1], int(w[2]), int(w[4]))
        elif section == "path" and len(w) == 6:
            paths[(int(w[0]), w[5].strip())] = (w[1], int(w[2]), int(w[3]))
    return reads, procs, paths


PROBE_PATH = "/bin/cachesim"


def vm_status(n):
    return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance",
                           str(n), "status"], cwd=REPO, capture_output=True,
                          text=True).stdout


def heaviest(before, after, idx, probe_pid0=0):
    """The entry whose count at `idx` grew most between two samples --
    less `probe_pid0` off pid 0, which the probes are charged to, and
    never the probe's own binary."""
    best, grew = None, 0
    for key, row in after.items():
        if isinstance(key, tuple) and key[1] == PROBE_PATH:
            continue
        was = before.get(key)
        d = row[idx] - (was[idx] if was and was[idx] <= row[idx] else 0)
        if key == 0:
            d -= probe_pid0
        if d > grew:
            best, grew = key, d
    return best, grew


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--window", type=int, default=WINDOW_S, help="seconds measured")
    ap.add_argument("--max-reads", type=float, default=MAX_READS, help="block pages/s")
    ap.add_argument("--max-writes", type=float, default=MAX_WRITES, help="block pages/s")
    args = ap.parse_args()
    n = args.instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"

    res = Results()
    tmp = tempfile.mkdtemp(prefix="idle_io_test_")
    img = os.path.join(tmp, "disk.img")
    wav = os.path.join(tmp, "out.wav")
    if not vm_status(n).startswith("vm: running"):
        copy_disk("disk.img", img, cwd=REPO)

    def vm(*a):
        return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                               "--instance", str(n), *a], cwd=REPO,
                              capture_output=True, text=True)

    # A guest already in this slot is the runner's, booted with what
    # this needs; only a hand run boots one. --reboot either way: the
    # boot word takes a reboot, and a default guest ends QEMU on one.
    mine = not vm("status").stdout.startswith("vm: running")
    dbg = None
    try:
        if mine and vm("--disk", img, "--audio", "hda", "--audio-wav", wav,
                       "--reboot", "start").returncode:
            res.check("the guest booted with an HDA card", False)
            return 1
        dbg = wait_serial(sock)
        if not res.check("the guest answers", dbg is not None):
            return 1
        dbg.send("sh bootcfg words 0 +cachesim")
        dbg.close()
        dbg = None
        reboot = vm("--timeout", "120", "reboot")
        dbg = wait_serial(sock) if reboot.returncode == 0 else None
        if not res.check("it rebooted with `cachesim`", dbg is not None,
                         (reboot.stdout + reboot.stderr).strip()[-120:]):
            return 1
        ps = dbg.send("sh ps") or ""
        # THE CASE THE BUG NEEDED. Without a ring-3 driver this boots the
        # setup every other test already had, and passes for that reason.
        res.check("snddrv and soundd are running", "snddrv" in ps and "soundd" in ps,
                  "ps lists neither" if "snd" not in ps else "")
        first = totals(dbg)
        if not res.check("the kernel is measuring (cachesim)", first is not None):
            return 1
        # One probe's own reads: two in a row, nothing idle between. The
        # smaller of two pairs, in case boot's I/O lands inside one.
        costs = []
        for _ in range(2):
            x, y = totals(dbg), totals(dbg)
            if x and y:
                costs.append(y[0] - x[0])
        probe = min(costs) if costs else 0
        print(f"  note  one probe costs {probe} block pages")

        # Boot's own I/O -- a first-login thumbnail pass, the journal
        # catching up -- is not idle. Wait, bounded, for one quiet step.
        prev, waited = totals(dbg), 0
        while prev and waited < SETTLE_MAX_S:
            time.sleep(SETTLE_STEP_S)
            waited += SETTLE_STEP_S
            cur = totals(dbg)
            if cur and (cur[0] - prev[0] - probe) / SETTLE_STEP_S <= args.max_reads:
                break
            prev = cur or prev
        print(f"  note  settled after {waited} s")

        a = totals(dbg)
        time.sleep(args.window)
        b = totals(dbg)
        if not res.check("both samples read back", a is not None and b is not None):
            return 1
        reads = max(0, b[0] - a[0] - probe) / args.window
        writes = sum(max(0, row[2] - (a[1].get(pid, (0, 0, 0))[2]))
                     for pid, row in b[1].items()) / args.window
        who_r, nr = heaviest(a[1], b[1], 1, probe)
        who_w, nw = heaviest(a[1], b[1], 2)
        path, np_ = heaviest(a[2], b[2], 1)
        blame_r = (f"heaviest reader {b[1][who_r][0]} (pid {who_r}, {nr} pages); "
                   if who_r is not None else "")
        blame_p = (f"busiest path {path[1]} by {b[2][path][0]} ({np_} calls)"
                   if path is not None else "")
        res.check(f"idle block reads {reads:.1f}/s <= {args.max_reads:g}/s",
                  reads <= args.max_reads, blame_r + blame_p)
        res.check(f"idle block writes {writes:.1f}/s <= {args.max_writes:g}/s",
                  writes <= args.max_writes,
                  f"heaviest writer {b[1][who_w][0]} (pid {who_w}, {nw} pages)"
                  if who_w is not None else "")
    finally:
        if dbg:
            dbg.close()
        if mine:
            vm("stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\nidle_io_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
