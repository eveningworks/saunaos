#ifndef ULIB_MATH_H
#define ULIB_MATH_H

// C99's <math.h>, whole: every function in its double, float and long
// double form, the classification and comparison macros, and the
// constants. Ring 3 only -- the kernel is built -mno-sse and has
// kernel/include/api/fixed.h (Q16.16, angles in TURNS) instead; the two
// never meet.
//
// WHERE EACH PART LIVES, split by how it can be wrong:
//   math.c        EXACT: rounding to integers, frexp/ldexp/scalbn/ilogb/
//                 logb, nextafter, fdim/fmax/fmin, nan. Bit reasoning;
//                 right or wrong with nothing in between.
//   math_rem.c    EXACT: fmod, remainder, remquo, by integer long division
//   math_fma.c    EXACT: fma, one rounding, by integer arithmetic
//   math_pio2.c   x mod pi/2 for ANY finite x (Payne-Hanek, against 2/pi
//                 to 16,640 bits), the argument reduction sin/cos/tan use
//   math_trig.c   sin cos tan asin acos atan atan2
//   math_exp.c    exp exp2 expm1 log log2 log10 log1p pow cbrt hypot,
//                 the hyperbolics and their inverses
//   math_special.c  erf erfc lgamma tgamma
//   math_float.c  every float form
//   math_ld.c, math_ld_trig.c  every long double form
//
// ACCURACY IS MEASURED, NOT CLAIMED. tools/tests/sweep/libc_diff.py
// --only math compares every function against the host's glibc over a
// value set that includes the edges, reports the worst error in ULPs per
// function, and fails above a stated bound. In brief:
//   - double: fdlibm's algorithms and coefficients (glibc and musl both
//     descend from them): within 1 ulp of glibc for the elementary
//     functions, 2 for erfc and lgamma, 6 for tgamma -- and lgamma's
//     relative error grows without bound near its zeros on the negative
//     axis, as fdlibm's and musl's does.
//   - float: the double function, rounded once. <= 1 ulp, what glibc's
//     own sinf/expf/powf do internally.
//   - long double: REAL 80-BIT where the x87 has an instruction for the
//     job (f2xm1, fyl2x, fyl2xp1, fpatan, fsqrt, frndint -- musl's choice
//     on x86-64), C over long double for sin/cos/tan (own reduction,
//     never fsin) and the rest; a few ulp of the 64-bit mantissa.
//     erfl, erfcl, lgammal and tgammal are the DOUBLE function widened,
//     with only the range past double's done in long double: 2^11 to
//     2^15 ulp of the long double result, i.e. double precision.
//     docs/decisions/build.md, "tolibc's long double maths is the x87's,
//     and its four special functions are double precision".
//
// ERRORS: math_errhandling is MATH_ERREXCEPT, as musl's is -- a domain
// error raises FE_INVALID, a pole FE_DIVBYZERO, and so on (<fenv.h>
// tests them); errno is NOT set, except by sqrt(), which GCC's own code
// for a negative argument calls expecting it.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The X/Open constants. Not in C99 -- glibc, musl and the BSDs all
// provide them anyway, and tolibc's bar is COMPLETENESS rather than
// this project's usual second-real-caller rule (docs/libc-design.md).
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define MATH_ERRNO       1
#define MATH_ERREXCEPT   2
#define math_errhandling MATH_ERREXCEPT

// What ilogb() returns for 0 and for a NaN: INT_MIN for both, glibc's
// and musl's value on x86.
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

// Bit tests, not comparisons: a comparison is something an optimiser is
// entitled to reason about, and these have to keep working regardless.
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)

// C99's classification and the comparisons that never raise "invalid"
// on a NaN -- the compiler's own, as glibc's are.
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))
#define isnormal(x)          __builtin_isnormal(x)
#define isgreater(x, y)      __builtin_isgreater((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal((x), (y))
#define isless(x, y)         __builtin_isless((x), (y))
#define islessequal(x, y)    __builtin_islessequal((x), (y))
#define islessgreater(x, y)  __builtin_islessgreater((x), (y))
#define isunordered(x, y)    __builtin_isunordered((x), (y))

// The types arithmetic is evaluated in: x86-64 does float in float and
// double in double (FLT_EVAL_METHOD 0).
typedef float  float_t;
typedef double double_t;

// Each function in its three forms, one line apiece. The long double
// forms work on the x87's 80-bit type.
#define M_DECL1(name) double name(double); float name##f(float); long double name##l(long double)
#define M_DECL2(name) double name(double, double); float name##f(float, float); \
                      long double name##l(long double, long double)

// --- trigonometric ----------------------------------------------------
M_DECL1(sin); M_DECL1(cos); M_DECL1(tan);
M_DECL1(asin); M_DECL1(acos); M_DECL1(atan);
// The quadrant-aware inverse tangent: atan(y/x) cannot tell (1,1) from
// (-1,-1) and this can, which is why every polar conversion uses it.
M_DECL2(atan2);

