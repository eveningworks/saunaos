// The error and gamma functions in double: erf, erfc, lgamma (with
// signgam and lgamma_r) and tgamma.
//
// erf/erfc and lgamma are fdlibm's: a rational minimax fit on each of a
// few intervals, and for erfc's tail exp(-x^2) formed as
// exp(-z*z) * exp((z-x)(z+x)) with z = x's top half, so the square that
// a single exp(-x*x) would round is exact. tgamma is musl's: Lanczos'
// approximation (g = 6.0247, 13 terms, as a rational function), the
// reflection formula below zero, and factorials looked up exactly.
//
// **lgamma HAS NO ULP BOUND NEAR ITS ZEROS FOR x < 0** (x ~ -2.457,
// -2.747, -3.144, ...). There the answer is near 0 while the terms
// that produce it are not, so their rounding is a huge fraction of the
// result -- fdlibm's and musl's lgamma share this; glibc added special
// code for it in 2.22. The harness measures lgamma away from those
// zeros and says so. Measured accuracy: docs/libc-design.md.
#include "math_internal.h"

// --- erf, erfc --------------------------------------------------------

static const double
    erx  = 8.45062911510467529297e-01,
    efx8 = 1.02703333676410069053e+00,
    // |x| < 0.84375: erf(x) = x + x*R(x^2)
    pp0  =  1.28379167095512558561e-01,
    pp1  = -3.25042107247001499370e-01,
    pp2  = -2.84817495755985104766e-02,
    pp3  = -5.77027029648944159157e-03,
    pp4  = -2.37630166566501626084e-05,
    qq1  =  3.97917223959155352819e-01,
    qq2  =  6.50222499887672944485e-02,
    qq3  =  5.08130628187576562776e-03,
    qq4  =  1.32494738004321644526e-04,
    qq5  = -3.96022827877536812320e-06,
    // 0.84375 <= |x| < 1.25: erf(x) = erx + P(s)/Q(s), s = |x| - 1
    pa0  = -2.36211856075265944077e-03,
    pa1  =  4.14856118683748331666e-01,
    pa2  = -3.72207876035701323847e-01,
    pa3  =  3.18346619901161753674e-01,
    pa4  = -1.10894694282396677476e-01,
    pa5  =  3.54783043256182359371e-02,
    pa6  = -2.16637559486879084300e-03,
    qa1  =  1.06420880400844228286e-01,
    qa2  =  5.40397917702171048937e-01,
    qa3  =  7.18286544141962662868e-02,
    qa4  =  1.26171219808761642112e-01,
    qa5  =  1.36370839120290507362e-02,
    qa6  =  1.19844998467991074170e-02,
    // 1.25 <= |x| < 1/0.35: erfc(x) = exp(-x^2 - 0.5625 + R/S)/x
    ra0  = -9.86494403484714822705e-03,
    ra1  = -6.93858572707181764372e-01,
    ra2  = -1.05586262253232909814e+01,
    ra3  = -6.23753324503260060396e+01,
    ra4  = -1.62396669462573470355e+02,
    ra5  = -1.84605092906711035994e+02,
    ra6  = -8.12874355063065934246e+01,
    ra7  = -9.81432934416914548592e+00,
    sa1  =  1.96512716674392571292e+01,
    sa2  =  1.37657754143519042600e+02,
    sa3  =  4.34565877475229228821e+02,
    sa4  =  6.45387271733267880336e+02,
    sa5  =  4.29008140027567833386e+02,
    sa6  =  1.08635005541779435134e+02,
    sa7  =  6.57024977031928170135e+00,
    sa8  = -6.04244152148580987438e-02,
    // 1/0.35 <= |x| < 28
    rb0  = -9.86494292470009928597e-03,
    rb1  = -7.99283237680523006574e-01,
    rb2  = -1.77579549177547519889e+01,
    rb3  = -1.60636384855821916062e+02,
    rb4  = -6.37566443368389627722e+02,
    rb5  = -1.02509513161107724954e+03,
    rb6  = -4.83519191608651397019e+02,
    sb1  =  3.03380607434824582924e+01,
    sb2  =  3.25792512996573918826e+02,
    sb3  =  1.53672958608443695994e+03,
    sb4  =  3.19985821950859553908e+03,
    sb5  =  2.55305040643316442583e+03,
    sb6  =  4.74528541206955367215e+02,
    sb7  = -2.24409524465858183362e+01;

