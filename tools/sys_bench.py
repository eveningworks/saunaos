#!/usr/bin/env python3
"""What a system call, a context switch and a read or write cost, on one build.

    python3 tools/sys_bench.py --kvm                     # this checkout's build, KVM
    python3 tools/sys_bench.py --kvm --runs 3 --json out.jsonl --label before
    python3 tools/sys_bench.py --host <asus-ip>          # real hardware, over remote.py

Boots the build on a COPY of its disk (or uses a running machine with
--host), runs /tests/syscall_bench and prints its rows: getpid, yield, a
one-byte pipe write+read, a two-process round trip and the switch derived
from it, and read/write of 64 B .. 1 MiB against a /tmp file beside two
ring-3 memcpys of the same size -- the copy floor, so what is left over is
the kernel's own per-call work (the bounce buffer, the fs path).

WHAT A NUMBER HERE IS NOT: a KVM number when run under TCG -- the
program's `machine:` line names the hypervisor. Compare two builds
measured the same way, never an absolute, and use --runs before believing
a difference of a few percent. Named by no runner: it produces a
comparison, not a verdict.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tree_lock  # noqa: E402
from harness import copy_disk  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "build/userland/tests/syscall_bench.elf")
ROW = re.compile(r"syscall_bench: (getpid|yield|pipe 1 B|round trip|switch)\s+(-?[\d.]+) ns\s+(-?[\d.]+) cycles")
SIZE = re.compile(r"syscall_bench: (\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)$")
MACHINE = re.compile(r"syscall_bench: machine: (.*)")


def vm(args, *rest, timeout=180):
    cmd = [sys.executable, os.path.join(REPO, "tools/vm.py"), "--instance", str(args.instance)]
    if args.kvm:
        cmd.append("--kvm")
    cmd += list(rest)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def remote(args, *rest, timeout=300):
    cmd = [sys.executable, os.path.join(REPO, "tools/remote.py"), "--host", args.host, *rest]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def sizes(args):
    return " ".join(v for v in args.sizes.split(",") if v.strip())


def parse(text):
    res = {"rows": {}, "sizes": {}}
    for line in text.splitlines():
        line = line.strip()
        m = MACHINE.search(line)
        if m:
            res["machine"] = m.group(1)
        m = ROW.search(line)
        if m:
            res["rows"][m.group(1)] = {"ns": float(m.group(2)), "cycles": float(m.group(3))}
        m = SIZE.search(line)
        if m:
            res["sizes"][int(m.group(1))] = {"read_ns": int(m.group(2)), "write_ns": int(m.group(3)),
                                             "copy_ns": int(m.group(4))}
    return res


def run_vm(args, disk):
    rc, out = vm(args, "--disk", disk, *(["--iso", args.iso] if args.iso else []), "start", timeout=300)
    if rc != 0:
        sys.exit(f"sys_bench: the VM did not start:\n{out[-2000:]}")
    try:
        time.sleep(args.settle)
        # SPAWNED, so it has a scheduler slot to fork from; its output lands in dmesg.
        vm(args, "exec", f"spawn /tests/syscall_bench {args.calls} {sizes(args)}")
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            time.sleep(2)
            _, log = vm(args, "exec", "dmesg -n 60")
            if "syscall_bench: done" in log:
                return parse(log)
        sys.exit("sys_bench: /tests/syscall_bench never finished -- see `dmesg`")
    finally:
        vm(args, "stop")


def run_host(args):
    rc, out = remote(args, "put", ELF, "/tmp/syscall_bench")
    if rc != 0:
        sys.exit(f"sys_bench: could not copy the program to {args.host}:\n{out[-2000:]}")
    _, out = remote(args, "--timeout", str(args.timeout), "exec", f"/tmp/syscall_bench {args.calls} {sizes(args)}",
                    timeout=args.timeout + 30)
    remote(args, "exec", "rm /tmp/syscall_bench")
    if "syscall_bench: done" not in out:
        sys.exit(f"sys_bench: the run did not finish on {args.host}:\n{out[-2000:]}")
    return parse(out)


def report(res):
    print(f"machine: {res.get('machine', '?')}")
    for name, r in res["rows"].items():
        print(f"  {name:<12} {r['ns']:10.1f} ns {r['cycles']:10.0f} cycles")
    print(f"  {'size':<12} {'read ns':>10} {'write ns':>10} {'copy ns':>10} {'read-copy':>10}")
    for size, s in res["sizes"].items():
        print(f"  {size:<12} {s['read_ns']:>10} {s['write_ns']:>10} {s['copy_ns']:>10} "
              f"{s['read_ns'] - s['copy_ns']:>10}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--iso", default=None)
    ap.add_argument("--instance", type=int, default=6)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--host", default=None, help="run on this SaunaOS machine instead of a VM")
    ap.add_argument("--calls", type=int, default=1000000, help="getpid calls; the other rows take a tenth")
    ap.add_argument("--sizes", default="", help="read/write sizes in bytes, comma-separated "
                    "(default 64 B .. 1 MiB)")
    ap.add_argument("--label", default="build")
    ap.add_argument("--settle", type=float, default=8.0, help="seconds after boot before measuring")
    ap.add_argument("--timeout", type=float, default=600.0, help="seconds the program may take")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--json", default=None, help="append the result to this JSON-lines file")
    args = ap.parse_args()

    runs = []
    if not args.host:
        tree_lock.hold("sys_bench")   # the copied disk is this build's
    if args.host:
        for _ in range(args.runs):
            runs.append(run_host(args))
    else:
        with tempfile.TemporaryDirectory(prefix="sys_bench.") as tmp:
            for _ in range(args.runs):
                disk = os.path.join(tmp, "disk.img")
                copy_disk(args.disk, disk)
                runs.append(run_vm(args, disk))
                os.remove(disk)
    for r in runs:
        report(r)
    if args.json:
        accel = "host" if args.host else ("kvm" if args.kvm else "tcg")
        with open(args.json, "a") as f:
            f.write(json.dumps({"label": args.label, "accel": accel, "runs": runs}) + "\n")


if __name__ == "__main__":
    main()
