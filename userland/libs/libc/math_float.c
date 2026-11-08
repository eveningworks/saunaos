// Every float form of <math.h>, as the double function rounded once
// (fmodf, remainderf, remquof: math_rem.c; fmaf: math_fma.c).
//
// For the EXACT functions this is exact: a float widens to a double
// without loss, and floor, frexp, fdim and the rest give a result that
// is itself a float. For the APPROXIMATE ones it is the design glibc's
// own expf/logf/powf/sinf use internally: a double result carries 29
// bits more than a float needs, so rounding it to float is correctly
// rounded except where the true value lies within 2^-29 of a halfway
// point -- the harness measures at most 1 ulp, and mostly 0. A separate
// float polynomial set (musl's choice) would be faster and is a second
// thing to get wrong.
//
// nextafterf and nexttowardf are the exceptions: a step of one FLOAT
// representation cannot be taken in double, so they work on the bits.
#include "math_internal.h"

#define F1(name) float name##f(float x) { return (float)name(x); }
#define F2(name) float name##f(float x, float y) { return (float)name(x, y); }

F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan) F2(atan2)
F1(sinh) F1(cosh) F1(tanh) F1(asinh) F1(acosh) F1(atanh)
F1(exp) F1(exp2) F1(expm1) F1(log) F1(log2) F1(log10) F1(log1p) F1(logb)
F1(cbrt) F2(hypot) F2(pow)
F1(erf) F1(erfc) F1(lgamma) F1(tgamma)
F1(ceil) F1(floor) F1(trunc) F1(round) F1(rint) F1(nearbyint)
F2(fdim) F2(fmax) F2(fmin)

float lgammaf_r(float x, int *sg) { return (float)lgamma_r(x, sg); }
int ilogbf(float x) { return ilogb(x); }
float frexpf(float x, int *e) { return (float)frexp(x, e); }
// Exact in double over float's whole range, so rounded once, to float.
float scalbnf(float x, int n) {
    if (n > 300) n = 300;
    else if (n < -300) n = -300;
    return (float)scalbn(x, n);
}
float ldexpf(float x, int n) { return scalbnf(x, n); }
float scalblnf(float x, long n) { return scalbnf(x, n > 300 ? 300 : n < -300 ? -300 : (int)n); }
float modff(float x, float *iptr) {
    double i;
    float r = (float)modf(x, &i);
    *iptr = (float)i;
    return r;
}

long lrintf(float x) { long r; __asm__("cvtss2si %1, %0" : "=r"(r) : "x"(x)); return r; }
long long llrintf(float x) { return lrintf(x); }
long lroundf(float x) { return (long)round(x); }
long long llroundf(float x) { return (long long)round(x); }

float fabsf(float x) { return __builtin_fabsf(x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }

// sqrtss, correctly rounded by the hardware.
float sqrtf(float x) {
    if (x < 0.0f) return (x - x) / (x - x);
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

M_HIDDEN unsigned long long __m_nan_payload(const char *s);
float nanf(const char *s) {
    return m_flt(0x7FC00000u | (uint32_t)(__m_nan_payload(s) & ((1u << 22) - 1)));
}

float nextafterf(float x, float y) {
    uint32_t ux = m_fbits(x), uy = m_fbits(y), ax, ay;
    if (isnan(x) || isnan(y)) return x + y;
    if (ux == uy) return y;
    ax = ux & 0x7fffffff;
    ay = uy & 0x7fffffff;
    if (ax == 0) {
        if (ay == 0) return y;
        ux = (uy & 0x80000000) | 1;
    } else if (ax > ay || ((ux ^ uy) & 0x80000000)) {
        ux--;
    } else {
        ux++;
    }
    uint32_t e = ux & 0x7f800000;
    if (e == 0x7f800000) FORCE_EVAL(x + x);
    if (e == 0) FORCE_EVAL(x * x + m_flt(ux) * m_flt(ux));
    return m_flt(ux);
}

float nexttowardf(float x, long double y) {
    uint32_t ux = m_fbits(x);
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    if (x == 0) {
        ux = 1;
        if (signbit(y)) ux |= 0x80000000;
    } else if (x < y) {
        if (signbit(x)) ux--; else ux++;
    } else {
        if (signbit(x)) ux++; else ux--;
    }
    uint32_t e = ux & 0x7f800000;
    if (e == 0x7f800000) FORCE_EVAL(x + x);
    if (e == 0) FORCE_EVAL(x * x + m_flt(ux) * m_flt(ux));
    return m_flt(ux);
}
