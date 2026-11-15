#!/usr/bin/env python3
"""Check w16's 16-bit CPU interpreter against the real CPU, and its JIT against the interpreter.

WHAT IS UNDER TEST. userland/utils/w16/cpu16.c, the interpreter w16 runs a
Windows 3.x program's code on (docs/w16-design.md, stage 1), and jit16*.c,
the translator of its 32-bit flat code (docs/w16-jit-design.md, stage 1).

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
pusha/popa, enter/leave, jcc/loop/jcxz, imul by an immediate, xlat; the
386's part of them -- every family under the 66 prefix (dwords, with the
registers' high words random) and the 67 prefix (32-bit addresses, SIB
with a scale), a GS override, and its 0F instructions: near jcc, setcc,
movzx/movsx, bt/bts/btr/btc, shld/shrd, bsf/bsr, imul r,rm, push gs; all
of it again in a 32-BIT code segment with a 32-bit stack (the D and B
bits a Win386 program's flat code runs with), where 66 and 67 ask for
the 16-bit sizes; and
the x87 (cpu16_x87.c): one random ESC instruction in every register and
memory form on a random FPU state -- numbers at float's and double's
edges, zeros, denormals, infinities, NaNs, empty registers, every
rounding and precision control, unmasked exceptions -- compared as both
FNSAVE images, the 387's instruction and operand pointers included. The fuzz
runs half its cases in 32-bit segments, with a FLAT selector (a linear
base, a 4 GiB limit) over a small arena, so ASan sees any access the
modulo-2^32 wrap lets out. Not covered here: far transfers, segment loads
and interrupts, which the NE samples exercise.

THE JIT'S ORACLE IS THE INTERPRETER, which the cases above hold to the CPU.
Mode `32 jit` runs each 32-bit case twice more over the same memory --
interpreted, and translated from its first instruction -- and `32 jit seq`
does the same with sequences of 2-6 instructions whose flags are all
defined, so the lazy flags carried between them must come out exactly
(sequences use 32-bit addresses and no GS, as a Win386 program's code
does). Each case takes one of three segment shapes: base-pointer segments
with a data limit the operands reach; a FLAT data selector over an arena
that ends where they do; an ordinary segment inside the arena with the code
inside the data's reach, so a store can rewrite the running block. A case
the interpreter faults on must fault the same, at the same instruction.
The fuzz runs a second time translated, each segment ending at a PROT_NONE
page, because ASan cannot see into generated code.

POSITIVE CONTROL (--positive-control): in a COPY of cpu16.c, ADC loses its
carry-in, SAR stops sign-extending, a SIB byte's scale is ignored and a
66-prefixed push pushes a word, and in one of cpu16_x87.c FLDENV
stops turning the registers for a new TOP and stores stop reaching memory
(the oracle must go red each time), and
mem() loses its limit check (the ASan fuzz of random code must go red);
and in a copy of jit16_x86.c, a limit check and the arena's bound let 64
bytes too many through, a store into translated code goes unnoticed, ADC
loses its carry-in and INC claims to write CF, and in jit16.c a call-out
runs on past the interpreter's fault (each red in `32 jit seq`) and the
dispatcher runs blocks past the tick, which `32 jit`'s first case -- a
translated loop polling a byte only the tick sets -- must catch.

A host whose kernel has no modify_ldt cannot run the oracle: the check
says SKIPPED and does not claim a pass. A host whose FNSAVE no longer
stores the x87's FCS/FDS (CPUID 7 EBX bit 13) or keeps FDP/FOP only for
an exception (bit 6) -- a recent Intel -- has those fields left out of the
comparison, and the driver says so. Needs gcc.
"""

# w16's 16-bit CPU against the host's own, in an LDT segment; and
# random code under ASan. gcc and a kernel with modify_ldt.
RUNNER = {
    "name": "cpu16_host",
    "covers": "w16's 16-bit CPU (8086-386) vs the real CPU, its JIT vs the interpreter, fuzzed",
    "parallel": True,
}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
SEEDS = (1, 2, 3)
CASES = 200000

FUZZ_CONTROL = ("    if ((uint64_t)off + size - 1 > d->limit) return 0;\n", "    if (0) return 0;\n")

