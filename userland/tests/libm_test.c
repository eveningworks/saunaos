// tolibc's <math.h>: the transcendentals, checked against an
// INDEPENDENT implementation -- and, in the second half, what only the
// MACHINE can tell: the long double functions run on the x87's own
// transcendental instructions, which the guest's CPU (or QEMU's
// emulation of it) computes, not the host's; and <fenv.h>'s rounding
// mode and flags live in MXCSR and the x87 control word, which the
// kernel must carry across a context switch.
// tools/tests/sweep/libc_diff.py measures every function's accuracy on
// the HOST; this checks the machine agrees.
//
// EVERY EXPECTED VALUE IN THIS FILE WAS GENERATED ON THE HOST -- by its
// Python, and for the ULP table by its glibc, printed as hex --
// (which calls the platform libm), not by running SaunaOS and writing
// down what it said. That distinction is the whole point: a test whose
// expectations came from the code under test agrees with the code under
// test, including where both are wrong. The generator is a few lines
// and lives in this file's git history rather than in tools/, because
// re-running it is a once-per-function event.
//
// THE TOLERANCE IS RELATIVE, not absolute, because these functions
// span 1e-10 to 1e300 and an absolute bound would be meaningless at
// both ends. 1e-12 relative is roughly the last three or four decimal
// digits of a double -- well short of the sub-ulp accuracy glibc
// reaches with multi-precision fallback paths, and far more than a
// program drawing a circle can tell.
//
// pow() gets a LOOSER bound and says so: it compounds log's error with
// exp's, and y*log(x) magnifies whatever log got wrong by |y|.
//
// Prints one line per FAILURE, plus a count -- 81 "ok" lines say
// nothing a single summary does not.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <sched.h>
#include <stdlib.h> // strtold
#include "rt/sys.h"

#include "base/utest.h"

// UTEST_QUIET, because this is a table of a couple of hundred values
// and the count in the epilogue is the evidence. The detail is
// formatted only for a failure.
static void report(const char *name, double got, double want, double tol) {
    double err;
    if (want == 0.0) err = fabs(got);
    else             err = fabs((got - want) / want);
    utest_checkf(err <= tol && !isnan(got),
                 "%s: got %.17g, wanted %.17g (relative error %.3g)",
                 name, got, want, err);
}

// The ordinary bound.
static void chk1(const char *n, double got, double want) { report(n, got, want, 1e-12); }
// atan2 near the axes returns values whose exactness depends on the
// quadrant logic rather than on a polynomial, but the same bound holds.
static void chk2(const char *n, double got, double want) { report(n, got, want, 1e-12); }
// pow's looser bound -- see the header.
static void chkp(const char *n, double got, double want) { report(n, got, want, 1e-11); }

// --- in ULPs, against the host glibc ---------------------------------
//
// The distance between two results' bit patterns, which are monotonic
// within a sign; a sign or NaN mismatch is a failure outright.
static uint64_t ulps_d(double g, double w) {
    if (isnan(g) || isnan(w)) return isnan(g) && isnan(w) ? 0 : UINT64_MAX;
    if (!signbit(g) != !signbit(w)) return UINT64_MAX;
    uint64_t a, b;
    memcpy(&a, &g, 8);
    memcpy(&b, &w, 8);
    return a > b ? a - b : b - a;
}

static unsigned __int128 ld_key(long double v) {
    uint64_t m;
    uint16_t se;
    memcpy(&m, &v, 8);
    memcpy(&se, (char *)&v + 8, 2);
    return ((unsigned __int128)(se & 0x7fff) << 63) + (m & 0x7fffffffffffffffull);
}

static uint64_t ulps_l(long double g, long double w) {
    if (isnan(g) || isnan(w)) return isnan(g) && isnan(w) ? 0 : UINT64_MAX;
    if (!signbit(g) != !signbit(w)) return UINT64_MAX;
    unsigned __int128 a = ld_key(g), b = ld_key(w), d = a > b ? a - b : b - a;
    return d > UINT64_MAX ? UINT64_MAX : (uint64_t)d;
}

