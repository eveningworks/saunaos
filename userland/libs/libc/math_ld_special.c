// erfl, erfcl, lgammal and tgammal: THE DOUBLE FUNCTION, WIDENED, and
// long double arithmetic only where the result lies outside double's
// range -- erfc's tail past x ~ 26, gamma past 171.6 and below -184,
// lgamma past 2^1000, and arguments too small for a double.
//
// So these four are double precision, about 2^11 ulp of the long double
// result, where every other long double function is real 80-bit. A
// genuine 64-bit erf or lgamma is a coefficient set per interval that
// would have to be fitted -- no source for one is on this host, and
// fitting minimax rationals to 2^-64 is its own project -- and nothing
// in this tree asks for more than double here. The measured error is in
// docs/libc-design.md, and docs/decisions/build.md has the choice.
//
// THE ARGUMENT IS NOT ROUNDED AWAY. A long double x is xd + d with xd
// its nearest double, and f(x) = f(xd) + f'(xd)*d to far below a
// double's last place; the derivative term is what keeps lgammal(1 +
// 2^-63) the tiny negative number it is instead of lgamma(1) = 0, and
// tgammal(-1 + 2^-64) finite instead of a pole. f' is exact for erf
// and is the digamma function for the gamma pair.
#include "math_internal.h"
#include "math_tables.h"

static const long double PIL = 3.14159265358979323846264338327950288L;
static const long double HALF_LOG_2PI = 0.918938533204672741780329736405617640L;
static const long double TWO_OVER_SQRTPI = 1.12837916709551257389615890312154517L;

// psi(x) = gamma'(x)/gamma(x), only ever the coefficient of a
// correction below a double's last place -- but in long double, because
// near lgamma's zeros at 1 and 2 that correction IS the result, and
// psi's own cancellation (-1 - 1/2 - ... + log 10) costs a double ~2^-50.
static long double digamma(long double x) {
    long double r = 0;
    if (x <= 0)   // reflection: psi(x) = psi(1-x) - pi/tan(pi x)
        return digamma(1 - x) - PIL / tanl(PIL * x);
    while (x < 10) { r -= 1 / x; x += 1; }
    long double f = 1 / (x * x);
    return r + logl(x) - 0.5L / x -
           f * (1.0L / 12 - f * (1.0L / 120 - f * (1.0L / 252 - f * (1.0L / 240 - f * (1.0L / 132 -
           f * (691.0L / 32760 - f * (1.0L / 12)))))));
}

long double erfl(long double x) {
    if (fabsl(x) < 0x1p-60L) return x * TWO_OVER_SQRTPI;
    if (isnan(x)) return x + x;
    if (fabsl(x) >= 8) return x > 0 ? 1 - 0x1p-100L : -1 + 0x1p-100L;   // +-1, inexact
    double xd = (double)x;
    long double d = x - xd;
    return erf(xd) + TWO_OVER_SQRTPI * expl(-(long double)xd * xd) * d;
}

// erfc(x) ~ e^(-x^2) / (x sqrt(pi)) * (1 - 1/(2x^2) + 3/(2x^2)^2 - ...),
// with x^2 formed exactly (hi + lo) so the exponential sees no rounding
// of it. At x >= 26 the series' terms fall by a factor of ~1350 each.
static long double erfc_tail(long double x) {
    long double h, l;
    ld_two_prod(x, x, &h, &l);
    long double w = 1 / (2 * h), term = 1, sum = 1;
    for (int k = 1; k <= 8; k++) {
        term *= -(2 * k - 1) * w;
        sum += term;
    }
    long double e = expl(-h) * (1 - l);
    return e * sum / (x * 1.77245385090551602729816748334114518L);   // sqrt(pi)
}

long double erfcl(long double x) {
    if (x > 110) return 0x1p-16382L * 0x1p-16382L;   // underflow (or +inf -> 0)
    if (x >= 26) return erfc_tail(x);
    if (isnan(x)) return x + x;
    if (x <= -8) return 2 - 0x1p-100L;   // 2, inexact
    double xd = (double)x;
    long double d = x - xd;
    return erfc(xd) - TWO_OVER_SQRTPI * expl(-(long double)xd * xd) * d;
}

