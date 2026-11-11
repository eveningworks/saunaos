#!/usr/bin/env python3
"""tolibc's printf, sscanf, strtod and strtold against glibc's, on the HOST.

`kernel/lib/kfmt.c` (the formatter, shared with the kernel),
`userland/libs/libc/printf_float.c` (its floating-point half),
`userland/libs/libc/scanf.c` and `userland/libs/libc/strtod.c` are
compiled with the host gcc beside a driver that makes the SAME call
through glibc and compares the bytes, the return value, the `%n` count,
the stored value's bits, `endptr` and `errno`.

WHY A HOST CHECK. The bugs this guards live in the last digit of
correctly-rounded decimal conversion -- ties broken on the exact binary
value, subnormals, carries into a new leading digit -- and a guest test
can only carry the handful of vectors somebody thought of. glibc is the
oracle a ported program is written against, and it shares no code with
what it judges. Two halves:

  (a) a fixed table: every flag/width/precision combination of
      f F e E g G a A over edge values (ties, subnormals, DBL_MAX, -0.0,
      inf, nan), the same with `L` over long doubles, the integer
      length modifiers and `%n`, strtod/strtof edge strings (overflow
      boundaries, the subnormal threshold, hex floats, inf/nan
      spellings), and sscanf scansets;
  (b) a seeded random sample: doubles and long doubles by random bits
      and as small-mantissa values (where decimal ties live), with random
      flags/width/precision/conversion; and strtod/strtof on printed
      values, random digit strings, and the EXACT MIDPOINTS between
      adjacent doubles and floats (the strings a correctly rounded
      parser has to break as ties), each also nudged up and down.

  python3 tools/hostchecks/libc_fmt_hostcheck.py
  python3 tools/hostchecks/libc_fmt_hostcheck.py --random 1000000 --seed 7
  python3 tools/hostchecks/libc_fmt_hostcheck.py --positive-control

OUT OF SCOPE: libm (sin, hypot -- their own last-ulp differences), the
current rounding mode (only round-to-nearest is compared), wide
characters, and glibc's locale-dependent `'` flag.

Exit status is non-zero if anything disagrees.
"""

# printf/sscanf/strtod against glibc. gcc and glibc only.
RUNNER = {
    "covers": "tolibc's printf, sscanf and strtod against glibc, table + random",
    "parallel": True,
    "needs": ["host_cc", "needs gcc on PATH"],
}

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402
ROOT = toolpath.REPO

# What is under test is renamed at compile time, so the driver's own
# calls reach glibc and only the toy_ names reach ours.
RENAME = ["sscanf", "vsscanf", "fscanf", "vfscanf", "scanf", "vscanf",
          "strtod", "strtof", "strtold", "atof"]

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <float.h>
#include <math.h>

size_t k_snprintf(char *out, size_t cap, const char *fmt, ...);
int toy_sscanf(const char *s, const char *fmt, ...);
double toy_strtod(const char *, char **);
float toy_strtof(const char *, char **);
long double toy_strtold(const char *, char **);
int strfromf128(char *, size_t, const char *, _Float128);

#define OUT 20000
static char A[OUT], B[OUT];
#define NCAT 9
static long n_cmp[NCAT], n_bad[NCAT];
enum { C_INT, C_DBL, C_LDBL, C_STRTOD, C_STRTOF, C_SCANF, C_RDBL, C_RLDBL, C_STRTOLD };
static const char *CAT[] = { "printf-int", "printf-double", "printf-ldouble",
                             "strtod", "strtof", "sscanf",
                             "printf-double-random", "printf-ldouble-random", "strtold" };
static int verbose_cap = 12;
static int shown[NCAT];

static void report(int cat, const char *what) {
    n_bad[cat]++;
    if (shown[cat]++ < verbose_cap) printf("BAD %s %s\n", CAT[cat], what);
}

static char line[2 * OUT + 512];

/* glibc's one known wrong answer here: `%#g` whose rounding carries
   into a new power of ten, printed in the exponent form, loses the
   trailing zeros `#` exists to keep -- `%#.2g` of 99.5 is "1.e+02" where
   C says "1.0e+02". Ours keeps C's; this recognises exactly
   that shape and counts it apart. */
static long n_quirk;
static void squash(const char *s, char *d) {
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') *d++ = *s++;
    while (*s == '0' && s[1] != '.') s++;    /* the '0' flag's padding */
    for (; *s; s++) {
        if (*s == ' ') continue;
        if (*s == '.') {
            const char *q = s + 1;
            while (*q == '0') q++;
            if (*q == 'e' || *q == 'E') { *d++ = '.'; s = q - 1; continue; }
        }
        *d++ = *s;
    }
    *d = 0;
}
static int glibc_hash_g(const char *fmt) {
    size_t n = strlen(fmt);
    if (!strchr(fmt, '#') || (fmt[n - 1] != 'g' && fmt[n - 1] != 'G')) return 0;
    static char a[OUT], b[OUT];
    squash(A, a); squash(B, b);
    return !strcmp(a, b) && strcmp(A, B);
}

