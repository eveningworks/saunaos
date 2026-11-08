// The EXACT long double functions: rounding to integers, the exponent
// functions, nextafter, the comparisons, nanl -- math.c's set for the
// x87's 80-bit type. Bits, or one correctly rounded x87 instruction
// (fsqrt, frndint, fistp).
//
// THE FORMAT HAS AN EXPLICIT INTEGER BIT. Bit 63 of the mantissa is
// stored, not implied, so "the next representable number" crosses a
// binade by carrying out of the mantissa into the exponent by hand, and
// a subnormal's exponent field 0 means 2^-16382 like the field 1 above
// it. union ldshape (math_internal.h) is the view.
#include <limits.h>
#include "math_internal.h"

long double fabsl(long double x) { return __builtin_fabsl(x); }
long double copysignl(long double x, long double y) { return __builtin_copysignl(x, y); }
long double sqrtl(long double x) { return x87_fsqrt(x); }

// --- to an integer ----------------------------------------------------

long double truncl(long double x) {
    union ldshape u = { x };
    int e = (u.i.se & 0x7fff) - 16383;
    if (e >= 63) return x;   // integral already, or inf/NaN
    if (e < 0) return (u.i.se >> 15) ? -0.0L : 0.0L;
    u.i.m &= ~((1ull << (63 - e)) - 1);
    return u.f;
}

long double floorl(long double x) {
    long double t = truncl(x);
    return (x < 0 && t != x) ? t - 1 : t;
}

long double ceill(long double x) {
    long double t = truncl(x);
    return (x > 0 && t != x) ? t + 1 : t;
}

long double roundl(long double x) {
    long double t = truncl(x), frac = x - t;   // the fraction, exactly
    if (frac >= 0.5L) return t + 1;
    if (frac <= -0.5L) return t - 1;
    return t;
}

long double rintl(long double x) { return x87_frndint(x); }

// frndint between a save and a restore of the x87 environment, in ONE
// asm so nothing is scheduled outside the pair: the status flags it
// raises (inexact) are put back as they were.
long double nearbyintl(long double x) {
    char env[28];
    long double r;
    __asm__ volatile("fnstenv %1\n\tfrndint\n\tfldenv %1" : "=t"(r), "+m"(env) : "0"(x));
    return r;
}

// fistp rounds in the current mode; a NaN or out of range stores the
// "integer indefinite", LONG_MIN.
long lrintl(long double x) { long r; __asm__("fistpll %0" : "=m"(r) : "t"(x) : "st"); return r; }
long long llrintl(long double x) { return lrintl(x); }
long lroundl(long double x) { return (long)roundl(x); }
long long llroundl(long double x) { return (long long)roundl(x); }

// --- the exponent -----------------------------------------------------

long double frexpl(long double x, int *e) {
    union ldshape u = { x };
    int ee = u.i.se & 0x7fff;
    if (!ee) {
        if (x != 0) {
            x = frexpl(x * 0x1p120L, e);
            *e -= 120;
        } else {
            *e = 0;
        }
        return x;
    }
    if (ee == 0x7fff) { *e = 0; return x; }
    *e = ee - 0x3ffe;
    u.i.se = (uint16_t)((u.i.se & 0x8000) | 0x3ffe);
    return u.f;
}

// musl's shape, as scalbn's: at most three multiplications by powers of
// two, the subnormal case reached by ONE final rounding.
long double scalbnl(long double x, int n) {
    if (n > 16383) {
        x *= 0x1p16383L; n -= 16383;
        if (n > 16383) { x *= 0x1p16383L; n -= 16383; if (n > 16383) n = 16383; }
    } else if (n < -16382) {
        x *= 0x1p-16382L * 0x1p113L; n += 16382 - 113;
        if (n < -16382) { x *= 0x1p-16382L * 0x1p113L; n += 16382 - 113; if (n < -16382) n = -16382; }
    }
    return x * ld_make(1ull << 63, 0x3fff + n);
}

long double ldexpl(long double x, int n) { return scalbnl(x, n); }

long double scalblnl(long double x, long n) {
    if (n > INT_MAX) n = INT_MAX;
    else if (n < INT_MIN) n = INT_MIN;
    return scalbnl(x, (int)n);
}

int ilogbl(long double x) {
    union ldshape u = { x };
    uint64_t m = u.i.m;
    int e = u.i.se & 0x7fff;
    if (!e) {
        if (m == 0) { FORCE_EVAL(0 / 0.0f); return FP_ILOGB0; }
        for (e = -0x3fff + 1; !(m >> 63); e--, m <<= 1) {}
        return e;
    }
    if (e == 0x7fff) { FORCE_EVAL(0 / 0.0f); return (m << 1) ? FP_ILOGBNAN : INT_MAX; }
    return e - 0x3fff;
}

long double logbl(long double x) {
    if (!isfinite(x)) return x * x;
    if (x == 0) return -1 / (x * x);
    return ilogbl(x);
}

long double modfl(long double x, long double *iptr) {
    long double i = truncl(x);
    *iptr = i;
    if (isinf(x)) return __builtin_copysignl(0.0L, x);
    return __builtin_copysignl(x - i, x);
}

// --- neighbours, comparisons, NaN -------------------------------------

long double nextafterl(long double x, long double y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    union ldshape ux = { x };
    if (x == 0) {
        union ldshape uy = { y };
        ux.i.m = 1;
        ux.i.se = uy.i.se & 0x8000;
    } else if ((x < y) == !(ux.i.se & 0x8000)) {   // away from zero
        ux.i.m++;
        if (ux.i.m << 1 == 0) { ux.i.m = 1ull << 63; ux.i.se++; }
    } else {                                       // toward zero
        if (ux.i.m << 1 == 0) {
            ux.i.se--;
            if (ux.i.se & 0x7fff) ux.i.m = 0;
        }
        ux.i.m--;
    }
    if ((ux.i.se & 0x7fff) == 0x7fff) return x + x;   // overflow, raised
    if ((ux.i.se & 0x7fff) == 0) FORCE_EVAL(x * x + ux.f * ux.f);   // underflow
    return ux.f;
}

long double nexttowardl(long double x, long double y) { return nextafterl(x, y); }

long double fdiml(long double x, long double y) {
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    return x > y ? x - y : 0;
}

long double fmaxl(long double x, long double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (signbit(x) != signbit(y)) return signbit(x) ? y : x;
    return x < y ? y : x;
}

long double fminl(long double x, long double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (signbit(x) != signbit(y)) return signbit(x) ? x : y;
    return x < y ? x : y;
}

M_HIDDEN unsigned long long __m_nan_payload(const char *s);
long double nanl(const char *s) {
    return ld_make(0xC000000000000000ull | (__m_nan_payload(s) & ((1ull << 62) - 1)), 0x7fff);
}
