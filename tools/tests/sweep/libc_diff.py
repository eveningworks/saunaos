#!/usr/bin/env python3
"""tolibc's formatter, number parsers and maths against glibc, case by case.

WHY THIS EXISTS
---------------
tolibc's bar is COMPLETENESS -- the opposite of the rest of this repo --
because its audience is code not yet written (docs/libc-design.md). A
happy-path test cannot hold that bar: it checks the cases whoever wrote
it thought of, which are the cases they got right. What catches an
EXPECTATION being wrong is an oracle that shares no code.

A two-minute version of this harness found six printf bugs and three
strtol bugs in one sitting, including `%hhu` of 256 printing 256 and
`strtol("0", &end, 0)` reporting no conversion at all. It also found
that `libc3_test.c` ASSERTED one of those bugs, so the suite would have
gone red on the fix -- which is the failure mode a self-referential test
suite has and a differential one does not.

Same shape as usnd_hostcheck.py and umd_hostcheck.py: compile this
repo's implementation with the host gcc and judge it by something that
was written by other people for other reasons.

HOW IT AVOIDS A SYMBOL CLASH
----------------------------
tolibc defines strtol, abort, qsort and twenty-odd more names glibc
also defines, so the two cannot simply be linked together. The wrapper
renames them at the C level -- `#define strtol toy_strtol` ahead of an
`#include` of the source -- rather than with objcopy, because that also
redirects tolibc's INTERNAL calls and leaves genuine externals alone.

THE MATHS (--only math) is the same idea in ULPs: every <math.h>
function in three types against glibc's libm, the worst error per
function checked against a documented bound -- see the comment above
MATH_BOUNDS. --break FUNC is its positive control.

WHAT IT DOES NOT CHECK, said plainly: anything with a side effect
(malloc, exit, the stdio streams) -- this is pure-function territory.
Nor does it run in the guest: it compiles the same source the guest
runs, which is what makes it fast and what makes it unable to see a
problem that only appears in ring 3.
"""

# tolibc's formatter and parsers against glibc, ~20,000 cases, and its
# maths in ULPs, ~370,000. Host gcc only; exits non-zero when anything
# differs or a function exceeds its bound.
RUNNER = {
    "covers": "tolibc's printf, strtol and <math.h> against glibc",
    "parallel": True,
    "needs": ["host_cc", "needs gcc on PATH"],
}

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)

# Every global tolibc's stdlib.c defines. Renamed wholesale so the
# object can sit beside glibc; the three under test get used, the rest
# just have to stop colliding.
STDLIB_SYMS = """abort abs atexit atof atoi atol atoll at_quick_exit bsearch div
exit _Exit labs ldiv llabs lldiv qsort quick_exit rand realloc srand
strtod strtol strtoll strtoul strtoull""".split()

# --- the printf matrix -------------------------------------------------
#
# Flags, widths and precisions are combined against each conversion
# rather than listed by hand: the bugs found so far were all in the
# INTERACTION (a sign emitted before the padding, a precision ignored
# for one conversion), which a list of hand-picked cases misses because
# whoever writes it picks the combinations they already believe work.
INT_CONVS = ["d", "i", "u", "x", "X", "o"]
FLAGS = ["", "-", "+", "#", "0", "0+"]
WIDTHS = ["", "5", "8"]
PRECS = ["", ".0", ".3"]   # .0 is where a precision and '#' collide
LENGTHS = ["", "hh", "h", "l", "ll"]
INT_ARGS = [0, 1, -1, 42, 255, 256, 65536, -2147483648]

# The float conversions get the same treatment, with the two flags the
# integers do not exercise: ' ' and '#'. The values are ones a correct
# conversion prints exactly, so a difference is a FLAG bug rather than
# the known rounding gap printf_float.c's header states. Keep them away
# from a half-way point at the precisions below: -2.5e-7 is stored just
# UNDER 2.5e-7, and scaling by ten rounds it up, so `%.0e` printed 3.
FLOAT_CONVS = ["f", "F", "e", "E", "g", "G"]
FLOAT_FLAGS = ["", "-", "+", " ", "#", "0", "0+", "0 ", "-0", "#0"]
FLOAT_WIDTHS = ["", "6", "12"]
FLOAT_PRECS = ["", ".0", ".3"]
FLOAT_ARGS = ["0.0", "-0.0", "1.0", "-1.5", "3.14159", "42.0", "0.000125",
              "123456.789", "1e20", "-3.75e-7", "1.0/0.0", "-1.0/0.0", "0.0/0.0"]

STR_CASES = [('%s', '"hello"'), ('%.2s', '"hello"'), ('%8s', '"hi"'),
             ('%-8s|', '"hi"'), ('%c', "'a'"), ('%5c', "'a'"),
             ('%-5c|', "'a'"), ('%%', None)]

# --- the parser matrix -------------------------------------------------
STRTOL_INPUTS = [
    '"0"', '"00"', '"0x"', '"0X"', '"09"', '"0xzz"', '"0x1f"', '"  42"',
    '"+42"', '"-42"', '"-0"', '"z"', '""', '" "', '"2147483648"',
    '"9223372036854775807"', '"-9223372036854775808"',
    '"9223372036854775808"', '"99999999999999999999999"',
    '"-99999999999999999999999"', '"18446744073709551615"',
    '"0777"', '"0b11"', '"ff"', '"FF"', '"7fffffffffffffff"',
]
STRTOL_BASES = [0, 8, 10, 16, 36]

# **LENGTH IS ITS OWN AXIS.** A written number's DIGIT COUNT and the
# magnitude it denotes are different things, and conflating them is what
# made "1" + 309 zeros + "e-309" -- whose value is 1 -- come out
# infinite. None of the short inputs below could have found that, so the
# long ones are built programmatically at the end of this list.
STRTOD_INPUTS = [
    '"0"', '"0.0"', '"1"', '"1.5"', '"-1.5"', '"  3.25"', '".5"', '"5."',
    '"1e3"', '"1e-3"', '"1E3"', '"1e"', '"1e+"', '"0e999"', '"1e999"',
    '"1e-999"', '"1e308"', '"1e309"', '"1e-310"', '"1e-323"',
    '"inf"', '"-inf"', '"nan"', '"0x1p2"', '"0x10"', '"z"', '""',
    '"3.14159265358979"', '"123456789012345678901234567890"',
    # The spelled-out forms: stopping at three characters is a wrong
    # answer, not a refusal -- "infinity" left "inity" for the caller.
    '"infinity"', '"INFINITY"', '"-infinity"', '"nan(1234)"', '"nan(_x9)"',
    '"nan()"', '"infi"', '"0x1p1024"', '"0x1p-1074"', '"0x1p-1080"',
]

# Numbers far longer than a double's range, whose VALUE is ordinary.
STRTOD_INPUTS += [
    '"1' + '0' * 309 + 'e-309"',      # == 1
    '"0.' + '1' * 310 + '"',          # == 0.111...
    '"' + '9' * 400 + '"',            # overflows however it is read
    '"0.' + '0' * 320 + '1"',         # a subnormal written the long way
]