static double erfc1(double x) {
    double s = fabs(x) - 1;
    double P = pa0 + s * (pa1 + s * (pa2 + s * (pa3 + s * (pa4 + s * (pa5 + s * pa6)))));
    double Q = 1 + s * (qa1 + s * (qa2 + s * (qa3 + s * (qa4 + s * (qa5 + s * qa6)))));
    return 1 - erx - P / Q;
}

static double erfc2(uint32_t ix, double x) {
    if (ix < 0x3ff40000) return erfc1(x);   // |x| < 1.25
    x = fabs(x);
    double s = 1 / (x * x), R, S;
    if (ix < 0x4006db6d) {   // |x| < 1/0.35
        R = ra0 + s * (ra1 + s * (ra2 + s * (ra3 + s * (ra4 + s * (ra5 + s * (ra6 + s * ra7))))));
        S = 1.0 + s * (sa1 + s * (sa2 + s * (sa3 + s * (sa4 + s * (sa5 + s * (sa6 + s * (sa7 + s * sa8)))))));
    } else {
        R = rb0 + s * (rb1 + s * (rb2 + s * (rb3 + s * (rb4 + s * (rb5 + s * rb6)))));
        S = 1.0 + s * (sb1 + s * (sb2 + s * (sb3 + s * (sb4 + s * (sb5 + s * (sb6 + s * sb7))))));
    }
    double z = m_trunc32(x);
    return exp(-z * z - 0.5625) * exp((z - x) * (z + x) + R / S) / x;
}

double erf(double x) {
    uint32_t ix = m_hi(x);
    int sign = (int)(ix >> 31);
    ix &= 0x7fffffff;
    if (ix >= 0x7ff00000) return 1 - 2 * sign + 1 / x;   // NaN, or +-1 at +-inf
    if (ix < 0x3feb0000) {   // |x| < 0.84375
        if (ix < 0x3e300000) return 0.125 * (8 * x + efx8 * x);   // |x| < 2^-28, no underflow
        double z = x * x;
        double r = pp0 + z * (pp1 + z * (pp2 + z * (pp3 + z * pp4)));
        double s = 1.0 + z * (qq1 + z * (qq2 + z * (qq3 + z * (qq4 + z * qq5))));
        return x + x * (r / s);
    }
    double y = ix < 0x40180000 ? 1 - erfc2(ix, x) : 1 - 0x1p-1022;   // |x| < 6
    return sign ? -y : y;
}

double erfc(double x) {
    uint32_t ix = m_hi(x);
    int sign = (int)(ix >> 31);
    ix &= 0x7fffffff;
    if (ix >= 0x7ff00000) return 2 * sign + 1 / x;   // NaN, or 0 / 2 at +-inf
    if (ix < 0x3feb0000) {   // |x| < 0.84375
        if (ix < 0x3c700000) return 1.0 - x;   // |x| < 2^-56
        double z = x * x;
        double r = pp0 + z * (pp1 + z * (pp2 + z * (pp3 + z * pp4)));
        double s = 1.0 + z * (qq1 + z * (qq2 + z * (qq3 + z * (qq4 + z * qq5))));
        double y = r / s;
        if (sign || ix < 0x3fd00000) return 1.0 - (x + x * y);   // x < 1/4
        return 0.5 - (x - 0.5 + x * y);
    }
    if (ix < 0x403c0000) return sign ? 2 - erfc2(ix, x) : erfc2(ix, x);   // |x| < 28
    return sign ? 2 - 0x1p-1022 : 0x1p-1022 * 0x1p-1022;
}

// --- lgamma -----------------------------------------------------------

static const double pi = 3.14159265358979311600e+00;

