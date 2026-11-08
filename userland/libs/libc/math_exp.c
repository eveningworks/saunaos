// The exponentials and logarithms in double, and what is built on them:
// exp exp2 expm1, log log2 log10 log1p, pow, cbrt, hypot, and the
// hyperbolic functions and their inverses.
//
// fdlibm's algorithms and minimax coefficients throughout (the set
// glibc and musl descend from; musl's arrangement of them). The shape
// of each: reduce to a small interval with an EXACT step -- x - k*ln2
// with ln2 split so that k*LN2_HI is exact, or a mantissa in
// [sqrt(2)/2, sqrt(2)) -- then a polynomial whose leading terms are
// added last, smallest first. Accuracy is measured per function by
// tools/tests/sweep/libc_diff.py --only math; the numbers are in
// docs/libc-design.md.
#include "math_internal.h"

static const double
    ln2_hi = 6.93147180369123816490e-01,   // the top 32 bits of ln 2
    ln2_lo = 1.90821492927058770002e-10,
    invln2 = 1.44269504088896338700e+00,
    huge = 1.0e+300,
    tiny = 1.0e-300;

// exp's polynomial: R(r^2) with exp(r) = 1 + r + r*c/(2-c).
static const double
    P1 =  1.66666666666666019037e-01,
    P2 = -2.77777777770155933842e-03,
    P3 =  6.61375632143793436117e-05,
    P4 = -1.65339022054652515390e-06,
    P5 =  4.13813679705723846039e-08;

// log's: log(1+f) = f - f^2/2 + s*(f^2/2 + R(z)), s = f/(2+f).
static const double
    Lg1 = 6.666666666666735130e-01,
    Lg2 = 3.999999999940941908e-01,
    Lg3 = 2.857142874366239149e-01,
    Lg4 = 2.222219843214978396e-01,
    Lg5 = 1.818357216161805012e-01,
    Lg6 = 1.531383769920937332e-01,
    Lg7 = 1.479819860511658591e-01;

double exp(double x) {
    static const double o_threshold = 7.09782712893383973096e+02;
    static const double u_threshold = -7.45133219101941108420e+02;
    uint32_t hx = m_hi(x);
    int sign = hx >> 31;
    hx &= 0x7fffffff;
    double hi = 0, lo = 0;
    int k = 0;
    if (hx >= 0x4086232b) {   // |x| >= 708.39, or NaN
        if (isnan(x)) return x + x;
        if (x > o_threshold) return huge * huge;   // overflow
        if (x < u_threshold) return tiny * tiny;   // underflow
    }
    if (hx > 0x3fd62e42) {   // |x| > 0.5 ln2: x = k*ln2 + r, |r| <= 0.5 ln2
        if (hx >= 0x3ff0a2b2) k = (int)(invln2 * x + (sign ? -0.5 : 0.5));   // |x| >= 1.5 ln2
        else k = 1 - sign - sign;
        hi = x - k * ln2_hi;   // exact: k*ln2_hi has 32 + 11 bits
        lo = k * ln2_lo;
        x = hi - lo;
    } else if (hx > 0x3e300000) {   // |x| > 2^-28
        hi = x;
    } else {
        FORCE_EVAL(huge + x);   // inexact
        return 1 + x;
    }
    double t = x * x;
    double c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    double y = 1 + (x * c / (2 - c) - lo + hi);
    if (k == 0) return y;
    return scalbn(y, k);
}

// 2^x = 2^n * 2^f with n = rint(x) and f = x - n EXACT, |f| <= 1/2;
// 2^f = exp(f*ln2), with the product's rounding error carried as t_lo
// rather than lost -- the obvious exp(x*ln2) loses it scaled by |x|.
double exp2(double x) {
    if (isnan(x)) return x + x;
    if (x >= 1024) return huge * huge;
    if (x < -1075) return tiny * tiny;
    double n = rint(x), f = x - n;
    static const double LN2 = 6.93147180559945286227e-01, LN2_TAIL = 2.319046813846299558e-17;
    const double split = 134217729.0;   // 2^27 + 1
    double cf = split * f, fh = cf - (cf - f), fl = f - fh;
    double cl = split * LN2, lh = cl - (cl - LN2), ll = LN2 - lh;
    double th = f * LN2;
    double tl = ((fh * lh - th) + fh * ll + fl * lh) + fl * ll + f * LN2_TAIL;
    double e = exp(th);
    return scalbn(e + e * tl, (int)n);
}