CPU = "userland/utils/w16/cpu16.c"
X87 = "userland/utils/w16/cpu16_x87.c"
JIT = "userland/utils/w16/jit16_x86.c"
# The JIT's other sources, staged as they are; lin16.c because jit16.c hooks its frees.
JIT_REST = ("userland/utils/w16/jit16.c", "userland/utils/w16/lin16.c")
HEADERS = ("cpu16.h", "jit16.h", "jit16_internal.h", "lin16.h")

CONTROLS = [
    ("adc carry-in", CPU, "        if (op == 0) cf = 0;\n", "        cf = 0;\n"),
    ("sar sign", CPU, "        int64_t sv = w == 2 ? (int32_t)v : w ? (int16_t)v : (int8_t)v;",
     "        int64_t sv = (int64_t)v;"),
    ("386 SIB scale ignored", CPU, "get32(c, (int)index) << scale", "get32(c, (int)index) << (scale & 0)"),
    ("386 a 66 push pushes a word", CPU, "    if (w == 2) push32(c, v);\n    else cpu16_push",
     "    if (w == 3) push32(c, v);\n    else cpu16_push"),
    ("x87 FLDENV keeps the registers' order", X87,
     "10 * ((i + top - top0) & 7)", "10 * ((i + 0 * (top - top0)) & 7)"),
    ("x87 a store never reaches memory", X87,
     "    if (len && !reads_only(op, reg)) memcpy(p, buf, len);\n", "\n"),
    # The 32-bit segments, run in mode 32: the D bit ignored, a B-bit stack still moving SP.
    ("32-bit code decoded as 16-bit", CPU, ".o32 = c->cs32, .a32 = c->cs32 }", ".o32 = 0, .a32 = 0 }", "32"),
    ("32-bit stack moves SP only", CPU,
     "    return ss_big(c) ? c->r[R_SP].e : c->r[R_SP].w;",
     "    return c->r[R_SP].w;", "32"),
    # The JIT (jit16_x86.c), against the interpreter in mode `32 jit seq`.
    ("jit: a limit check 64 bytes late", JIT,
     "        b1(e, 0x3D); b4(e, s->limit - (size - 1));         // cmp eax, limit - (size - 1)",
     "        b1(e, 0x3D); b4(e, s->limit - (size - 1) + 64);", "32 jit seq"),
    ("jit: the arena's bound 64 bytes late", JIT,
     "    b1(e, 0x3D); b4(e, x->c->lin_size - size);              // cmp eax, lin_size - size",
     "    b1(e, 0x3D); b4(e, x->c->lin_size - size + 64);", "32 jit seq"),
    ("jit: a store into translated code not caught", JIT, "    if (write && x->lines) {", "    if (write && 0) {",
     "32 jit seq"),
    ("jit: ADC and SBB lose the carry-in", JIT,
     "static void need_cf(struct jx *x) {\n    if (x->valid & F_CF) return;",
     "static void need_cf(struct jx *x) {\n    return;", "32 jit seq"),
    ("jit: a call-out runs on past the interpreter's fault", "userland/utils/w16/jit16.c",
     "    if (c->fault || c->exited) { j->st.callout_exit++; return 2; }",
     "    if (c->fault || c->exited) { j->st.callout_exit++; return 0; }", "32 jit seq"),
    ("jit: translated code keeps the tick out", "userland/utils/w16/jit16.c",
     "    uint64_t stop = (c->steps | (CPU16_TICK_STEPS - 1)) + 1;", "    uint64_t stop = (uint64_t)-1;", "32 jit"),
    ("jit: INC claims to write CF", JIT, "        op1(e, w, 0xFF, op >= 0x48, &l);\n        produce(x, ALL6 & ~F_CF);",
     "        op1(e, w, 0xFF, op >= 0x48, &l);\n        produce(x, ALL6);", "32 jit seq"),
]
# The cases in 16-bit segments, then in 32-bit ones (cpu16_check.c); then the 32-bit ones again
# through the JIT, one instruction a case and then sequences of them, against the interpreter.
MODES = ("16", "32", "32 jit", "32 jit seq")


def stage_all(tmp, edits=(), apply=False, where=CPU):
    """Every source the driver links, `where` with the edits; their paths."""
    for h in HEADERS:
        hostcheck.stage(tmp, f"userland/utils/w16/{h}")
    return [hostcheck.stage(tmp, f, edits=edits if where == f else (), apply=apply, tool="cpu16_hostcheck")
            for f in (CPU, X87, JIT) + JIT_REST]


