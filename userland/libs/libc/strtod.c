// strtod/strtof/strtold/atof -- text to floating point, CORRECTLY
// ROUNDED: the result is the representable value nearest the decimal the
// text denotes, a tie going to the even one, so strtod(printf("%.17g"))
// is the double printed and strtof is never a double rounded twice.
//
// **A FIRST GUESS, THEN EXACT COMPARISON.** The guess comes from the
// first 19 digits scaled in double arithmetic and is a few units in the
// last place out at worst. Then the text is compared EXACTLY -- big
// integers, no floating point -- against the midpoint between the guess
// and its neighbour, and the guess steps until the text lies between
// its two midpoints. The midpoint is the whole question: everything else
// about rounding to nearest follows from which side of it the text is.
//
// **ONLY THE FIRST `keep` SIGNIFICANT DIGITS ARE HELD, AND THAT IS
// EXACT, not an approximation.** A midpoint between two doubles has at
// most 768 significant digits ((2m+1) * 5^1075 at the bottom of the
// range), so once 800 are held the rest can only say "a little more
// than this" -- `sticky` -- which decides a comparison only when the held
// digits equal a midpoint exactly. A longer input changes nothing else.
//
// ERANGE follows glibc: on overflow, and on an inexact result that is
// TINY -- below the smallest normal after rounding to the format's
// precision with an unbounded exponent (x86's tininess rule) -- which
// includes a nonzero input that rounds to zero.
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <string.h>

typedef unsigned __int128 u128;

// One IEEE binary format, by its shape. `dmin`/`dmax` bound the decimal
// magnitude: text below 10^dmin rounds to zero whatever its digits, text
// at or above 10^dmax overflows -- so nothing past them reaches the
// big-integer code, which is what bounds its size. `keep` is how many
// significant digits are held (more than any midpoint has), `bigw` the
// words a big integer needs for them: the larger of `keep` digits and
// (2m+1) * 5^(keep - dmin).
//
// x87's 80-bit format is handled as an IEEE one with mbits 63 -- the
// explicit integer bit is put back when the result is packed -- and
// needs 11,600 digits (a midpoint has up to ~11,520) and 620 words:
// musl's floatscan holds the same for the same reason.
struct fmt { int mbits, ebits, kmin, dmin, dmax, keep, bigw; };
static const struct fmt F64 = { 52, 11, -1074, -324, 309, 800, 48 };
static const struct fmt F32 = { 23, 8, -149, -46, 39, 800, 48 };
static const struct fmt F80 = { 63, 15, -16445, -4951, 4933, 11600, 620 };

#define SMALL_BIGW 48   // F64's and F32's, held on the stack

// `cap` words at `w`, the caller's; the sizes above are what keep every
// product inside them.
struct big { int n, cap; uint64_t *w; };

static void big_muladd(struct big *b, uint64_t m, uint64_t a) {
    uint64_t carry = a;
    for (int i = 0; i < b->n; i++) {
        u128 p = (u128)b->w[i] * m + carry;
        b->w[i] = (uint64_t)p;
        carry = (uint64_t)(p >> 64);
    }
    if (carry && b->n < b->cap) b->w[b->n++] = carry;
}

static void big_mulpow5(struct big *b, int e) {
    while (e >= 27) { big_muladd(b, 7450580596923828125ULL, 0); e -= 27; }   // 5^27
    uint64_t p = 1;
    while (e-- > 0) p *= 5;
    if (p != 1) big_muladd(b, p, 0);
}