double expm1(double x) {
    static const double o_threshold = 7.09782712893383973096e+02;
    static const double
        Q1 = -3.33333333333331316428e-02,
        Q2 =  1.58730158725481460165e-03,
        Q3 = -7.93650757867487942473e-05,
        Q4 =  4.00821782732936239552e-06,
        Q5 = -2.01099218183624371326e-07;
    uint64_t ub = m_bits(x);
    uint32_t hx = (uint32_t)(ub >> 32) & 0x7fffffff;
    int k, sign = (int)(ub >> 63);
    double hi, lo, c = 0, t, e, y;
    if (hx >= 0x4043687A) {   // |x| >= 56*ln2
        if (isnan(x)) return x;
        if (sign) return -1;
        if (x > o_threshold) return x * 0x1p1023;
    }
    if (hx > 0x3fd62e42) {   // |x| > 0.5 ln2
        if (hx < 0x3FF0A2B2) {   // and < 1.5 ln2
            if (!sign) { hi = x - ln2_hi; lo = ln2_lo; k = 1; }
            else       { hi = x + ln2_hi; lo = -ln2_lo; k = -1; }
        } else {
            k = (int)(invln2 * x + (sign ? -0.5 : 0.5));
            t = k;
            hi = x - t * ln2_hi;
            lo = t * ln2_lo;
        }
        x = hi - lo;
        c = (hi - x) - lo;
    } else if (hx < 0x3c900000) {   // |x| < 2^-54
        if (hx < 0x00100000) FORCE_EVAL((float)x);
        return x;
    } else {
        k = 0;
    }
    // x is now in the primary range.
    double hfx = 0.5 * x, hxs = x * hfx;
    double r1 = 1.0 + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    t = 3.0 - r1 * hfx;
    e = hxs * ((r1 - t) / (6.0 - x * t));
    if (k == 0) return x - (x * e - hxs);
    e = x * (e - c) - c;
    e -= hxs;
    // exp(x) ~ 2^k (x_reduced - e + 1)
    if (k == -1) return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return 1.0 + 2.0 * (x - e);
    }
    double twopk = m_dbl((uint64_t)(0x3ff + k) << 52);
    if (k < 0 || k > 56) {   // exp(x) - 1 is exp(x) to working precision
        y = x - e + 1.0;
        if (k == 1024) y = y * 2.0 * 0x1p1023;
        else y = y * twopk;
        return y - 1.0;
    }
    double u = m_dbl((uint64_t)(0x3ff - k) << 52);   // 2^-k
    if (k < 20) y = (x - e + (1 - u)) * twopk;
    else        y = (x - (e + u) + 1) * twopk;
    return y;
}

// The reduction every log shares: x = 2^k * (1+f), 1+f in
// [sqrt(2)/2, sqrt(2)). Returns 0 with *out set for the special values.
static int log_reduce(double x, int *k, double *f, double *out) {
    uint64_t u = m_bits(x);
    uint32_t hx = (uint32_t)(u >> 32);
    *k = 0;
    if (hx < 0x00100000 || hx >> 31) {
        if (u << 1 == 0) { *out = -1 / (x * x); return 0; }   // log(+-0) = -inf
        if (hx >> 31) { *out = (x - x) / 0.0; return 0; }     // log(<0) = NaN
        // A subnormal: scale it up.
        *k -= 54;
        x *= 0x1p54;
        u = m_bits(x);
        hx = (uint32_t)(u >> 32);
    } else if (hx >= 0x7ff00000) {
        *out = x;
        return 0;
    } else if (hx == 0x3ff00000 && (uint32_t)u == 0) {
        *out = 0;
        return 0;
    }
    // Reduce x into [sqrt(2)/2, sqrt(2)].
    hx += 0x3ff00000 - 0x3fe6a09e;
    *k += (int)(hx >> 20) - 0x3ff;
    hx = (hx & 0x000fffff) + 0x3fe6a09e;
    u = (uint64_t)hx << 32 | (u & 0xffffffff);
    *f = m_dbl(u) - 1.0;
    return 1;
}