// sin(pi*x) for x > 0 exactly reduced: x mod 2 is exact, and pi is
// applied only after the reduction to [-1/4, 1/4].
static double sin_pi(double x) {
    x = 2.0 * (x * 0.5 - floor(x * 0.5));   // x mod 2
    int n = (int)(x * 4.0);
    n = (n + 1) / 2;
    x -= n * 0.5;
    x *= pi;
    switch (n) {
    default:
    case 0: return __m_sin(x, 0.0, 0);
    case 1: return __m_cos(x, 0.0);
    case 2: return __m_sin(-x, 0.0, 0);
    case 3: return -__m_cos(x, 0.0);
    }
}

int signgam;

double lgamma_r(double x, int *signgamp) {
    static const double
        a0  =  7.72156649015328655494e-02, a1  =  3.22467033424113591611e-01,
        a2  =  6.73523010531292681824e-02, a3  =  2.05808084325167332806e-02,
        a4  =  7.38555086081402883957e-03, a5  =  2.89051383673415629091e-03,
        a6  =  1.19270763183362067845e-03, a7  =  5.10069792153511336608e-04,
        a8  =  2.20862790713908385557e-04, a9  =  1.08011567247583939954e-04,
        a10 =  2.52144565451257326939e-05, a11 =  4.48640949618915160150e-05,
        tc  =  1.46163214496836224576e+00, tf  = -1.21486290535849611461e-01,
        tt  = -3.63867699703950536541e-18,
        t0  =  4.83836122723810047042e-01, t1  = -1.47587722994593911752e-01,
        t2  =  6.46249402391333854778e-02, t3  = -3.27885410759859649565e-02,
        t4  =  1.79706750811820387126e-02, t5  = -1.03142241298341437450e-02,
        t6  =  6.10053870246291332635e-03, t7  = -3.68452016781138256760e-03,
        t8  =  2.25964780900612472250e-03, t9  = -1.40346469989232843813e-03,
        t10 =  8.81081882437654011382e-04, t11 = -5.38595305356740546715e-04,
        t12 =  3.15632070903625950361e-04, t13 = -3.12754168375120860518e-04,
        t14 =  3.35529192635519073543e-04,
        u0  = -7.72156649015328655494e-02, u1  =  6.32827064025093366517e-01,
        u2  =  1.45492250137234768737e+00, u3  =  9.77717527963372745603e-01,
        u4  =  2.28963728064692451092e-01, u5  =  1.33810918536787660377e-02,
        v1  =  2.45597793713041134822e+00, v2  =  2.12848976379893395361e+00,
        v3  =  7.69285150456672783825e-01, v4  =  1.04222645593369134254e-01,
        v5  =  3.21709242282423911810e-03,
        s0  = -7.72156649015328655494e-02, s1  =  2.14982415960608852501e-01,
        s2  =  3.25778796408930981787e-01, s3  =  1.46350472652464452805e-01,
        s4  =  2.66422703033638609560e-02, s5  =  1.84028451407337715652e-03,
        s6  =  3.19475326584100867617e-05,
        r1  =  1.39200533467621045958e+00, r2  =  7.21935547567138069525e-01,
        r3  =  1.71933865632803078993e-01, r4  =  1.86459191715652901344e-02,
        r5  =  7.77942496381893596434e-04, r6  =  7.32668430744625636189e-06,
        w0  =  4.18938533204672725052e-01, w1  =  8.33333333333329678849e-02,
        w2  = -2.77777777728775536470e-03, w3  =  7.93650558643019558500e-04,
        w4  = -5.95187557450339963135e-04, w5  =  8.36339918996282139126e-04,
        w6  = -1.63092934096575273989e-03;
    uint64_t u = m_bits(x);
    double t, y, z, nadj = 0, p, p1, p2, p3, q, r, w;
    int i;
    *signgamp = 1;
    int sign = (int)(u >> 63);
    uint32_t ix = (uint32_t)(u >> 32) & 0x7fffffff;
    if (ix >= 0x7ff00000) return x * x;
    if (ix < (0x3ff - 70) << 20) {   // |x| < 2^-70: -log(|x|)
        if (sign) { x = -x; *signgamp = -1; }
        return -log(x);
    }
    if (sign) {
        x = -x;
        t = sin_pi(x);
        if (t == 0.0) return 1.0 / (x - x);   // a non-positive integer: a pole
        if (t > 0.0) *signgamp = -1;
        else t = -t;
        nadj = log(pi / (t * x));
    }

    if ((ix == 0x3ff00000 || ix == 0x40000000) && (uint32_t)u == 0) {   // 1 and 2
        r = 0;
    } else if (ix < 0x40000000) {   // x < 2
        if (ix <= 0x3feccccc) {   // lgamma(x) = lgamma(x+1) - log(x)
            r = -log(x);
            if (ix >= 0x3FE76944)      { y = 1.0 - x; i = 0; }
            else if (ix >= 0x3FCDA661) { y = x - (tc - 1.0); i = 1; }
            else                       { y = x; i = 2; }
        } else {
            r = 0.0;
            if (ix >= 0x3FFBB4C3)      { y = 2.0 - x; i = 0; }   // [1.7316, 2]
            else if (ix >= 0x3FF3B4C4) { y = x - tc; i = 1; }    // [1.23, 1.73]
            else                       { y = x - 1.0; i = 2; }
        }
        switch (i) {
        case 0:
            z = y * y;
            p1 = a0 + z * (a2 + z * (a4 + z * (a6 + z * (a8 + z * a10))));
            p2 = z * (a1 + z * (a3 + z * (a5 + z * (a7 + z * (a9 + z * a11)))));
            p = y * p1 + p2;
            r += (p - 0.5 * y);
            break;
        case 1:
            z = y * y;
            w = z * y;
            p1 = t0 + w * (t3 + w * (t6 + w * (t9 + w * t12)));
            p2 = t1 + w * (t4 + w * (t7 + w * (t10 + w * t13)));
            p3 = t2 + w * (t5 + w * (t8 + w * (t11 + w * t14)));
            p = z * p1 - (tt - w * (p2 + y * p3));
            r += tf + p;
            break;
        default:
            p1 = y * (u0 + y * (u1 + y * (u2 + y * (u3 + y * (u4 + y * u5)))));
            p2 = 1.0 + y * (v1 + y * (v2 + y * (v3 + y * (v4 + y * v5))));
            r += -0.5 * y + p1 / p2;
            break;
        }
    } else if (ix < 0x40200000) {   // 2 <= x < 8
        i = (int)x;
        y = x - (double)i;
        p = y * (s0 + y * (s1 + y * (s2 + y * (s3 + y * (s4 + y * (s5 + y * s6))))));
        q = 1.0 + y * (r1 + y * (r2 + y * (r3 + y * (r4 + y * (r5 + y * r6)))));
        r = 0.5 * y + p / q;
        z = 1.0;   // lgamma(1+s) = log(s) + lgamma(s)
        switch (i) {
        case 7: z *= y + 6.0; /* fallthrough */
        case 6: z *= y + 5.0; /* fallthrough */
        case 5: z *= y + 4.0; /* fallthrough */
        case 4: z *= y + 3.0; /* fallthrough */
        case 3: z *= y + 2.0;
            r += log(z);
            break;
        }
    } else if (ix < 0x43900000) {   // 8 <= x < 2^58: Stirling
        t = log(x);
        z = 1.0 / x;
        y = z * z;
        w = w0 + z * (w1 + y * (w2 + y * (w3 + y * (w4 + y * (w5 + y * w6)))));
        r = (x - 0.5) * (t - 1.0) + w;
    } else {   // x >= 2^58
        r = x * (log(x) - 1.0);
    }
    if (sign) r = nadj - r;
    return r;
}