// r = p * m, for an m of up to 128 bits.
static void big_mul128(struct big *r, const struct big *p, u128 m) {
    uint64_t lo = (uint64_t)m, hi = (uint64_t)(m >> 64);
    int n = p->n;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        u128 t = (u128)p->w[i] * lo + carry;
        r->w[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    r->w[n] = carry;
    r->n = n + 1;
    if (hi) {
        r->w[n + 1] = 0;
        r->n = n + 2;
        carry = 0;
        for (int i = 0; i < n; i++) {
            u128 t = (u128)p->w[i] * hi + r->w[i + 1] + carry;
            r->w[i + 1] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        r->w[n + 1] += carry;
    }
    while (r->n && !r->w[r->n - 1]) r->n--;
}

static int big_bits(const struct big *b) {
    return b->n ? 64 * b->n - __builtin_clzll(b->w[b->n - 1]) : 0;
}

// Word `i` of b << s, without building it.
static uint64_t big_word(const struct big *b, int s, int i) {
    int j = i - (s >> 6), bs = s & 63;
    uint64_t hi = j >= 0 && j < b->n ? b->w[j] : 0;
    if (!bs) return hi;
    uint64_t lo = j >= 1 && j - 1 < b->n ? b->w[j - 1] : 0;
    return hi << bs | lo >> (64 - bs);
}

// The sign of (a << sa) - (b << sb).
static int big_cmp(const struct big *a, int sa, const struct big *b, int sb) {
    int la = big_bits(a), lb = big_bits(b);
    if (!la || !lb) return (la != 0) - (lb != 0);
    la += sa;
    lb += sb;
    if (la != lb) return la > lb ? 1 : -1;
    for (int i = (la - 1) >> 6; i >= 0; i--) {
        uint64_t x = big_word(a, sa, i), y = big_word(b, sb, i);
        if (x != y) return x > y ? 1 : -1;
    }
    return 0;
}

// The text's value, exactly: L * 2^q, divided by P = 5^-q when q < 0,
// plus a little more when `sticky`. R is dec_cmp()'s scratch.
struct dec { struct big L, P, R; int q, sticky; };

// The sign of value - M * 2^k.
static int dec_cmp(struct dec *d, u128 M, int k) {
    struct big *R = &d->R;
    if (d->q < 0) {
        big_mul128(R, &d->P, M);
    } else {
        R->w[0] = (uint64_t)M;
        R->w[1] = (uint64_t)(M >> 64);
        R->n = R->w[1] ? 2 : R->w[0] ? 1 : 0;
    }
    int mn = d->q < k ? d->q : k;
    int c = big_cmp(&d->L, d->q - mn, R, k - mn);
    return c ? c : d->sticky;
}

// A non-negative pattern as m * 2^k.
static void unpack(const struct fmt *F, u128 u, u128 *m, int *k) {
    u128 E = u >> F->mbits, f = u & (((u128)1 << F->mbits) - 1);
    *m = E ? f | (u128)1 << F->mbits : f;
    *k = F->kmin + (E ? (int)E - 1 : 0);
}

// The bit patterns of the non-negative values of one format are in the
// same order as the values, so "the next double up" is u + 1.
static u128 round_dec(const struct fmt *F, struct dec *d, u128 u, int *erange) {
    u128 inf = (((u128)1 << F->ebits) - 1) << F->mbits, m;
    int k;
    if (u > inf - 1) u = inf - 1;
    for (;;) {                          // up while the text is past u's upper midpoint
        if (u == inf) break;
        unpack(F, u, &m, &k);
        int c = dec_cmp(d, 2 * m + 1, k - 1);
        if (c > 0 || (c == 0 && (u & 1))) u++;
        else break;
    }
    while (u > 0) {                     // down while it is short of the lower one
        unpack(F, u - 1, &m, &k);
        int c = dec_cmp(d, 2 * m + 1, k - 1);
        if (c < 0 || (c == 0 && !((u - 1) & 1))) u--;
        else break;
    }
    u128 min_normal = (u128)1 << F->mbits;
    if (u == inf) {
        *erange = 1;
    } else if (u < min_normal) {
        unpack(F, u, &m, &k);
        if (!u || dec_cmp(d, m, k)) *erange = 1;
    } else if (u == min_normal) {
        // Rounded UP to the smallest normal: tiny all the same if, with
        // an unbounded exponent, it would have stayed below it.
        if (dec_cmp(d, ((u128)1 << (F->mbits + 2)) - 1, F->kmin - 2) < 0) *erange = 1;
    }
    return u;
}

static int clz128(u128 x) {
    uint64_t hi = (uint64_t)(x >> 64);
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)x);
}

