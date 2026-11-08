// fma, fmaf and fmal: x*y + z with ONE rounding, computed EXACTLY in
// integers and rounded once at the end.
//
// The x86-64 baseline has no FMA instruction, so this is the whole
// implementation rather than a fallback. The product of two 64-bit
// mantissas is 128 bits (one `mulq`), z is aligned against it in a
// 256-bit accumulator, and the exact sum is rounded to the target's
// precision in the CURRENT rounding mode -- including the subnormal
// range, where fewer bits survive. One core serves all three types; only
// the mantissa width and the smallest exponent differ.
//
// The classic shortcut -- x*y split by Dekker, summed in double-double
// -- is right almost everywhere and wrong in a double-rounding corner
// near halfway cases, which is exactly the corner an fma exists for.
// glibc's dbl-64 fma has a page of code for that corner; integers have
// no corner. The cost is speed, which an fma here never had.
#include "math_internal.h"

typedef unsigned __int128 u128;

struct ufp { int sign; int e; uint64_t m; };   // value = m * 2^e, bit 63 of m set

// The 256-bit accumulator, least significant word first.
static void put(uint64_t w[4], u128 v, int shift) {
    for (int i = 0; i < 4; i++) w[i] = 0;
    int q = shift / 64, s = shift % 64;
    uint64_t lo = (uint64_t)v, hi = (uint64_t)(v >> 64);
    uint64_t parts[3];
    parts[0] = lo << s;
    parts[1] = s ? (hi << s) | (lo >> (64 - s)) : hi;
    parts[2] = s ? hi >> (64 - s) : 0;
    for (int i = 0; i < 3; i++)
        if (q + i < 4) w[q + i] = parts[i];
}