double lgamma(double x) { return lgamma_r(x, &signgam); }

// --- tgamma -----------------------------------------------------------

#define LANCZOS_N 12
static const double gmhalf = 5.524680040776729583740234375;   // g - 1/2
static const double Snum[LANCZOS_N + 1] = {
    23531376880.410759688572007674451636754734846804940,
    42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843,
    17921034426.037209699919755754458931112671403265390,
    6039542586.3520280050642916443072979210699388420708,
    1439720407.3117216736632230727949123939715485786772,
    248874557.86205415651146038641322942321632125127801,
    31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768,
    186056.26539522349504029498971604569928220784236328,
    8071.6720023658162106380029022722506138218516325024,
    210.82427775157934587250973392071336271166969580291,
    2.5066282746310002701649081771338373386264310793408,
};
// x(x+1)...(x+11), whose roots are the poles S's numerator cancels.
static const double Sden[LANCZOS_N + 1] = {
    0, 39916800, 120543840, 150917976, 105258076, 45995730, 13339535,
    2637558, 357423, 32670, 1925, 66, 1,
};
static const double fact[] = {
    1, 1, 2, 6, 24, 120, 720, 5040.0, 40320.0, 362880.0, 3628800.0, 39916800.0,
    479001600.0, 6227020800.0, 87178291200.0, 1307674368000.0, 20922789888000.0,
    355687428096000.0, 6402373705728000.0, 121645100408832000.0,
    2432902008176640000.0, 51090942171709440000.0, 1124000727777607680000.0,
};