struct ulp_case { const char *name; long double got, want; int ld; unsigned bound; };
#define DBL(n, e, w, b) { n, (e), (w), 0, b }
#define FLT(n, e, w, b) { n, (double)(e), (double)(w), 0, b }
#define LD(n, e, w, b)  { n, (e), (w), 1, b }

// The bounds are libc_diff.py's MATH_BOUNDS for each function: a machine
// that needs a looser one disagrees with the host, which is the finding.
// The long double half is where that can happen -- f2xm1, fyl2x, fpatan
// and fsqrt are the guest CPU's -- and fma, fmal, powl(3, 40) and fmodl
// are EXACT, to the bit. A float result is compared as a double, which
// is exact.
static void ulp_table(void) {
    volatile double one = 1.0;   // keeps an argument from being folded
    struct ulp_case t[] = {
        DBL("sin(1e22)", sin(1e22 * one), -0x1.b453ab76bf397p-1, 1),
        DBL("cos(1e22)", cos(1e22 * one), 0x1.0be2cef01c8f4p-1, 1),
        DBL("tan(1e22)", tan(1e22 * one), -0x1.a0f79c1b6b257p+0, 1),
        DBL("sin(2^1023)", sin(0x1p1023 * one), 0x1.205248cbdb76p-1, 1),
        // The double nearest a multiple of pi/2: Payne-Hanek's worst case.
        DBL("cos(nearest pi/2 multiple)", cos(0x1.6ac5b262ca1ffp+849 * one), -0x1.14ae72e6ba22fp-61, 1),
        DBL("exp(709.7)", exp(709.7 * one), 0x1.d75ae7a50ee14p+1023, 1),
        DBL("exp(-745)", exp(-745.0 * one), 0x0.0000000000001p-1022, 1),
        DBL("exp2(-1074)", exp2(-1074.0 * one), 0x0.0000000000001p-1022, 0),
        DBL("log(min subnormal)", log(0x1p-1074 * one), -0x1.74385446d71c3p+9, 1),
        DBL("log10(2)", log10(2.0 * one), 0x1.34413509f79ffp-2, 1),
        DBL("pow(1.0000001, 1e9)", pow(1.0000001 * one, 1e9), 0x1.349445c228792p+144, 1),
        DBL("pow(-2, -1074)", pow(-2.0 * one, -1074.0), 0x0.0000000000001p-1022, 0),
        // -3 exactly: glibc's own cbrt gives -0x1.8000000000001p+1 here.
        DBL("cbrt(-27)", cbrt(-27.0 * one), -3.0, 0),
        DBL("hypot(3e300, 4e300)", hypot(3e300 * one, 4e300), 0x1.ddd4baa009303p+998, 1),
        DBL("expm1(1e-10)", expm1(1e-10 * one), 0x1.b7cdfd9dda4e3p-34, 1),
        DBL("log1p(-1e-10)", log1p(-1e-10 * one), -0x1.b7cdfd9dda4e3p-34, 1),
        DBL("atanh(0.5)", atanh(0.5 * one), 0x1.193ea7aad030bp-1, 1),
        DBL("asinh(-1e300)", asinh(-1e300 * one), -0x1.59bbfd8b83e44p+9, 1),
        DBL("acos(-0.999)", acos(-0.999 * one), 0x1.8c66280ae928ep+1, 1),
        DBL("atan2(-1, -0.5)", atan2(-1.0 * one, -0.5), -0x1.0468a8ace4df6p+1, 1),
        DBL("erf(0.5)", erf(0.5 * one), 0x1.0a7ef5c18edd2p-1, 1),
        DBL("erfc(10)", erfc(10.0 * one), 0x1.7d8a7f2a8a2dp-149, 2),
        // -3.5, not -2.5: between -2 and -3 lgamma passes through zero
        // twice, and the reflection formula's cancellation costs fdlibm's
        // (and this) ~10 ulp there -- math_special.c's header.
        DBL("lgamma(-3.5)", lgamma(-3.5 * one), -0x1.4f1b0fe64a5d8p+0, 2),
        DBL("tgamma(0.5)", tgamma(0.5 * one), 0x1.c5bf891b4ef6bp+0, 6),
        DBL("tgamma(170.5)", tgamma(170.5 * one), 0x1.9589f849167a8p+1015, 6),
        // 2^-54, where 0.1*10 - 1 rounded twice is 0: one rounding is the point.
        DBL("fma(0.1, 10, -1)", fma(0.1 * one, 10.0, -1.0), 0x1p-54, 0),
        DBL("remainder(1e300, 3)", remainder(1e300 * one, 3.0), 0x0p+0, 0),
        FLT("sinf(1e10)", sinf(1e10f * (float)one), -0x1.f334c8p-2f, 1),
        FLT("expf(88.7)", expf(88.7f * (float)one), 0x1.f46ff8p+127f, 1),
        FLT("powf(2.5, 3.5)", powf(2.5f * (float)one, 3.5f), 0x1.8b48e2p+4f, 1),
        FLT("tgammaf(4.5)", tgammaf(4.5f * (float)one), 0x1.74371ep+3f, 1),
        FLT("fmaf(1+2^-12, 1+2^-12, -1)", fmaf(1.000244140625f * (float)one, 1.000244140625f, -1.0f),
            0x1.0008p-11f, 0),
        LD("expl(11356)", expl(11356.0L * one), 0x9.7ae01b5ed4a38fcp+16380L, 1),
        LD("expl(-0.5)", expl(-0.5L * one), 0x9.b4597e37cb04ff4p-4L, 1),
        LD("exp2l(0.3)", exp2l(0.3L * one), 0x9.d9623dffc19470ep-3L, 1),
        LD("expm1l(1e-10)", expm1l(1e-10L * one), 0xd.be6feceed2717d7p-37L, 2),
        LD("logl(1e4000)", logl(1e4000L * one), 0x8.fe95c8a78a8b80ep+10L, 1),
        LD("logl(1+2^-40)", logl(1.0L * one + 0x1p-40L), 0xf.fffffffff8p-44L, 1),
        LD("log2l(3)", log2l(3.0L * one), 0xc.ae00d1cfdeb43dp-3L, 1),
        LD("log10l(7)", log10l(7.0L * one), 0xd.858585bc661f94cp-4L, 1),
        LD("log1pl(1e-30)", log1pl(1e-30L * one), 0xa.2425ff75e14fc32p-103L, 2),
        LD("powl(1.5, 1000.5)", powl(1.5L * one, 1000.5L), 0x9.8bebcaad1465fabp+582L, 2),
        LD("powl(2, -16000)", powl(2.0L * one, -16000.0L), 0x8p-16003L, 0),
        LD("powl(3, 40)", powl(3.0L * one, 40.0L), 0xa.8b8b452291fe821p+60L, 0),
        LD("sinl(1e4000)", sinl(1e4000L * one), 0x9.0de0837a745738ap-4L, 1),
        LD("cosl(1e22)", cosl(1e22L * one), 0x8.5f167780e479c9ap-4L, 1),
        LD("tanl(1.5)", tanl(1.5L * one), 0xe.19f6a85c43bbad3p+0L, 2),
        LD("atan2l(1, -1)", atan2l(1.0L * one, -1.0L), 0x9.6cbe3f9990e91a8p-2L, 1),
        LD("asinl(0.5)", asinl(0.5L * one), 0x8.60a91c16b9b2c23p-4L, 2),
        LD("acosl(-0.25)", acosl(-0.25L * one), 0xe.967ae3e38785c71p-3L, 1),
        LD("atanl(1e10)", atanl(1e10L * one), 0xc.90fdaa1ea6f0281p-3L, 1),
        LD("sqrtl(2)", sqrtl(2.0L * one), 0xb.504f333f9de6484p-3L, 0),
        LD("cbrtl(10)", cbrtl(10.0L * one), 0x8.9e24209c3827654p-2L, 1),
        LD("hypotl(1e4000, 1e4000)", hypotl(1e4000L * one, 1e4000L), 0x9.44cece11eba5bd3p+13285L, 1),
        LD("coshl(11000)", coshl(11000.0L * one), 0xc.838961a26b2addap+15865L, 3),
        LD("tanhl(0.3)", tanhl(0.3L * one), 0x9.526ed3ec8a7d089p-5L, 2),
        LD("erfcl(30)", erfcl(30.0L * one), 0xe.54a046e0a514445p-1308L, 4096),
        LD("tgammal(1000.5)", tgammal(1000.5L * one), 0xa.aa776a9abffa3fdp+8521L, 32768),
        LD("lgammal(1e3000)", lgammal(1e3000L * one), 0xb.9dc4f61d1e2779ap+9975L, 8192),
        LD("fmal(1/3, 3, -1)", fmal(1.0L / 3 * one, 3.0L, -1.0L), 0x8p-68L, 0),
        LD("fmodl(1e4000, 7)", fmodl(1e4000L * one, 7.0L), 0xcp-1L, 0),
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint64_t u = t[i].ld ? ulps_l(t[i].got, t[i].want)
                             : ulps_d((double)t[i].got, (double)t[i].want);
        utest_checkf(u <= t[i].bound, "%s: %llu ulp from glibc, bound %u", t[i].name,
                     (unsigned long long)u, t[i].bound);
    }
}