static int cmp256(const uint64_t a[4], const uint64_t b[4]) {
    for (int i = 3; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

static void add256(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    unsigned carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 s = (u128)a[i] + b[i] + carry;
        r[i] = (uint64_t)s;
        carry = (unsigned)(s >> 64);
    }
}

static void sub256(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {   // a >= b
    unsigned borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t bi = b[i] + borrow;
        unsigned nb = (bi < b[i]) || (a[i] < bi);
        r[i] = a[i] - bi;
        borrow = nb;
    }
}

static int bit_at(const uint64_t w[4], int pos) {
    if (pos < 0 || pos >= 256) return 0;
    return (int)(w[pos / 64] >> (pos % 64) & 1);
}

static int any_below(const uint64_t w[4], int pos) {   // a set bit in [0, pos)
    if (pos <= 0) return 0;
    if (pos >= 256) pos = 256;
    for (int i = 0; i < pos / 64; i++)
        if (w[i]) return 1;
    if (pos % 64 && (w[pos / 64] & ((1ull << (pos % 64)) - 1))) return 1;
    return 0;
}

static uint64_t bits64_at(const uint64_t w[4], int pos) {   // bits [pos, pos+64)
    if (pos >= 256) return 0;
    int q = pos / 64, s = pos % 64;
    uint64_t lo = w[q] >> s;
    if (s && q + 1 < 4) lo |= w[q + 1] << (64 - s);
    return lo;
}

// Rounding mode in C's encoding order: 0 nearest, 1 down, 2 up, 3 zero
// (the RC field of MXCSR and of the x87 control word alike).
static int mode_sse(void) { uint32_t c; __asm__ volatile("stmxcsr %0" : "=m"(c)); return (c >> 13) & 3; }
static int mode_x87(void) { uint16_t c; __asm__ volatile("fnstcw %0" : "=m"(c)); return (c >> 10) & 3; }

// The exact a*b + c, rounded to PREC bits with the last place no lower
// than 2^EMIN. *zero when it cancels exactly; else value = *m * 2^*e.
static void fma_core(struct ufp a, struct ufp b, struct ufp c, int prec, int emin, int mode,
                     int *sign, uint64_t *m, int *e, int *zero) {
    u128 p = (u128)a.m * b.m;
    int ep = a.e + b.e, sp = a.sign ^ b.sign;
    uint64_t cm = c.m;
    int ec = c.e;
    *zero = 0;
    // One far below the other only decides the rounding: it is replaced
    // by a single bit just under the larger one's lowest, which rounds
    // the same way any nonzero value that small would.
    if (ec + 63 < ep - 2) { cm = 1; ec = ep - 2; }
    else if (ep + 127 < ec - 2) { p = 1; ep = ec - 2; }
    int base = ep < ec ? ep : ec;
    uint64_t A[4], C[4], R[4];
    put(A, p, ep - base);
    put(C, cm, ec - base);
    if (sp == c.sign) {
        add256(R, A, C);
        *sign = sp;
    } else {
        int k = cmp256(A, C);
        if (k == 0) { *zero = 1; return; }
        if (k > 0) { sub256(R, A, C); *sign = sp; }
        else       { sub256(R, C, A); *sign = c.sign; }
    }
    int top = 255;
    while (!bit_at(R, top)) top--;
    int lsb = top - prec + 1;
    if (base + lsb < emin) lsb = emin - base;
    if (lsb <= 0) {   // fits as it is: exact
        *m = bits64_at(R, 0);
        *e = base;
        return;
    }
    uint64_t mm = bits64_at(R, lsb);
    int half = bit_at(R, lsb - 1), rest = any_below(R, lsb - 1);
    int up;
    switch (mode) {
    case 0:  up = half && (rest || (mm & 1)); break;
    case 1:  up = (half || rest) && *sign; break;
    case 2:  up = (half || rest) && !*sign; break;
    default: up = 0; break;
    }
    mm += (uint64_t)up;
    if (prec == 64 && up && mm == 0) { mm = 1ull << 63; lsb++; }   // carried out
    *m = mm;
    *e = base + lsb;
}

static struct ufp unpack(int sign, uint64_t m, int e) {
    struct ufp u;
    int s = __builtin_clzll(m);
    u.sign = sign; u.m = m << s; u.e = e - s;
    return u;
}

static struct ufp unpack_d(double v) {
    uint64_t b = m_bits(v);
    int ex = (int)(b >> 52 & 0x7ff);
    uint64_t m = b & ((1ull << 52) - 1);
    if (ex) m |= 1ull << 52;
    return unpack((int)(b >> 63), m, (ex ? ex : 1) - 1075);
}

static struct ufp unpack_ld(long double v) {
    union ldshape u = { v };
    int ex = u.i.se & 0x7fff;
    return unpack(u.i.se >> 15, u.i.m, (ex ? ex : 1) - 16383 - 63);
}

double fma(double x, double y, double z) {
    // What the hardware computes right on its own: a non-finite
    // operand, an exactly zero product (the sum is one rounding), or a
    // zero z (the product is). An infinite z with a finite product IS
    // the answer even where x*y would overflow on its way to it.
    if (!isfinite(x) || !isfinite(y)) return x * y + z;
    if (isnan(z)) return z + z;
    if (isinf(z)) return z;
    if (x == 0 || y == 0) return x * y + z;
    if (z == 0) return x * y;
    int sign, e, zero;
    uint64_t m;
    int mode = mode_sse();
    fma_core(unpack_d(x), unpack_d(y), unpack_d(z), 53, -1074, mode, &sign, &m, &e, &zero);
    if (zero) return mode == 1 ? -0.0 : 0.0;
    double r = scalbn((double)m, e);
    return sign ? -r : r;
}

// The double core at float's width: a float widens exactly, and the
// rounded result is a float scaled into double's range, so the last
// conversion is exact (or overflows, in the current mode).
float fmaf(float x, float y, float z) {
    if (!isfinite(x) || !isfinite(y)) return x * y + z;
    if (isnan(z)) return z + z;
    if (isinf(z)) return z;
    if (x == 0 || y == 0) return x * y + z;
    if (z == 0) return x * y;
    int sign, e, zero;
    uint64_t m;
    int mode = mode_sse();
    fma_core(unpack_d(x), unpack_d(y), unpack_d(z), 24, -149, mode, &sign, &m, &e, &zero);
    if (zero) return mode == 1 ? -0.0f : 0.0f;
    double r = scalbn((double)m, e);
    return (float)(sign ? -r : r);
}

long double fmal(long double x, long double y, long double z) {
    if (!isfinite(x) || !isfinite(y)) return x * y + z;
    if (isnan(z)) return z + z;
    if (isinf(z)) return z;
    if (x == 0 || y == 0) return x * y + z;
    if (z == 0) return x * y;
    int sign, e, zero;
    uint64_t m;
    int mode = mode_x87();
    fma_core(unpack_ld(x), unpack_ld(y), unpack_ld(z), 64, -16445, mode, &sign, &m, &e, &zero);
    if (zero) return mode == 1 ? -0.0L : 0.0L;
    long double r = scalbnl((long double)m, e);
    return sign ? -r : r;
}
