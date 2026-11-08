// The long double exponentials and logarithms, pow, cbrt, hypot and the
// hyperbolics: REAL 80-bit results, on the x87's own instructions.
//
// f2xm1 (2^x - 1 for |x| <= 1), fyl2x (y*log2 x), fyl2xp1 (y*log2(1+x)
// for small x), fscale and fsqrt are each accurate to about an ulp of
// the 64-bit mantissa, which is the precision nothing else on this CPU
// offers; musl's x86-64 expl/logl/log1pl are the same instructions.
// What this file adds is getting the ARGUMENTS to them exactly: e^x is
// 2^(x*log2 e) only if x*log2 e is formed without losing the bits that
// its integer part pushes out, so x is reduced by k*ln2 with ln 2 in two
// pieces first (math_tables.h), the way the double exp() does it.
//
// pow is the one that needs more: y*log2(x) is up to 16,384 in size,
// so an ulp of it is 2^-50, and that absolute error becomes the relative
// error of the result. log2(x) is therefore carried as a DOUBLE long
// double (hi + lo, ~120 bits) from an atanh series, the product with y
// formed exactly, and only the final 2^f rounded.
//
// Measured accuracy per function: docs/libc-design.md.
#include "math_internal.h"
#include "math_tables.h"

static void two_sum(long double a, long double b, long double *s, long double *e) {
    *s = a + b;
    long double bb = *s - a;
    *e = (a - (*s - bb)) + (b - bb);
}

long double exp2l(long double x) {
    if (isnan(x)) return x + x;
    if (x >= 16384) return x * 0x1p16383L;   // overflow (or +inf)
    if (x < -16446) return 0x1p-16382L * 0x1p-16382L;   // underflow (or -inf -> 0)
    long double n = x87_frndint(x), f = x - n;   // exact, |f| <= 1
    return x87_fscale(x87_f2xm1(f) + 1, n);
}

// x = k*ln2 + r with k*LN2L_HI exact (48 bits times |k| < 2^15), so r
// is right to ~2^-100; then e^r = 2^(r*log2 e).
static long double reduce_ln2(long double x, long double *k) {
    *k = x87_frndint(x * M_LOG2EL_HI);
    long double r = (x - *k * M_LN2L_HI) - *k * M_LN2L_LO;
    return r * M_LOG2EL_HI + r * M_LOG2EL_LO;
}

long double expl(long double x) {
    if (isnan(x)) return x + x;
    if (x > 11357.3L) return x * 0x1p16383L;
    if (x < -11400) return 0x1p-16382L * 0x1p-16382L;
    long double k, t = reduce_ln2(x, &k);
    return x87_fscale(x87_f2xm1(t) + 1, k);
}

// e^x - 1 = 2^k * (2^t - 1) + (2^k - 1): f2xm1 gives the first factor
// exactly where it is small, and 2^k - 1 is exact for |k| <= 64, so the
// one rounding is the final sum -- no cancellation near 0.
long double expm1l(long double x) {
    if (isnan(x)) return x + x;
    if (fabsl(x) < 0x1p-65L) return x;   // x itself, and the sign of a zero kept
    if (x > 11357.3L) return x * 0x1p16383L;
    if (x < -64) return -1 + 0x1p-100L;   // -1, inexact
    long double k, t = reduce_ln2(x, &k);
    long double u = x87_f2xm1(t);
    if (k == 0) return u;
    long double tk = x87_fscale(1.0L, k);
    if (k > 64) return x87_fscale(u + 1, k) - 1;
    return u * tk + (tk - 1);
}

// Near 1 the logs take fyl2xp1 on x - 1 (exact there): fyl2x of a
// number close to 1 would be log2 of its rounding.
#define NEAR_ONE(x) ((x) > 0.70710678118654752440L && (x) < 1.29289321881345247560L)