// --- hyperbolic -------------------------------------------------------
M_DECL1(sinh); M_DECL1(cosh); M_DECL1(tanh);
M_DECL1(asinh); M_DECL1(acosh); M_DECL1(atanh);

// --- exponential and logarithmic --------------------------------------
M_DECL1(exp); M_DECL1(exp2);
// e^x - 1 and log(1 + x), accurate where x is near 0 and the obvious
// forms lose every digit to the 1.
M_DECL1(expm1); M_DECL1(log1p);
M_DECL1(log); M_DECL1(log2); M_DECL1(log10);
// The exponent of x as an integer / as a floating value.
int ilogb(double); int ilogbf(float); int ilogbl(long double);
M_DECL1(logb);
// x * 2^e, and the reverse: frexp writes the exponent and returns a
// mantissa in [0.5, 1).
double ldexp(double, int); float ldexpf(float, int); long double ldexpl(long double, int);
double frexp(double, int *); float frexpf(float, int *); long double frexpl(long double, int *);
double scalbn(double, int); float scalbnf(float, int); long double scalbnl(long double, int);
double scalbln(double, long); float scalblnf(float, long); long double scalblnl(long double, long);
// Splits into integer and fractional parts; the fraction keeps the
// sign, so modf(-0.5) gives -0.5 and -0.0.
double modf(double, double *); float modff(float, float *); long double modfl(long double, long double *);

// --- power and absolute value -----------------------------------------
M_DECL1(fabs); M_DECL1(sqrt); M_DECL1(cbrt);
// sqrt(x*x + y*y) without the overflow or the rounding of the sum.
M_DECL2(hypot);
// An integer y is handled exactly as C asks -- a negative x works, and
// pow(2, 10) is 1024 to the bit.
M_DECL2(pow);

// --- error and gamma --------------------------------------------------
M_DECL1(erf); M_DECL1(erfc); M_DECL1(lgamma); M_DECL1(tgamma);
// lgamma() writes the sign of gamma(x) here (POSIX); the _r forms take
// a pointer instead, for a caller that shares nothing between threads.
extern int signgam;
double lgamma_r(double, int *); float lgammaf_r(float, int *); long double lgammal_r(long double, int *);

// --- nearest integer --------------------------------------------------
M_DECL1(ceil); M_DECL1(floor); M_DECL1(trunc);
// Half away from zero -- NOT the hardware's half to even, which is
// rint() in the default rounding mode.
M_DECL1(round);
// In the CURRENT rounding mode; nearbyint() never raises FE_INEXACT.
M_DECL1(rint); M_DECL1(nearbyint);
long lrint(double); long lrintf(float); long lrintl(long double);
long long llrint(double); long long llrintf(float); long long llrintl(long double);
// Out of range is unspecified in C; here it is LONG_MIN, the hardware's.
long lround(double); long lroundf(float); long lroundl(long double);
long long llround(double); long long llroundf(float); long long llroundl(long double);

// --- remainder --------------------------------------------------------
// fmod truncates the quotient, remainder rounds it to nearest-even;
// both are EXACT. remquo also stores the quotient's sign and low bits.
M_DECL2(fmod); M_DECL2(remainder);
double remquo(double, double, int *); float remquof(float, float, int *);
long double remquol(long double, long double, int *);

// --- manipulation -----------------------------------------------------
M_DECL2(copysign);
// A quiet NaN; a numeric string ("0x12") becomes its payload, as glibc's.
double nan(const char *); float nanf(const char *); long double nanl(const char *);
M_DECL2(nextafter);
double nexttoward(double, long double); float nexttowardf(float, long double);
long double nexttowardl(long double, long double);

// --- maximum, minimum, positive difference, multiply-add -------------
// fmax/fmin return the number when the other argument is a NaN.
M_DECL2(fdim); M_DECL2(fmax); M_DECL2(fmin);
// x*y + z with ONE rounding. Computed exactly in integers -- there is
// no FMA instruction in the x86-64 baseline -- so it is correct, not fast.
double fma(double, double, double); float fmaf(float, float, float);
long double fmal(long double, long double, long double);

#undef M_DECL1
#undef M_DECL2

// THE TREE'S OWN C IS FREESTANDING (-ffreestanding, so -fno-builtin),
// where GCC would compile sqrt() as a call. These three were inline
// functions before they were extern ones, and every program in the
// tree was built expecting `sqrtsd`: a macro keeps that. A hosted
// compile (the cross gcc, or any C++) treats the names as builtins on
// its own and sees no macro. C allows a library function to be a macro
// too; `(sqrt)(x)` and `&sqrt` still reach the real one.
#if !defined(__cplusplus) && !__STDC_HOSTED__
#define sqrt(x)        __builtin_sqrt(x)
#define fabs(x)        __builtin_fabs(x)
#define copysign(x, y) __builtin_copysign((x), (y))
#endif

#ifdef __cplusplus
}
#endif

#endif
