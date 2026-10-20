#!/usr/bin/env python3
"""Check w16's 16-bit CPU interpreter against the real CPU, on the host.

WHAT IS UNDER TEST. userland/w16/cpu16.c, the interpreter w16 runs a
Windows 3.x program's code on (docs/w16-design.md, stage 1).

THE ORACLE IS THE HOST'S OWN CPU. tools/w16/cpu16_check.c puts a 16-bit
code segment, a data segment and a stack segment in the process's LDT
(modify_ldt -- Wine's route for 16-bit code), and runs each random
instruction twice: in the interpreter, and natively in compatibility
mode, entered from 64-bit code by a far call. Registers, flags (each
instruction's UNDEFINED flags masked, as the manuals list them) and both
segments' bytes must match. Cases the interpreter faults on (a divide
error) are not run natively and are counted as skipped.

Covered: the ALU ops in every form, inc/dec, shifts and rotates by 1,
CL and an immediate, mul/imul/div/idiv, mov/lea/xchg, the flag ops, BCD,
string ops with and without REP/REPE/REPNE in both directions, push/pop,
pusha/popa, enter/leave, jcc/loop/jcxz, imul by an immediate, xlat, and
the x87 (cpu16_x87.c): one random ESC instruction in every register and
memory form on a random FPU state -- numbers at float's and double's
edges, zeros, denormals, infinities, NaNs, empty registers, every
rounding and precision control, unmasked exceptions -- compared as both
FNSAVE images, the 387's instruction and operand pointers included. Not
covered here: far transfers, segment loads and interrupts, which the
NE samples exercise.

POSITIVE CONTROL (--positive-control): in a COPY of cpu16.c, ADC loses its
carry-in and SAR stops sign-extending, and in one of cpu16_x87.c FLDENV
stops turning the registers for a new TOP and stores stop reaching memory
(the oracle must go red each time), and
mem() loses its limit check (the ASan fuzz of random code must go red).

A host whose kernel has no modify_ldt cannot run the oracle: the check
says SKIPPED and does not claim a pass. Needs gcc.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
SEEDS = (1, 2, 3)
CASES = 200000

FUZZ_CONTROL = ("    if ((uint32_t)off + size - 1 > d->limit) {", "    if (0) {")

CPU = "userland/w16/cpu16.c"
X87 = "userland/w16/cpu16_x87.c"

CONTROLS = [
    ("adc carry-in", CPU, "        if (op == 0) cf = 0;\n", "        cf = 0;\n"),
    ("sar sign", CPU, "        int32_t sv = w ? (int16_t)v : (int8_t)v;", "        int32_t sv = (int32_t)v;"),
    ("x87 FLDENV keeps the registers' order", X87,
     "10 * ((i + top - top0) & 7)", "10 * ((i + 0 * (top - top0)) & 7)"),
    ("x87 a store never reaches memory", X87,
     "    if (len && !reads_only(op, reg)) memcpy(p, buf, len);\n", "\n"),
]


def build(tmp, edits=(), apply=False, where=CPU):
    cpu = hostcheck.stage(tmp, CPU, edits=edits if where == CPU else (), apply=apply,
                          tool="cpu16_hostcheck")
    hostcheck.stage(tmp, "userland/w16/cpu16.h")
    x87 = hostcheck.stage(tmp, X87, edits=edits if where == X87 else (), apply=apply,
                          tool="cpu16_hostcheck")
    drv = os.path.join(ROOT, "tools", "w16", "cpu16_check.c")
    flags = ["-O2", "-g", "-no-pie", "-Wall", "-Wextra", "-Werror"]
    return hostcheck.compile(tmp, "cpu16_check", [drv, cpu, x87], flags=flags, includes=[tmp],
                             tool="cpu16_hostcheck")


def run(exe, cases, seed):
    r = subprocess.run([exe, str(cases), str(seed)], capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break ADC and SAR in a copy of cpu16.c; the check must go RED")
    ap.add_argument("--cases", type=int, default=CASES, help="cases per seed")
    args = ap.parse_args()

    if args.positive_control:
        red = []
        for name, where, before, after in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                exe = build(tmp, edits=[(before, after)], apply=True, where=where)
                rc, out = run(exe, 50000, 7)
                if rc == 3:
                    print("cpu16_hostcheck: SKIPPED -- the host kernel has no modify_ldt")
                    return 0
                first = next((ln for ln in out.splitlines() if ln.startswith("FAIL")), "")
                print(f"  {'RED  ' if rc == 1 else 'green'} {name}: {first}")
                red.append(rc == 1)
        # The fuzz's own control: no limit check, so random code must escape.
        with tempfile.TemporaryDirectory() as tmp:
            cpu = hostcheck.stage(tmp, "userland/w16/cpu16.c", edits=[FUZZ_CONTROL], apply=True,
                                  tool="cpu16_hostcheck")
            hostcheck.stage(tmp, "userland/w16/cpu16.h")
            x87 = hostcheck.stage(tmp, "userland/w16/cpu16_x87.c")
            asan = hostcheck.compile(tmp, "cpu16_fuzz",
                                     [os.path.join(ROOT, "tools", "w16", "cpu16_check.c"), cpu, x87],
                                     flags=["-O1", "-g", "-no-pie", "-fsanitize=address,undefined",
                                            "-fno-sanitize-recover=all"],
                                     includes=[tmp], tool="cpu16_hostcheck")
            fz = subprocess.run([asan, "fuzz", "20000", "5"], capture_output=True, text=True)
            why = next((ln.strip() for ln in fz.stderr.splitlines() if "ERROR:" in ln), "")
            print(f"  {'RED  ' if fz.returncode else 'green'} no limit check (fuzz): {why[:90]}")
            red.append(fz.returncode != 0)
        print(f"cpu16_hostcheck: positive control {'RED as it must be' if all(red) else 'FAILED to go red'}")
        return 0 if all(red) else 1

    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        for seed in SEEDS:
            rc, out = run(exe, args.cases, seed)
            if rc == 3:
                print("cpu16_hostcheck: SKIPPED -- the host kernel has no modify_ldt")
                return 0
            last = out.strip().splitlines()[-1] if out.strip() else "(no output)"
            if rc:
                fails += 1
                print(out[-4000:])
            print(f"  {'ok  ' if rc == 0 else 'FAIL'} seed {seed}: {last}")
        # Random bytes as code under ASan/UBSan: fault or finish, never escape.
        asan = hostcheck.compile(tmp, "cpu16_fuzz",
                                 [os.path.join(ROOT, "tools", "w16", "cpu16_check.c"),
                                  os.path.join(tmp, "cpu16.c"), os.path.join(tmp, "cpu16_x87.c")],
                                 flags=["-O1", "-g", "-no-pie", "-Wall", "-Wextra", "-Werror",
                                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
                                 includes=[tmp], tool="cpu16_hostcheck")
        fz = subprocess.run([asan, "fuzz", "20000", "5"], capture_output=True, text=True)
        if fz.returncode:
            fails += 1
            why = [ln for ln in fz.stderr.splitlines() if "ERROR:" in ln or "runtime error" in ln]
            print("  FAIL fuzz: " + (why[0].strip() if why else f"exit {fz.returncode}"))
            print(fz.stderr[-3000:])
        else:
            print(f"  ok   {fz.stdout.strip()}, no fault reached the host")
    print(f"cpu16_hostcheck: {'FAILED' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