static void cmp_out(int cat, const char *fmt, const char *val, size_t na, int nb) {
    n_cmp[cat]++;
    if (((long)na != nb || strcmp(A, B)) && glibc_hash_g(fmt)) { n_quirk++; return; }
    if ((long)na != nb || strcmp(A, B)) {
        snprintf(line, sizeof line, "fmt=\"%s\" v=%s ours=\"%s\"(%zu) glibc=\"%s\"(%d)",
                 fmt, val, A, na, B, nb);
        report(cat, line);
    }
}

/* --- the integer side ------------------------------------------------ */
#define CHK(fmt, ...) do { \
    size_t na = k_snprintf(A, OUT, fmt, __VA_ARGS__); \
    int nb = snprintf(B, OUT, fmt, __VA_ARGS__); \
    cmp_out(C_INT, fmt, #__VA_ARGS__, na, nb); } while (0)
#define CHKN(T, fmt, ...) do { \
    T n1 = (T)-77, n2 = (T)-77; \
    size_t na = k_snprintf(A, OUT, fmt, __VA_ARGS__, &n1); \
    int nb = snprintf(B, OUT, fmt, __VA_ARGS__, &n2); \
    cmp_out(C_INT, fmt, #__VA_ARGS__, na, nb); \
    n_cmp[C_INT]++; \
    if (n1 != n2) { snprintf(line, sizeof line, "fmt=\"%s\" %%n stored %lld, glibc %lld", \
                             fmt, (long long)n1, (long long)n2); report(C_INT, line); } \
    } while (0)

static void ints(void) {
    CHK("[%td]", (ptrdiff_t)-9);
    CHK("[%ti] [%tu] [%tx] [%to]", (ptrdiff_t)-123456789012LL, (ptrdiff_t)77,
        (ptrdiff_t)255, (ptrdiff_t)8);
    CHK("[%jd] [%ju] [%jx] [%jX]", (intmax_t)INT64_MIN, (uintmax_t)UINT64_MAX,
        (uintmax_t)0xfeedfacecafeULL, (uintmax_t)0xabcULL);
    CHK("[%+12jd] [%-12td|] [%012jd] [%.15td]", (intmax_t)42, (ptrdiff_t)-5,
        (intmax_t)-42, (ptrdiff_t)7);
    CHK("[%zd] [%zu] [%lld] [%llu] [%hhd] [%hd] [%hhu]", (ssize_t)-3, (size_t)3,
        -1LL, 18446744073709551615ULL, 300, 70000, 511);
    CHK("[%td/%d] [%jd/%d]", (ptrdiff_t)-1, 71, (intmax_t)2, 72);
    CHK("[%Lf/%d]", 1.5L, 73);
    CHKN(int, "abc%s%n", "");
    CHKN(int, "%d%n", 12345);
    CHKN(int, "%-10s|%n", "x");
    CHKN(signed char, "%300d%hhn", 1);
    CHKN(short, "%70000d%hn", 1);
    CHKN(long, "%s%ln", "hello");
    CHKN(long long, "%s%lln", "hello!");
    CHKN(intmax_t, "%5d%jn", 1);
    CHKN(ptrdiff_t, "%5d%tn", 1);
    CHKN(size_t, "%5d%zn", 1);
    CHKN(int, "%.3f%n", 2.5);
    { /* %n counts what WOULD have been written, past a truncation */
        int n1 = -1, n2 = -1;
        char a[4], b[4];
        size_t na = k_snprintf(a, sizeof a, "abcdef%n", &n1);
        int nb = snprintf(b, sizeof b, "abcdef%n", &n2);
        n_cmp[C_INT]++;
        if ((long)na != nb || n1 != n2 || strcmp(a, b)) {
            snprintf(line, sizeof line, "truncated %%n: ours %zu/%d/\"%s\" glibc %d/%d/\"%s\"",
                     na, n1, a, nb, n2, b);
            report(C_INT, line);
        }
    }
    /* `n` takes its own argument: what follows must still line up */
    { int n1 = 0, n2 = 0;
      size_t na = k_snprintf(A, OUT, "ab%ncd%d", &n1, 99);
      int nb = snprintf(B, OUT, "ab%ncd%d", &n2, 99);
      cmp_out(C_INT, "ab%ncd%d", "&n,99", na, nb); }
}

/* --- the float side --------------------------------------------------- */
static const char *FLAGS = "-+ #0";
static const char *WIDTHS[] = { "", "1", "12", "40" };
static const char *PRECS[] = { "", ".0", ".1", ".2", ".3", ".6", ".10", ".17", ".25", ".60" };
static const char CONVS[] = "fFeEgGaA";

static void build_fmt(char *f, unsigned flags, const char *w, const char *p, const char *len, char c) {
    char *q = f;
    *q++ = '%';
    for (int i = 0; i < 5; i++) if (flags & (1u << i)) *q++ = FLAGS[i];
    q += sprintf(q, "%s%s%s%c", w, p, len, c);
}

static void one_d(int cat, const char *f, double v) {
    size_t na = k_snprintf(A, OUT, f, v);
    int nb = snprintf(B, OUT, f, v);
    char val[64];
    uint64_t bits; memcpy(&bits, &v, 8);
    snprintf(val, sizeof val, "%a(0x%016llx)", v, (unsigned long long)bits);
    cmp_out(cat, f, val, na, nb);
}

static void one_ld(int cat, const char *f, long double v) {
    size_t na = k_snprintf(A, OUT, f, v);
    int nb = snprintf(B, OUT, f, v);
    char val[80];
    unsigned char by[10]; memcpy(by, &v, 10);
    int k = snprintf(val, sizeof val, "%La(0x", v);
    for (int i = 9; i >= 0; i--) k += snprintf(val + k, sizeof val - k, "%02x", by[i]);
    snprintf(val + k, sizeof val - k, ")");
    cmp_out(cat, f, val, na, nb);
}

static double dvals[128];
static int ndv;
static long double ldvals[128];
static int nldv;

static double from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static long double ld_from(uint64_t mant, unsigned se) {
    long double v = 0; unsigned char by[16] = {0};
    memcpy(by, &mant, 8); by[8] = (unsigned char)se; by[9] = (unsigned char)(se >> 8);
    memcpy(&v, by, 10);
    return v;
}

static void values(void) {
    double d[] = {
        0.0, -0.0, 1.0, -1.0, 0.1, 0.5, 1.5, 2.5, 3.5, -2.5, 0.125, 0.375, 0.05,
        1e-5, 9.5, 99.5, 0.0001, 123456.789, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23,
        DBL_MAX, -DBL_MAX, DBL_MIN, DBL_MIN / 2, 4.9406564584124654e-324,
        DBL_EPSILON, 3.141592653589793, 2.718281828459045, 1.5849625007211561,
        INFINITY, -INFINITY, NAN, -NAN, 9.999999, 0.9999995, 999999.5, 1.0 / 3,
        2.0 / 3, 1.5e-323, 1e300, 1e-300, 0x1.fffffffffffffp-1, 0.3, 1e100,
        123.456, 65536.0, 4294967296.0, 9007199254740993.0, 0.000123456, 2.5e-5,
        5e-5, 0.015625, 1024.5, 0x1p-1022, 0x1.8p+0, 0x1.f8p+0, 0x1.08p+0,
        0x1.0000000000008p+0, 0x1.0000000000018p+0, 0x1.fffffffffffff8p+0 - 0,
        9.5367431640625e-07, 1e-310, 7.0, 15.0, 0.95, 0.995, 99.995,
    };
    ndv = (int)(sizeof d / sizeof d[0]);
    memcpy(dvals, d, sizeof d);
    long double l[] = {
        0.0L, -0.0L, 1.0L, -1.0L, 0.1L, 0.5L, 2.5L, 3.5L, 1.5L, 0.125L, 1e-5L,
        LDBL_MAX, -LDBL_MAX, LDBL_MIN, LDBL_MIN / 2, LDBL_TRUE_MIN, LDBL_EPSILON,
        3.14159265358979323846L, 2.71828182845904523536L, 1e4000L, 1e-4000L,
        1e300L, (long double)DBL_MAX, (long double)DBL_MIN, 1.0L / 3,
        123456.789L, 999999.5L, 9.5L, 0.9999995L,
        (long double)INFINITY, -(long double)INFINITY, (long double)NAN,
        0x1.fffffffffffffffep0L, 0x1.0000000000000001p0L, 0x8p-3L,
    };
    nldv = (int)(sizeof l / sizeof l[0]);
    memcpy(ldvals, l, sizeof l);
    ldvals[nldv++] = ld_from(0x0000000000000001ULL, 0);          /* smallest subnormal */
    ldvals[nldv++] = ld_from(0x7fffffffffffffffULL, 0);          /* largest subnormal */
    ldvals[nldv++] = ld_from(0x4000000000000000ULL, 0);          /* a mid subnormal */
}

static void floats_table(void) {
    char f[64];
    for (unsigned fl = 0; fl < 32; fl++)
        for (int w = 0; w < 4; w++)
            for (int p = 0; p < 10; p++)
                for (int c = 0; CONVS[c]; c++) {
                    build_fmt(f, fl, WIDTHS[w], PRECS[p], "", CONVS[c]);
                    for (int i = 0; i < ndv; i++) one_d(C_DBL, f, dvals[i]);
                    build_fmt(f, fl, WIDTHS[w], PRECS[p], "L", CONVS[c]);
                    for (int i = 0; i < nldv; i++) one_ld(C_LDBL, f, ldvals[i]);
                }
    /* every digit there is: the exact expansions, and far past them */
    static const char *BIG[] = { "%.1100f", "%.800e", "%.770g", "%#.770g", "%.400a",
                                 "%.340f", "%.20a", "%.0a", "%.1a", "%.13a", "%.14a" };
    for (unsigned k = 0; k < sizeof BIG / sizeof BIG[0]; k++)
        for (int i = 0; i < ndv; i++) one_d(C_DBL, BIG[k], dvals[i]);
    static const char *LBIG[] = { "%.5000Lf", "%.5000Le", "%.4000Lg", "%.18La", "%.0La",
                                  "%.15La", "%.16La", "%Lf" };
    for (unsigned k = 0; k < sizeof LBIG / sizeof LBIG[0]; k++)
        for (int i = 0; i < nldv; i++) {
            /* %.5000Lf of LDBL_TRUE_MIN is all zeros; fine, it still has to match */
            one_ld(C_LDBL, LBIG[k], ldvals[i]);
        }
    /* the brief's own lines */
    one_d(C_DBL, "%.17g", 2.718281828459045);
    one_d(C_DBL, "%.17g", 0.1);
    one_d(C_DBL, "%.0f", 2.5);
    one_d(C_DBL, "%.0f", 3.5);
    one_d(C_DBL, "%.3a", 3.14159);
    one_d(C_DBL, "%A", -0.5);
    one_d(C_DBL, "%a", 1.0);
}

/* --- strtod / strtof ------------------------------------------------ */
static void strtod_one(const char *s) {
    char *e1, *e2;
    errno = 0; double a = toy_strtod(s, &e1); int er1 = errno;
    errno = 0; double b = strtod(s, &e2); int er2 = errno;
    n_cmp[C_STRTOD]++;
    if (memcmp(&a, &b, 8) || e1 != e2 || er1 != er2) {
        snprintf(line, sizeof line, "\"%.200s\" ours %.17g (%a) end+%td errno %d, glibc %.17g (%a) end+%td errno %d",
                 s, a, a, e1 - s, er1, b, b, e2 - s, er2);
        report(C_STRTOD, line);
    }
}
static void strtof_one(const char *s) {
    char *e1, *e2;
    errno = 0; float a = toy_strtof(s, &e1); int er1 = errno;
    errno = 0; float b = strtof(s, &e2); int er2 = errno;
    n_cmp[C_STRTOF]++;
    if (memcmp(&a, &b, 4) || e1 != e2 || er1 != er2) {
        snprintf(line, sizeof line, "\"%.200s\" ours %.9g (%a) end+%td errno %d, glibc %.9g (%a) end+%td errno %d",
                 s, a, a, e1 - s, er1, b, b, e2 - s, er2);
        report(C_STRTOF, line);
    }
}
static void strtold_one(const char *s) {
    char *e1, *e2;
    errno = 0; long double a = toy_strtold(s, &e1); int er1 = errno;
    errno = 0; long double b = strtold(s, &e2); int er2 = errno;
    n_cmp[C_STRTOLD]++;
    if (memcmp(&a, &b, 10) || e1 != e2 || er1 != er2) {
        snprintf(line, sizeof line, "\"%.200s\" ours %La end+%td errno %d, glibc %La end+%td errno %d",
                 s, a, e1 - s, er1, b, e2 - s, er2);
        report(C_STRTOLD, line);
    }
}
static void strto_both(const char *s) { strtod_one(s); strtof_one(s); strtold_one(s); }

/* The EXACT midpoint above |x| between two long doubles -- 65 bits, so
   printed through _Float128 -- then nudged up and cut short. */
static char midbuf[13000], midup[13100];
static void strtold_midpoint(long double x) {
    long double a = fabsl(x);
    if (!isfinite(a) || a == LDBL_MAX) return;
    _Float128 mid = ((_Float128)a + (_Float128)nextafterl(a, INFINITY)) / 2;
    strfromf128(midbuf, sizeof midbuf, "%.12000e", mid);
    char *e = strchr(midbuf, 'e'), ex[16];
    strcpy(ex, e);
    char *z = e;
    while (z[-1] == '0') z--;
    strcpy(z, ex);
    strtold_one(midbuf);
    size_t m = (size_t)(strchr(midbuf, 'e') - midbuf);
    memcpy(midup, midbuf, m); strcpy(midup + m, "000001"); strcat(midup, midbuf + m);
    strtold_one(midup);
    if (m > 30) { memcpy(midup, midbuf, 30); strcpy(midup + 30, midbuf + m); strtold_one(midup); }
}

static char longbuf[4096];
static void strto_table(void) {
    static const char *S[] = {
        "0.1", "1e-310", "123456789012345678901234567890", "3.4028235e38",
        "2.2250738585072011e-308", "2.2250738585072012e-308", "2.2250738585072014e-308",
        "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324",
        "2.47032822920623272088e-324", "1.7976931348623157e308", "1.7976931348623158e308",
        "1.7976931348623159e308", "1e309", "-1e309", "1e308", "-0", "0", "+0.0e-999",
        "0x1p-1074", "0x1p-1075", "0x1.8p-1075", "0x1p-1076", "0x1.fffffffffffff8p0",
        "0x1.000000000000008p0", "0x1.0000000000000081p0", "0x1.0000000000000180p0",
        "0x1.fffffffffffffp1023", "0x1.fffffffffffff8p1023", "0x1p1024", "0X1.8P3", "0x.8p1",
        "0x1p", "0x", "0x.p1", "0xg", "inf", "-Infinity", "INFINITY", "infinit", "nan",
        "-nan", "nan(123)", "nan(", "NaN(abc_9)x", " +.5e+3x", "1e", "1e+", ".", "-.e1",
        "1.e5", "00000000000000000000000000000000001.5", "9007199254740993",
        "9007199254740993.0000000000000000001", "9007199254740992.9999999999999999999",
        "1e99999999999", "1e-99999999999", "0e99999", "0.0000e-1000000", "  \t\n7",
        "1.17549435e-38", "1.4e-45", "7e-46", "7.1e-46", "3.40282356e38",
        "3.4028235677973366e38", "3.4028235677973367e38", "16777217", "16777217.000001",
        "16777219", "1.1754942e-38", "1.17549421e-38", "8.507059e37", "0x1.000001p0",
        "0x1.0000018p0", "0x1.fffffe8p127", "0x1.ffffffp127",
        "1.00000005960464477539062499", "1.000000059604644775390625",
        "1.00000005960464477539062501", "4.35", "100", "2.5", "1e23", "8.5e-5", "-123.456e-7",
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) strto_both(S[i]);
    /* x87's own edges: LDBL_MAX and past it, the subnormal threshold, 64-bit mantissas */
    static const char *L[] = {
        "1.18973149535723176502e+4932", "1.18973149535723176508e+4932", "1e4933", "-1e4933",
        "3.6451995318824746025e-4951", "1.8225997659412373012e-4951",
        "1.8225997659412373013e-4951", "1e-4952", "3.3621031431120935063e-4932",
        "0x1p-16445", "0x1p-16446", "0x1.8p-16446", "0x1.fffffffffffffffep16383",
        "0x1.ffffffffffffffffp16383", "0x1p16384", "18446744073709551615",
        "18446744073709551616", "18446744073709551617", "0xffffffffffffffff",
        "0xffffffffffffffff8", "0x1ffffffffffffffff", "0x1.0000000000000001p0",
        "0x1.00000000000000008p0", "0x1.00000000000000018p0", "1e400", "1e-400",
    };
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) strtold_one(L[i]);
    snprintf(longbuf, sizeof longbuf, "%Lf", LDBL_MAX / 100);   /* std::stold's own case */
    strtold_one(longbuf);
    strtold_midpoint(LDBL_MIN);
    strtold_midpoint(0x1p-16445L);
    strtold_midpoint(1.0L);
    /* long inputs: 1000 digits, and 330 leading zeros */
    char *p = longbuf;
    for (int i = 0; i < 1000; i++) *p++ = (char)('1' + i % 9);
    *p = 0; strto_both(longbuf);
    strcpy(p, "e-1300"); strto_both(longbuf);
    strcpy(p, "e-700"); strto_both(longbuf);
    p = longbuf; *p++ = '0'; *p++ = '.';
    for (int i = 0; i < 330; i++) *p++ = '0';
    strcpy(p, "1"); strto_both(longbuf);
    strcpy(p, "24703282292062327208828439643411068618252990130716238221279284125033775364"); strto_both(longbuf);
    /* the midpoint below 1, out to 800 digits, then a 1 beyond them */
    snprintf(longbuf, sizeof longbuf, "%.790Lf", (1.0L + nextafter(1.0, 2.0)) / 2);
    strto_both(longbuf);
    strcat(longbuf, "1"); strto_both(longbuf);
    p = longbuf + sprintf(longbuf, "0.");
    for (int i = 0; i < 1200; i++) *p++ = '0';
    strcpy(p, "1e1200"); strto_both(longbuf);
}

