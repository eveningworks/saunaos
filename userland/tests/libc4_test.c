// Stage 4 of the C library: %f/%e/%g, strtod, and the algebraic half of
// <math.h> (docs/libc-design.md).
//
// HOW THIS IS ASSERTED, and why it is not "the number looked right".
// Every check compares the FORMATTED TEXT against a literal, because
// that is the only thing a caller of printf can observe and because a
// float comparison would hide exactly the rounding this file is about.
// The accuracy limit is real and documented (printf_float.c): digits
// come from repeated scaling, not from exact arithmetic over the
// mantissa, so nothing here asserts a 17th significant digit -- and the
// test says so rather than quietly choosing easy inputs.
//
// The load-bearing checks:
//  - **9.999 at two places** proves rounding CARRIES into the integer
//    part. Rounding the fraction separately gets 9.99 -> "9.100" or
//    "10.00" wrong, and every friendlier input passes either way.
//  - **%g on both sides of its exponent rule** proves the rule is
//    implemented rather than one branch of it.
//  - **fmod on a huge ratio** proves it is not x - trunc(x/y)*y, which
//    loses everything once x/y passes the mantissa.
//  - **A %f in a KERNEL format string must stay literal** -- checked
//    from the kernel's own side by a KTEST, not here.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "rt/sys.h"

#include "base/utest.h"

static char buf[128];

// Formats and compares against the expected text, printing what came
// out when they differ -- a bare pass/fail on a formatter is very hard
// to act on.
static void fmt_is(const char *fmt, double v, const char *want, const char *what) {
    snprintf(buf, sizeof buf, fmt, v);
    int ok = strcmp(buf, want) == 0;
    if (!ok) {
        static char msg[192];
        snprintf(msg, sizeof msg, "%s   -- got \"%s\", wanted \"%s\"", what, buf, want);
        utest_check(0, msg);
    } else {
        utest_check(1, what);
    }
}