// mant * 2^bexp (plus a little when `sticky`), rounded with its lowest
// kept bit at 2^k.
static u128 round_at(u128 mant, int sticky, long bexp, long k, int *inexact) {
    long s = k - bexp;
    if (s <= 0) { *inexact = sticky; return mant << -s; }
    u128 m = s >= 128 ? 0 : mant >> s;
    u128 rem = s >= 128 ? mant : mant & (((u128)1 << s) - 1);
    int up;
    if (s > 128) up = 0;                                // below a half that mant cannot reach
    else {
        u128 half = (u128)1 << (s - 1);
        up = rem > half || (rem == half && (sticky || (m & 1)));
    }
    *inexact = rem || sticky;
    return m + (u128)up;
}

// A hex float is binary already: round its bits directly.
static u128 round_bin(const struct fmt *F, u128 mant, int sticky, long bexp, int *erange) {
    long t = 127 - clz128(mant) + bexp;                 // the top bit's exponent
    long k = t - F->mbits, ku = k;
    if (k < F->kmin) k = F->kmin;
    int inexact, ux;
    u128 m = round_at(mant, sticky, bexp, k, &inexact);
    u128 mu = round_at(mant, sticky, bexp, ku, &ux);
    int tiny = ku < F->kmin && !(ku == F->kmin - 1 && mu == (u128)1 << (F->mbits + 1));
    if (m == (u128)1 << (F->mbits + 1)) { m >>= 1; k++; }
    u128 inf = (((u128)1 << F->ebits) - 1) << F->mbits;
    if (inexact && tiny) *erange = 1;
    if (m < (u128)1 << F->mbits) return m;              // subnormal (or zero)
    long E = k - F->kmin + 1;
    if (E >= (1L << F->ebits) - 1) { *erange = 1; return inf; }
    return (u128)E << F->mbits | (m & (((u128)1 << F->mbits) - 1));
}

// 1e0..1e22 -- every power of ten a double represents EXACTLY.
static const double POW10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// The first guess: v * 10^e in steps of 1e22, so no intermediate
// overflows before the result does. A few units in the last place out,
// which round_dec() walks off.
static double guess(double v, long e) {
    while (e > 22)  { v *= 1e22; e -= 22; }
    while (e < -22) { v /= 1e22; e += 22; }
    return e >= 0 ? v * POW10[e] : v / POW10[-e];
}

// 10^n by squaring, n < 8192: some tens of roundings, not thousands.
static long double pow10l_rough(long n) {
    long double p = 1, b = 10;
    for (; n; n >>= 1) {
        if (n & 1) p *= b;
        if (n > 1) b *= b;
    }
    return p;
}

// F80's guess, in two halves so neither power overflows on its own.
static long double guessl(long double v, long e) {
    long n = e < 0 ? -e : e;
    long double a = pow10l_rough(n / 2), b = pow10l_rough(n - n / 2);
    return e >= 0 ? v * a * b : v / a / b;
}

// A long double as F80's pattern, and back. Bytes 0-7 are the mantissa
// with its integer bit, 8-9 the sign and exponent.
static u128 ld_bits(long double x) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, &x, 8);
    memcpy(&se, (const char *)&x + 8, 2);
    return (u128)(se & 0x7FFF) << 63 | (mant & 0x7FFFFFFFFFFFFFFFULL);
}

static long double ld_from(u128 u) {
    uint64_t E = (uint64_t)(u >> 63) & 0x7FFF, f = (uint64_t)u & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t mant = E ? f | 1ULL << 63 : f;
    uint16_t se = (uint16_t)(E | ((u >> 78) & 1 ? 0x8000 : 0));
    long double x = 0;
    memcpy(&x, &mant, 8);
    memcpy((char *)&x + 8, &se, 2);
    return x;
}

static int ci_prefix(const char *s, const char *word) {
    for (; *word; s++, word++)
        if (tolower((unsigned char)*s) != *word) return 0;
    return 1;
}

static int hexval(int c) {
    return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
}

