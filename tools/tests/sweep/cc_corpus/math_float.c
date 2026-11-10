/* <math.h>, <float.h> and <fenv.h>: the functions, the limits, rounding, and IEEE edge cases. */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static volatile double vzero = 0.0;

int main(void) {
    printf("sqrt %.17g cbrt %.17g hypot %.17g\n", sqrt(2.0), cbrt(-27.0), hypot(3e200, 4e200));
    printf("exp %.17g log %.17g log10 %.17g log2 %.17g\n", exp(1.0), log(10.0), log10(2.0), log2(3.0));
    printf("pow %.17g %.17g %.17g\n", pow(2.0, 0.5), pow(-8.0, 3.0), pow(10.0, -3.0));
    printf("sin %.17g cos %.17g tan %.17g\n", sin(1.0), cos(1.0), tan(0.5));
    printf("asin %.17g acos %.17g atan %.17g atan2 %.17g\n", asin(0.5), acos(-0.5), atan(1.0), atan2(-1.0, -1.0));
    printf("sinh %.17g cosh %.17g tanh %.17g\n", sinh(1.0), cosh(1.0), tanh(0.5));
    printf("floor %g ceil %g trunc %g round %g rint %g nearbyint %g\n", floor(-2.5), ceil(-2.5), trunc(-2.7),
           round(2.5), rint(2.5), nearbyint(3.5));
    printf("fmod %g remainder %g modf ", fmod(7.5, 2.0), remainder(7.5, 2.0));
    double ip;
    double fp = modf(-3.75, &ip);
    printf("%g %g frexp ", ip, fp);
    int e;
    double m = frexp(48.0, &e);
    printf("%g %d ldexp %g\n", m, e, ldexp(0.75, 4));
    printf("fabs %g copysign %g fmax %g fmin %g fdim %g fma %.17g\n", fabs(-0.0), copysign(3.0, -0.0),
           fmax(1.0, NAN), fmin(-1.0, 2.0), fdim(5.0, 7.0), fma(0.1, 10.0, -1.0));
    printf("lround %ld llrint %lld ilogb %d logb %g expm1 %.17g log1p %.17g\n", lround(-2.5), llrint(2.5),
           ilogb(1000.0), logb(0.1), expm1(1e-10), log1p(1e-10));
    printf("erf %.17g tgamma %.17g lgamma %.17g\n", erf(0.5), tgamma(5.0), lgamma(10.0));
    printf("float sqrtf %.9g expf %.9g sinf %.9g powf %.9g\n", (double)sqrtf(2.0f), (double)expf(1.0f),
           (double)sinf(1.0f), (double)powf(2.0f, 10.0f));

    double inf = 1.0 / vzero, nan = vzero / vzero;
    printf("isinf %d isnan %d isfinite %d signbit %d fpclassify %d %d %d\n", isinf(inf) != 0, isnan(nan) != 0,
           isfinite(1.0) != 0, signbit(-0.0) != 0, fpclassify(0.0) == FP_ZERO, fpclassify(DBL_MIN / 2) == FP_SUBNORMAL,
           fpclassify(inf) == FP_INFINITE);
    printf("nan==nan %d inf-inf nan %d\n", nan == nan, isnan(inf - inf) != 0);
    printf("DBL_EPSILON %.17g DBL_MAX %.17g DBL_MIN %.17g FLT_MAX %.9g DBL_DIG %d LDBL_MANT_DIG %d\n",
           DBL_EPSILON, DBL_MAX, DBL_MIN, (double)FLT_MAX, DBL_DIG, LDBL_MANT_DIG);
    printf("nextafter %.17g\n", nextafter(1.0, 2.0) - 1.0);

    int modes[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    const char *names[] = { "nearest", "upward", "downward", "towardzero" };
    for (int i = 0; i < 4; i++) {
        fesetround(modes[i]);
        volatile double a = 1.0, b = 3.0;
        printf("round %s %.17g %g\n", names[i], a / b, rint(-1.5));
    }
    fesetround(FE_TONEAREST);
    feclearexcept(FE_ALL_EXCEPT);
    volatile double t = 1.0 / vzero;
    (void)t;
    printf("divbyzero raised %d\n", fetestexcept(FE_DIVBYZERO) != 0);

    long double ld = 1.0L / 3.0L;
    printf("long double %.21Lg %.21Lg\n", ld, sqrtl(2.0L));
    double acc = 0;
    for (int i = 1; i <= 100000; i++) acc += 1.0 / ((double)i * i);
    printf("basel %.17g\n", acc);
    return 0;
}
