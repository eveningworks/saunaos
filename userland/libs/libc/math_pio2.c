// x mod pi/2, for any finite double or long double: the argument
// reduction sin, cos and tan stand on.
//
// **THE REDUCTION IS WHERE A TRIG FUNCTION'S ACCURACY LIVES.** The
// polynomials only ever see |r| <= pi/4; everything a large argument can
// get wrong happens here. Subtracting n*pi/2 with pi/2 held in a few
// doubles (Cody-Waite) is exact only while n is small -- the first
// libm here used it and was right to |x| ~ 1.6e6 and wrong after. This is
// Payne-Hanek, which every serious libm uses past the Cody-Waite range:
// x = m * 2^e with m an integer, so x * (2/pi) only needs the bits of
// 2/pi from about 2^-e onward -- the earlier ones contribute multiples of
// 4, which change no quadrant. A 256-bit window of them times the 64-bit
// m is an exact integer product; its top bits are the quadrant and the
// next 192 the fraction. With no Cody-Waite fast path in front: one
// path, exact for every argument, costs a few multiplies.
//
// THE PRECISION BUDGET. A double can lie within about 2^-61 of a
// multiple of pi/2 (and a long double, with more numbers to choose
// from, nearer still), so the fraction loses that many leading zeros
// before it has any significant bits. 192 bits of fraction leaves well
// over a long double's 64 after the worst case, and the window's
// truncation error, below 2^-190, is under all of them.
//
// The bits of 2/pi are math_tables.h, generated from integers alone by
// tools/gen/gen_libm_tables.py.
#include "math_internal.h"
#include "math_tables.h"

typedef unsigned __int128 u128;

// The bit stream of 2/pi, 64 bits starting at bit index i (0 = the
// first bit after the point). Bits before the point are zero, which is
// what lets a small x use the same window arithmetic as a huge one.
static uint64_t stream64(int i) {
    int w = i >= 0 ? i / 64 : -((-i + 63) / 64);
    int s = i - w * 64;
    uint64_t a = (w >= 0 && w < M_TWO_OVER_PI_WORDS) ? m_two_over_pi[w] : 0;
    uint64_t b = (w + 1 >= 0 && w + 1 < M_TWO_OVER_PI_WORDS) ? m_two_over_pi[w + 1] : 0;
    return s ? (a << s) | (b >> (64 - s)) : a;
}

// |x| = m * 2^e (m < 2^64, any e the formats allow). Returns n mod 4
// and r = |x| - n*pi/2 as *hi + *lo, |r| <= pi/4.
static int payne_hanek(uint64_t m, int e, long double *hi, long double *lo) {
    // The window starts at weight 2^-(e-1): every bit before it,
    // times m * 2^e, is a multiple of 4. g[0] is the most significant.
    uint64_t g[4];
    for (int j = 0; j < 4; j++) g[j] = stream64(e - 2 + 64 * j);
    // P = m * G, 320 bits, P[0] least significant. Its value is
    // P * 2^-254: bits 254 and 255 are the quadrant.
    uint64_t P[5];
    u128 carry = 0;
    for (int j = 3; j >= 0; j--) {
        u128 t = (u128)m * g[j] + carry;
        P[3 - j] = (uint64_t)t;
        carry = t >> 64;
    }
    P[4] = (uint64_t)carry;
    int n = (int)(P[3] >> 62);
    // The fraction, 192 bits (f[0] most significant) from bit 253 down.
    uint64_t f[3];
    f[0] = (P[3] << 2) | (P[2] >> 62);
    f[1] = (P[2] << 2) | (P[1] >> 62);
    f[2] = (P[1] << 2) | (P[0] >> 62);
    int neg = 0;
    if (f[0] >> 63) {
        // Past one half: the nearer multiple is the next one, and the
        // remainder is negative -- 1 - f, by two's complement.
        n = (n + 1) & 3;
        neg = 1;
        f[2] = ~f[2] + 1;
        f[1] = ~f[1] + (f[2] == 0);
        f[0] = ~f[0] + (f[2] == 0 && f[1] == 0);
    }
    // Normalise the fraction so its top bit is set, keeping 128 bits.
    int shift = 0;
    while (f[0] == 0 && shift < 128) {
        f[0] = f[1]; f[1] = f[2]; f[2] = 0; shift += 64;
    }
    if (f[0] == 0) { *hi = *lo = 0; return n; }   // cannot happen for a finite x
    int z = __builtin_clzll(f[0]);
    if (z) {
        f[0] = (f[0] << z) | (f[1] >> (64 - z));
        f[1] = (f[1] << z) | (f[2] >> (64 - z));
    }
    shift += z;
    // frac = (f0 + f1 * 2^-64) * 2^-(64 + shift); times pi/2 as a
    // double-long-double, then scaled. Each conversion is exact.
    long double a = (long double)f[0], b = (long double)f[1] * 0x1p-64L;
    long double ph, pl;
    ld_two_prod(a, M_PIO2L_HI, &ph, &pl);
    pl += a * M_PIO2L_LO + b * M_PIO2L_HI;
    long double h = ph + pl;
    long double l = pl - (h - ph);
    h = scalbnl(h, -64 - shift);
    l = scalbnl(l, -64 - shift);
    *hi = neg ? -h : h;
    *lo = neg ? -l : l;
    return n;
}

int __m_rem_pio2(double x, double *y) {
    uint64_t b = m_bits(x);
    int ex = (int)(b >> 52 & 0x7ff);
    uint64_t m = b & ((1ull << 52) - 1);
    if (ex) m |= 1ull << 52;
    long double hi, lo;
    int n = payne_hanek(m, (ex ? ex : 1) - 1075, &hi, &lo);
    double y0 = (double)(hi + lo);
    double y1 = (double)((hi - (long double)y0) + lo);
    if (b >> 63) { y0 = -y0; y1 = -y1; n = -n; }
    y[0] = y0;
    y[1] = y1;
    return n & 3;
}

int __m_rem_pio2l(long double x, long double *hi, long double *lo) {
    union ldshape u = { x };
    int ex = u.i.se & 0x7fff;
    int n = payne_hanek(u.i.m, (ex ? ex : 1) - 16383 - 63, hi, lo);
    if (u.i.se >> 15) { *hi = -*hi; *lo = -*lo; n = -n; }
    return n & 3;
}