// Stirling's series for log gamma(x), x large, in long double.
static long double lgamma_stirling(long double x) {
    long double z = 1 / x, z2 = z * z;
    long double s = z * (1.0L / 12 - z2 * (1.0L / 360 - z2 * (1.0L / 1260 - z2 * (1.0L / 1680))));
    return (x - 0.5L) * (logl(x) - 1) + (HALF_LOG_2PI - 0.5L) + s;
}

// sin(pi*x) for a long double x: x - k for the nearest integer k is
// exact, and pi is applied only to that -- pi*x rounded first would be
// pi*x's rounding error, which near an integer is the whole answer.
static long double sinpil(long double x) {
    long double r = fmodl(x, 2.0L);   // exact, (-2, 2)
    long double k = roundl(r), f = r - k;
    long double s = sinl(PIL * f);
    return fmodl(k, 2.0L) != 0 ? -s : s;
}

// A negative x whose nearest double is an integer: the double functions
// would see a pole that x is not on. Reflected into x > 0 instead.
static int near_pole(long double x) {
    double xd = (double)x;
    return x < 0 && xd == floor(xd) && (long double)xd != x;
}

long double lgammal_r(long double x, int *sg) {
    long double a = fabsl(x);
    *sg = 1;
    if (isnan(x)) return x + x;
    if (a < 0x1p-1000L) {   // below double's range: -log|x|
        if (x == 0) return 1 / a;   // pole
        if (x < 0) *sg = -1;
        return -logl(a);
    }
    if (near_pole(x)) {   // log|gamma(x)| = log(pi / |sin(pi x)|) - lgamma(1 - x)
        long double s = sinpil(x);
        int sg2;
        long double r = logl(PIL / fabsl(s)) - lgammal_r(1 - x, &sg2);
        *sg = s > 0 ? 1 : -1;
        return r;
    }
    if (a < 0x1p1000L) {
        double xd = (double)x;
        long double r = lgamma_r(xd, sg);
        if (isfinite(r)) r += digamma(xd) * (x - xd);
        return r;
    }
    if (isinf(x)) return INFINITY;
    if (x < 0) return 1 / (x - x);   // every long double this large is an integer: a pole
    return x * (logl(x) - 1);
}

long double lgammal(long double x) { return lgammal_r(x, &signgam); }

long double tgammal(long double x) {
    if (isnan(x)) return x + x;
    if (x == INFINITY) return x;
    if (x == -INFINITY) return (x - x) / (x - x);
    if (fabsl(x) < 0x1p-1000L) return 1 / x;   // 1/x, a pole at +-0
    if (x < 0 && truncl(x) == x) return (x - x) / (x - x);   // a pole: invalid
    if (near_pole(x) && x > -169)   // gamma(x) = pi / (sin(pi x) gamma(1 - x))
        return PIL / (sinpil(x) * tgammal(1 - x));
    if (x > -169 && x < 171.5L) {   // where the double result neither over- nor underflows
        double xd = (double)x;
        long double r = tgamma(xd);
        if (isfinite(r) && r != 0) r += r * digamma(xd) * (x - xd);
        return r;
    }
    if (x > 0) {
        if (x > 1755.5L) return 0x1p16383L * 0x1p16383L;   // overflow
        return expl(lgamma_stirling(x));
    }
    // x <= -169: gamma(x) = -pi / (x sin(pi x) gamma(-x)), as logs.
    long double s = sinpil(x);
    if (x < -1800) return s > 0 ? 0x1p-16382L * 0x1p-16382L : -(0x1p-16382L * 0x1p-16382L);
    long double r = expl(logl(PIL / fabsl(x * s)) - lgamma_stirling(-x));
    return s > 0 ? r : -r;
}