// --- <fenv.h>, on the machine ---------------------------------------
static void fenv_checks(void) {
    volatile double half = 1.5, zero = 0.0, two5 = 2.5;
    volatile long double halfl = 1.5L;

    // The rounding mode reaches SSE (rint, lrint) and the x87 (rintl)
    // alike, and survives a trip through the scheduler.
    utest_check(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD, "fesetround(FE_UPWARD)");
    sched_yield();
    utest_check(fegetround() == FE_UPWARD, "the rounding mode survives a context switch");
    utest_check(rint(half) == 2.0 && rint(-half) == -1.0 && lrint(two5) == 3,
                "rint/lrint round up in FE_UPWARD");
    utest_check(rintl(halfl) == 2.0L && rintl(-halfl) == -1.0L, "rintl rounds up: the x87 has the mode too");
    utest_check(fma(1.0 * half / half, 1.0, 0x1p-60) == 1.0 + 0x1p-52, "fma rounds up in FE_UPWARD");
    fesetround(FE_TOWARDZERO);
    utest_check(rint(-half) == -1.0 && lrint(-two5) == -2 && rintl(-halfl) == -1.0L,
                "FE_TOWARDZERO truncates");
    fesetround(FE_TONEAREST);
    utest_check(rint(two5) == 2.0 && rint(half) == 2.0 && fegetround() == FE_TONEAREST,
                "back to nearest-even");
    utest_check(fesetround(0x123) != 0, "fesetround refuses a mode that is not one");

    // The flags, raised by real arithmetic in each unit.
    feclearexcept(FE_ALL_EXCEPT);
    utest_check(fetestexcept(FE_ALL_EXCEPT) == 0, "feclearexcept clears every flag");
    volatile double r = 1.0 / zero;
    (void)r;
    utest_check(fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO, "1/0 raises FE_DIVBYZERO (SSE)");
    feclearexcept(FE_ALL_EXCEPT);
    volatile long double rl = 1.0L / (long double)zero;
    (void)rl;
    utest_check(fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO, "1/0 raises FE_DIVBYZERO (x87)");
    feclearexcept(FE_ALL_EXCEPT);
    utest_check(isnan(log(-1.0 + zero)) && fetestexcept(FE_INVALID), "log(-1) raises FE_INVALID");
    feclearexcept(FE_ALL_EXCEPT);
    utest_check(nearbyint(two5) == 2.0 && !fetestexcept(FE_INEXACT), "nearbyint raises no FE_INEXACT");
    utest_check(rint(two5) == 2.0 && fetestexcept(FE_INEXACT), "rint does raise it");

    // The environment as a whole.
    fenv_t env;
    feraiseexcept(FE_OVERFLOW);
    utest_check(feholdexcept(&env) == 0 && fetestexcept(FE_ALL_EXCEPT) == 0,
                "feholdexcept saves and clears");
    feraiseexcept(FE_UNDERFLOW);
    feupdateenv(&env);
    utest_check(fetestexcept(FE_OVERFLOW | FE_UNDERFLOW) == (FE_OVERFLOW | FE_UNDERFLOW),
                "feupdateenv restores the saved flags and keeps the new ones");
    fesetround(FE_DOWNWARD);
    fegetenv(&env);
    fesetenv(FE_DFL_ENV);
    utest_check(fegetround() == FE_TONEAREST && fetestexcept(FE_ALL_EXCEPT) == 0,
                "FE_DFL_ENV is round-to-nearest with no flags");
    fesetenv(&env);
    utest_check(fegetround() == FE_DOWNWARD, "fesetenv puts a saved environment back");
    fesetenv(FE_DFL_ENV);
    fexcept_t saved;
    feraiseexcept(FE_INEXACT);
    fegetexceptflag(&saved, FE_ALL_EXCEPT);
    feclearexcept(FE_ALL_EXCEPT);
    fesetexceptflag(&saved, FE_INEXACT);
    utest_check(fetestexcept(FE_ALL_EXCEPT) == FE_INEXACT, "fesetexceptflag restores a saved flag");
    feclearexcept(FE_ALL_EXCEPT);

    // C99's odds and ends, each in one line.
    int q = 0;
    utest_check(remquo(10.0 * half / half, 3.0, &q) == 1.0 && q == 3, "remquo(10, 3) is 1, quotient 3");
    utest_check(nextafter(1.0, 2.0) == 1.0 + 0x1p-52 && nextafterf(0.0f, -1.0f) == -0x1p-149f,
                "nextafter steps one representation");
    utest_check(ilogb(0x1p-1074) == -1074 && ilogb(0.0) == FP_ILOGB0 && logbl(0x1p-16445L) == -16445,
                "ilogb and logbl count subnormal exponents");
    utest_check(tgamma(5.0) == 24.0 && lgamma(1.0) == 0.0 && erf(0.0) == 0.0, "exact gamma at integers");
    utest_check(fmax(-0.0, 0.0) == 0.0 && !signbit(fmax(-0.0, 0.0)) && signbit(fmin(-0.0, 0.0)),
                "fmax/fmin order -0 below +0");
    utest_check(isnan(nan("")) && fpclassify(nanl("7")) == FP_NAN && lround(-2.5) == -3 &&
                llrint(1e15) == 1000000000000000LL, "nan, lround, llrint");
}