/* --- sscanf ----------------------------------------------------------- */
struct sc { const char *fmt, *in, *types; };
static const struct sc SC[] = {
    { "%[^=]=%[^;]", "key=value;rest", "ss" },
    { "%[a-z]", "abc123", "s" }, { "%[]a]", "]a]b", "s" }, { "%[^]]", "abc]d", "s" },
    { "%3[a-z]", "abcdef", "s" }, { "%[a-]", "a-b", "s" }, { "%[-a]", "-a-b", "s" },
    { "%[^\n]", "line one\nline two", "s" }, { "%[0-9]%n", "123abc", "si" },
    { "%*[a-z]%d", "abc42", "i" }, { "%[a-z]", "123", "s" }, { "%[a-z]", "", "s" },
    { " %[a-z]", "   abc", "s" }, { "%[a-z]", "  abc", "s" }, { "%[^ ] %[^ ]", "one two", "ss" },
    { "%[a-cx-z]%c", "abzq", "ss" }, { "%[^a-c]", "xyzb", "s" }, { "%2[^,],%s", "abc,d", "ss" },
    { "%[^,],%[^,],%d", "x,y,3", "ssi" }, { "%[A-Za-z0-9_]", "Name_9!", "s" },
    { "%d%n", "42 rest", "ii" }, { "%d %n%s", "1  x", "iis" }, { "%s%hhn", "abc", "sc" },
    { "%s%hn", "abcd", "sh" }, { "%s%ln", "abcde", "sl" }, { "%*d%n", "  77", "i" },
    { "%5c", "abcdefg", "s" }, { "%c%c", "a b", "ss" },
    { "%lf", "0.1", "d" }, { "%f", "3.4028235e38", "f" }, { "%f", "1.00000005960464477539062501", "f" },
    { "%Lf", "1.5", "L" }, { "%Lg", "0.375", "L" }, { "%le", "1e-310", "d" },
    { "%la", "0x1.8p1", "d" }, { "%lg %n", "2.5x", "di" }, { "%lf", "123456789012345678901234567890", "d" },
    { "%td", "-9", "l" }, { "%jd", "123", "l" }, { "%zu", "77", "l" }, { "%lld", "-5", "l" },
    { "%d%%%d", "4%5", "ii" }, { "%x %o %i %i", "ff 17 0x10 010", "iiii" },
    { "%lf%lf", "1e5 -inf", "dd" }, { "%3lf", "12345", "d" },
};
static void scanf_table(void) {
    for (unsigned i = 0; i < sizeof SC / sizeof SC[0]; i++) {
        unsigned char a[4][64], b[4][64];
        memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
        int ra = toy_sscanf(SC[i].in, SC[i].fmt, a[0], a[1], a[2], a[3]);
        int rb = sscanf(SC[i].in, SC[i].fmt, b[0], b[1], b[2], b[3]);
        n_cmp[C_SCANF]++;
        int bad = ra != rb;
        for (int k = 0; SC[i].types[k]; k++) {
            size_t n = 64;
            switch (SC[i].types[k]) {
            case 'i': n = sizeof(int); break;
            case 'l': n = sizeof(long); break;
            case 'h': n = sizeof(short); break;
            case 'c': n = 1; break;
            case 'd': n = 8; break;
            case 'f': n = 4; break;
            case 'L': n = 10; break;
            }
            if (memcmp(a[k], b[k], n)) bad = 1;
        }
        if (bad) {
            snprintf(line, sizeof line, "fmt=\"%s\" in=\"%s\" ours %d glibc %d (slot 0: \"%.20s\" vs \"%.20s\")",
                     SC[i].fmt, SC[i].in, ra, rb, (char *)a[0], (char *)b[0]);
            report(C_SCANF, line);
        }
    }
}

