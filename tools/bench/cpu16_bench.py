#!/usr/bin/env python3
"""How fast w16's CPU runs 32-bit code, on the host: MIPS on a fixed workload.

    python3 tools/bench/cpu16_bench.py                 # this checkout's cpu16.c
    python3 tools/bench/cpu16_bench.py --rev HEAD~3    # another revision's, to compare
    python3 tools/bench/cpu16_bench.py --perf          # and where the time goes (perf report)

The workload is tools/w16/bench32.c -- an LZSS decompressor, a transparent
sprite blit, a CRC and a sort, what a 1990s game's 32-bit code spends its
time in -- compiled -m32 into a flat blob and run by userland/utils/w16/cpu16.c
in a 32-bit segment (tools/w16/cpu16_bench.c), as a Win386 program's code is.
Each run's checksum must equal the same C's compiled for the host, so a
speed-up that broke an instruction is a FAIL, not a better number.

WHAT A NUMBER HERE IS NOT: the speed inside SaunaOS. It is the interpreter
alone, on the host, at -O2 -- a guest under KVM runs the same code a little
slower, under TCG far slower. Compare two revisions on ONE host, best of
several runs (`--runs`); a difference of a few percent is noise.

Needs gcc that can compile -m32 (no 32-bit libraries: the blob links none).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

ROOT = toolpath.REPO
W16 = "userland/utils/w16"
CPU_FILES = ("cpu16.c", "cpu16.h", "cpu16_x87.c")
JIT_FILES = ("jit16.c", "jit16_x86.c", "jit16.h", "jit16_internal.h", "lin16.c", "lin16.h")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"cpu16_bench: {' '.join(cmd)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def stage_cpu(tmp, rev):
    """The CPU's sources -- this checkout's, or `rev`'s from git -- into tmp. The JIT's are staged
    only where the revision has them (it came after cpu16.c's 32-bit registers)."""
    files = CPU_FILES
    if rev is None or subprocess.run(["git", "-C", ROOT, "cat-file", "-e", f"{rev}:{W16}/jit16.c"],
                                     capture_output=True).returncode == 0:
        files = CPU_FILES + JIT_FILES
    for f in files:
        dst = os.path.join(tmp, f)
        if rev:
            src = run(["git", "-C", ROOT, "show", f"{rev}:{W16}/{f}"])
        else:
            with open(os.path.join(ROOT, W16, f)) as fh:
                src = fh.read()
        with open(dst, "w") as fh:
            fh.write(src)


def build(tmp, rev):
    stage_cpu(tmp, rev)
    work = os.path.join(ROOT, "tools", "w16", "bench32.c")
    obj, elf, blob = (os.path.join(tmp, n) for n in ("bench32.o", "bench32.elf", "bench32.bin"))
    run(["gcc", "-m32", "-march=i486", "-mno-mmx", "-mno-sse", "-O2", "-ffreestanding", "-fno-pic", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
         "-fno-builtin", "-c", work, "-o", obj])
    run(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-Ttext=0", "-e", "bench", "-o", elf, obj])
    run(["objcopy", "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", elf, blob])
    entry = next(int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) T bench$", run(["nm", elf]), re.M))
    exe = os.path.join(tmp, "cpu16_bench")
    srcs = [os.path.join(tmp, f) for f in ("cpu16.c", "cpu16_x87.c", "jit16.c", "jit16_x86.c", "lin16.c")
            if os.path.exists(os.path.join(tmp, f))]
    run(["gcc", "-O2", "-g", "-std=gnu11", "-I", tmp, "-o", exe, os.path.join(ROOT, "tools", "w16", "cpu16_bench.c")]
        + srcs + ["-x", "c", work, "-Dbench=bench_native"])
    return exe, blob, entry


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rev", help="time this git revision's cpu16.c instead of the checkout's")
    ap.add_argument("--runs", type=int, default=5, help="runs; the best is reported (default 5)")
    ap.add_argument("--perf", action="store_true", help="record one run with perf and print the top functions")
    ap.add_argument("--jit", action="store_true", help="run through the JIT (userland/utils/w16/jit16.c)")
    ap.add_argument("--jit-threshold", type=int, default=0, metavar="N",
                    help="with --jit: arrivals before a block is translated (0: the JIT's default)")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe, blob, entry = build(tmp, args.rev)
        jit = [str(args.jit_threshold)] if args.jit else []
        r = subprocess.run([exe, blob, hex(entry), str(args.runs)] + jit, capture_output=True, text=True)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            return 1
        if args.perf:
            data = os.path.join(tmp, "perf.data")
            # cycles:u -- user space only, what a perf_event_paranoid of 2 still lets a user record
            subprocess.run(["perf", "record", "-q", "-e", "cycles:u", "-o", data, exe, blob, hex(entry), "1"] + jit,
                           capture_output=True)
            rep = subprocess.run(["perf", "report", "-i", data, "--no-children", "--stdio", "--percent-limit", "1.5"],
                                 capture_output=True, text=True).stdout
            print("\n".join(ln for ln in rep.splitlines() if ln.strip() and not ln.startswith("#")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