// log(1+f) - f + f^2/2 = s*(f^2/2 + R): the part below the leading terms.
static double log_tail(double f, double *hfsq_out) {
    double hfsq = 0.5 * f * f, s = f / (2.0 + f), z = s * s, w = z * z;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    *hfsq_out = hfsq;
    return s * (hfsq + t2 + t1);
}

double log(double x) {
    int k;
    double f, out, hfsq;
    if (!log_reduce(x, &k, &f, &out)) return out;
    double tail = log_tail(f, &hfsq);
    double dk = k;
    return tail + dk * ln2_lo - hfsq + f + dk * ln2_hi;
}

// log2 and log10 carry log(1+f) as hi + lo, hi with its low 32 bits
// cleared, so the multiplication by 1/ln2 (or 1/ln10, as hi + lo too)
// keeps the bits a single rounded product would lose.
double log2(double x) {
    static const double ivln2hi = 1.44269504072144627571e+00, ivln2lo = 1.67517131648865118353e-10;
    int k;
    double f, out, hfsq;
    if (!log_reduce(x, &k, &f, &out)) return out;
    double tail = log_tail(f, &hfsq);
    double hi = m_trunc32(f - hfsq);
    double lo = f - hi - hfsq + tail;
    double val_hi = hi * ivln2hi;
    double val_lo = (lo + hi) * ivln2lo + lo * ivln2hi;
    double y = k, w = y + val_hi;
    val_lo += (y - w) + val_hi;
    return val_lo + w;
}

double log10(double x) {
    static const double
        ivln10hi = 4.34294481878168880939e-01,
        ivln10lo = 2.50829467116452752298e-11,
        log10_2hi = 3.01029995663611771306e-01,
        log10_2lo = 3.69423907715893078616e-13;
    int k;
    double f, out, hfsq;
    if (!log_reduce(x, &k, &f, &out)) return out;
    double tail = log_tail(f, &hfsq);
    double hi = m_trunc32(f - hfsq);
    double lo = f - hi - hfsq + tail;
    double val_hi = hi * ivln10hi;
    double dk = k, y = dk * log10_2hi;
    double val_lo = dk * log10_2lo + (lo + hi) * ivln10lo + lo * ivln10hi;
    double w = y + val_hi;
    val_lo += (y - w) + val_hi;
    return val_lo + w;
}

double log1p(double x) {
    uint64_t u = m_bits(x);
    uint32_t hx = (uint32_t)(u >> 32), hu;
    int k = 1;
    double c = 0, f = 0;
    if (hx < 0x3fda827a || hx >> 31) {   // 1+x < sqrt(2)+
        if (hx >= 0xbff00000) {   // x <= -1.0
            if (x == -1) return x / 0.0;    // -inf, divide by zero
            return (x - x) / 0.0;           // NaN
        }
        if (hx << 1 < 0x3ca00000u << 1) {   // |x| < 2^-53
            if ((hx & 0x7ff00000) == 0) FORCE_EVAL((float)x);
            return x;
        }
        if (hx <= 0xbfd2bec4) {   // sqrt(2)/2- <= 1+x < sqrt(2)+
            k = 0;
            c = 0;
            f = x;
        }
    } else if (hx >= 0x7ff00000) {
        return x;
    }
    if (k) {
        double uf = 1 + x;
        u = m_bits(uf);
        hu = (uint32_t)(u >> 32);
        hu += 0x3ff00000 - 0x3fe6a09e;
        k = (int)(hu >> 20) - 0x3ff;
        // The rounding error of 1+x, carried as c, divided by 1+x.
        if (k < 54) {
            c = k >= 2 ? 1 - (uf - x) : x - (uf - 1);
            c /= uf;
        } else {
            c = 0;
        }
        hu = (hu & 0x000fffff) + 0x3fe6a09e;
        u = (uint64_t)hu << 32 | (u & 0xffffffff);
        f = m_dbl(u) - 1;
    }
    double hfsq;
    double tail = log_tail(f, &hfsq);
    double dk = k;
    return tail + (dk * ln2_lo + c) - hfsq + f + dk * ln2_hi;
}