// The Lanczos sum as a rational function, for x > 0. Above 8 it is
// evaluated in 1/x, so neither polynomial overflows.
static double lanczos_sum(double x) {
    double num = 0, den = 0;
    if (x < 8) {
        for (int i = LANCZOS_N; i >= 0; i--) { num = num * x + Snum[i]; den = den * x + Sden[i]; }
    } else {
        for (int i = 0; i <= LANCZOS_N; i++) { num = num / x + Snum[i]; den = den / x + Sden[i]; }
    }
    return num / den;
}

// sin(pi*x) for x > 0, as lgamma's but signed: the reflection needs it.
static double sinpi_signed(double x) {
    x = x * 0.5;
    x = 2 * (x - floor(x));
    int n = (int)(4 * x);
    n = (n + 1) / 2;
    x -= n * 0.5;
    x *= pi;
    switch (n) {
    default:
    case 0: return __m_sin(x, 0, 0);
    case 1: return __m_cos(x, 0);
    case 2: return __m_sin(-x, 0, 0);
    case 3: return -__m_cos(x, 0);
    }
}

double tgamma(double x) {
    uint64_t u = m_bits(x);
    uint32_t ix = (uint32_t)(u >> 32) & 0x7fffffff;
    int sign = (int)(u >> 63);
    if (ix >= 0x7ff00000) return x + INFINITY;   // NaN, +inf, and -inf -> NaN
    if (ix < (0x3ff - 54) << 20) return 1 / x;   // |x| < 2^-54: 1/x; +-0 is a pole
    if (x == floor(x)) {   // integers: poles below zero, exact factorials above
        if (sign) return 0 / 0.0;
        if (x <= sizeof fact / sizeof *fact) return fact[(int)x - 1];
    }
    if (ix >= 0x40670000) {   // |x| >= 184
        if (sign) {
            FORCE_EVAL((float)(0x1p-126 / x));
            if (floor(x) * 0.5 == floor(x * 0.5)) return 0;
            return -0.0;
        }
        return x * 0x1p1023;   // overflow
    }
    double absx = sign ? -x : x;
    // y = |x| + g - 1/2, with its rounding error kept in dy.
    double y = absx + gmhalf, dy;
    if (absx > gmhalf) { dy = y - absx; dy -= gmhalf; }
    else               { dy = y - gmhalf; dy -= absx; }
    double z = absx - 0.5;
    double r = lanczos_sum(absx) * exp(-y);
    if (x < 0) {   // reflection: gamma(x) = -pi / (x sin(pi x) gamma(-x))
        r = -pi / (sinpi_signed(absx) * absx * r);
        dy = -dy;
        z = -z;
    }
    r += dy * (gmhalf + 0.5) * r / y;
    z = pow(y, 0.5 * z);   // y^(z/2) twice: y^z alone overflows first
    return r * z * z;
}
