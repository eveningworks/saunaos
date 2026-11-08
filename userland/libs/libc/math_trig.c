// sin, cos, tan and the inverse trig functions, in double.
//
// fdlibm's algorithms and coefficients, the set glibc's and musl's
// descend from (musl's shape, written from it). Each forward function is
// math_pio2.c's exact reduction to [-pi/4, pi/4] plus a kernel that
// takes the reduced argument as TWO doubles, so the bits the reduction
// kept below the first double's last place still reach the answer --
// that tail is the difference between under one ulp and several.
//
// **THE COEFFICIENTS ARE MINIMAX, NOT TAYLOR**: fitted to minimise the
// worst error over the whole reduced interval, where a Taylor series is
// best at 0 and worst at the ends. Do not "simplify" them into
// factorials. Their accuracy is measured, per function, by
// tools/tests/sweep/libc_diff.py --only math against the host's glibc.
//
// **WHY NOT THE x87's fsin/fcos/fptan**: Intel documents their built-in
// reduction as using a 66-bit pi, so they lose accuracy near multiples
// of pi/2 and for large arguments; and emulators have computed them
// through the host's double sin(). Every serious libm left them.
#include "math_internal.h"

// --- the kernels, |x| <= ~pi/4, x + y the reduced argument ------------

// sin(x+y); iy = 0 says y is zero and its terms can go.
double __m_sin(double x, double y, int iy) {
    static const double
        S1 = -1.66666666666666324348e-01,
        S2 =  8.33333333332248946124e-03,
        S3 = -1.98412698298579493134e-04,
        S4 =  2.75573137070700676789e-06,
        S5 = -2.50507602534068634195e-08,
        S6 =  1.58969099521155010221e-10;
    double z = x * x, w = z * z;
    double r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    double v = z * x;
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

// cos(x+y). 1 - x^2/2 is formed as w plus the rounding error of w, so
// the large first terms cost no precision.
double __m_cos(double x, double y) {
    static const double
        C1 =  4.16666666666666019037e-02,
        C2 = -1.38888888888741095749e-03,
        C3 =  2.48015872894767294178e-05,
        C4 = -2.75573143513906633035e-07,
        C5 =  2.08757232129817482790e-09,
        C6 = -1.13596475577881948265e-11;
    double z = x * x, w = z * z;
    double r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    double hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

// tan(x+y), or -1/tan(x+y) when odd. Above 0.6744 the argument is
// reflected about pi/4, where tan is near 1, so the polynomial never
// sees the steep part.
double __m_tan(double x, double y, int odd) {
    static const double T[] = {
         3.33333333333334091986e-01,
         1.33333333333201242699e-01,
         5.39682539762260521377e-02,
         2.18694882948595424599e-02,
         8.86323982359930005737e-03,
         3.59207910759131235356e-03,
         1.45620945432529025516e-03,
         5.88041240820264096874e-04,
         2.46463134818469906812e-04,
         7.81794442939557092300e-05,
         7.14072491382608190305e-05,
        -1.85586374855275456654e-05,
         2.59073051863633712884e-05,
    };
    static const double pio4 = 7.85398163397448278999e-01, pio4lo = 3.06161699786838301793e-17;
    uint32_t hx = m_hi(x);
    int big = (hx & 0x7fffffff) >= 0x3FE59428;
    int sign = 0;
    if (big) {
        sign = hx >> 31;
        if (sign) { x = -x; y = -y; }
        x = (pio4 - x) + (pio4lo - y);
        y = 0.0;
    }
    double z = x * x, w = z * z;
    double r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    double v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    double s = z * x;
    r = y + z * (s * (r + v) + y) + s * T[0];
    w = x + r;
    if (big) {
        s = 1 - 2 * odd;
        v = s - 2.0 * (x + (r - w * w / (w + s)));
        return sign ? -v : v;
    }
    if (!odd) return w;
    // -1/(x+r) straight has up to 2 ulp of error; a split division
    // with a correction step does not.
    double w0 = m_trunc32(w);
    v = r - (w0 - x);
    double a = -1.0 / w, a0 = m_trunc32(a);
    return a0 + a * (1.0 + a0 * w0 + a0 * v);
}

// --- sin, cos, tan ----------------------------------------------------

double sin(double x) {
    uint32_t ix = m_hi(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {   // |x| ~<= pi/4
        if (ix < 0x3e500000) {   // |x| < 2^-26: sin(x) = x, inexact unless 0
            if (ix < 0x00100000) FORCE_EVAL(x * 0x1p-120);
            return x;
        }
        return __m_sin(x, 0.0, 0);
    }
    if (ix >= 0x7ff00000) return x - x;   // NaN, and invalid for an infinity
    double y[2];
    switch (__m_rem_pio2(x, y)) {
    case 0:  return  __m_sin(y[0], y[1], 1);
    case 1:  return  __m_cos(y[0], y[1]);
    case 2:  return -__m_sin(y[0], y[1], 1);
    default: return -__m_cos(y[0], y[1]);
    }
}

double cos(double x) {
    uint32_t ix = m_hi(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        if (ix < 0x3e46a09e) {   // |x| < 2^-27 * sqrt(2): cos(x) = 1, inexact unless 0
            FORCE_EVAL(x + 0x1p120);
            return 1.0;
        }
        return __m_cos(x, 0);
    }
    if (ix >= 0x7ff00000) return x - x;
    double y[2];
    switch (__m_rem_pio2(x, y)) {
    case 0:  return  __m_cos(y[0], y[1]);
    case 1:  return -__m_sin(y[0], y[1], 1);
    case 2:  return -__m_cos(y[0], y[1]);
    default: return  __m_sin(y[0], y[1], 1);
    }
}

double tan(double x) {
    uint32_t ix = m_hi(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        if (ix < 0x3e400000) {   // |x| < 2^-27
            FORCE_EVAL(ix < 0x00100000 ? x / 0x1p120f : x + 0x1p120f);
            return x;
        }
        return __m_tan(x, 0.0, 0);
    }
    if (ix >= 0x7ff00000) return x - x;
    double y[2];
    int n = __m_rem_pio2(x, y);
    return __m_tan(y[0], y[1], n & 1);
}

// --- the inverse trig -------------------------------------------------

static const double
    pio2_hi = 1.57079632679489655800e+00,
    pio2_lo = 6.12323399573676603587e-17;

// asin(x) = x + x^3 * R(x^2) on [0, 0.5], R a rational minimax fit.
static double asin_r(double z) {
    static const double
        pS0 =  1.66666666666666657415e-01,
        pS1 = -3.25565818622400915405e-01,
        pS2 =  2.01212532134862925881e-01,
        pS3 = -4.00555345006794114027e-02,
        pS4 =  7.91534994289814532176e-04,
        pS5 =  3.47933107596021167570e-05,
        qS1 = -2.40339491173441421878e+00,
        qS2 =  2.02094576023350569471e+00,
        qS3 = -6.88283971605453293030e-01,
        qS4 =  7.70381505559019352791e-02;
    double p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    double q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    return p / q;
}

double asin(double x) {
    uint32_t hx = m_hi(x), ix = hx & 0x7fffffff;
    if (ix >= 0x3ff00000) {   // |x| >= 1 or NaN
        if (((ix - 0x3ff00000) | m_lo(x)) == 0) return x * pio2_hi + 0x1p-120f;
        return 0 / (x - x);
    }
    if (ix < 0x3fe00000) {   // |x| < 0.5
        if (ix < 0x3e500000 && ix >= 0x00100000) return x;   // tiny: x, inexact
        return x + x * asin_r(x * x);
    }
    // asin(x) = pi/2 - 2*asin(sqrt((1-|x|)/2)), with the square root
    // split so its square is exact when |x| is not near 1.
    double z = (1 - fabs(x)) * 0.5;
    double s = sqrt(z), r = asin_r(z);
    if (ix >= 0x3fef3333) {   // |x| > 0.975
        x = pio2_hi - (2 * (s + s * r) - pio2_lo);
    } else {
        double f = m_trunc32(s);
        double c = (z - f * f) / (s + f);
        x = 0.5 * pio2_hi - (2 * s * r - (pio2_lo - 2 * c) - (0.5 * pio2_hi - 2 * f));
    }
    return (hx >> 31) ? -x : x;
}

double acos(double x) {
    uint32_t hx = m_hi(x), ix = hx & 0x7fffffff;
    if (ix >= 0x3ff00000) {
        if (((ix - 0x3ff00000) | m_lo(x)) == 0) {
            if (hx >> 31) return 2 * pio2_hi + 0x1p-120f;
            return 0;
        }
        return 0 / (x - x);
    }
    if (ix < 0x3fe00000) {   // |x| < 0.5
        if (ix <= 0x3c600000) return pio2_hi + 0x1p-120f;   // |x| < 2^-57
        return pio2_hi - (x - (pio2_lo - x * asin_r(x * x)));
    }
    if (hx >> 31) {   // x < -0.5: acos(x) = pi - 2*asin(sqrt((1+x)/2))
        double z = (1.0 + x) * 0.5;
        double s = sqrt(z);
        double w = asin_r(z) * s - pio2_lo;
        return 2 * (pio2_hi - (s + w));
    }
    // x > 0.5: acos(x) = 2*asin(sqrt((1-x)/2))
    double z = (1.0 - x) * 0.5;
    double s = sqrt(z);
    double df = m_trunc32(s);
    double c = (z - df * df) / (s + df);
    double w = asin_r(z) * s + c;
    return 2 * (df + w);
}

double atan(double x) {
    static const double atanhi[] = {
        4.63647609000806093515e-01,   // atan(0.5)
        7.85398163397448278999e-01,   // atan(1)
        9.82793723247329054082e-01,   // atan(1.5)
        1.57079632679489655800e+00,   // atan(inf)
    };
    static const double atanlo[] = {
        2.26987774529616870924e-17,
        3.06161699786838301793e-17,
        1.39033110312309984516e-17,
        6.12323399573676603587e-17,
    };
    static const double aT[] = {
         3.33333333333329318027e-01,
        -1.99999999998764832476e-01,
         1.42857142725034663711e-01,
        -1.11111104054623557880e-01,
         9.09088713343650656196e-02,
        -7.69187620504482999495e-02,
         6.66107313738753120669e-02,
        -5.83357013379057348645e-02,
         4.97687799461593236017e-02,
        -3.65315727442169155270e-02,
         1.62858201153657823623e-02,
    };
    uint32_t hx = m_hi(x), ix = hx & 0x7fffffff;
    int sign = hx >> 31, id;
    if (ix >= 0x44100000) {   // |x| >= 2^66, or NaN
        if (isnan(x)) return x;
        double z = atanhi[3] + 0x1p-120f;
        return sign ? -z : z;
    }
    if (ix < 0x3fdc0000) {   // |x| < 0.4375
        if (ix < 0x3e400000) {   // |x| < 2^-27
            if (ix < 0x00100000) FORCE_EVAL((float)x);
            return x;
        }
        id = -1;
    } else {
        // Reduce to |x| < 0.4375 about the nearest of 0.5, 1, 1.5 and
        // infinity, where atan's value is known to double-double.
        x = fabs(x);
        if (ix < 0x3ff30000) {          // |x| < 1.1875
            if (ix < 0x3fe60000) { id = 0; x = (2.0 * x - 1.0) / (2.0 + x); }   // 7/16 <= |x| < 11/16
            else                 { id = 1; x = (x - 1.0) / (x + 1.0); }
        } else {
            if (ix < 0x40038000) { id = 2; x = (x - 1.5) / (1.0 + 1.5 * x); }   // |x| < 2.4375
            else                 { id = 3; x = -1.0 / x; }
        }
    }
    double z = x * x, w = z * z;
    double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - (x * (s1 + s2) - atanlo[id] - x);
    return sign ? -z : z;
}

double atan2(double y, double x) {
    static const double pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    if (isnan(x) || isnan(y)) return x + y;
    uint32_t ix = m_hi(x), lx = m_lo(x), iy = m_hi(y), ly = m_lo(y);
    if (((ix - 0x3ff00000) | lx) == 0) return atan(y);   // x == 1
    int m = (int)(((iy >> 31) & 1) | ((ix >> 30) & 2));   // 2*sign(x) + sign(y)
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;
    if ((iy | ly) == 0) {   // y = +-0
        switch (m) {
        case 0: case 1: return y;
        case 2: return pi;
        default: return -pi;
        }
    }
    if ((ix | lx) == 0) return (m & 1) ? -pi / 2 : pi / 2;   // x = +-0
    if (ix == 0x7ff00000) {   // x = +-inf
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0: return pi / 4;
            case 1: return -pi / 4;
            case 2: return 3 * pi / 4;
            default: return -3 * pi / 4;
            }
        }
        switch (m) {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return pi;
        default: return -pi;
        }
    }
    // |y/x| > 2^64, or y infinite: the answer is +-pi/2 to the last bit.
    if (ix + (64 << 20) < iy || iy == 0x7ff00000) return (m & 1) ? -pi / 2 : pi / 2;
    double z;
    if ((m & 2) && iy + (64 << 20) < ix) z = 0;   // |y/x| < 2^-64, x < 0
    else z = atan(fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}