int main(void) {
    utest_begin("libc4_test", "%%f/%%e/%%g, strtod and math.h", 0);

    // --- %f ----------------------------------------------------------
    fmt_is("%f", 0.0, "0.000000", "%f defaults to six places");
    fmt_is("%.2f", 3.14159, "3.14", "%.2f rounds down");
    fmt_is("%.2f", 3.14559, "3.15", "and up");
    fmt_is("%.2f", 9.999, "10.00", "a carry reaches the INTEGER part");
    // A TIE GOES TO EVEN, on the exact binary value -- glibc's answer.
    // This line asserted "3" while the digits came from scaling by ten.
    fmt_is("%.0f", 2.5, "2", "%.0f breaks a tie to even, down");
    fmt_is("%.0f", 3.5, "4", "and up");
    fmt_is("%.2f", 0.125, "0.12", "a tie past the point too");
    fmt_is("%.3f", -0.0005, "-0.001", "a negative keeps its sign through rounding");
    fmt_is("%.1f", 1234567.89, "1234567.9", "a seven-digit integer part is whole");
    fmt_is("%f", 1.0 / 0.0, "inf", "infinity");
    fmt_is("%f", -1.0 / 0.0, "-inf", "and negative infinity");
    // NAN, not 0.0/0.0: on x86-64 the NaN that division produces has
    // its SIGN BIT SET, so it prints as "-nan" -- and that is correct,
    // not a bug. glibc does the same, and C permits printf to report a
    // NaN's sign. Asserting against 0.0/0.0 was testing the FPU's
    // choice of default NaN rather than this formatter.
    fmt_is("%F", NAN, "NAN", "%F reports NaN in upper case");
    fmt_is("%f", 0.0 / 0.0, "-nan", "and a division's NaN keeps the sign bit x86 gives it");

    // --- %e ----------------------------------------------------------
    fmt_is("%e", 0.0, "0.000000e+00", "%e of zero");
    fmt_is("%.2e", 1234.5, "1.23e+03", "%.2e");
    fmt_is("%.2e", 0.000123, "1.23e-04", "a negative exponent");
    fmt_is("%.0e", 9.9, "1e+01", "a mantissa rounding to 10 bumps the exponent");
    fmt_is("%.3E", 6.02214e23, "6.022E+23", "%E is upper case and pads the exponent to two");

    // --- %g ----------------------------------------------------------
    fmt_is("%g", 100.0, "100", "%g drops a trailing point");
    fmt_is("%g", 0.0001, "0.0001", "and stays fixed at the -4 boundary");
    fmt_is("%g", 0.00001, "1e-05", "and switches to exponent past it");
    fmt_is("%g", 1234567.0, "1.23457e+06", "and past six significant digits");
    fmt_is("%.3g", 1234.0, "1.23e+03", "%.3g counts SIGNIFICANT digits");
    fmt_is("%g", 1.5, "1.5", "a real fraction keeps its digits");

    // --- exact digits, at any precision ------------------------------
    fmt_is("%.17g", 2.718281828459045, "2.7182818284590451", "%.17g prints the double's own digits");
    fmt_is("%.17g", 0.1, "0.10000000000000001", "0.1 is not 0.1");
    fmt_is("%.20f", 0.1, "0.10000000000000000555", "the exact expansion, past 17 digits");
    fmt_is("%.0f", 1e22, "10000000000000000000000", "1e22 is exact");
    fmt_is("%.0f", 1e23, "99999999999999991611392", "1e23 is not");
    fmt_is("%.3e", 4.9406564584124654e-324, "4.941e-324", "the smallest subnormal");

    // --- %a -------------------------------------------------------------
    fmt_is("%a", 1.0, "0x1p+0", "%a of one");
    fmt_is("%.3a", 3.14159, "0x1.922p+1", "%.3a rounds the hex digits");
    fmt_is("%A", -0.5, "-0X1P-1", "%A is upper case");
    fmt_is("%a", 4.9406564584124654e-324, "0x0.0000000000001p-1022", "a subnormal keeps a 0 lead");

    // --- long double: `L` consumes a long double ---------------------
    snprintf(buf, sizeof buf, "[%Lf][%.3Le][%.*Lg][%La][%d]", 1.5L, 2.5L, 21, 0.1L, 1.0L, 7);
    utest_checkf(strcmp(buf, "[1.500000][2.500e+00][0.100000000000000000001][0x8p-3][7]") == 0,
                 "%%Lf/%%Le/%%Lg/%%La, and the %%d after them -- got \"%s\"", buf);

    // --- t, j and n ---------------------------------------------------
    int n = -1;
    snprintf(buf, sizeof buf, "%td %jd%n %zu", (ptrdiff_t)-9, (intmax_t)42, &n, (size_t)3);
    utest_checkf(strcmp(buf, "-9 42 3") == 0 && n == 5, "%%td %%jd %%n %%zu -- got \"%s\", n=%d", buf, n);

    // --- width applies to floats too ---------------------------------
    snprintf(buf, sizeof buf, "[%10.2f][%-10.2f]", 3.5, 3.5);
    utest_check(strcmp(buf, "[      3.50][3.50      ]") == 0, "width and '-' work on a float");
    // A float's precision does not cancel '0', as an integer's does.
    fmt_is("%08.3f", 3.14159, "0003.142", "'0' pads a float with zeros, after the sign");
    fmt_is("%+.1f", 2.0, "+2.0", "'+' signs a positive float");
    fmt_is("% .1e", 2.0, " 2.0e+00", "' ' leaves a sign's room on a positive float");
    fmt_is("%#.0f", 3.0, "3.", "'#' keeps the point with no digits after it");
    fmt_is("%#g", 1.5, "1.50000", "'#' keeps %g's trailing zeros");
    fmt_is("%06f", 1.0 / 0.0, "   inf", "inf takes no zeros");

    // --- strtod ------------------------------------------------------
    char *end;
    utest_check(strtod("1.5", &end) == 1.5 && *end == '\0', "strtod reads a simple value");
    utest_check(strtod("-0.25xyz", &end) == -0.25 && strcmp(end, "xyz") == 0,
          "and endptr is the first unused byte");
    utest_check(strtod("1e3", &end) == 1000.0, "an exponent");
    utest_check(strtod("1.5e-3", &end) == 0.0015, "a negative exponent");
    const char *bad = "abc";
    utest_check(strtod(bad, &end) == 0.0 && end == bad, "no conversion returns the original pointer");
    // "1e" is a value followed by a letter, NOT a number with an empty
    // exponent -- the classic strtod edge case.
    const char *trunc_exp = "1e";
    utest_check(strtod(trunc_exp, &end) == 1.0 && end == trunc_exp + 1,
          "\"1e\" parses as 1 with the 'e' left over");
    utest_check(isinf(strtod("inf", &end)) && isnan(strtod("nan", &end)),
          "inf and nan are accepted");
    // Round trip through the formatter at a precision the accuracy note
    // actually promises.
    snprintf(buf, sizeof buf, "%.6f", strtod("3.141593", &end));
    utest_check(strcmp(buf, "3.141593") == 0, "a six-place value survives text -> double -> text");
    // Correctly rounded: these three were each one or more ULP out.
    fmt_is("%.17g", strtod("1e-310", 0), "9.9999999999999694e-311", "strtod of a subnormal");
    fmt_is("%.17g", strtod("123456789012345678901234567890", 0), "1.2345678901234568e+29",
           "strtod of 30 digits");
    fmt_is("%.17g", strtod("1.7976931348623157e308", 0), "1.7976931348623157e+308", "strtod of DBL_MAX");
    utest_check(strtof("3.4028235e38", 0) == 3.40282347e+38f, "strtof rounds to FLT_MAX before the range check");
    utest_check(strtof("1.00000005960464477539062501", 0) == 1.00000012f,
                "strtof rounds the text, not a double rounded from it");

    // --- sscanf: scansets ----------------------------------------------
    char k[16], v[16];
    int r = sscanf("key=value;rest", "%[^=]=%[^;]", k, v);
    utest_checkf(r == 2 && !strcmp(k, "key") && !strcmp(v, "value"),
                 "a scanset pair -- %d, \"%s\", \"%s\"", r, k, v);
    r = sscanf("]a]b-c", "%3[]a]%n", k, &n);
    utest_checkf(r == 1 && !strcmp(k, "]a]") && n == 3, "']' first is a member, and a width -- %d", r);
    long double ld = 0;
    utest_check(sscanf("0.375", "%Lf", &ld) == 1 && ld == 0.375L, "%Lf stores a long double");

    // --- math.h ------------------------------------------------------
    utest_check(sqrt(144.0) == 12.0, "sqrt");
    utest_check(fabs(-3.5) == 3.5 && fabs(3.5) == 3.5, "fabs");
    utest_check(floor(2.7) == 2.0 && floor(-2.1) == -3.0, "floor goes toward -inf");
    utest_check(ceil(2.1) == 3.0 && ceil(-2.7) == -2.0, "ceil goes toward +inf");
    utest_check(trunc(2.7) == 2.0 && trunc(-2.7) == -2.0, "trunc goes toward zero");
    utest_check(round(2.5) == 3.0 && round(-2.5) == -3.0, "round goes half AWAY from zero");
    utest_check(floor(1e300) == 1e300, "floor of a value with no fraction bits is itself");
    utest_check(copysign(3.0, -1.0) == -3.0, "copysign");
    utest_check(ldexp(1.5, 3) == 12.0, "ldexp");
    int e;
    utest_check(frexp(12.0, &e) == 0.75 && e == 4, "frexp splits into mantissa and exponent");
    utest_check(fmod(10.0, 3.0) == 1.0, "fmod");
    utest_check(fmod(-10.0, 3.0) == -1.0, "fmod keeps the dividend's sign");
    // EXACT expected values, not a range. "the answer is in [0, y)" is
    // satisfied by the naive x - trunc(x/y)*y, which returns 0.0 here
    // -- a control that broke fmod this way changed nothing until this
    // check named the number it wanted.
    //
    // 2^100 is exactly representable, and 2 == -1 (mod 3) so
    // 2^100 == 1 (mod 3); likewise 2^4 == 1 (mod 5) and 100 is a
    // multiple of 4, so 2^100 == 1 (mod 5). Both answers are 1.0, and
    // both ratios are far past what a 53-bit mantissa can hold.
    double big = ldexp(1.0, 100);
    utest_check(fmod(big, 3.0) == 1.0, "fmod(2^100, 3) is exactly 1, not an approximation");
    utest_check(fmod(big, 5.0) == 1.0, "fmod(2^100, 5) too -- the ratio is past the mantissa");
    utest_check(isnan(fmod(1.0, 0.0)), "fmod by zero is NaN");
    utest_check(isinf(INFINITY) && !isfinite(INFINITY) && isnan(NAN), "the classification macros");
    utest_check(signbit(-0.0) && !signbit(0.0), "signbit sees a negative zero");

    return utest_end();
}