long double logl(long double x) {
    if (NEAR_ONE(x)) return x87_fyl2xp1(x87_ln2(), x - 1);
    return x87_fyl2x(x87_ln2(), x);
}

long double log2l(long double x) {
    if (NEAR_ONE(x)) return x87_fyl2xp1(1.0L, x - 1);
    return x87_fyl2x(1.0L, x);
}

long double log10l(long double x) {
    if (NEAR_ONE(x)) return x87_fyl2xp1(x87_lg2(), x - 1);
    return x87_fyl2x(x87_lg2(), x);
}

long double log1pl(long double x) {
    if (isnan(x) || x == INFINITY) return x + x;
    // fyl2xp1 takes |x| < 1 - sqrt(2)/2.
    if (x > -0.29289321881345247560L && x < 0.29289321881345247560L)
        return x87_fyl2xp1(x87_ln2(), x);
    if (x < 0) return x87_fyl2x(x87_ln2(), 1 + x);   // 1 + x is exact below 0
    // 1 + x rounds above it; log(1+x) = log(u) + (1+x-u)/u, the
    // correction fdlibm's log1p carries too.
    long double u = 1 + x, c = (x - (u - 1)) / u;
    return x87_fyl2x(x87_ln2(), u) + c;
}

// --- pow --------------------------------------------------------------

// log2(x) for x > 0 finite as hi + lo, to ~2^-110 relative.
// x = 2^e * m, m in [sqrt(1/2), sqrt(2)); s = (m-1)/(m+1) in double
// long double; log2 m = (2/ln 2) * (s + s^3/3 + s^5/5 + ...), the
// first two terms in double long double, the rest (below 2^-15) in
// long double, to s^31 (|s| <= 0.1716, so the next term is < 2^-82).
static void log2_dd(long double x, long double *hi, long double *lo) {
    static const long double K_HI = 0xb8aa3b295c17f0bcp-62L;   // 2/ln 2 = 2*log2(e)
    static const long double K_LO = -0x82f0025f2dc582eep-127L;
    int e;
    long double m = frexpl(x, &e);   // [0.5, 1)
    if (m < 0.70710678118654752440L) { m *= 2; e--; }
    long double num = m - 1;          // exact
    long double dh, dl;
    two_sum(m, 1.0L, &dh, &dl);      // m + 1, exactly
    long double sh = num / dh, p, pe;
    ld_two_prod(sh, dh, &p, &pe);
    long double sl = ((num - p) - pe - sh * dl) / dh;
    // s^2 and s^3, double long double
    long double zh, zl, ch, cl;
    ld_two_prod(sh, sh, &zh, &zl);
    zl += 2 * sh * sl;
    ld_two_prod(sh, zh, &ch, &cl);
    cl += sh * zl + sl * zh;
    // s^3 / 3
    long double c3h = ch / 3, q, qe;
    ld_two_prod(c3h, 3.0L, &q, &qe);
    long double c3l = ((ch - q) - qe + cl) / 3;
    // s^5 * (1/5 + z/7 + ... + z^13/31)
    long double z = zh, poly = 1.0L / 31;
    for (int k = 29; k >= 5; k -= 2) poly = poly * z + 1.0L / k;
    long double tail = ch * zh * poly;
    long double th, tl;
    two_sum(sh, c3h, &th, &tl);
    tl += sl + c3l + tail;
    // times 2/ln 2
    long double lh, ll;
    ld_two_prod(K_HI, th, &lh, &ll);
    ll += K_HI * tl + K_LO * th;
    // plus e
    long double ah, al;
    two_sum((long double)e, lh, &ah, &al);
    al += ll;
    *hi = ah + al;
    *lo = al - (*hi - ah);
}

// y an odd integer?  (y is already known to be an integer.)
static int odd_int(long double y) {
    if (fabsl(y) >= 0x1p64L) return 0;
    return fmodl(y, 2.0L) != 0;
}