// --- pow --------------------------------------------------------------
//
// fdlibm's e_pow: log2(|x|) to about 2^-64 relative as t1 + t2 (an
// atanh-like series in s = (x-1)/(x+1), split hi + lo at every step),
// times y split the same way, then 2^(that) by exp's polynomial. THE
// SPECIAL CASES ARE THE HARD PART: C99 Annex F specifies about twenty
// of them (pow(-1, +-inf) = 1, pow(x, +-0) = 1 for any x even a NaN,
// a negative base with an odd-integer exponent, ...), and most
// disagreement between libms lives there rather than in the arithmetic.

double pow(double x, double y) {
    static const double
        bp[] = { 1.0, 1.5 },
        dp_h[] = { 0.0, 5.84962487220764160156e-01 },
        dp_l[] = { 0.0, 1.35003920212974897128e-08 },
        two53 = 9007199254740992.0,
        L1 = 5.99999999999994648725e-01,
        L2 = 4.28571428578550184252e-01,
        L3 = 3.33333329818377432918e-01,
        L4 = 2.72728123808534006489e-01,
        L5 = 2.30660745775561754067e-01,
        L6 = 2.06975017800338417784e-01,
        lg2 = 6.93147180559945286227e-01,
        lg2_h = 6.93147182464599609375e-01,
        lg2_l = -1.90465429995776804525e-09,
        ovt = 8.0085662595372944372e-17,
        cp = 9.61796693925975554329e-01,
        cp_h = 9.61796700954437255859e-01,
        cp_l = -7.02846165095275826516e-09,
        ivln2 = 1.44269504088896338700e+00,
        ivln2_h = 1.44269502162933349609e+00,
        ivln2_l = 1.92596299112661746887e-08;
    double z, ax, z_h, z_l, p_h, p_l, y1, t1, t2, r, s, t, u, v, w;
    int32_t i, j, k, yisint, n, hx, hy, ix, iy;
    uint32_t lx, ly;

    hx = (int32_t)m_hi(x); lx = m_lo(x);
    hy = (int32_t)m_hi(y); ly = m_lo(y);
    ix = hx & 0x7fffffff;
    iy = hy & 0x7fffffff;

    if ((iy | ly) == 0) return 1.0;                   // x**0 = 1, even a NaN
    if (hx == 0x3ff00000 && lx == 0) return 1.0;      // 1**y = 1, even a NaN
    if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lx != 0) ||
        iy > 0x7ff00000 || (iy == 0x7ff00000 && ly != 0))
        return x + y;

    // yisint: 0 y is not an integer, 1 an odd one, 2 an even one --
    // asked only when x < 0.
    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000) {
            yisint = 2;   // |y| >= 2^53: even
        } else if (iy >= 0x3ff00000) {
            k = (iy >> 20) - 0x3ff;
            if (k > 20) {
                uint32_t jj = ly >> (52 - k);
                if ((jj << (52 - k)) == ly) yisint = 2 - (int32_t)(jj & 1);
            } else if (ly == 0) {
                uint32_t jj = (uint32_t)iy >> (20 - k);
                if ((int32_t)(jj << (20 - k)) == iy) yisint = 2 - (int32_t)(jj & 1);
            }
        }
    }

    if (ly == 0) {
        if (iy == 0x7ff00000) {   // y is +-inf
            if (((ix - 0x3ff00000) | lx) == 0) return 1.0;      // (-1)**+-inf
            if (ix >= 0x3ff00000) return hy >= 0 ? y : 0.0;     // |x| > 1
            return hy >= 0 ? 0.0 : -y;                          // |x| < 1
        }
        if (iy == 0x3ff00000) return hy >= 0 ? x : 1.0 / x;     // y is +-1
        if (hy == 0x40000000) return x * x;                     // y is 2
        if (hy == 0x3fe00000 && hx >= 0) return sqrt(x);        // y is 0.5, x >= +0
    }

    ax = fabs(x);
    if (lx == 0 && (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000)) {   // +-0, +-inf, +-1
        z = ax;
        if (hy < 0) z = 1.0 / z;
        if (hx < 0) {
            if (((ix - 0x3ff00000) | yisint) == 0) z = (z - z) / (z - z);   // (-1)**non-int
            else if (yisint == 1) z = -z;
        }
        return z;
    }

    s = 1.0;   // the sign of the result
    if (hx < 0) {
        if (yisint == 0) return (x - x) / (x - x);   // (x<0)**(non-int)
        if (yisint == 1) s = -1.0;
    }

    if (iy > 0x41e00000) {   // |y| > 2^31
        if (iy > 0x43f00000) {   // |y| > 2^64: must over/underflow
            if (ix <= 0x3fefffff) return hy < 0 ? huge * huge : tiny * tiny;
            if (ix >= 0x3ff00000) return hy > 0 ? huge * huge : tiny * tiny;
        }
        if (ix < 0x3fefffff) return hy < 0 ? s * huge * huge : s * tiny * tiny;
        if (ix > 0x3ff00000) return hy > 0 ? s * huge * huge : s * tiny * tiny;
        // |1-x| <= 2^-20: log(x) by its series is enough.
        t = ax - 1.0;
        w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        u = ivln2_h * t;
        v = t * ivln2_l - w * ivln2;
        t1 = m_trunc32(u + v);
        t2 = v - (t1 - u);
    } else {
        double ss, s2, s_h, s_l, t_h, t_l;
        n = 0;
        if (ix < 0x00100000) {   // subnormal x
            ax *= two53;
            n -= 53;
            ix = (int32_t)m_hi(ax);
        }
        n += ((ix) >> 20) - 0x3ff;
        j = ix & 0x000fffff;
        ix = j | 0x3ff00000;   // normalised
        if (j <= 0x3988E) k = 0;          // |x| < sqrt(3/2)
        else if (j < 0xBB67A) k = 1;      // |x| < sqrt(3)
        else { k = 0; n += 1; ix -= 0x00100000; }
        ax = m_dbl(((uint64_t)(uint32_t)ix << 32) | m_lo(ax));

        // ss = s_h + s_l = (x-1)/(x+1) or (x-1.5)/(x+1.5)
        u = ax - bp[k];
        v = 1.0 / (ax + bp[k]);
        ss = u * v;
        s_h = m_trunc32(ss);
        t_h = m_dbl((uint64_t)(uint32_t)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)) << 32);
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l);
        // log(ax)
        s2 = ss * ss;
        r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = m_trunc32(3.0 + s2 + r);
        t_l = r - ((t_h - 3.0) - s2);
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        // 2/(3 log2) * (ss + ...)
        p_h = m_trunc32(u + v);
        p_l = v - (p_h - u);
        z_h = cp_h * p_h;
        z_l = cp_l * p_h + p_l * cp + dp_l[k];
        // log2(ax) = (ss + ...) * 2/(3 log2) = n + dp_h + z_h + z_l
        t = (double)n;
        t1 = m_trunc32(((z_h + z_l) + dp_h[k]) + t);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }

    // (y1 + y2) * (t1 + t2), y split the same way
    y1 = m_trunc32(y);
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    j = (int32_t)m_hi(z);
    i = (int32_t)m_lo(z);
    if (j >= 0x40900000) {   // z >= 1024
        if (((j - 0x40900000) | i) != 0) return s * huge * huge;
        if (p_l + ovt > z - p_h) return s * huge * huge;
    } else if ((j & 0x7fffffff) >= 0x4090cc00) {   // z <= -1075
        if (((uint32_t)j - 0xc090cc00u) | (uint32_t)i) return s * tiny * tiny;
        if (p_l <= z - p_h) return s * tiny * tiny;
    }

    // 2**(p_h + p_l)
    i = j & 0x7fffffff;
    k = (i >> 20) - 0x3ff;
    n = 0;
    if (i > 0x3fe00000) {   // |z| > 0.5: n = [z + 0.5]
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 20) - 0x3ff;
        t = m_dbl((uint64_t)(uint32_t)(n & ~(0x000fffff >> k)) << 32);
        n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    t = m_trunc32(p_l + p_h);
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    r = (z * t1) / (t1 - 2.0) - (w + z * w);
    z = 1.0 - (r - z);
    j = (int32_t)m_hi(z);
    j += (int32_t)((uint32_t)n << 20);
    if ((j >> 20) <= 0) z = scalbn(z, n);   // subnormal result
    else z = m_dbl(((uint64_t)(uint32_t)j << 32) | m_lo(z));
    return s * z;
}

