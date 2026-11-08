// The EXACT half of <math.h>, in double: rounding to integers, the
// exponent functions, nextafter, the comparisons and nan(). Every
// function here is integer reasoning over the IEEE-754 representation
// or a single correctly rounded operation -- there is no approximation
// in this file, which is why there is no accuracy note. (The float
// forms are math_float.c's, the long double ones math_ld.c's.)
//
// Written against the BITS rather than with casts through integers,
// because a double can hold values no integer type can and
// `(double)(long long)x` is undefined once |x| exceeds LLONG_MAX. That
// is not hypothetical for floor(1e300).
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include "math_internal.h"

#define EXP_MASK  0x7FF0000000000000ull
#define FRAC_MASK 0x000FFFFFFFFFFFFFull
#define SIGN_MASK 0x8000000000000000ull

// The three <math.h> makes macros in freestanding C, defined under
// their real names: the parentheses stop the macro expanding.
double (fabs)(double x) { return __builtin_fabs(x); }
double (copysign)(double x, double y) { return __builtin_copysign(x, y); }

// THE OUT-OF-LINE sqrt() GCC CALLS: with errno semantics GCC follows
// its sqrtsd with a call to `sqrt` for a negative argument, to set
// errno -- so every caller that cannot prove its argument non-negative
// needs this symbol (Duktape's Math.sqrt). The one function in <math.h>
// that sets errno, for that reason.
double (sqrt)(double x) {
    if (x < 0.0) { errno = EDOM; return (x - x) / (x - x); }
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

// --- to an integer ----------------------------------------------------

double trunc(double x) {
    uint64_t b = m_bits(x);
    int e = (int)((b & EXP_MASK) >> 52) - 1023;
    // e < 0: |x| < 1, so the integer part is a signed zero.
    if (e < 0) return m_dbl(b & SIGN_MASK);
    // e >= 52: every mantissa bit is already an integer, including the
    // infinities and NaNs, which must come back unchanged.
    if (e >= 52) return x;
    // Clear the fraction bits the exponent does not reach.
    uint64_t keep = ~(FRAC_MASK >> e);
    return m_dbl(b & keep);
}

double floor(double x) {
    double t = trunc(x);
    // Toward negative infinity: a negative value that lost a fraction
    // has to go one further down.
    if (x < 0.0 && t != x) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > 0.0 && t != x) return t + 1.0;
    return t;
}

double round(double x) {
    // Half away from zero, which is C's round() -- NOT the
    // round-half-to-even the hardware does, and the difference is
    // visible at exactly 0.5. x - t is the fraction, exactly.
    double t = trunc(x);
    double frac = x - t;
    if (frac >= 0.5) return t + 1.0;
    if (frac <= -0.5) return t - 1.0;
    return t;
}

// In the CURRENT rounding mode, by the 2^52 trick: adding 2^52 to a
// smaller magnitude pushes every fraction bit off the end of the
// mantissa, rounded as the mode says, and subtracting it back is exact.
// The sign is put back because (-0.3 + -2^52) + 2^52 is +0, not -0.
double rint(double x) {
    uint64_t b = m_bits(x);
    if (((b & EXP_MASK) >> 52) >= 1023 + 52) return x;   // integral, inf, NaN
    double s = (b & SIGN_MASK) ? -0x1p52 : 0x1p52;
    double y = x + s;
    __asm__("" : "+x"(y));   // keep the two operations apart
    y -= s;
    return __builtin_copysign(y, x);
}

// rint() without FE_INEXACT: the status flags are saved around it and
// put back, so whatever the addition raised is forgotten. The barrier
// pins the arithmetic between the two.
double nearbyint(double x) {
    uint32_t csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    double r = rint(x);
    __asm__ volatile("" : "+x"(r));
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    return r;
}

// cvtsd2si rounds in the current mode and gives the "integer
// indefinite" LONG_MIN for a NaN or out of range -- what glibc returns.
long lrint(double x) { long r; __asm__("cvtsd2si %1, %0" : "=r"(r) : "x"(x)); return r; }
long long llrint(double x) { return lrint(x); }
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }

// --- the exponent -----------------------------------------------------

double frexp(double x, int *e) {
    uint64_t b = m_bits(x);
    int ex = (int)((b & EXP_MASK) >> 52);
    if (ex == 0) { // zero or subnormal
        if ((b & ~SIGN_MASK) == 0) { *e = 0; return x; }
        // Normalise the subnormal by scaling up a known amount, then
        // correcting the exponent.
        x *= 0x1p54;
        b = m_bits(x);
        ex = (int)((b & EXP_MASK) >> 52);
        *e = ex - 1022 - 54;
    } else if (ex == 0x7FF) { // inf or NaN
        *e = 0;
        return x;
    } else {
        *e = ex - 1022;
    }
    // Force the exponent to 1022, which puts the mantissa in [0.5, 1).
    return m_dbl((b & ~EXP_MASK) | ((uint64_t)1022 << 52));
}