long double powl(long double x, long double y) {
    if (y == 0) return 1;             // even for a NaN x
    if (x == 1) return 1;             // even for a NaN y
    if (isnan(x) || isnan(y)) return x + y;
    int yint = truncl(y) == y;        // inf counts as an even integer here
    int yodd = !isinf(y) && yint && odd_int(y);
    if (isinf(y)) {
        long double ax = fabsl(x);
        if (ax == 1) return 1;        // (-1)**+-inf
        if ((ax > 1) == (y > 0)) return INFINITY;
        return 0;
    }
    if (x == 0) {
        if (y < 0) return yodd ? 1 / x : 1 / fabsl(x);   // +-inf, divide by zero
        return yodd ? x : 0;
    }
    if (isinf(x)) {
        if (x > 0) return y > 0 ? x : 0;
        if (y > 0) return yodd ? x : -x;
        return yodd ? -0.0L : 0.0L;
    }
    long double sign = 1;
    if (x < 0) {
        if (!yint) return (x - x) / (x - x);   // invalid
        if (yodd) sign = -1;
        x = -x;
    }
    if (x == 1) return sign;   // (-1)**integer, however large
    // A small positive integer power by repeated multiplication in
    // double long double: exact while it can be, one rounding at the end
    // -- pow(3, 2) is 9 to the bit.
    if (yint && y >= 1 && y <= 64) {
        int e = ilogbl(x);
        if (e > -16000 / 64 && e < 16000 / 64) {
            long double rh = 1, rl = 0, bh = x, bl = 0;
            unsigned n = (unsigned)y;
            for (;;) {
                if (n & 1) {
                    long double h, l;
                    ld_two_prod(rh, bh, &h, &l);
                    l += rh * bl + rl * bh;
                    two_sum(h, l, &rh, &rl);
                }
                n >>= 1;
                if (!n) break;
                long double h, l;
                ld_two_prod(bh, bh, &h, &l);
                l += 2 * bh * bl;
                two_sum(h, l, &bh, &bl);
            }
            return sign * (rh + rl);
        }
    }
    // |y| this large over- or underflows for every x != 1 (|log2 x| is
    // at least ~2^-64), and would overflow the splitting below.
    if (fabsl(y) > 0x1p100L)
        return sign * (((x > 1) == (y > 0)) ? 0x1p16383L * 0x1p16383L : 0x1p-16382L * 0x1p-16382L);
    long double lh, ll;
    log2_dd(x, &lh, &ll);
    long double yh, yl;
    ld_two_prod(y, lh, &yh, &yl);
    yl += y * ll;
    if (yh > 16384) return sign * 0x1p16383L * 0x1p16383L;
    if (yh < -16447) return sign * 0x1p-16382L * 0x1p-16382L;
    long double n = x87_frndint(yh);
    long double f = (yh - n) + yl;
    return sign * x87_fscale(x87_f2xm1(f) + 1, n);
}

// --- cbrt, hypot ------------------------------------------------------

// The double cbrt for 53 bits, then one Newton step in long double with
// the cube formed exactly, for the rest.
long double cbrtl(long double x) {
    if (!isfinite(x) || x == 0) return x + x;
    int e;
    long double m = frexpl(fabsl(x), &e);   // [0.5, 1)
    int r = e % 3;
    if (r < 0) r += 3;
    m = ldexpl(m, r);                        // [0.5, 4), e - r divisible by 3
    e -= r;
    long double t = cbrt((double)m);
    long double p, pe, c, ce;
    ld_two_prod(t, t, &p, &pe);
    ld_two_prod(p, t, &c, &ce);
    long double resid = (c - m) + ce + pe * t;
    t -= resid / (3 * p);
    t = ldexpl(t, e / 3);
    return x < 0 ? -t : t;
}

