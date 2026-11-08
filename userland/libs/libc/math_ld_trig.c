// The long double trigonometric functions.
//
// sinl, cosl and tanl are OUR reduction (math_pio2.c's Payne-Hanek, to
// ~120 bits) and polynomials in long double -- NOT fsin/fcos/fptan,
// whose built-in reduction uses a 66-bit pi and loses accuracy near
// multiples of pi/2, and which emulators have computed through a host
// double. The polynomials are plain Taylor terms, deliberately: on
// |x| <= pi/4 the series to x^21 (sin) and x^22 (cos) is already below
// 2^-82 relative, so a minimax fit would save two or three terms and add
// a coefficient table nobody here can regenerate. tanl is their ratio.
//
// The inverse functions are fpatan, which IS accurate (atan2 with C's
// quadrant and signed-zero rules in one instruction), as musl's are.
#include "math_internal.h"

long double __m_sinl(long double x, long double y) {
    static const long double
        S1 = -1.0L / 6, S2 = 1.0L / 120, S3 = -1.0L / 5040, S4 = 1.0L / 362880,
        S5 = -1.0L / 39916800, S6 = 1.0L / 6227020800.0L,
        S7 = -1.0L / 1307674368000.0L, S8 = 1.0L / 355687428096000.0L,
        S9 = -1.0L / 121645100408832000.0L, S10 = 1.0L / 51090942171709440000.0L;
    long double z = x * x, v = z * x;
    long double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * (S6 + z * (S7 + z * (S8 + z * (S9 + z * S10)))))));
    // sin(x+y) ~ sin(x) + y*(1 - x^2/2), the small terms first.
    return x - ((z * (0.5L * y - v * r) - y) - v * S1);
}

long double __m_cosl(long double x, long double y) {
    static const long double
        C1 = 1.0L / 24, C2 = -1.0L / 720, C3 = 1.0L / 40320, C4 = -1.0L / 3628800,
        C5 = 1.0L / 479001600, C6 = -1.0L / 87178291200.0L,
        C7 = 1.0L / 20922789888000.0L, C8 = -1.0L / 6402373705728000.0L,
        C9 = 1.0L / 2432902008176640000.0L, C10 = -1.0L / 1124000727777607680000.0L;
    long double z = x * x;
    long double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * (C6 + z * (C7 + z * (C8 + z * (C9 + z * C10)))))))));
    // 1 - x^2/2 as w plus its rounding error, so the big terms cost nothing.
    long double hz = 0.5L * z, w = 1.0L - hz;
    return w + (((1.0L - w) - hz) + (z * r - x * y));
}

#define PIO4L 0.785398163397448309615660845819875721L

long double sinl(long double x) {
    if (!isfinite(x)) return x - x;
    if (fabsl(x) <= PIO4L) {
        if (fabsl(x) < 0x1p-33L) return x;
        return __m_sinl(x, 0);
    }
    long double hi, lo;
    switch (__m_rem_pio2l(x, &hi, &lo)) {
    case 0:  return  __m_sinl(hi, lo);
    case 1:  return  __m_cosl(hi, lo);
    case 2:  return -__m_sinl(hi, lo);
    default: return -__m_cosl(hi, lo);
    }
}

long double cosl(long double x) {
    if (!isfinite(x)) return x - x;
    if (fabsl(x) <= PIO4L) {
        if (fabsl(x) < 0x1p-33L) return 1;
        return __m_cosl(x, 0);
    }
    long double hi, lo;
    switch (__m_rem_pio2l(x, &hi, &lo)) {
    case 0:  return  __m_cosl(hi, lo);
    case 1:  return -__m_sinl(hi, lo);
    case 2:  return -__m_cosl(hi, lo);
    default: return  __m_sinl(hi, lo);
    }
}

long double tanl(long double x) {
    if (!isfinite(x)) return x - x;
    long double hi = x, lo = 0;
    int n = 0;
    if (fabsl(x) <= PIO4L) {
        if (fabsl(x) < 0x1p-33L) return x;
    } else {
        n = __m_rem_pio2l(x, &hi, &lo);
    }
    long double s = __m_sinl(hi, lo), c = __m_cosl(hi, lo);
    return (n & 1) ? -c / s : s / c;
}

long double atan2l(long double y, long double x) { return x87_fpatan(y, x); }
long double atanl(long double x) { return x87_fpatan(x, 1.0L); }

// asin x = atan2(x, sqrt((1-x)(1+x))): 1-x is exact near 1, where
// 1 - x*x would round away what is left. Out of [-1, 1] the square
// root is a NaN, with invalid.
long double asinl(long double x) {
    if (isnan(x)) return x + x;
    return x87_fpatan(x, x87_fsqrt((1 - x) * (1 + x)));
}

long double acosl(long double x) {
    if (isnan(x)) return x + x;
    return x87_fpatan(x87_fsqrt((1 - x) * (1 + x)), x);
}
