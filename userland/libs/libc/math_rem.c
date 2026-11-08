// fmod, remainder and remquo, in all three precisions, over ONE exact
// core: integer long division of the mantissas.
//
// The remainder of two floating values is always exactly representable
// (it is a multiple of y's last place and smaller than y), so these are
// exact functions, and the obvious x - trunc(x/y)*y is not: x/y rounds
// once its quotient passes the mantissa, and from there every digit of
// the answer is noise. Shift-and-subtract one quotient bit at a time
// costs a loop of (exponent difference) steps -- at worst 32,766 for a
// long double, microseconds -- and is the same algorithm in every
// precision, which is why there is one of it. The x87's fprem/fprem1
// would do the long double case in hardware; emulators have computed
// them through a host double, which is exactly the rounding this exists
// to avoid, so the integers win on being the same everywhere.
#include "math_internal.h"

// Both mantissas NORMALISED (bit 63 set). The remainder comes back
// unnormalised: value = *rm * 2^*re.
void __m_rem_core(uint64_t mx, int ex, uint64_t my, int ey, int nearest,
                  uint64_t *rm, int *re, unsigned *q, int *flip) {
    unsigned quo = 0;
    int fl = 0;
    uint64_t r;
    if (ex < ey) {
        // |x| < |y|: the truncated quotient is 0. remainder() still
        // wants the nearest, which is 1 when 2|x| > |y|.
        r = mx;
        *re = ex;
        if (nearest && ex == ey - 1 && mx > my) {
            // |y| - |x| at x's exponent: 2*my - mx < my, so it fits.
            r = (uint64_t)((((unsigned __int128)my) << 1) - mx);
            quo = 1;
            fl = 1;
        }
    } else {
        int d = ex - ey;
        r = mx;
        if (r >= my) { r -= my; quo = 1; }
        // Invariant: value left = r * 2^(ey + d), with r < my. Doubling
        // r can carry past bit 63; (carry:r) - my is below my, so the
        // wrapped 64-bit subtraction is exactly right.
        while (d > 0) {
            uint64_t carry = r >> 63;
            r <<= 1;
            quo <<= 1;
            d--;
            if (carry || r >= my) { r -= my; quo |= 1; }
        }
        *re = ey;
        // Round the quotient to nearest, ties to EVEN: past the halfway
        // point the remainder is taken from the next multiple instead.
        if (nearest && r && (r > my - r || (r == my - r && (quo & 1)))) {
            r = my - r;
            quo++;
            fl = 1;
        }
    }
    *rm = r;
    *q = quo;
    *flip = fl;
}

// A double as mantissa and exponent, normalised to bit 63.
static uint64_t unpack_d(double v, int *e) {
    uint64_t b = m_bits(v) & ~(1ull << 63);
    int ex = (int)(b >> 52);
    uint64_t m = b & ((1ull << 52) - 1);
    if (ex) { m |= 1ull << 52; *e = ex - 1075; }
    else    { *e = -1074; }
    int s = __builtin_clzll(m);
    *e -= s;
    return m << s;
}

static double rem_d(double x, double y, int nearest, int *quo) {
    if (quo) *quo = 0;
    if (isnan(x) || isnan(y)) return x + y;
    if (y == 0 || isinf(x)) return (x * y) / (x * y);   // invalid
    if (isinf(y) || x == 0) return x;
    int ex, ey, flip;
    unsigned q;
    uint64_t mx = unpack_d(x, &ex), my = unpack_d(y, &ey), r;
    int er;
    __m_rem_core(mx, ex, my, ey, nearest, &r, &er, &q, &flip);
    // r carries at most 53 significant bits (both inputs did), so the
    // conversion and the scaling are exact; so is a subnormal result,
    // which is a multiple of y's last place.
    double v = scalbn((double)r, er);
    int neg = !!signbit(x) ^ flip;
    if (quo) {
        int qq = (int)(q & 7);
        *quo = (!signbit(x) != !signbit(y)) ? -qq : qq;
    }
    return neg ? -v : v;
}

double fmod(double x, double y) { return rem_d(x, y, 0, 0); }
double remainder(double x, double y) { return rem_d(x, y, 1, 0); }
double remquo(double x, double y, int *quo) { return rem_d(x, y, 1, quo); }

// The float forms through the double ones, which is exact: the inputs
// widen without loss and the exact remainder of two floats is a float.
float fmodf(float x, float y) { return (float)rem_d(x, y, 0, 0); }
float remainderf(float x, float y) { return (float)rem_d(x, y, 1, 0); }
float remquof(float x, float y, int *quo) { return (float)rem_d(x, y, 1, quo); }

// --- long double: the same core over the x87's explicit 64-bit mantissa

static uint64_t unpack_ld(long double v, int *e) {
    union ldshape u = { v };
    int ex = u.i.se & 0x7fff;
    uint64_t m = u.i.m;
    // A pseudo-denormal's exponent field 0 still means 2^-16382.
    *e = (ex ? ex : 1) - 16383 - 63;
    int s = __builtin_clzll(m);
    *e -= s;
    return m << s;
}

static long double rem_ld(long double x, long double y, int nearest, int *quo) {
    if (quo) *quo = 0;
    if (isnan(x) || isnan(y)) return x + y;
    if (y == 0 || isinf(x)) return (x * y) / (x * y);
    if (isinf(y) || x == 0) return x;
    int ex, ey, er, flip;
    unsigned q;
    uint64_t mx = unpack_ld(x, &ex), my = unpack_ld(y, &ey), r;
    __m_rem_core(mx, ex, my, ey, nearest, &r, &er, &q, &flip);
    long double v = scalbnl((long double)r, er);
    if (quo) {
        int qq = (int)(q & 7);
        *quo = (!signbit(x) != !signbit(y)) ? -qq : qq;
    }
    return (!!signbit(x) ^ flip) ? -v : v;
}

long double fmodl(long double x, long double y) { return rem_ld(x, y, 0, 0); }
long double remainderl(long double x, long double y) { return rem_ld(x, y, 1, 0); }
long double remquol(long double x, long double y, int *quo) { return rem_ld(x, y, 1, quo); }