long double hypotl(long double x, long double y) {
    x = fabsl(x);
    y = fabsl(y);
    if (isinf(x) || isinf(y)) return INFINITY;
    if (isnan(x) || isnan(y)) return x + y;
    if (x < y) { long double t = x; x = y; y = t; }
    if (y == 0) return x;
    int ex = ilogbl(x), ey = ilogbl(y);
    if (ex - ey > 66) return x + y;
    long double z = 1;
    // Scaled so both squares, and their Dekker halves, are normal.
    if (ex > 8000) { z = 0x1p8500L; x *= 0x1p-8500L; y *= 0x1p-8500L; }
    else if (ey < -8000) { z = 0x1p-8500L; x *= 0x1p8500L; y *= 0x1p8500L; }
    long double hx, lx, hy, ly;
    ld_two_prod(x, x, &hx, &lx);
    ld_two_prod(y, y, &hy, &ly);
    return z * x87_fsqrt(ly + lx + hy + hx);
}

// --- the hyperbolics, the double ones' formulas in long double --------

#define LN2L (M_LN2L_HI + M_LN2L_LO)

long double sinhl(long double x) {
    long double h = signbit(x) ? -0.5L : 0.5L, a = fabsl(x);
    if (isnan(x)) return x + x;
    if (a < 11356) {
        if (a < 0x1p-33L) return x;
        long double t = expm1l(a);
        if (a < 1) return h * (2 * t - t * t / (t + 1));
        return h * (t + t / (t + 1));
    }
    // e^a / 2 past where e^a overflows: (e^(a/2) / 2) * e^(a/2).
    long double t = expl(0.5L * a);
    return (h * t) * t;
}

long double coshl(long double x) {
    long double a = fabsl(x);
    if (isnan(x)) return x + x;
    if (a < 0.6931471805599453094L) {
        if (a < 0x1p-33L) return 1;
        long double t = expm1l(a);
        return 1 + t * t / (2 * (1 + t));
    }
    if (a < 11356) {
        long double t = expl(a);
        return 0.5L * (t + 1 / t);
    }
    long double t = expl(0.5L * a);
    return (0.5L * t) * t;
}

long double tanhl(long double x) {
    long double a = fabsl(x), t;
    if (isnan(x)) return x + x;
    if (a > 0.5493061443340548457L) {   // log(3)/2
        if (a > 24) t = 1 - 0x1p-100L;
        else { t = expm1l(2 * a); t = 1 - 2 / (t + 2); }
    } else if (a > 0.2554128118829953416L) {   // log(5/3)/2
        t = expm1l(2 * a);
        t = t / (t + 2);
    } else if (a >= 0x1p-33L) {
        t = expm1l(-2 * a);
        t = -t / (t + 2);
    } else {
        return x;
    }
    return signbit(x) ? -t : t;
}

long double asinhl(long double x) {
    long double a = fabsl(x), r;
    if (!isfinite(x)) return x + x;
    if (a >= 0x1p32L)       r = logl(a) + LN2L;
    else if (a >= 2)        r = logl(2 * a + 1 / (x87_fsqrt(a * a + 1) + a));
    else if (a >= 0x1p-32L) r = log1pl(a + a * a / (x87_fsqrt(a * a + 1) + 1));
    else return x;
    return signbit(x) ? -r : r;
}

long double acoshl(long double x) {
    if (isnan(x)) return x + x;
    if (x < 2) return log1pl(x - 1 + x87_fsqrt((x - 1) * (x - 1) + 2 * (x - 1)));
    if (x < 0x1p32L) return logl(2 * x - 1 / (x + x87_fsqrt(x * x - 1)));
    return logl(x) + LN2L;
}

long double atanhl(long double x) {
    long double a = fabsl(x), r;
    if (isnan(x)) return x + x;
    if (a < 0.5L) {
        if (a < 0x1p-33L) return x;
        r = 0.5L * log1pl(2 * a + 2 * a * a / (1 - a));
    } else {
        r = 0.5L * log1pl(2 * (a / (1 - a)));
    }
    return signbit(x) ? -r : r;
}