// The whole parse, for any format; returns the result's bit pattern.
// `store` is 3 * F->bigw words, the big integers' room.
static u128 parse(const struct fmt *F, const char *nptr, char **endptr, uint64_t *store) {
    const char *s = nptr;
    u128 sign_bit = (u128)1 << (F->mbits + F->ebits);
    u128 inf = (((u128)1 << F->ebits) - 1) << F->mbits;
    int erange = 0;
    u128 bits;

    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

    // **"infinity" AND "nan(...)" ARE THE LONGER SPELLINGS C SPECIFIES**,
    // and C says take the LONGEST match: stopping at "inf" leaves "inity"
    // for the caller to trip over.
    if (ci_prefix(s, "inf")) {
        s += ci_prefix(s, "infinity") ? 8 : 3;
        bits = inf;
        goto done;
    }
    if (ci_prefix(s, "nan")) {
        s += 3;
        bits = inf | (u128)1 << (F->mbits - 1);         // the quiet NaN
        // The n-char-sequence is consumed, and -- glibc's reading -- if it
        // is a whole number it becomes the payload.
        if (*s == '(') {
            const char *q = s + 1;
            while (*q == '_' || isalnum((unsigned char)*q)) q++;
            if (*q == ')') {
                char *pe;
                unsigned long long pl = strtoull(s + 1, &pe, 0);
                if (pe == q) bits |= (u128)pl & (((u128)1 << (F->mbits - 1)) - 1);
                s = q + 1;
            }
        }
        goto done;
    }

    // **HEXADECIMAL FLOATS, C99's 0x1.8p3 FORM** -- the decimal-free way
    // to write a double, which test vectors and serialisers use exactly
    // because it round-trips. 60 bits are held and the rest only marks
    // the value "a little more"; the binary exponent is exact.
    // 124 bits are held, enough to round F80's 64.
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        (isxdigit((unsigned char)s[2]) ||
         (s[2] == '.' && isxdigit((unsigned char)s[3])))) {
        s += 2;
        u128 mant = 0;
        int sticky = 0;
        long bexp = 0;
        for (; isxdigit((unsigned char)*s); s++) {
            int d = hexval((unsigned char)*s);
            if (mant >> 120) { sticky |= d != 0; bexp += 4; }
            else mant = mant * 16 + (u128)d;
        }
        if (*s == '.') {
            s++;
            for (; isxdigit((unsigned char)*s); s++) {
                int d = hexval((unsigned char)*s);
                if (mant >> 120) sticky |= d != 0;
                else { mant = mant * 16 + (u128)d; bexp -= 4; }
            }
        }
        if (*s == 'p' || *s == 'P') {
            const char *ps = s + 1;
            int pneg = 0;
            if (*ps == '+' || *ps == '-') { pneg = (*ps == '-'); ps++; }
            if (isdigit((unsigned char)*ps)) {          // "0x1p" -- the p is not ours
                long pe = 0;
                for (; isdigit((unsigned char)*ps); ps++)
                    if (pe < 1000000) pe = pe * 10 + (*ps - '0');
                bexp += pneg ? -pe : pe;
                s = ps;
            }
        }
        bits = mant ? round_bin(F, mant, sticky, bexp, &erange) : 0;
        goto done;
    }

    // **THE MANTISSA IS AN INTEGER AND THE DIGIT COUNT IS AN EXPONENT**:
    // the value is D * 10^q. A leading zero is not a significant digit
    // (on the fraction side it still moves the point).
    struct dec d;
    d.L = (struct big){ 0, F->bigw, store };
    d.P = (struct big){ 0, F->bigw, store + F->bigw };
    d.R = (struct big){ 0, F->bigw, store + 2 * F->bigw };
    d.q = 0;
    d.sticky = 0;
    uint64_t chunk = 0, hi = 0;
    int clen = 0, hin = 0, any = 0, nz = 0;
    long nsig = 0, q = 0;
    for (int frac = 0; frac < 2; frac++) {
        if (frac) {
            if (*s != '.') break;
            s++;
        }
        for (; isdigit((unsigned char)*s); s++) {
            int c = *s - '0';
            any = 1;
            if (!nz && !c) { if (frac) q--; continue; }
            nz = 1;
            if (nsig >= F->keep) {          // past what can matter: its place still counts
                d.sticky |= c != 0;
                if (!frac) q++;
                continue;
            }
            nsig++;
            if (frac) q--;
            chunk = chunk * 10 + (uint64_t)c;
            if (++clen == 19) { big_muladd(&d.L, 10000000000000000000ULL, chunk); chunk = 0; clen = 0; }
            if (hin < 19) { hi = hi * 10 + (uint64_t)c; hin++; }
        }
    }
    if (!any) { s = nptr; bits = 0; neg = 0; goto done; }
    if (clen) {
        uint64_t p = 1;
        for (int i = 0; i < clen; i++) p *= 10;
        big_muladd(&d.L, p, chunk);
    }

    // exp10 carries the point's position even when no 'e' follows. "1e"
    // with no exponent digits is a 1 and an 'e' that is not ours.
    if (*s == 'e' || *s == 'E') {
        const char *es = s + 1;
        int eneg = 0;
        if (*es == '+' || *es == '-') { eneg = (*es == '-'); es++; }
        if (isdigit((unsigned char)*es)) {
            long e = 0;
            for (; isdigit((unsigned char)*es); es++)
                if (e < 1000000) e = e * 10 + (*es - '0');
            q += eneg ? -e : e;
            s = es;
        }
    }

    if (!nsig) { bits = 0; goto done; }                 // zero, whatever the exponent
    long mag = nsig + q;                                // 10^(mag-1) <= value < 10^mag
    if (mag <= F->dmin) { bits = 0; erange = 1; goto done; }
    if (mag - 1 >= F->dmax) { bits = inf; erange = 1; goto done; }
    d.q = (int)q;
    if (d.q > 0) big_mulpow5(&d.L, d.q);
    if (d.q < 0) {                                      // 5^-q, once: every comparison needs it
        d.P.n = 1;
        d.P.w[0] = 1;
        big_mulpow5(&d.P, -d.q);
    }

    u128 u;
    double g = F == &F80 ? 0 : guess((double)hi, q + nsig - hin);
    if (F == &F80) {
        u = ld_bits(guessl((long double)hi, q + nsig - hin));
    } else if (F == &F32) {
        float f = (float)g;
        uint32_t b32;
        __builtin_memcpy(&b32, &f, sizeof b32);
        u = b32;
    } else {
        uint64_t b64;
        __builtin_memcpy(&b64, &g, sizeof b64);
        u = b64;
    }
    bits = round_dec(F, &d, u, &erange);