def build(tmp, edits=(), apply=False, where=CPU):
    srcs = stage_all(tmp, edits, apply, where)
    drv = os.path.join(ROOT, "tools", "w16", "cpu16_check.c")
    flags = ["-O2", "-g", "-no-pie", "-Wall", "-Wextra", "-Werror"]
    return hostcheck.compile(tmp, "cpu16_check", [drv] + srcs, flags=flags, includes=[tmp],
                             tool="cpu16_hostcheck")


def run(exe, cases, seed, mode="16"):
    r = subprocess.run([exe, str(cases), str(seed)] + mode.split(), capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break ADC, SAR, SIB and a 32-bit push in a copy of cpu16.c; the check must go RED")
    ap.add_argument("--cases", type=int, default=CASES, help="cases per seed")
    args = ap.parse_args()

    if args.positive_control:
        red = []
        for name, where, before, after, *mode in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                exe = build(tmp, edits=[(before, after)], apply=True, where=where)
                m = mode[0] if mode else "16"
                # A store into the running block is rare in random cases: the JIT's get more.
                rc, out = run(exe, 200000 if "jit" in m else 50000, 7, m)
                if rc == 3:
                    print("cpu16_hostcheck: SKIPPED -- the host kernel has no modify_ldt")
                    return 0
                # A broken interpreter may compute an address the CPU then faults on, which ends
                # the driver: that is red too.
                first = next((ln for ln in out.splitlines() if ln.startswith("FAIL")), "")
                if rc not in (0, 1) and not first:
                    first = f"the native run died (exit {rc})"
                print(f"  {'RED  ' if rc else 'green'} {name}: {first}")
                red.append(rc != 0)
        # The fuzz's own control: no limit check, so random code must escape.
        with tempfile.TemporaryDirectory() as tmp:
            srcs = stage_all(tmp, edits=[FUZZ_CONTROL], apply=True, where=CPU)
            asan = hostcheck.compile(tmp, "cpu16_fuzz",
                                     [os.path.join(ROOT, "tools", "w16", "cpu16_check.c")] + srcs,
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
        for mode in MODES:
            for seed in SEEDS:
                rc, out = run(exe, args.cases, seed, mode)
                if rc == 3:
                    print("cpu16_hostcheck: SKIPPED -- the host kernel has no modify_ldt")
                    return 0
                last = out.strip().splitlines()[-1] if out.strip() else "(no output)"
                if rc:
                    fails += 1
                    print(out[-4000:])
                label = mode.replace(" jit seq", "-bit through the JIT, sequences,").replace(
                    " jit", "-bit through the JIT,") if "jit" in mode else f"{mode}-bit"
                print(f"  {'ok  ' if rc == 0 else 'FAIL'} {label} seed {seed}: {last}")
        # Random bytes as code under ASan/UBSan: fault or finish, never escape -- interpreted, then
        # translated, where each segment ends at a PROT_NONE page ASan cannot see past.
        asan = hostcheck.compile(tmp, "cpu16_fuzz",
                                 [os.path.join(ROOT, "tools", "w16", "cpu16_check.c")]
                                 + [os.path.join(tmp, os.path.basename(f)) for f in (CPU, X87, JIT) + JIT_REST],
                                 flags=["-O1", "-g", "-no-pie", "-Wall", "-Wextra", "-Werror",
                                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
                                 includes=[tmp], tool="cpu16_hostcheck")
        for extra, what in (([], "interpreted"), (["jit"], "translated")):
            fz = subprocess.run([asan, "fuzz", "20000", "5"] + extra, capture_output=True, text=True)
            if fz.returncode:
                fails += 1
                why = [ln for ln in fz.stderr.splitlines() if "ERROR:" in ln or "runtime error" in ln]
                print(f"  FAIL fuzz, {what}: " + (why[0].strip() if why else f"exit {fz.returncode}"))
                print(fz.stderr[-3000:])
            else:
                print(f"  ok   {fz.stdout.strip()} ({what}), no fault reached the host")
    print(f"cpu16_hostcheck: {'FAILED' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