// --- cbrt, hypot ------------------------------------------------------

// A 5-bit guess from the exponent bits divided by three (Kahan's
// trick), a polynomial to 23 bits, then one Newton step rounded to 53.
double cbrt(double x) {
    static const uint32_t B1 = 715094163, B2 = 696219795;
    static const double
        Q0 =  1.87595182427177009643,
        Q1 = -1.88497979543377169875,
        Q2 =  1.621429720105354466140,
        Q3 = -0.758397934778766047437,
        Q4 =  0.145996192886612446982;
    uint64_t u = m_bits(x);
    uint32_t hx = (uint32_t)(u >> 32) & 0x7fffffff;
    if (hx >= 0x7ff00000) return x + x;
    if (hx < 0x00100000) {   // zero or subnormal
        u = m_bits(x * 0x1p54);
        hx = (uint32_t)(u >> 32) & 0x7fffffff;
        if (hx == 0) return x;
        hx = hx / 3 + B2;
    } else {
        hx = hx / 3 + B1;
    }
    u &= 1ull << 63;
    u |= (uint64_t)hx << 32;
    double t = m_dbl(u);
    double r = (t * t) * (t / x);
    t = t * ((Q0 + r * (Q1 + r * Q2)) + ((r * r) * r) * (Q3 + r * Q4));
    // Round t away from zero to 23 bits, so t*t is exact.
    u = m_bits(t);
    u = (u + 0x80000000) & 0xffffffffc0000000ull;
    t = m_dbl(u);
    double s = t * t;
    r = x / s;
    double w = t + t;
    r = (r - t) / (w + r);
    return t + t * r;
}