done:
    if (endptr) *endptr = (char *)s;
    if (erange) errno = ERANGE;
    return neg ? bits | sign_bit : bits;
}

double strtod(const char *nptr, char **endptr) {
    uint64_t store[3 * SMALL_BIGW];
    uint64_t b = (uint64_t)parse(&F64, nptr, endptr, store);
    double v;
    __builtin_memcpy(&v, &b, sizeof v);
    return v;
}

float strtof(const char *nptr, char **endptr) {
    uint64_t store[3 * SMALL_BIGW];
    uint32_t b = (uint32_t)parse(&F32, nptr, endptr, store);
    float v;
    __builtin_memcpy(&v, &b, sizeof v);
    return v;
}

double atof(const char *s) { return strtod(s, 0); }

// Correctly rounded to x87's 64 bits, as glibc's is. Its big integers
// are ~15 KiB, so they come from the heap: no memory is ENOMEM and no
// conversion, never a less exact answer.
long double strtold(const char *nptr, char **endptr) {
    uint64_t *store = malloc(3 * (size_t)F80.bigw * sizeof *store);
    if (!store) {
        if (endptr) *endptr = (char *)nptr;
        errno = ENOMEM;
        return 0;
    }
    u128 b = parse(&F80, nptr, endptr, store);
    free(store);
    return ld_from(b);
}