// x * 2^n, as at most three multiplications by powers of two, each
// EXACT unless the result overflows or goes subnormal. musl's shape:
// a subnormal result is reached by one final multiply from a value
// still normal, so it is rounded ONCE -- two steps through the
// subnormal range would round twice.
double scalbn(double x, int n) {
    double y = x;
    if (n > 1023) {
        y *= 0x1p1023; n -= 1023;
        if (n > 1023) { y *= 0x1p1023; n -= 1023; if (n > 1023) n = 1023; }
    } else if (n < -1022) {
        y *= 0x1p-1022 * 0x1p53; n += 1022 - 53;
        if (n < -1022) { y *= 0x1p-1022 * 0x1p53; n += 1022 - 53; if (n < -1022) n = -1022; }
    }
    return y * m_dbl((uint64_t)(0x3ff + n) << 52);
}

double ldexp(double x, int e) { return scalbn(x, e); }

double scalbln(double x, long n) {
    if (n > INT_MAX) n = INT_MAX;
    else if (n < INT_MIN) n = INT_MIN;
    return scalbn(x, (int)n);
}

int ilogb(double x) {
    uint64_t b = m_bits(x);
    int e = (int)(b >> 52 & 0x7ff);
    if (e == 0) {
        b <<= 12;
        if (b == 0) { FORCE_EVAL(0 / 0.0f); return FP_ILOGB0; }
        for (e = -0x3ff; !(b >> 63); e--, b <<= 1) {}
        return e;
    }
    if (e == 0x7ff) { FORCE_EVAL(0 / 0.0f); return (b << 12) ? FP_ILOGBNAN : INT_MAX; }
    return e - 0x3ff;
}

double logb(double x) {
    if (!isfinite(x)) return x * x;
    if (x == 0) return -1 / (x * x);   // -inf, and FE_DIVBYZERO
    return ilogb(x);
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    *iptr = i;
    // The fractional part keeps the SIGN of the input, including for a
    // negative value whose integer part is zero -- modf(-0.5) is -0.5
    // and -0.0, not -0.5 and 0.0.
    if (isinf(x)) return __builtin_copysign(0.0, x);
    return __builtin_copysign(x - i, x);
}

// --- neighbours, comparisons, NaN -------------------------------------

// One representation step toward y: adjacent doubles of one sign are
// adjacent integers in their bits, which is the reason the format is
// laid out sign-magnitude with the exponent on top.
double nextafter(double x, double y) {
    uint64_t ux = m_bits(x), uy = m_bits(y), ax, ay;
    if (isnan(x) || isnan(y)) return x + y;
    if (ux == uy) return y;
    ax = ux & ~SIGN_MASK;
    ay = uy & ~SIGN_MASK;
    if (ax == 0) {
        if (ay == 0) return y;
        ux = (uy & SIGN_MASK) | 1;
    } else if (ax > ay || ((ux ^ uy) & SIGN_MASK)) {
        ux--;
    } else {
        ux++;
    }
    int e = (int)(ux >> 52 & 0x7ff);
    if (e == 0x7ff) FORCE_EVAL(x + x);           // overflow
    if (e == 0) FORCE_EVAL(x * x + m_dbl(ux) * m_dbl(ux));  // underflow
    return m_dbl(ux);
}

double nexttoward(double x, long double y) {
    uint64_t ux = m_bits(x);
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    if (x == 0) {
        ux = 1;
        if (signbit(y)) ux |= SIGN_MASK;
    } else if (x < y) {
        if (signbit(x)) ux--; else ux++;
    } else {
        if (signbit(x)) ux++; else ux--;
    }
    int e = (int)(ux >> 52 & 0x7ff);
    if (e == 0x7ff) FORCE_EVAL(x + x);
    if (e == 0) FORCE_EVAL(x * x + m_dbl(ux) * m_dbl(ux));
    return m_dbl(ux);
}

double fdim(double x, double y) {
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    return x > y ? x - y : 0;
}

// A NaN loses to a number, and -0 is below +0 -- IEEE 754's maxNum,
// which a plain comparison gets wrong for both.
double fmax(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (signbit(x) != signbit(y)) return signbit(x) ? y : x;
    return x < y ? y : x;
}

double fmin(double x, double y) {
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (signbit(x) != signbit(y)) return signbit(x) ? x : y;
    return x < y ? x : y;
}

// glibc's reading of the n-char-sequence: a whole number in strtoull's
// base-0 syntax is the payload, anything else is payload 0.
M_HIDDEN unsigned long long __m_nan_payload(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    return *end ? 0 : v;
}

double nan(const char *s) {
    return m_dbl(0x7FF8000000000000ull | (__m_nan_payload(s) & ((1ull << 51) - 1)));
}