// The squares are formed EXACTLY (hi + lo by Dekker's split), so the
// only rounding is in the final sum and square root; scaled first so
// nothing over- or underflows that the result does not.
static void sq(double *hi, double *lo, double x) {
    const double split = 0x1p27 + 1;
    double xc = x * split, xh = x - xc + xc, xl = x - xh;
    *hi = x * x;
    *lo = xh * xh - *hi + 2 * xh * xl + xl * xl;
}

double hypot(double x, double y) {
    uint64_t ux = m_bits(x) & ~(1ull << 63), uy = m_bits(y) & ~(1ull << 63);
    if (ux < uy) { uint64_t t = ux; ux = uy; uy = t; }
    int ex = (int)(ux >> 52), ey = (int)(uy >> 52);
    x = m_dbl(ux);
    y = m_dbl(uy);
    // hypot(inf, NaN) is inf: the infinity wins, in either order.
    if (ey == 0x7ff) return y;
    if (ex == 0x7ff || uy == 0) return x;
    if (ex - ey > 64) return x + y;
    double z = 1;
    if (ex > 0x3ff + 510) { z = 0x1p700; x *= 0x1p-700; y *= 0x1p-700; }
    else if (ey < 0x3ff - 450) { z = 0x1p-700; x *= 0x1p700; y *= 0x1p700; }
    double hx, lx, hy, ly;
    sq(&hx, &lx, x);
    sq(&hy, &ly, y);
    return z * sqrt(ly + lx + hy + hx);
}

// --- the hyperbolics --------------------------------------------------

// exp(x)*sign for x up to 2*709, where exp(x) alone overflows but
// exp(x)/2 (sinh, cosh) need not: exp(x - k ln2) * 2^(k-1), k = 2043.
double __m_expo2(double x, double sign) {
    static const double kln2 = 0x1.62066151add8bp+10;
    double scale = m_dbl((uint64_t)(0x3ff + 2043 / 2) << 52);
    return exp(x - kln2) * (sign * scale) * scale;
}

