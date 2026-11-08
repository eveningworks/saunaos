// What the math_*.c files share and nothing else includes: the bit
// views of the three floating types, the x87 instructions the long
// double half is built on, and the kernels one file computes and
// another calls. The split itself is <math.h>'s header comment.
#ifndef TOLIBC_MATH_INTERNAL_H
#define TOLIBC_MATH_INTERNAL_H

#include <math.h>
#include <stdint.h>

#define M_HIDDEN __attribute__((visibility("hidden")))

static inline uint64_t m_bits(double v) { uint64_t b; __builtin_memcpy(&b, &v, 8); return b; }
static inline double m_dbl(uint64_t b) { double v; __builtin_memcpy(&v, &b, 8); return v; }
static inline uint32_t m_fbits(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); return b; }
static inline float m_flt(uint32_t b) { float v; __builtin_memcpy(&v, &b, 4); return v; }
static inline uint32_t m_hi(double v) { return (uint32_t)(m_bits(v) >> 32); }
static inline uint32_t m_lo(double v) { return (uint32_t)m_bits(v); }
// The low 32 bits cleared: a double whose product with another such
// double is EXACT, the splitting every fdlibm kernel leans on.
static inline double m_trunc32(double v) { return m_dbl(m_bits(v) & 0xFFFFFFFF00000000ull); }

// The x87 80-bit format: an EXPLICIT integer bit (bit 63 of the
// mantissa), unlike double's hidden one, then 15 exponent bits and the
// sign. Ten bytes of a sixteen-byte object; the rest is padding.
union ldshape {
    long double f;
    struct { uint64_t m; uint16_t se; } i;
};
static inline uint64_t ld_mant(long double v) { union ldshape u = { v }; return u.i.m; }
static inline int ld_sexp(long double v) { union ldshape u = { v }; return u.i.se; }
static inline long double ld_make(uint64_t m, int se) {
    union ldshape u; u.f = 0; u.i.m = m; u.i.se = (uint16_t)se; return u.f;
}

// Evaluated for its side effect -- the floating-point exception it
// raises -- and kept by the volatile against an optimiser that sees an
// unused value.
#define FORCE_EVAL(x) do { volatile __typeof__(x) m_fe_ = (x); (void)m_fe_; } while (0)

// --- the x87 instructions (the long double half, math_ld*.c) ----------
//
// The constraints are GCC's documented x87 forms: "t" is st(0), "u"
// st(1), and an instruction that pops st(1) names it as clobbered.

static inline long double x87_f2xm1(long double x) {   // 2^x - 1, |x| <= 1
    long double r; __asm__("f2xm1" : "=t"(r) : "0"(x)); return r;
}
static inline long double x87_fyl2x(long double y, long double x) {  // y*log2(x)
    long double r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline long double x87_fyl2xp1(long double y, long double x) {  // y*log2(1+x)
    long double r; __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline long double x87_fpatan(long double y, long double x) {  // atan2(y, x)
    long double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline long double x87_fscale(long double x, long double n) {  // x * 2^trunc(n)
    long double r; __asm__("fscale" : "=t"(r) : "0"(x), "u"(n)); return r;
}
static inline long double x87_frndint(long double x) {  // to an integer, in the current mode
    long double r; __asm__("frndint" : "=t"(r) : "0"(x)); return r;
}
static inline long double x87_fsqrt(long double x) {
    long double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r;
}
static inline long double x87_ln2(void) { long double r; __asm__("fldln2" : "=t"(r)); return r; }
static inline long double x87_lg2(void) { long double r; __asm__("fldlg2" : "=t"(r)); return r; }

// a*b as an exact hi + lo, by Veltkamp's split at half the mantissa
// (2^32 + 1 for 64 bits). Dekker's product; no FMA on the x87.
static inline void ld_two_prod(long double a, long double b, long double *hi, long double *lo) {
    const long double split = 4294967297.0L;
    long double ca = split * a, ah = ca - (ca - a), al = a - ah;
    long double cb = split * b, bh = cb - (cb - b), bl = b - bh;
    *hi = a * b;
    *lo = ((ah * bh - *hi) + ah * bl + al * bh) + al * bl;
}

// --- the shared kernels ----------------------------------------------

// fdlibm's kernels on |x| <= ~pi/4, with y the low part of a reduced
// argument (math_trig.c).
M_HIDDEN double __m_sin(double x, double y, int iy);
M_HIDDEN double __m_cos(double x, double y);
M_HIDDEN double __m_tan(double x, double y, int odd);
// x reduced by pi/2 (math_pio2.c): returns the quadrant (mod 4) and
// x - n*pi/2 as y[0] + y[1]. For any finite x, to full precision.
M_HIDDEN int __m_rem_pio2(double x, double *y);
// The same for a long double, as *hi + *lo.
M_HIDDEN int __m_rem_pio2l(long double x, long double *hi, long double *lo);
// exp(x) * sign, for x where exp(x) alone overflows but the scaled
// result (sinh, cosh) does not.
M_HIDDEN double __m_expo2(double x, double sign);
// The long double sin/cos kernels on |x| <= ~pi/4 (math_ld_trig.c).
M_HIDDEN long double __m_sinl(long double x, long double y);
M_HIDDEN long double __m_cosl(long double x, long double y);

// The exact remainder of |x| by |y| (math_rem.c): both as integer
// mantissa and binary exponent, value = m * 2^e. NEAREST selects
// remainder() (quotient rounded to even) over fmod() (truncated).
// Returns the remainder's mantissa in *rm at exponent *re, and the low
// bits of the quotient in *q; *flip is 1 when the result's sign is
// the opposite of x's (remainder only).
M_HIDDEN void __m_rem_core(uint64_t mx, int ex, uint64_t my, int ey, int nearest,
                           uint64_t *rm, int *re, unsigned *q, int *flip);

#endif