/* --- the random half ------------------------------------------------- */
static uint64_t rs;
static uint64_t rnd(void) {           /* splitmix64 */
    uint64_t z = (rs += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

static double rand_double(void) {
    switch (rn(3)) {
    case 0: return from_bits(rnd());
    case 1: return ldexp((double)(rnd() >> rn(64)), (int)rn(200) - 100);
    default: return ldexp((double)(int64_t)(rnd() >> (40 + rn(24))) * (rn(2) ? 1 : -1),
                          (int)rn(40) - 30);
    }
}
static long double rand_ldouble(void) {
    switch (rn(3)) {
    case 0: {
        uint64_t m = rnd();
        unsigned se = (unsigned)rnd() & 0xffff;
        if ((se & 0x7fff) != 0) m |= 1ULL << 63; else m &= ~(1ULL << 63);
        return ld_from(m, se);
    }
    case 1: return ldexpl((long double)(rnd() >> rn(64)), (int)rn(400) - 200);
    default: return ldexpl((long double)(int64_t)(rnd() >> (40 + rn(24))) * (rn(2) ? 1 : -1),
                           (int)rn(40) - 30);
    }
}
static void rand_fmt(char *f, const char *len) {
    unsigned fl = rn(4) ? 0 : rn(32);
    char w[8] = "", p[8] = "";
    if (!rn(4)) snprintf(w, sizeof w, "%u", rn(30));
    if (rn(6)) snprintf(p, sizeof p, ".%u", rn(10) ? rn(30) : rn(130));
    build_fmt(f, fl, w, p, len, CONVS[rn(8)]);
    if (w[0] == '0') { /* "%00f" is not a width; keep the format meaningful */
        build_fmt(f, fl, "", p, len, CONVS[rn(8)]);
    }
}

static void random_half(long count) {
    char f[64], s[1024];
    for (long i = 0; i < count; i++) { rand_fmt(f, ""); one_d(C_RDBL, f, rand_double()); }
    for (long i = 0; i < count / 3; i++) { rand_fmt(f, "L"); one_ld(C_RLDBL, f, rand_ldouble()); }
    for (long i = 0; i < count / 8; i++) {
        long double x = rand_ldouble();
        snprintf(s, sizeof s, "%.*Le", (int)rn(25), x);
        strtold_one(s);
        if (!rn(16)) strtold_midpoint(x);
    }
    for (long i = 0; i < count / 2; i++) {
        double x = rand_double();
        switch (rn(4)) {
        case 0: snprintf(s, sizeof s, "%.*e", (int)rn(26), x); strtod_one(s); break;
        case 1: snprintf(s, sizeof s, "%.*g", (int)rn(26), x); strto_both(s); break;
        case 2: {                    /* the exact midpoint above |x|, and its neighbours */
            double a = fabs(x);
            if (!isfinite(a) || a == DBL_MAX) break;
            long double mid = ((long double)a + (long double)nextafter(a, INFINITY)) / 2;
            int k = snprintf(s, sizeof s, "%.780Le", mid);
            /* strip the zeros that pad past the exact expansion */
            char *e = strchr(s, 'e'); char ex[16]; strcpy(ex, e);
            char *z = e; while (z[-1] == '0') z--;
            strcpy(z, ex); (void)k;
            strtod_one(s);
            char up[1024]; size_t m = (size_t)(strchr(s, 'e') - s);
            memcpy(up, s, m); strcpy(up + m, "000001"); strcat(up, s + m); strtod_one(up);
            if (m > 25) { memcpy(up, s, 25); strcpy(up + 25, s + m); strtod_one(up); }
            /* and a float's midpoint, which a double holds exactly */
            float fa = (float)a;
            if (isfinite(fa) && fa != FLT_MAX) {
                double fm = ((double)fa + (double)nextafterf(fa, INFINITY)) / 2;
                snprintf(s, sizeof s, "%.200e", fm);
                e = strchr(s, 'e'); strcpy(ex, e); z = e; while (z[-1] == '0') z--; strcpy(z, ex);
                strtof_one(s);
                m = (size_t)(strchr(s, 'e') - s);
                memcpy(up, s, m); strcpy(up + m, "01"); strcat(up, s + m); strtof_one(up);
                if (m > 12) { memcpy(up, s, 12); strcpy(up + 12, s + m); strtof_one(up); }
            }
            break;
        }
        default: {                   /* random digit strings */
            char *p = s;
            if (rn(2)) *p++ = '-';
            int nd = 1 + (int)rn(40), dot = (int)rn(nd + 1);
            for (int d = 0; d < nd; d++) { if (d == dot) *p++ = '.'; *p++ = (char)('0' + rn(10)); }
            sprintf(p, "e%d", (int)rn(700) - 360);
            strto_both(s);
            sprintf(p, "e%d", (int)rn(100) - 50);
            strtof_one(s);
        }
        }
    }
}

int main(int argc, char **argv) {
    long count = argc > 1 ? atol(argv[1]) : 0;
    rs = argc > 2 ? strtoull(argv[2], 0, 10) : 1;
    if (argc > 3) verbose_cap = atoi(argv[3]);
    values();
    ints();
    floats_table();
    strto_table();
    scanf_table();
    random_half(count);
    for (int c = 0; c < NCAT; c++) printf("TOTAL %s %ld %ld\n", CAT[c], n_cmp[c], n_bad[c]);
    printf("QUIRK %ld\n", n_quirk);
    return 0;
}
"""

# Positive control: break a TIE upward instead of to even, in printf's
# rounding and in strtod's -- the one-line mistake every hand-written
# conversion makes first. Both halves must go red.
CONTROL_PRINTF = ("rest > 0 || (rest == 0 && (lastd & 1))", "rest >= 0")
CONTROL_STRTOD = [("if (c > 0 || (c == 0 && (u & 1))) u++;", "if (c >= 0) u++;"),
                  ("if (c < 0 || (c == 0 && !((u - 1) & 1))) u--;", "if (c < 0) u--;")]


def build(tmp, poison):
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    api = os.path.join(ROOT, "kernel", "include", "api")
    pf = hostcheck.stage(tmp, "userland/libs/libc/printf_float.c", edits=[CONTROL_PRINTF],
                         apply=poison, tool="libc_fmt_hostcheck")
    st = hostcheck.stage(tmp, "userland/libs/libc/strtod.c", edits=CONTROL_STRTOD,
                         apply=poison, tool="libc_fmt_hostcheck")
    objs = []
    # Everything compiles against the HOST's headers (kfmt's own api/
    # first, as ring 3 builds it); tolibc's headers would need the
    # build's generated ones. glibc's headers give sscanf an asm label
    # (__isoc99_sscanf), so ours are renamed in the OBJECT afterwards --
    # both spellings -- and the driver's own calls still reach glibc.
    units = [
        (os.path.join(ROOT, "kernel/lib/kfmt.c"), False),
        (os.path.join(ROOT, "kernel/lib/knum.c"), False),
        (os.path.join(ROOT, "kernel/lib/string.c"), False),
        (pf, False),
        (os.path.join(ROOT, "userland/libs/libc/scanf.c"), True),
        (st, True),
    ]
    for i, (src, rename) in enumerate(units):
        obj = os.path.join(tmp, "u%d.o" % i)
        cmd = ["gcc", "-std=gnu11", "-O2", "-w", "-fno-builtin", "-U_FORTIFY_SOURCE",
               "-c", "-o", obj, src]
        if not rename:
            cmd += ["-I" + api]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit("libc_fmt_hostcheck: compile of %s failed:\n%s" % (src, r.stderr))
        if rename:
            have = set(subprocess.run(["nm", "-j", obj], capture_output=True, text=True,
                                      check=True).stdout.split())
            ren = []
            for name in RENAME:
                for spelt in (name, "__isoc99_" + name):
                    if spelt in have:
                        ren += ["--redefine-sym", "%s=toy_%s" % (spelt, name)]
            subprocess.run(["objcopy", *ren, obj], check=True)
        objs.append(obj)
    return hostcheck.compile(tmp, "libc_fmt_host", [drv, *objs, "-lm"],
                             flags=("-std=gnu11", "-O1", "-w", "-U_FORTIFY_SOURCE"),
                             tool="libc_fmt_hostcheck")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--random", type=int, default=400000,
                    help="random doubles printed (long doubles: a third; strtod: half) "
                         "(default 400000)")
    ap.add_argument("--seed", type=int, default=20261110)
    ap.add_argument("--show", type=int, default=12, help="mismatches shown per category")
    ap.add_argument("--keep", metavar="DIR", help="build in DIR and leave the binary there")
    ap.add_argument("--positive-control", action="store_true",
                    help="break the tie rule in a COPY and require the check to fail")
    args = ap.parse_args()
    if not shutil.which("gcc"):
        print("libc_fmt_hostcheck: SKIP -- no host gcc")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        if args.keep:
            tmp = args.keep
            os.makedirs(tmp, exist_ok=True)
        exe = build(tmp, args.positive_control)
        r = subprocess.run([exe, str(args.random), str(args.seed), str(args.show)],
                           capture_output=True, text=True, errors="replace")
    bad = 0
    total = 0
    red = set()
    for ln in r.stdout.splitlines():
        if ln.startswith("BAD "):
            print("  " + ln[4:][:600])
        elif ln.startswith("QUIRK "):
            print("libc_fmt_hostcheck: %s outputs differ only by glibc's %%#g carry bug "
                  "(C's answer kept, not counted)" % ln.split()[1])
        elif ln.startswith("TOTAL "):
            _, cat, n, b = ln.split()
            total += int(n)
            bad += int(b)
            if int(b):
                red.add(cat.split("-")[0])
            print("libc_fmt_hostcheck: %-22s %8d compared, %6d disagreed" % (cat, int(n), int(b)))
    if r.returncode != 0:
        print("libc_fmt_hostcheck: the driver died (status %d)\n%s" % (r.returncode, r.stderr[-2000:]))
        return 1
    print("libc_fmt_hostcheck: %d comparisons, %d disagreed" % (total, bad))
    if args.positive_control:
        if {"printf", "strtod", "strtold"} <= red:
            print("libc_fmt_hostcheck: positive control OK -- printf, strtod and strtold all went red")
            return 0
        print("libc_fmt_hostcheck: POSITIVE CONTROL DID NOT FAIL in %s -- "
              "this check measures nothing there" % sorted({"printf", "strtod", "strtold"} - red))
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