double sinh(double x) {
    uint64_t u = m_bits(x);
    double h = (u >> 63) ? -0.5 : 0.5;
    u &= ~(1ull << 63);
    double absx = m_dbl(u);
    uint32_t w = (uint32_t)(u >> 32);
    if (w < 0x40862e42) {   // |x| < log(DBL_MAX)
        double t = expm1(absx);
        if (w < 0x3ff00000) {
            if (w < 0x3ff00000 - (26 << 20)) return x;   // tiny: sinh(x) = x
            return h * (2 * t - t * t / (t + 1));
        }
        return h * (t + t / (t + 1));
    }
    return __m_expo2(absx, 2 * h);
}

double cosh(double x) {
    uint64_t u = m_bits(x) & ~(1ull << 63);
    x = m_dbl(u);
    uint32_t w = (uint32_t)(u >> 32);
    if (w < 0x3fe62e42) {   // |x| < ln2
        if (w < 0x3ff00000 - (26 << 20)) {
            FORCE_EVAL(x + 0x1p120f);
            return 1;
        }
        double t = expm1(x);
        return 1 + t * t / (2 * (1 + t));
    }
    if (w < 0x40862e42) {   // |x| < log(DBL_MAX)
        double t = exp(x);
        return 0.5 * (t + 1 / t);
    }
    return __m_expo2(x, 1.0);
}

double tanh(double x) {
    uint64_t u = m_bits(x);
    int sign = (int)(u >> 63);
    u &= ~(1ull << 63);
    x = m_dbl(u);
    uint32_t w = (uint32_t)(u >> 32);
    double t;
    if (w > 0x3fe193ea) {   // |x| > log(3)/2 ~ 0.5493, or NaN
        if (w > 0x40340000) {   // |x| > 20, or NaN
            t = 1 - 0 / x;      // 1, with inexact; NaN stays NaN
        } else {
            t = expm1(2 * x);
            t = 1 - 2 / (t + 2);
        }
    } else if (w > 0x3fd058ae) {   // |x| > log(5/3)/2 ~ 0.2554
        t = expm1(2 * x);
        t = t / (t + 2);
    } else if (w >= 0x00100000) {
        t = expm1(-2 * x);
        t = -t / (t + 2);
    } else {
        FORCE_EVAL((float)x);
        t = x;
    }
    return sign ? -t : t;
}

double asinh(double x) {
    uint64_t u = m_bits(x);
    unsigned e = (unsigned)(u >> 52 & 0x7ff);
    int s = (int)(u >> 63);
    x = m_dbl(u & ~(1ull << 63));
    if (e >= 0x3ff + 26) {          // |x| >= 2^26: log(2x)
        x = log(x) + 0.693147180559945309417232121458176568;
    } else if (e >= 0x3ff + 1) {    // |x| >= 2
        x = log(2 * x + 1 / (sqrt(x * x + 1) + x));
    } else if (e >= 0x3ff - 26) {   // |x| >= 2^-26
        x = log1p(x + x * x / (sqrt(x * x + 1) + 1));
    } else {
        FORCE_EVAL(x + 0x1p120f);
    }
    return s ? -x : x;
}

double acosh(double x) {
    unsigned e = (unsigned)(m_bits(x) >> 52 & 0x7ff);
    if (e < 0x3ff + 1)    // |x| < 2: NaN below 1 by the square root
        return log1p(x - 1 + sqrt((x - 1) * (x - 1) + 2 * (x - 1)));
    if (e < 0x3ff + 26)   // |x| < 2^26
        return log(2 * x - 1 / (x + sqrt(x * x - 1)));
    return log(x) + 0.693147180559945309417232121458176568;   // NaN for x < 0
}

double atanh(double x) {
    uint64_t u = m_bits(x);
    unsigned e = (unsigned)(u >> 52 & 0x7ff);
    int s = (int)(u >> 63);
    double y = m_dbl(u & ~(1ull << 63));
    if (e < 0x3ff - 1) {   // |x| < 0.5
        if (e < 0x3ff - 32) {
            if (e == 0) FORCE_EVAL((float)y);
        } else {
            y = 0.5 * log1p(2 * y + 2 * y * y / (1 - y));
        }
    } else {
        y = 0.5 * log1p(2 * (y / (1 - y)));   // inf at 1, NaN past it
    }
    return s ? -y : y;
}