def build(workdir, extra_cflags=()):
    """Compile the probe. Returns the binary path, or raises."""
    renames = "\n".join(f"#define {s} toy_{s}" for s in STDLIB_SYMS)
    open(os.path.join(workdir, "toy_stdlib.c"), "w").write(
        "// Generated by tools/tests/sweep/libc_diff.py -- renames tolibc's globals so\n"
        "// the object can be linked beside glibc. See the module docstring.\n"
        + renames + '\n#include "userland/libs/libc/stdlib.c"\n')

    # tolibc's stdlib.c reaches the kernel allocator and sys_exit. None
    # of the functions under test calls them, but the linker still wants
    # them defined -- so they abort loudly rather than returning
    # something a future test might quietly rely on.
    open(os.path.join(workdir, "stubs.c"), "w").write(r"""
#include <stdio.h>
#include <stdlib.h>
static void nope(const char *who) {
    fprintf(stderr, "libc_diff: %s reached -- this harness is "
                    "pure-function only\n", who);
    abort();
}
void sys_exit(int c) { (void)c; nope("sys_exit"); }
int sys_getpid(void) { nope("sys_getpid"); return 0; }
void *kmalloc(unsigned long n) { (void)n; nope("kmalloc"); return 0; }
void *kcalloc(unsigned long a, unsigned long b) { (void)a; (void)b; nope("kcalloc"); return 0; }
unsigned long kmalloc_size(const void *p) { (void)p; nope("kmalloc_size"); return 0; }
void kfree(void *p) { (void)p; nope("kfree"); }
int sys_set_abort_msg(const char *b, unsigned long c) { (void)b; (void)c; nope("sys_set_abort_msg"); return 0; }
int sys_getrandom(void *b, unsigned long n) { (void)b; (void)n; nope("sys_getrandom"); return 0; }
void sys_yield(void) { nope("sys_yield"); }
""")

    src = os.path.join(workdir, "probe.c")

    # **THE PROBE IS COMPILED WITHOUT tolibc's HEADERS ON ITS PATH, AND
    # THAT IS THE WHOLE CORRECTNESS OF THIS HARNESS.**
    # userland/libs/libc/include/stdio.h makes vsnprintf a static inline around
    # k_vsnprintf and #defines snprintf to k_snprintf. With that header
    # reachable, both sides of every comparison are tolibc and the
    # harness reports perfect agreement while measuring NOTHING -- which
    # is exactly what it did until this was found. The probe declares the
    # tolibc entry points by hand instead.
    TOY_INCLUDES = ["-I" + REPO,
                    "-I" + os.path.join(REPO, "userland/libs/libc/include"),
                    "-I" + os.path.join(REPO, "kernel/include/api"),
                    "-I" + os.path.join(REPO, "kernel/include/abi"),
                    "-I" + os.path.join(REPO, "userland"), "-I" + os.path.join(REPO, "userland", "libs"),
                    # kconfig.h is GENERATED (build.conf -> build/gen), and
                    # timer.h reaches it through syscall_abi.h.
                    "-I" + os.path.join(REPO, "build/gen")]

    objs = []
    for name, path in (("toy_stdlib", os.path.join(workdir, "toy_stdlib.c")),
                       ("stubs", os.path.join(workdir, "stubs.c")),
                       ("kfmt", os.path.join(REPO, "kernel/lib/kfmt.c")),
                       ("knum", os.path.join(REPO, "kernel/lib/knum.c")),
                       ("kstring", os.path.join(REPO, "kernel/lib/string.c")),
                       ("pfloat", os.path.join(REPO, "userland/libs/libc/printf_float.c"))):
        obj = os.path.join(workdir, name + ".o")
        r = subprocess.run(["gcc", "-w", "-O0", "-c", "-o", obj, path,
                            *TOY_INCLUDES], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{name} did not compile:\n" + r.stderr[:1500])
        objs.append(obj)

    # -O0 on purpose: the probe is I/O bound, and optimising a table of
    # thousands of entries costs minutes for nothing. No SaunaOS include
    # path here -- see above.
    r = subprocess.run(["gcc", "-w", "-O0", "-o", os.path.join(workdir, "probe"),
                        src, *objs, *extra_cflags], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("probe did not build:\n" + r.stderr[:2000])
    return os.path.join(workdir, "probe")


def gen_probe(workdir, sections, exact_float=False):
    """Write probe.c. Each check prints one TAB-separated result line."""
    out = []
    out.append('#include <stdio.h>')
    out.append('#define EXACT_FLOAT %d' % (1 if exact_float else 0))
    out.append('#include <stdlib.h>')
    out.append('#include <errno.h>')
    out.append('#include <string.h>')
    out.append('#include <math.h>')
    # DECLARED BY HAND, not included: see build() for why tolibc's
    # headers must not be on this translation unit's path.
    out.append('#include <stdarg.h>')
    out.append('unsigned long k_snprintf(char *, unsigned long, const char *, ...);')
    out.append('unsigned long k_vsnprintf(char *, unsigned long, const char *, va_list);')
    out.append('long toy_strtol(const char *, char **, int);')
    out.append('unsigned long toy_strtoul(const char *, char **, int);')
    out.append('double toy_strtod(const char *, char **);')
    # A difference is reported with both answers so the reader never has
    # to re-run anything to see what happened.
    out.append(r'''
static int fails, checks;
// Within one ULP: same sign, same magnitude, adjacent representations.
static int near(double a, double b) {
    if (a == b) return 1;
    if (a != a || b != b) return 0;          // NaN is never near anything
    long long ia, ib;
    __builtin_memcpy(&ia, &a, 8); __builtin_memcpy(&ib, &b, 8);
    long long d = ia > ib ? ia - ib : ib - ia;
    return d <= 1;
}
static char dbuf[2][40]; static int dslot;
static const char *fmtd(double v) {
    dslot ^= 1; snprintf(dbuf[dslot], sizeof dbuf[0], "%.17g", v);
    return dbuf[dslot];
}
static void cmp(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want)) { fails++; printf("DIFF\t%s\tsaunaos=[%s]\tglibc=[%s]\n", what, got, want); }
}
#define FMT1(fmt, arg) do {                                   \
    char g[128], w[128]; char lbl[64];                        \
    snprintf(lbl, sizeof lbl, "%s <- %s", fmt, #arg);         \
    k_snprintf(g, sizeof g, fmt, arg);                        \
    snprintf(w, sizeof w, fmt, arg);                          \
    cmp(lbl, g, w);                                           \
} while (0)
''')
    out.append("int main(void) {")
    # **ALWAYS ON, NOT BEHIND A FLAG.** This harness once compared
    # tolibc against ITSELF for every printf case and reported perfect
    # agreement, because tolibc's <stdio.h> makes vsnprintf an inline
    # around k_vsnprintf. The two must be different functions or nothing
    # below means anything.
    out.append(r'''    if ((void *)snprintf == (void *)k_snprintf ||
        (void *)vsnprintf == (void *)k_vsnprintf) {
        printf("SELFTEST\tthe oracle IS the implementation -- "
               "tolibc headers leaked into the probe\n");
        return 2;
    }''')
    out.extend(sections)
    out.append('    printf("SUMMARY\\t%d\\t%d\\n", checks, fails);')
    out.append("    return 0;")
    out.append("}")
    open(os.path.join(workdir, "probe.c"), "w").write("\n".join(out) + "\n")


def printf_sections(broken=False):
    """Format/argument pairs as a TABLE, not one C statement each.

    The first version emitted a statement per case, and the COMPILE took
    minutes. A harness nobody can afford to run is one nobody runs, so
    the cases are data and the probe loops over them.

    Every argument is passed as a `long`, which is deliberate rather
    than sloppy: varargs promote, so what a printf really receives for
    %hhu IS a promoted integer, and reading it back narrowed is the
    behaviour under test. Both implementations get the identical value.
    """
    s = ['    {', '        static const struct { const char *f; long v; } T[] = {']
    for length in LENGTHS:
        for conv in INT_CONVS:
            for flag in FLAGS:
                for width in WIDTHS:
                    for prec in PRECS:
                        fmt = f"%{flag}{width}{prec}{length}{conv}"
                        for a in INT_ARGS:
                            s.append(f'            {{ "{fmt}", {a}L }},')
    s.append('        };')
    s.append('        char g[160], w[160], lbl[96];')
    s.append('        for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {')
    s.append('            k_snprintf(g, sizeof g, T[i].f, T[i].v);')
    s.append('            snprintf(w, sizeof w, T[i].f, T[i].v);')
    s.append('            snprintf(lbl, sizeof lbl, "%s <- %ld", T[i].f, T[i].v);')
    s.append('            cmp(lbl, g, w);')
    s.append('        }')
    s.append('    }')
    s.append('    {')
    s.append('        static const struct { const char *f; double v; } F[] = {')
    for conv in FLOAT_CONVS:
        for flag in FLOAT_FLAGS:
            for width in FLOAT_WIDTHS:
                for prec in FLOAT_PRECS:
                    for a in FLOAT_ARGS:
                        s.append(f'            {{ "%{flag}{width}{prec}{conv}", {a} }},')
    s.append('        };')
    s.append('        char g[160], w[160], lbl[96];')
    s.append('        for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {')
    s.append('            k_snprintf(g, sizeof g, F[i].f, F[i].v);')
    s.append('            snprintf(w, sizeof w, F[i].f, F[i].v);')
    s.append('            snprintf(lbl, sizeof lbl, "%s <- %.17g", F[i].f, F[i].v);')
    s.append('            cmp(lbl, g, w);')
    s.append('        }')
    s.append('    }')
    for fmt, arg in STR_CASES:
        if arg is None:
            s.append(f'    {{ char g[128], w[128]; k_snprintf(g, sizeof g, "{fmt}");'
                     f' snprintf(w, sizeof w, "{fmt}"); cmp("{fmt}", g, w); }}')
        else:
            s.append(f'    FMT1("{fmt}", {arg});')
    return s


def parser_sections():
    s = []
    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; long gv, wv; int gerr, werr;''')
    for inp in STRTOL_INPUTS:
        lbl = inp.replace('"', '\\"')
        for base in STRTOL_BASES:
            s.append(f'''
        errno = 0; gv = toy_strtol({inp}, &ge, {base}); gerr = errno;
        errno = 0; wv = strtol({inp}, &we, {base}); werr = errno;
        snprintf(gbuf, sizeof gbuf, "%ld/%d/%d", gv, (int)(ge - ({inp})), gerr == ERANGE);
        snprintf(wbuf, sizeof wbuf, "%ld/%d/%d", wv, (int)(we - ({inp})), werr == ERANGE);
        cmp("strtol({lbl}, {base}) value/consumed/ERANGE", gbuf, wbuf);''')
    s.append("    }")

    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; unsigned long gv, wv;''')
    for inp in STRTOL_INPUTS:
        lbl = inp.replace('"', '\\"')
        for base in (0, 10, 16):
            s.append(f'''
        gv = toy_strtoul({inp}, &ge, {base});
        wv = strtoul({inp}, &we, {base});
        snprintf(gbuf, sizeof gbuf, "%lu/%d", gv, (int)(ge - ({inp})));
        snprintf(wbuf, sizeof wbuf, "%lu/%d", wv, (int)(we - ({inp})));
        cmp("strtoul({lbl}, {base}) value/consumed", gbuf, wbuf);''')
    s.append("    }")

    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; double gv, wv;''')
    for inp in STRTOD_INPUTS:
        lbl = inp.replace('"', '\\"')
        # Compared as %.17g plus the consumed count: the exact decimal
        # is what a rounding bug moves, and a tolerance would hide it.
        s.append(f'''
        gv = toy_strtod({inp}, &ge);
        wv = strtod({inp}, &we);
        // COMPARED TO WITHIN ONE ULP, not bit for bit. tolibc's strtod
        // applies the decimal exponent by repeated multiplication and
        // is accurate to a couple of ULP by construction, which its own
        // comment says; a correctly-rounded conversion is a separate
        // project (docs/libc-design.md). Demanding equality here would
        // report that KNOWN limit as a failure on every run and bury
        // the categorical bugs -- a NaN, a lost subnormal -- that this
        // harness exists to catch. --exact-float turns it off.
        snprintf(gbuf, sizeof gbuf, "%s/%d", EXACT_FLOAT || !near(gv, wv) ?
                 fmtd(gv) : fmtd(wv), (int)(ge - ({inp})));
        snprintf(wbuf, sizeof wbuf, "%s/%d", fmtd(wv), (int)(we - ({inp})));
        cmp("strtod({lbl}) value/consumed", gbuf, wbuf);''')
    s.append("    }")
    return s


# --- the maths: <math.h> against glibc's libm, in ULPs ------------------
#
# Every function in its three types, over a value set built for it: the
# edges (signed zeros, the subnormal boundaries, the largest finite, the
# infinities, NaN), log-uniform values across the whole exponent range,
# dense values where the function has its interesting range, and the
# cases a particular function gets wrong -- arguments a hair from a
# multiple of pi/2, integers and halves for rounding, poles, overflow
# thresholds, fma's halfway points. The worst error per function is
# reported in ULPs (the distance between the two results' bit patterns,
# monotonic within a sign) and checked against MATH_BOUNDS.
#
# **THE ORACLE IS glibc, NOT THE TRUTH.** glibc's double functions are
# within an ulp of the true value (most of them correctly rounded since
# 2.28), its float ones correctly rounded (CORE-MATH, 2.41 on), its long
# double ones within a few ulp -- so a bound here is "ULPs from glibc",
# which is the true error plus glibc's own. A sign of zero that differs,
# a NaN where glibc has a number or the reverse, and any integer output
# (an exponent, a quotient's bits, lrint) that differs are CATEGORICAL
# failures, whatever the bound.

import random
import struct
from fractions import Fraction

# (precision, smallest normal exponent, largest exponent) per type.
MATH_TYPES = {"d": (53, -1022, 1023), "f": (24, -126, 127), "l": (64, -16382, 16383)}
MATH_CTYPE = {"d": "double", "f": "float", "l": "long double"}
MATH_SUFFIX = {"d": "", "f": "f", "l": "l"}

# Every public name tolibc's <math.h> declares, renamed so the objects can
# sit beside glibc's libm (see STDLIB_SYMS for why at the C level).
MATH_BASE1 = """sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh exp exp2
expm1 log1p log log2 log10 logb fabs sqrt cbrt erf erfc lgamma tgamma ceil floor
trunc round rint nearbyint""".split()
MATH_BASE2 = "atan2 hypot pow fmod remainder copysign nextafter fdim fmax fmin".split()
MATH_OTHER = "ilogb ldexp frexp scalbn scalbln modf lrint llrint lround llround remquo nan nexttoward fma".split()
MATH_SYMS = ([b + s for b in MATH_BASE1 + MATH_BASE2 + MATH_OTHER for s in ("", "f", "l")]
             + ["lgamma_r", "lgammaf_r", "lgammal_r", "signgam"])

# (function, signature, value set, bound in ULPs, types). Signatures:
# 1 = T f(T), 2 = T f(T,T), 3 = fma, i = T f(T,int), L = T f(T,long),
# I = int f(T), R = long f(T), RR = long long f(T), E = frexp,
# M = modf, Q = remquo, G = lgamma_r, X = nexttoward, N = nan.
# A bound of 0 is EXACT, to the bit. "ld" bounds are the long double's.
#
# THE BOUNDS ARE THE DOCUMENTED ACCURACY (docs/libc-design.md, "What
# <math.h> measures"), set from a measurement and not above what was
# measured by more than the next round number. Raising one is a change
# to the documentation, not to the test.
MATH_BOUNDS = [
    # trig
    ("sin", "1", "trig", {"d": 1, "f": 1, "l": 1}),
    ("cos", "1", "trig", {"d": 1, "f": 1, "l": 1}),
    ("tan", "1", "trig", {"d": 1, "f": 1, "l": 2}),
    ("asin", "1", "unit", {"d": 1, "f": 1, "l": 2}),
    ("acos", "1", "unit", {"d": 1, "f": 1, "l": 1}),
    ("atan", "1", "gen", {"d": 1, "f": 1, "l": 1}),
    ("atan2", "2", "atan2", {"d": 1, "f": 1, "l": 1}),
    # hyperbolic
    ("sinh", "1", "hyp", {"d": 2, "f": 1, "l": 3}),
    ("cosh", "1", "hyp", {"d": 1, "f": 1, "l": 3}),
    ("tanh", "1", "hyp", {"d": 2, "f": 1, "l": 2}),
    ("asinh", "1", "gen", {"d": 1, "f": 1, "l": 1}),
    ("acosh", "1", "pos", {"d": 1, "f": 1, "l": 1}),
    ("atanh", "1", "unit", {"d": 1, "f": 1, "l": 2}),
    # exponential and logarithmic
    ("exp", "1", "exp", {"d": 1, "f": 1, "l": 1}),
    ("exp2", "1", "exp2", {"d": 1, "f": 1, "l": 1}),
    ("expm1", "1", "exp", {"d": 1, "f": 1, "l": 2}),
    ("log", "1", "pos", {"d": 1, "f": 1, "l": 1}),
    ("log2", "1", "pos", {"d": 1, "f": 1, "l": 1}),
    ("log10", "1", "pos", {"d": 1, "f": 1, "l": 1}),
    ("log1p", "1", "log1p", {"d": 1, "f": 1, "l": 2}),
    ("logb", "1", "gen", {"d": 0, "f": 0, "l": 0}),
    ("ilogb", "I", "gen", {"d": 0, "f": 0, "l": 0}),
    ("frexp", "E", "gen", {"d": 0, "f": 0, "l": 0}),
    ("ldexp", "i", "scale", {"d": 0, "f": 0, "l": 0}),
    ("scalbn", "i", "scale", {"d": 0, "f": 0, "l": 0}),
    ("scalbln", "L", "scale", {"d": 0, "f": 0, "l": 0}),
    ("modf", "M", "round", {"d": 0, "f": 0, "l": 0}),
    # power and absolute value
    ("fabs", "1", "gen", {"d": 0, "f": 0, "l": 0}),
    ("sqrt", "1", "pos", {"d": 0, "f": 0, "l": 0}),
    # 2 for double is glibc's error, not ours: at -0xb.e7210c039746p+518
    # the true root rounds to -0x1.cfec91806eed8p+173 (checked with an
    # integer cube root), which is ours; glibc's cbrt gives ...eed6.
    ("cbrt", "1", "gen", {"d": 2, "f": 1, "l": 1}),
    ("hypot", "2", "hypot", {"d": 1, "f": 1, "l": 1}),
    ("pow", "2", "pow", {"d": 1, "f": 1, "l": 2}),
    # error and gamma. The long double four are the DOUBLE function
    # widened (math_ld_special.c): 2^11 to 2^15 ulp of a long double is
    # double precision, and past double's range gamma's logs cost more.
    ("erf", "1", "erf", {"d": 1, "f": 1, "l": 2048}),
    ("erfc", "1", "erf", {"d": 2, "f": 1, "l": 4096}),
    ("lgamma", "1", "lgamma", {"d": 2, "f": 1, "l": 8192}),
    ("lgamma_r", "G", "lgamma", {"d": 2, "f": 1, "l": 8192}),
    ("tgamma", "1", "gamma", {"d": 6, "f": 1, "l": 32768}),
    # nearest integer
    ("ceil", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("floor", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("trunc", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("round", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("rint", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("nearbyint", "1", "round", {"d": 0, "f": 0, "l": 0}),
    ("lrint", "R", "roundi", {"d": 0, "f": 0, "l": 0}),
    ("llrint", "RR", "roundi", {"d": 0, "f": 0, "l": 0}),
    ("lround", "R", "roundi", {"d": 0, "f": 0, "l": 0}),
    ("llround", "RR", "roundi", {"d": 0, "f": 0, "l": 0}),
    # remainder
    ("fmod", "2", "rem", {"d": 0, "f": 0, "l": 0}),
    ("remainder", "2", "rem", {"d": 0, "f": 0, "l": 0}),
    ("remquo", "Q", "rem", {"d": 0, "f": 0, "l": 0}),
    # manipulation
    ("copysign", "2", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("nextafter", "2", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("nexttoward", "X", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("nan", "N", None, {"d": 0, "f": 0, "l": 0}),
    # max, min, difference, fma
    ("fdim", "2", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("fmax", "2", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("fmin", "2", "pairs", {"d": 0, "f": 0, "l": 0}),
    ("fma", "3", "fma", {"d": 0, "f": 0, "l": 0}),
]

# Functions checked under all four rounding modes, not only to nearest.
MATH_ALL_MODES = {"rint", "nearbyint", "lrint", "llrint", "fma", "sqrt"}

# Deliberate breaks for --positive-control math: (file, old text, new
# text). Each must turn its function RED and leave the rest green.
MATH_BREAKS = {
    # sin's last kernel coefficient off by 1e-14 (its fifth digit): x^13
    # times that is ~4 ulp at pi/4 and nothing near 0. (Off in the sixth
    # digit it stays under an ulp everywhere, and this harness rightly
    # said nothing -- the control has to be a break that matters.)
    "sin": ("userland/libs/libc/math_trig.c",
            "S6 =  1.58969099521155010221e-10", "S6 =  1.58979099521155010221e-10"),
    # fma rounding a tie AWAY from even: wrong by one ulp at exact ties only.
    "fma": ("userland/libs/libc/math_fma.c",
            "up = half && (rest || (mm & 1));", "up = half;"),
}


class MVal:
    """sign * m * 2^e exactly, or a special ('inf', 'nan')."""
    __slots__ = ("s", "m", "e", "special")

    def __init__(self, s=0, m=0, e=0, special=None):
        self.s, self.m, self.e, self.special = s, m, e, special

    def key(self):
        return (self.special, self.s, self.m, self.e) if self.special else (None, self.s, self.m, self.e)

    def lit(self, t):
        suf = {"d": "", "f": "f", "l": "L"}[t]
        if self.special == "inf":
            return "-INFINITY" if self.s else "INFINITY"
        if self.special == "nan":
            return "NAN"
        if self.m == 0:
            return ("-0.0" if self.s else "0.0") + suf
        return "%s0x%xp%d%s" % ("-" if self.s else "", self.m, self.e, suf)

    def as_fraction(self):
        return Fraction(self.m) * Fraction(2) ** self.e * (-1 if self.s else 1)


def mround(t, v):
    """A Fraction (or float) rounded to type t's precision -> MVal. Overflow
    becomes an infinity, underflow rounds into the subnormals."""
    p, emin, emax = MATH_TYPES[t]
    if isinstance(v, float):
        if v != v:
            return MVal(special="nan")
        if v in (float("inf"), float("-inf")):
            return MVal(s=int(v < 0), special="inf")
        s = int(v < 0 or (v == 0 and struct.pack(">d", v)[0] >> 7))
        v = Fraction(v)
    else:
        s = int(v < 0)
    v = abs(v)
    if v == 0:
        return MVal(s=s)
    e = v.numerator.bit_length() - v.denominator.bit_length()
    while Fraction(2) ** e > v:
        e -= 1
    while Fraction(2) ** (e + 1) <= v:
        e += 1
    lsb = max(e - p + 1, emin - p + 1)
    q = v / Fraction(2) ** lsb
    m = q.numerator // q.denominator
    r = q - m
    if r > Fraction(1, 2) or (r == Fraction(1, 2) and m & 1):
        m += 1
    if m and (m.bit_length() + lsb - 1) > emax:
        return MVal(s=s, special="inf")
    return MVal(s=s, m=m, e=lsb)


def m_edges(t):
    p, emin, emax = MATH_TYPES[t]
    lsb = emin - p + 1
    out = []
    for s in (0, 1):
        out += [MVal(s=s), MVal(s, 1, lsb), MVal(s, 2 ** (p - 1) - 1, lsb), MVal(s, 2 ** (p - 1), lsb),
                MVal(s, 2 ** p - 1, emax - p + 1), MVal(s, special="inf"),
                MVal(s, 1, 0), MVal(s, 1, -1), MVal(s, 1, 1), MVal(s, 3, -1),
                MVal(s, 2 ** p - 1, -p), MVal(s, 2 ** (p - 1) + 1, -(p - 1))]
    out.append(MVal(special="nan"))
    return out


def m_logu(rng, t, n, lo, hi, signed=True):
    p = MATH_TYPES[t][0]
    out = []
    for _ in range(n):
        m = rng.getrandbits(p - 1) | (1 << (p - 1))
        out.append(MVal(int(signed and rng.random() < 0.5), m, rng.randint(lo, hi) - (p - 1)))
    return out


def m_uniform(rng, t, n, a, b):
    p = MATH_TYPES[t][0]
    out = []
    for _ in range(n):
        # A random real with p + 8 bits of resolution, then rounded.
        v = Fraction(a) + (Fraction(b) - Fraction(a)) * Fraction(rng.getrandbits(p + 8), 2 ** (p + 8))
        out.append(mround(t, v))
    return out


def m_near(t, v, k=2):
    """v rounded, and its k neighbours either side."""
    c = mround(t, v)
    out = [c]
    if c.special or c.m == 0:
        return out
    for d in range(1, k + 1):
        out += [MVal(c.s, c.m + d, c.e), MVal(c.s, c.m - d, c.e)]
    return out


def m_pi():
    # pi to 400 bits by Machin, integers only (as tools/gen/gen_libm_tables.py).
    def atan_inv(n, bits):
        x = (1 << bits) // n
        s, k, sign = x, 1, -1
        while x:
            x //= n * n
            k += 2
            s += sign * (x // k)
            sign = -sign
        return s
    g = 464
    return Fraction(16 * atan_inv(5, g) - 4 * atan_inv(239, g), 1 << g)


def math_sets(t):
    """name -> list of MVal (or of tuples of MVal, for the multi-argument
    sets). Deterministic: one seeded generator per type."""
    p, emin, emax = MATH_TYPES[t]
    rng = random.Random({"d": 1, "f": 2, "l": 3}[t])
    pi = m_pi()
    ed = m_edges(t)
    big = {"d": 709.78, "f": 88.72, "l": 11356.52}[t]   # ln(max)
    gen = ed + m_logu(rng, t, 700, emin - p, emax) + m_uniform(rng, t, 300, -10, 10)
    trig = ed + m_uniform(rng, t, 800, -10, 10) + m_logu(rng, t, 300, -30, min(emax, 1100))
    for k in range(1, 120):
        trig += m_near(t, pi * k / 2, 2)
    trig += m_logu(rng, t, 200, 60, emax)
    if t == "d":
        trig.append(MVal(0, 6381956970095103, 797))   # the double nearest a multiple of pi/2
    unit = ed + m_uniform(rng, t, 1000, -1, 1) + m_uniform(rng, t, 100, -1.5, 1.5) + \
        [MVal(s, 2 ** p - k, -p) for s in (0, 1) for k in range(1, 20)] + m_logu(rng, t, 100, -60, -1)
    exp = ed + m_uniform(rng, t, 800, -big * 1.06, big * 1.01) + m_logu(rng, t, 200, -70, 0) + \
        m_uniform(rng, t, 200, -2, 2)
    for v in (big, -big, -big * 1.0498, Fraction(1, 2) * Fraction(69314718, 10 ** 8)):
        exp += m_near(t, Fraction(v), 3)
    e2 = {"d": 1075, "f": 150, "l": 16446}[t]
    exp2 = ed + m_uniform(rng, t, 800, -e2 - 2, emax + 2) + m_uniform(rng, t, 200, -2, 2) + \
        m_logu(rng, t, 100, -70, 0) + [mround(t, Fraction(k, 2)) for k in range(-40, 41)]
    pos = ed + m_logu(rng, t, 800, emin - p, emax, signed=False) + m_uniform(rng, t, 200, 0, 4) + \
        [MVal(0, 2 ** p - k, -p) for k in range(1, 16)] + \
        [MVal(0, 2 ** (p - 1) + k, -(p - 1)) for k in range(1, 16)] + \
        [mround(t, 1 + Fraction(1, 2 ** j)) for j in range(1, p + 1)] + \
        [mround(t, 1 - Fraction(1, 2 ** j)) for j in range(1, p + 1)]
    log1p = ed + m_uniform(rng, t, 800, -1, 1) + m_logu(rng, t, 400, emin - p, emax) + \
        [MVal(1, 2 ** p - k, -p) for k in range(1, 16)] + m_uniform(rng, t, 100, -1.5, -0.99)
    hyp = ed + m_uniform(rng, t, 600, -3, 3) + m_uniform(rng, t, 300, -big * 1.01, big * 1.01) + \
        m_logu(rng, t, 200, -70, 0) + m_near(t, Fraction(big), 3) + m_near(t, Fraction(big) + Fraction(1, 2), 3)
    rnd = list(ed)
    for k in range(-20, 21):
        for frac in (0, Fraction(1, 2), Fraction(1, 4), Fraction(3, 4)):
            rnd += m_near(t, Fraction(k) + frac, 2)
    for j in (p - 2, p - 1, p, p + 1):
        rnd += m_near(t, Fraction(2) ** j, 2) + m_near(t, -Fraction(2) ** j, 2)
        rnd += m_near(t, Fraction(2) ** j + Fraction(1, 2), 1)
    rnd += m_logu(rng, t, 300, -10, p + 4) + m_uniform(rng, t, 200, -100, 100)
    # lrint and friends: only where the integer fits a long (C leaves the
    # rest unspecified, and glibc and the hardware agree on LONG_MIN anyway).
    roundi = [v for v in rnd if not v.special and abs(v.as_fraction()) < 2 ** 62]
    erf = ed + m_uniform(rng, t, 800, -7, 7) + m_uniform(rng, t, 200, -30, 30) + \
        m_logu(rng, t, 150, emin - p, 0)
    if t == "l":
        erf += m_uniform(rng, t, 200, 20, 110)
    gmax = {"d": 172, "f": 36, "l": 1756}[t]
    gamma = ed + m_uniform(rng, t, 700, -gmax - 15, gmax) + m_logu(rng, t, 100, -80, 0) + \
        [mround(t, Fraction(k)) for k in range(-12, 40)] + \
        [mround(t, Fraction(k) + Fraction(1, 2)) for k in range(-40, 40)] + \
        [mround(t, Fraction(-k) + Fraction(s, 2 ** 20)) for k in range(1, 12) for s in (1, -1)]
    if t == "l":
        gamma += m_uniform(rng, t, 100, -1790, -184)
    # lgamma away from its zeros on the negative axis (math_special.c says why).
    lgamma = [v for v in ed if v.special or v.s == 0] + m_logu(rng, t, 500, emin - p, emax, signed=False) + \
        m_uniform(rng, t, 300, 0, 50) + m_uniform(rng, t, 200, -2, 0) + \
        [mround(t, -Fraction(k) - Fraction(q, 4)) for k in range(3, 60) for q in (1, 2, 3)] + \
        [mround(t, 1 + Fraction(s, 2 ** j)) for j in range(1, 30) for s in (1, -1)] + \
        [mround(t, 2 + Fraction(s, 2 ** j)) for j in range(1, 30) for s in (1, -1)]
    scale = [(v, n) for v in ed + m_logu(rng, t, 60, emin - p, emax) + m_uniform(rng, t, 20, -4, 4)
             for n in (0, 1, -1, 7, -7, 60, -60, p, -p, emax, -emax, emin - p, emax - emin + p,
                       2 * emax, -2 * emax, 2 ** 31 - 1, -2 ** 31)]
    sp = ed + [mround(t, Fraction(v)) for v in (3, -3, Fraction(1, 3), 10, -10, 100.5, -2.5, 1e6)]
    pairs = [(a, b) for a in sp for b in sp] + \
        list(zip(m_logu(rng, t, 400, emin - p, emax), m_logu(rng, t, 400, emin - p, emax))) + \
        list(zip(m_uniform(rng, t, 400, -10, 10), m_uniform(rng, t, 400, -10, 10)))
    atan2 = pairs + list(zip(m_logu(rng, t, 300, -40, 40), m_logu(rng, t, 300, -40, 40)))
    hyp2 = pairs + list(zip(m_logu(rng, t, 400, -40, 40), m_logu(rng, t, 400, -40, 40))) + \
        list(zip(m_logu(rng, t, 200, emax - 40, emax), m_logu(rng, t, 200, emax - 40, emax))) + \
        list(zip(m_logu(rng, t, 200, emin - p, emin + 40), m_logu(rng, t, 200, emin - p, emin + 40)))
    rem = pairs + list(zip(m_logu(rng, t, 500, -20, 200), m_logu(rng, t, 500, -20, 20))) + \
        [(mround(t, Fraction(k) * y.as_fraction() + Fraction(d, 64) * y.as_fraction()), y)
         for y in m_uniform(rng, t, 40, 0.1, 9) for k in (1, 2, 3, 7) for d in (-32, 0, 32)]
    # pow: x near 1 with large y, negative x with integer y, and the special
    # cross product.
    powp = [(a, b) for a in sp for b in sp] + \
        list(zip(m_logu(rng, t, 600, -40, 40, signed=False), m_uniform(rng, t, 600, -40, 40))) + \
        list(zip(m_uniform(rng, t, 200, 0.9, 1.1), m_uniform(rng, t, 200, -2000, 2000))) + \
        [(mround(t, Fraction(-k) - Fraction(1, 8)), mround(t, Fraction(n))) for k in range(0, 6)
         for n in range(-9, 10)] + \
        [(mround(t, Fraction(k)), mround(t, Fraction(n))) for k in range(2, 12) for n in range(-30, 40)] + \
        list(zip(m_logu(rng, t, 200, emin, emax, signed=False), m_uniform(rng, t, 200, -1.5, 1.5)))
    # fma: random triples, triples whose z cancels most of x*y, and EXACT
    # TIES -- x*y + z exactly halfway between two representable values.
    fma = [(a, b, c) for a in sp[:14] for b in sp[:14] for c in sp[:14]]
    xs, ys = m_logu(rng, t, 600, -30, 30), m_logu(rng, t, 600, -30, 30)
    zs = m_logu(rng, t, 600, -60, 60)
    fma += list(zip(xs, ys, zs))
    for a, b in zip(xs[:300], ys[:300]):
        prod = a.as_fraction() * b.as_fraction()
        near = mround(t, prod)
        if not near.special:
            neg = MVal(1 - near.s, near.m, near.e)
            fma.append((a, b, neg))
    for _ in range(300):
        # (1 + u)(1 + v) with u, v at half precision: the product has a
        # bit just below the last place, and z moves it onto a tie.
        h = p // 2 + 1
        u = rng.getrandbits(h - 1) | 1
        v = rng.getrandbits(h - 1) | 1
        a = MVal(0, (1 << h) + u, -h)
        b = MVal(0, (1 << h) + v, -h)
        prod = a.as_fraction() * b.as_fraction()
        lo = mround(t, prod)
        half = Fraction(1, 2 ** p)   # one half ulp of a value in [1, 2)
        target = lo.as_fraction() + half * rng.choice((1, -1))
        z = mround(t, target - prod)
        fma.append((a, b, z))
        fma.append((MVal(1, a.m, a.e), b, MVal(1 - z.s, z.m, z.e) if not z.special else z))
    return {"gen": gen, "trig": trig, "unit": unit, "exp": exp, "exp2": exp2, "pos": pos,
            "log1p": log1p, "hyp": hyp, "round": rnd, "roundi": roundi, "erf": erf, "gamma": gamma,
            "lgamma": lgamma, "scale": scale, "pairs": pairs, "atan2": atan2, "hypot": hyp2,
            "rem": rem, "pow": powp, "fma": fma}


def math_probe(work, only=None):
    """Write the probe that runs every MATH_BOUNDS entry; returns its path."""
    L = []
    L.append("#define _GNU_SOURCE")
    L.append("#include <math.h>\n#include <fenv.h>\n#include <stdio.h>\n#include <string.h>")
    L.append("#include <stdint.h>\n#include <stdlib.h>\n#include <limits.h>")
    L.append("typedef long double ld;")
    # The toy declarations, by hand: tolibc's <math.h> is not on this
    # translation unit's path, for the reason build() gives.
    for t in "dfl":
        T, s = MATH_CTYPE[t], MATH_SUFFIX[t]
        for b in MATH_BASE1:
            L.append(f"{T} toy_{b}{s}({T});")
        for b in MATH_BASE2:
            L.append(f"{T} toy_{b}{s}({T}, {T});")
        L.append(f"int toy_ilogb{s}({T}); {T} toy_ldexp{s}({T}, int); {T} toy_frexp{s}({T}, int *);")
        L.append(f"{T} toy_scalbn{s}({T}, int); {T} toy_scalbln{s}({T}, long); {T} toy_modf{s}({T}, {T} *);")
        L.append(f"long toy_lrint{s}({T}); long long toy_llrint{s}({T}); long toy_lround{s}({T});")
        L.append(f"long long toy_llround{s}({T}); {T} toy_remquo{s}({T}, {T}, int *);")
        L.append(f"{T} toy_nan{s}(const char *); {T} toy_nexttoward{s}({T}, ld); {T} toy_fma{s}({T}, {T}, {T});")
    L.append("double toy_lgamma_r(double, int *); float toy_lgammaf_r(float, int *);")
    L.append("ld toy_lgammal_r(ld, int *);")
    L.append(r'''
#define CAT 1e30
static int fails;
// The distance between two results in units of the last place: the
// difference of their bit patterns, which are monotonic within a sign.
// Anything that is not a difference of degree is CAT.
static double dist_d(double g, double w) {
    if (isnan(g) || isnan(w)) return isnan(g) && isnan(w) ? 0 : CAT;
    if (signbit(g) != signbit(w)) return CAT;
    uint64_t a, b; memcpy(&a, &g, 8); memcpy(&b, &w, 8);
    return a > b ? (double)(a - b) : (double)(b - a);
}
static double dist_f(float g, float w) {
    if (isnan(g) || isnan(w)) return isnan(g) && isnan(w) ? 0 : CAT;
    if (signbit(g) != signbit(w)) return CAT;
    uint32_t a, b; memcpy(&a, &g, 4); memcpy(&b, &w, 4);
    return a > b ? (double)(a - b) : (double)(b - a);
}
static unsigned __int128 key_l(ld v) {
    uint64_t m; uint16_t se; memcpy(&m, &v, 8); memcpy(&se, (char *)&v + 8, 2);
    unsigned e = se & 0x7fff;
    return ((unsigned __int128)e << 63) + (m & 0x7fffffffffffffffull);
}
static double dist_l(ld g, ld w) {
    if (isnan(g) || isnan(w)) return isnan(g) && isnan(w) ? 0 : CAT;
    if (signbit(g) != signbit(w)) return CAT;
    unsigned __int128 a = key_l(g), b = key_l(w);
    return a > b ? (double)(a - b) : (double)(b - a);
}
static const char *mode_name(int m) {
    return m == FE_TONEAREST ? "" : m == FE_DOWNWARD ? " [down]" : m == FE_UPWARD ? " [up]" : " [zero]";
}
struct worst { double d; long n; char in[160], got[64], want[64]; };
static void note(struct worst *w, double d, const char *in, const char *got, const char *want) {
    w->n++;
    if (d > w->d || !w->in[0]) {
        w->d = d;
        snprintf(w->in, sizeof w->in, "%s", in);
        snprintf(w->got, sizeof w->got, "%s", got);
        snprintf(w->want, sizeof w->want, "%s", want);
    }
}
static void report(const char *name, const char *type, int mode, struct worst *w, double bound) {
    int bad = w->d > bound;
    if (bad) fails++;
    printf("MATH\t%s%s\t%s\t%ld\t%.0f\t%.0f\t%s\t%s\t%s\t%s\n", name, mode_name(mode), type, w->n, w->d,
           bound, bad ? "FAIL" : "ok", w->in, w->got, w->want);
}
#define FD(v) (snprintf(fb[fbi ^= 1], 64, "%a", (double)(v)), fb[fbi])
#define FL(v) (snprintf(fb[fbi ^= 1], 64, "%La", (ld)(v)), fb[fbi])
static char fb[2][64]; static int fbi;
''')
    D = []        # the value tables, at file scope
    main_at = len(L)
    L.append("int main(void) {")
    L.append("    char in[160], g1[64], w1[64];")
    L.append("    (void)in; (void)g1; (void)w1;")
    names = None if only is None else set(only)
    for t in "dfl":
        sets = math_sets(t)
        T, s = MATH_CTYPE[t], MATH_SUFFIX[t]
        dist = {"d": "dist_d", "f": "dist_f", "l": "dist_l"}[t]
        fmt = "FL" if t == "l" else "FD"
        used = {}

        def arr(setname, k=None):
            key = (setname, k)
            if key not in used:
                vals = sets[setname]
                vname = f"S_{t}_{setname}" + ("" if k is None else f"_{k}")
                if k is None:
                    items = [v.lit(t) for v in vals]
                    ctype = T
                else:
                    items = [str(v[k]) if isinstance(v[k], int) else v[k].lit(t) for v in vals]
                    ctype = "long" if setname == "scale" and k == 1 else T
                D.append(f"static const {ctype} {vname}[] = {{")
                for i in range(0, len(items), 6):
                    D.append("    " + ", ".join(items[i:i + 6]) + ",")
                D.append("};")
                used[key] = (vname, len(items))
            return used[key]

        for name, sig, setname, bounds in MATH_BOUNDS:
            if names is not None and name not in names:
                continue
            fn = (name[:-2] + s + "_r") if name == "lgamma_r" else name + s
            bound = bounds[t]
            modes = ["FE_TONEAREST", "FE_DOWNWARD", "FE_UPWARD", "FE_TOWARDZERO"] \
                if name in MATH_ALL_MODES else ["FE_TONEAREST"]
            if sig == "N":
                L.append("    { struct worst w = {0};")
                for arg in ('""', '"0"', '"1"', '"123"', '"0x12"', '"0x7ffff"', '"abc"', '"12z"',
                            '"0xffffffffffffffff"', '"077"'):
                    if t == "d":
                        L.append(f'      {{ double g = toy_nan({arg}), r = nan({arg}); uint64_t a, b;'
                                 f' memcpy(&a, &g, 8); memcpy(&b, &r, 8);'
                                 f' note(&w, a == b ? 0 : CAT, "nan(" {arg} ")", FD(g), FD(r)); }}')
                    elif t == "f":
                        L.append(f'      {{ float g = toy_nanf({arg}), r = nanf({arg}); uint32_t a, b;'
                                 f' memcpy(&a, &g, 4); memcpy(&b, &r, 4);'
                                 f' note(&w, a == b ? 0 : CAT, "nanf(" {arg} ")", FD(g), FD(r)); }}')
                    else:
                        L.append(f'      {{ ld g = toy_nanl({arg}), r = nanl({arg});'
                                 f' note(&w, key_l(g) == key_l(r) ? 0 : CAT, "nanl(" {arg} ")", FL(g), FL(r)); }}')
                L.append(f'      report("{fn}", "{t}", FE_TONEAREST, &w, {bound}); }}')
                continue
            for mode in modes:
                L.append(f"    {{ struct worst w = {{0}}; fesetround({mode});")
                if sig in ("1", "I", "R", "RR", "E", "M", "G"):
                    a, n = arr(setname)
                    L.append(f"      for (int i = 0; i < {n}; i++) {{ {T} x = {a}[i];")
                    L.append(f'        snprintf(in, sizeof in, "%La", (ld)x);')
                    if sig == "1":
                        L.append(f"        {T} g = toy_{fn}(x), wv = {fn}(x);")
                        L.append(f"        note(&w, {dist}(g, wv), in, {fmt}(g), {fmt}(wv)); }}")
                    elif sig == "I":
                        L.append(f"        int g = toy_{fn}(x), wv = {fn}(x);")
                        L.append(f'        snprintf(g1, 64, "%d", g); snprintf(w1, 64, "%d", wv);')
                        L.append("        note(&w, g == wv ? 0 : CAT, in, g1, w1); }")
                    elif sig in ("R", "RR"):
                        it = "long" if sig == "R" else "long long"
                        L.append(f"        {it} g = toy_{fn}(x), wv = {fn}(x);")
                        L.append(f'        snprintf(g1, 64, "%lld", (long long)g); snprintf(w1, 64, "%lld", (long long)wv);')
                        L.append("        note(&w, g == wv ? 0 : CAT, in, g1, w1); }")
                    elif sig == "E":
                        L.append(f"        int eg = 0, ew = 0; {T} g = toy_{fn}(x, &eg), wv = {fn}(x, &ew);")
                        L.append(f'        snprintf(g1, 64, "%La e%d", (ld)g, eg); snprintf(w1, 64, "%La e%d", (ld)wv, ew);')
                        L.append(f"        note(&w, (isfinite(x) && eg != ew) ? CAT : {dist}(g, wv), in, g1, w1); }}")
                    elif sig == "M":
                        L.append(f"        {T} ig = 0, iw = 0; {T} g = toy_{fn}(x, &ig), wv = {fn}(x, &iw);")
                        L.append(f'        snprintf(g1, 64, "%La + %La", (ld)ig, (ld)g); snprintf(w1, 64, "%La + %La", (ld)iw, (ld)wv);')
                        L.append(f"        double d = {dist}(g, wv), d2 = {dist}(ig, iw);")
                        L.append("        note(&w, d > d2 ? d : d2, in, g1, w1); }")
                    elif sig == "G":
                        L.append(f"        int sg = 0, sw = 0; {T} g = toy_{fn}(x, &sg), wv = {fn}(x, &sw);")
                        L.append(f'        snprintf(g1, 64, "%La s%d", (ld)g, sg); snprintf(w1, 64, "%La s%d", (ld)wv, sw);')
                        L.append(f"        note(&w, (isfinite(wv) && sg != sw) ? CAT : {dist}(g, wv), in, g1, w1); }}")
                elif sig in ("2", "Q", "X"):
                    a, n = arr(setname, 0)
                    b, _ = arr(setname, 1)
                    L.append(f"      for (int i = 0; i < {n}; i++) {{ {T} x = {a}[i], y = {b}[i];")
                    L.append(f'        snprintf(in, sizeof in, "%La, %La", (ld)x, (ld)y);')
                    if sig == "2":
                        L.append(f"        {T} g = toy_{fn}(x, y), wv = {fn}(x, y);")
                        L.append(f"        note(&w, {dist}(g, wv), in, {fmt}(g), {fmt}(wv)); }}")
                    elif sig == "X":
                        L.append(f"        {T} g = toy_{fn}(x, (ld)y), wv = {fn}(x, (ld)y);")
                        L.append(f"        note(&w, {dist}(g, wv), in, {fmt}(g), {fmt}(wv)); }}")
                    else:
                        L.append(f"        int qg = 0, qw = 0; {T} g = toy_{fn}(x, y, &qg), wv = {fn}(x, y, &qw);")
                        L.append(f'        snprintf(g1, 64, "%La q%d", (ld)g, qg); snprintf(w1, 64, "%La q%d", (ld)wv, qw);')
                        # Only the low three bits and the sign are specified.
                        L.append("        int ag = abs(qg) & 7, aw = abs(qw) & 7;")
                        L.append("        int qbad = !isnan(wv) && (ag != aw || (ag && (qg < 0) != (qw < 0)));")
                        L.append(f"        note(&w, qbad ? CAT : {dist}(g, wv), in, g1, w1); }}")
                elif sig == "3":
                    a, n = arr(setname, 0)
                    b, _ = arr(setname, 1)
                    c, _ = arr(setname, 2)
                    L.append(f"      for (int i = 0; i < {n}; i++) {{ {T} x = {a}[i], y = {b}[i], z = {c}[i];")
                    L.append(f'        snprintf(in, sizeof in, "%La, %La, %La", (ld)x, (ld)y, (ld)z);')
                    L.append(f"        {T} g = toy_{fn}(x, y, z), wv = {fn}(x, y, z);")
                    L.append(f"        note(&w, {dist}(g, wv), in, {fmt}(g), {fmt}(wv)); }}")
                elif sig in ("i", "L"):
                    a, n = arr(setname, 0)
                    b, _ = arr(setname, 1)
                    it = "int" if sig == "i" else "long"
                    L.append(f"      for (int i = 0; i < {n}; i++) {{ {T} x = {a}[i]; {it} k = ({it}){b}[i];")
                    L.append(f'        snprintf(in, sizeof in, "%La, %ld", (ld)x, (long)k);')
                    L.append(f"        {T} g = toy_{fn}(x, k), wv = {fn}(x, k);")
                    L.append(f"        note(&w, {dist}(g, wv), in, {fmt}(g), {fmt}(wv)); }}")
                L.append(f'      fesetround(FE_TONEAREST); report("{fn}", "{t}", {mode}, &w, {bound}); }}')
    # nearbyint must not raise inexact, rint must.
    L.append(r'''
    {
        struct worst w = {0};
        volatile double h = 2.5;
        feclearexcept(FE_ALL_EXCEPT); (void)toy_nearbyint(h);
        int nb = fetestexcept(FE_INEXACT);
        feclearexcept(FE_ALL_EXCEPT); (void)toy_rint(h);
        int ri = fetestexcept(FE_INEXACT);
        feclearexcept(FE_ALL_EXCEPT); (void)toy_nearbyintl((ld)h);
        int nbl = fetestexcept(FE_INEXACT);
        note(&w, (nb || !ri || nbl) ? CAT : 0, "2.5", nb ? "nearbyint raised inexact" : "-",
             !ri ? "rint did not raise inexact" : "-");
        report("nearbyint-flags", "-", FE_TONEAREST, &w, 0);
    }''')
    L.append('    printf("MATHSUMMARY\\t%d\\n", fails);')
    L.append("    return 0;\n}")
    L[main_at:main_at] = D
    path = os.path.join(work, "math_probe.c")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")
    return path


def math_build(work, break_name=None):
    """Compile tolibc's maths with every public name renamed, and the
    probe beside it against glibc's libm. Returns the binary."""
    renames = "\n".join(f"#define {s} toy_{s}" for s in MATH_SYMS)
    with open(os.path.join(work, "toy_math_rename.h"), "w") as f:
        f.write("// Generated by tools/tests/sweep/libc_diff.py: tolibc's maths renamed.\n" + renames + "\n")
    src_dir = os.path.join(REPO, "userland/libs/libc")
    sources = sorted(f for f in os.listdir(src_dir) if f.startswith("math") and f.endswith(".c"))
    objs = []
    for name in sources:
        path = os.path.join(src_dir, name)
        if break_name:
            bfile, old, new = MATH_BREAKS[break_name]
            if os.path.join(REPO, bfile) == path:
                text = open(path).read()
                if text.count(old) != 1:
                    raise RuntimeError(f"the {break_name} break no longer matches {bfile}: {old!r}")
                path = os.path.join(work, "broken_" + name)
                with open(path, "w") as f:
                    f.write(text.replace(old, new))
        obj = os.path.join(work, name + ".o")
        # The tree's numerics (-O2, no FMA contraction on this ISA) but
        # HOSTED, so <math.h>'s freestanding macros stay out of the way of
        # the renames.
        r = subprocess.run(["gcc", "-w", "-O2", "-fno-builtin", "-c", "-o", obj,
                            "-include", os.path.join(work, "toy_math_rename.h"),
                            "-I" + src_dir, "-I" + os.path.join(src_dir, "include"),
                            "-I" + os.path.join(REPO, "kernel/include/api"),
                            "-I" + os.path.join(REPO, "kernel/include/abi"),
                            "-I" + os.path.join(REPO, "build/gen"), path],
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"{name} did not compile:\n" + r.stderr[:1500])
        objs.append(obj)
    exe = os.path.join(work, "math_probe")
    r = subprocess.run(["gcc", "-w", "-O0", "-o", exe, os.path.join(work, "math_probe.c"), *objs, "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("the math probe did not build:\n" + r.stderr[:2000])
    return exe


def run_math(work, args):
    only = args.math_only.split(",") if args.math_only else None
    math_probe(work, only)
    try:
        exe = math_build(work, args.break_math)
    except RuntimeError as e:
        print("libc_diff: HARNESS FAILURE (math)")
        print("  " + str(e)[:1500].replace("\n", "\n  "))
        return 2, []
    r = subprocess.run([exe], capture_output=True, text=True)
    rows = [l.split("\t")[1:] for l in r.stdout.splitlines() if l.startswith("MATH\t")]
    summ = [l for l in r.stdout.splitlines() if l.startswith("MATHSUMMARY\t")]
    if not summ:
        print("libc_diff: the math probe did not finish:\n" + (r.stdout + r.stderr)[-1500:])
        return 2, rows
    print(f"\n{'function':<24}{'type':<5}{'cases':>7}{'worst':>10}{'bound':>10}  worst input")
    for name, t, n, worst, bound, verdict, inp, got, want in rows:
        shown = "CAT" if float(worst) >= 1e29 else worst
        flag = "" if verdict == "ok" else "   <-- FAIL"
        if verdict != "ok" or args.verbose_math or float(worst) > 0:
            print(f"{name:<24}{t:<5}{n:>7}{shown:>10}{bound:>10}  {inp[:60]}{flag}")
            if verdict != "ok":
                print(f"{'':<24}     saunaos={got}  glibc={want}")
    failed = [row for row in rows if row[5] != "ok"]
    print(f"\nlibc_diff: {len(rows)} math checks, {sum(int(r[2]) for r in rows)} cases, "
          f"{len(failed)} over their bound (every exact one at 0 ULPs is not listed)")
    return (1 if failed else 0), rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--only", choices=("printf", "parsers", "math"),
                    help="run one part")
    ap.add_argument("--math-only", metavar="FUNCS",
                    help="comma-separated math functions to check (base names: sin,fma)")
    ap.add_argument("--verbose-math", action="store_true",
                    help="list every math check, the exact ones at 0 ULPs too")
    ap.add_argument("--break", dest="break_math", choices=sorted(MATH_BREAKS),
                    help="POSITIVE CONTROL for the maths: build with this function "
                         "deliberately broken, and require it to be reported")
    ap.add_argument("--max-diffs", type=int, default=40,
                    help="stop listing after N differences (default 40)")
    ap.add_argument("--exact-float", action="store_true",
                    help="require strtod to match glibc BIT FOR BIT rather "
                         "than within one ULP -- shows the known rounding gap")
    ap.add_argument("--positive-control", action="store_true",
                    help="compare glibc against ITSELF with one case "
                         "deliberately corrupted, and require a difference")
    args = ap.parse_args()

    if not shutil.which("gcc"):
        print("libc_diff: no gcc on PATH")
        return 2

    work = tempfile.mkdtemp(prefix="libc_diff.")
    try:
        if args.only == "math" or args.break_math:
            status, rows = run_math(work, args)
            if args.break_math:
                hit = [r for r in rows if r[0].split(" ")[0] in (args.break_math, args.break_math + "f",
                                                                  args.break_math + "l") and r[5] != "ok"]
                if not hit:
                    print(f"positive control: FAILED -- {args.break_math} was broken and "
                          "nothing reported it")
                    return 1
                print(f"positive control: ok -- the broken {args.break_math} was reported "
                      f"({len(hit)} check(s) over their bound)")
                return 0
            return status
        math_status = 0
        if args.only is None:
            math_status, _ = run_math(work, args)
        sections = []
        if args.only != "parsers":
            sections += printf_sections()
        if args.only != "printf":
            sections += parser_sections()
        if args.positive_control:
            # tolibc's OWN snprintf compared against itself must agree;
            # a deliberate mismatch beside it must not. If this passes,
            # the harness is not comparing anything.
            sections.append('    cmp("positive control", "a", "b");')
        gen_probe(work, sections, args.exact_float)
        try:
            binary = build(work)
        except RuntimeError as e:
            print("libc_diff: HARNESS FAILURE")
            print("  " + str(e)[:1500].replace("\n", "\n  "))
            return 2

        r = subprocess.run([binary], capture_output=True, text=True)
        diffs = [l for l in r.stdout.splitlines() if l.startswith("DIFF\t")]
        summary = [l for l in r.stdout.splitlines() if l.startswith("SUMMARY\t")]
        checks, fails = (summary[0].split("\t")[1:] if summary else ("?", "?"))

        shown = diffs[:args.max_diffs]
        for line in shown:
            _, what, got, want = line.split("\t", 3)
            print(f"  {what}\n      {got}\n      {want}")
        if len(diffs) > len(shown):
            print(f"  ... and {len(diffs) - len(shown)} more "
                  f"(raise --max-diffs to see them)")

        print(f"\nlibc_diff: {checks} cases, {fails} differ from glibc")
        if args.positive_control:
            if not diffs:
                print("positive control: FAILED -- a deliberate mismatch "
                      "was not reported. This harness compares nothing.")
                return 1
            print("positive control: ok -- the deliberate mismatch was reported")
        return 1 if int(fails) or math_status else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