int main(void) {
    utest_begin("libm_test", "the transcendentals, against independently computed values", UTEST_QUIET);

    chk1("sin", sin(0), 0);
    chk1("sin", sin(0.5), 0.47942553860420301);
    chk1("sin", sin(1), 0.8414709848078965);
    chk1("sin", sin(2), 0.90929742682568171);
    chk1("sin", sin(3), 0.14112000805986721);
    chk1("sin", sin(-1.5), -0.99749498660405445);
    chk1("sin", sin(100), -0.50636564110975879);
    chk1("sin", sin(1000000), -0.34999350217129294);
    chk1("cos", cos(0), 1);
    chk1("cos", cos(0.5), 0.87758256189037276);
    chk1("cos", cos(1), 0.54030230586813977);
    chk1("cos", cos(2), -0.41614683654714241);
    chk1("cos", cos(3), -0.98999249660044542);
    chk1("cos", cos(-1.5), 0.070737201667702906);
    chk1("cos", cos(100), 0.86231887228768389);
    chk1("cos", cos(1000000), 0.93675212753314474);
    chk1("tan", tan(0.5), 0.54630248984379048);
    chk1("tan", tan(1), 1.5574077246549023);
    chk1("tan", tan(-1), -1.5574077246549023);
    chk1("tan", tan(2), -2.1850398632615189);
    chk1("exp", exp(0), 1);
    chk1("exp", exp(1), 2.7182818284590451);
    chk1("exp", exp(-1), 0.36787944117144233);
    chk1("exp", exp(10), 22026.465794806718);
    chk1("exp", exp(-10), 4.5399929762484854e-05);
    chk1("exp", exp(700), 1.0142320547350045e+304);
    chk1("exp", exp(0.5), 1.6487212707001282);
    chk1("log", log(1), 0);
    chk1("log", log(2), 0.69314718055994529);
    chk1("log", log(10), 2.3025850929940459);
    chk1("log", log(0.5), -0.69314718055994529);
    chk1("log", log(10000000000), 23.025850929940457);
    chk1("log", log(1e-10), -23.025850929940457);
    chk1("log", log(2.7182818284590451), 1);
    chk1("atan", atan(0), 0);
    chk1("atan", atan(0.5), 0.46364760900080609);
    chk1("atan", atan(1), 0.78539816339744828);
    chk1("atan", atan(2), 1.1071487177940904);
    chk1("atan", atan(-3), -1.2490457723982544);
    chk1("atan", atan(100), 1.5607966601082315);
    chk1("asin", asin(0), 0);
    chk1("asin", asin(0.5), 0.52359877559829893);
    chk1("asin", asin(-0.5), -0.52359877559829893);
    chk1("asin", asin(0.90000000000000002), 1.1197695149986342);
    chk1("acos", acos(0), 1.5707963267948966);
    chk1("acos", acos(0.5), 1.0471975511965979);
    chk1("acos", acos(-0.5), 2.0943951023931957);
    chk1("acos", acos(0.90000000000000002), 0.45102681179626236);
    chk1("sinh", sinh(0), 0);
    chk1("sinh", sinh(1), 1.1752011936438014);
    chk1("sinh", sinh(-2), -3.6268604078470186);
    chk1("sinh", sinh(5), 74.203210577788752);
    chk1("cosh", cosh(0), 1);
    chk1("cosh", cosh(1), 1.5430806348152437);
    chk1("cosh", cosh(-2), 3.7621956910836314);
    chk1("cosh", cosh(5), 74.209948524787848);
    chk1("tanh", tanh(0), 0);
    chk1("tanh", tanh(0.5), 0.46211715726000974);
    chk1("tanh", tanh(-1), -0.76159415595576485);
    chk1("tanh", tanh(3), 0.99505475368673046);
    chk1("cbrt", cbrt(8), 2);
    chk1("cbrt", cbrt(27), 3.0000000000000004);
    chk1("cbrt", cbrt(-64), -4);
    chk1("cbrt", cbrt(2), 1.2599210498948734);
    chk1("log2", log2(1024), 10);
    chk1("log10", log10(1000), 3);
    chk1("exp2", exp2(10), 1024);
    chk2("atan2", atan2(1, 1), 0.78539816339744828);
    chk2("atan2", atan2(1, -1), 2.3561944901923448);
    chk2("atan2", atan2(-1, -1), -2.3561944901923448);
    chk2("atan2", atan2(-1, 1), -0.78539816339744828);
    chk2("atan2", atan2(0, -1), 3.1415926535897931);
    chk2("atan2", atan2(3, 4), 0.64350110879328437);
    chkp("pow", pow(2, 10), 1024);
    chkp("pow", pow(2, 0.5), 1.4142135623730951);
    chkp("pow", pow(10, -3), 0.001);
    chkp("pow", pow(2.5, 3.7000000000000002), 29.67413253642086);
    chkp("pow", pow(-2, 3), -8);
    chk1("hypot", hypot(3, 4), 5);
    chk1("hypot", hypot(9.9999999999999997e+199, 9.9999999999999997e+199), 1.414213562373095e+200);
    chk1("hypot", hypot(0, 5), 5);
    // --- the identities, which no table of values can fake ------------
    //
    // A function can match a list of sampled points and still be wrong
    // between them; an identity has to hold everywhere it is tested.
    for (int i = -30; i <= 30; i++) {
        double x = i * 0.37;
        report("sin^2+cos^2", sin(x) * sin(x) + cos(x) * cos(x), 1.0, 1e-12);
        report("exp(log)", (x > 0.1) ? log(exp(x)) : x, x, 1e-12);
        report("atan(tan)", (fabs(x) < 1.5) ? atan(tan(x)) : x, x, 1e-11);
    }
    // The squared identity is still worth having -- but only where it
    // is well conditioned, which is |x| small enough that cosh and
    // sinh are not yet nearly equal.
    for (int i = 1; i <= 8; i++) {
        double x = i * 0.25;
        report("cosh^2-sinh^2 (small x)",
               cosh(x) * cosh(x) - sinh(x) * sinh(x), 1.0, 1e-12);
    }
    for (int i = 1; i <= 40; i++) {
        double x = i * 0.25;
        report("log(exp)", exp(log(x)), x, 1e-12);
        report("cbrt^3", cbrt(x) * cbrt(x) * cbrt(x), x, 1e-11);
        // cosh + sinh == exp, NOT cosh^2 - sinh^2 and not
        // (cosh-sinh)(cosh+sinh). BOTH of those are ill-conditioned:
        // for large x, cosh and sinh are each about e^x/2, so their
        // difference cancels almost every digit and the check ends up
        // measuring the subtraction rather than the functions. The
        // first form reported errors up to 3e-8 and the second up to
        // 2.6e-8 against a cosh and sinh that were individually correct
        // to 1e-15 -- the test was wrong twice before it was right.
        //
        // Adding them cancels nothing, and it pins both against a third
        // function rather than only against each other.
        report("cosh+sinh == exp", cosh(x) + sinh(x), exp(x), 1e-12);
    }

    // --- the special cases -------------------------------------------
    //
    // Where libms actually disagree with each other, and where a
    // polynomial that is accurate in the middle says nothing.
    utest_check(!(pow(2.0, 10.0) != 1024.0), "pow(2,10) is not exactly 1024");
    utest_check(!(pow(0.0, 0.0) != 1.0), "pow(0,0) must be 1");
    utest_check(!(pow(1.0, 1.0/0.0) != 1.0), "pow(1,inf) must be 1");
    utest_check(!(!isnan(pow(-8.0, 0.5))), "a negative base with a fractional exponent must be NaN");
    utest_check(!(pow(-2.0, 3.0) != -8.0), "a negative base with an integer exponent must work");
    utest_check(!(!isinf(hypot(1.0/0.0, 0.0))), "hypot of infinity");
    utest_check(!(log(0.0) != -1.0/0.0), "log(0) must be -inf");
    utest_check(!(!isnan(log(-1.0))), "log of a negative must be NaN");
    utest_check(!(!isnan(asin(2.0))), "asin out of domain must be NaN");
    utest_check(!(tanh(100.0) != 1.0), "tanh saturates at 1");
    utest_check(!(exp(-1000.0) != 0.0), "exp underflows to 0");
    utest_check(!(!isinf(exp(1000.0))), "exp overflows to inf");
    double ip = 0;
    utest_check(!(modf(-2.5, &ip) != -0.5 || ip != -2.0), "modf keeps the sign");

    // --- C99's classification and quiet comparisons -------------------
    volatile double nan_ = 0.0 / 0.0, inf_ = 1.0 / 0.0, tiny = 4.9e-324, zero = 0.0;
    utest_check(fpclassify(nan_) == FP_NAN && fpclassify(inf_) == FP_INFINITE &&
                fpclassify(zero) == FP_ZERO && fpclassify(tiny) == FP_SUBNORMAL &&
                fpclassify(1.5) == FP_NORMAL, "fpclassify names all five classes");
    utest_check(isnormal(1.5) && !isnormal(tiny) && !isnormal(zero), "isnormal");
    utest_check(isgreater(2.0, 1.0) && !isgreater(nan_, 1.0) && isless(1.0, 2.0) &&
                islessgreater(1.0, 2.0) && !islessgreater(nan_, nan_) &&
                isunordered(nan_, 1.0) && !isunordered(1.0, 2.0),
                "the comparison macros are false, not raising, on a NaN");
    char *end = NULL;
    long double ld = strtold("2.5e3x", &end);
    utest_check(ld == 2500.0L && end && *end == 'x', "strtold parses, stopping at the end");

    ulp_table();
    fenv_checks();
    return utest_end();
}
