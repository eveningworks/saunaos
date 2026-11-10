// RING 3's half of kfmt's floating-point conversion: %f %e %g %a and
// their uppercase forms, for double and (`L`) the x87 long double. The
// kernel's half is kernel/lib/kfmt_nofloat.c and returns 0; api/kfmt.h
// says why the split exists (the kernel is built -mno-sse).
//
// **EVERY DIGIT IS EXACT, AND THE ROUNDING IS DECIDED ON THE EXACT
// BINARY VALUE.** A finite value is m * 2^e with m < 2^64, so its decimal
// expansion is finite and is produced digit by digit with no error: the
// integer part as a base-10^9 number (m shifted left by e), the fraction
// as a binary fixed-point numerator over 2^F, multiplied by 10^19 per
// chunk of digits. Rounding looks at the EXACT tail -- above, below or
// exactly one half -- and breaks a tie to even, which is glibc's answer
// in the default rounding mode: `%.0f` of 2.5 is "2", of 3.5 is "4", and
// `%.17g` round-trips through strtod. Nothing is allocated; the two
// arrays are sized from the exponent on the stack (a double needs at
// most ~150 bytes, a long double at its extremes ~2.2 KiB).
//
// **THE CONVERSION RUNS TWICE: once to PLAN, once to WRITE.** Rounding
// can carry through every digit (9.995 at %.2f is 10.00, one digit
// longer), so the field's length -- which the padding needs before the
// first byte -- is not known until the last digit has been seen, and the
// digits can be far too many to hold (%.5000Lf). The plan pass records
// what rounding does; the write pass regenerates the same digits and
// applies it. Changing what one pass reads without the other prints the
// wrong number of digits.
//
// NaN and the infinities are detected from the BITS, not by comparing
// the value against itself.
#include <kfmt.h>
#include <stdint.h>

typedef unsigned __int128 u128;

enum { K_ZERO, K_FIN, K_INF, K_NAN };

// A finite value is m * 2^e. `bexp` is the raw exponent field, which %a
// prints from.
struct num { int neg, kind, is_long; uint64_t m; int e; unsigned bexp; };

static void decode_d(double v, struct num *n) {
    uint64_t b;
    __builtin_memcpy(&b, &v, sizeof b);
    unsigned E = (unsigned)(b >> 52) & 0x7ff;
    uint64_t F = b & ((1ULL << 52) - 1);
    n->neg = (int)(b >> 63);
    n->is_long = 0;
    n->bexp = E;
    if (E == 0x7ff) { n->kind = F ? K_NAN : K_INF; return; }
    n->m = E ? F | (1ULL << 52) : F;
    n->e = (E ? (int)E : 1) - 1075;
    n->kind = n->m ? K_FIN : K_ZERO;
}

// The x87 format: a 64-bit significand with its integer bit EXPLICIT,
// then a 15-bit exponent and the sign.
static void decode_ld(long double v, struct num *n) {
    unsigned char by[10];
    __builtin_memcpy(by, &v, sizeof by);
    uint64_t m;
    __builtin_memcpy(&m, by, sizeof m);
    unsigned se = by[8] | (unsigned)by[9] << 8;
    unsigned E = se & 0x7fff;
    n->neg = (int)(se >> 15);
    n->is_long = 1;
    n->bexp = E;
    if (E == 0x7fff) { n->kind = (m << 1) ? K_NAN : K_INF; return; }
    n->m = m;
    n->e = (E ? (int)E : 1) - 16383 - 63;
    n->kind = m ? K_FIN : K_ZERO;
}

// --- the exact digit stream --------------------------------------------

#define BIL   1000000000u
#define TEN19 10000000000000000000ull

struct gen {
    uint32_t *lb; int nl;       // the integer part, base 10^9, least significant limb first
    uint64_t *fw; int nw, F;    // the fraction, fw / 2^F with fw < 2^F, low word first
    int il;                     // the next limb to read, counting down; -1 past the last
    unsigned char ch[20];       // the digits of the limb or fraction chunk being read
    int cl, cp;
    int vz;                     // zeros to give before the first real digit
    long nint;                  // decimal digits in the integer part, 0 when it is 0
};

static void gen_init(struct gen *g, const struct num *n, uint32_t *lb, uint64_t *fw) {
    uint64_t ip;
    g->lb = lb; g->fw = fw;
    g->nl = 0; g->nw = 0; g->F = 0; g->cl = g->cp = 0; g->vz = 0;
    int e = n->kind == K_FIN ? n->e : 0;
    uint64_t m = n->kind == K_FIN ? n->m : 0;
    if (e >= 0) {
        ip = m;
    } else {
        g->F = -e;
        ip = g->F < 64 ? m >> g->F : 0;
        fw[0] = g->F < 64 ? m & ((1ULL << g->F) - 1) : m;
        g->nw = fw[0] != 0;
    }
    while (ip) { lb[g->nl++] = (uint32_t)(ip % BIL); ip /= BIL; }
    // m * 2^e for e > 0, by doubling in base 10^9 -- 29 bits at a time,
    // the most that keeps a limb's shifted value and its carry in 64 bits.
    while (e > 0) {
        int sh = e < 29 ? e : 29;
        uint64_t carry = 0;
        for (int i = 0; i < g->nl; i++) {
            uint64_t t = ((uint64_t)lb[i] << sh) + carry;
            lb[i] = (uint32_t)(t % BIL);
            carry = t / BIL;
        }
        while (carry) { lb[g->nl++] = (uint32_t)(carry % BIL); carry /= BIL; }
        e -= sh;
    }
    g->il = g->nl - 1;
    g->nint = 0;
    if (g->nl) {
        g->nint = 9L * (g->nl - 1) + 1;
        for (uint32_t t = lb[g->nl - 1]; t >= 10; t /= 10) g->nint++;
    }
}

static void limb_chunk(struct gen *g) {
    uint32_t x = g->lb[g->il];
    int n = 9;
    if (g->il == g->nl - 1) { n = 1; for (uint32_t t = x; t >= 10; t /= 10) n++; }
    for (int i = n - 1; i >= 0; i--) { g->ch[i] = (unsigned char)(x % 10); x /= 10; }
    g->cl = n;
    g->cp = 0;
    g->il--;
}

// The next 19 fraction digits: multiply the numerator by 10^19 and take
// what crossed the binary point. Exact, because 10^19 < 2^64 and the
// product is carried in 128 bits.
static void frac_chunk(struct gen *g) {
    uint64_t carry = 0;
    for (int i = 0; i < g->nw; i++) {
        u128 p = (u128)g->fw[i] * TEN19 + carry;
        g->fw[i] = (uint64_t)p;
        carry = (uint64_t)(p >> 64);
    }
    if (carry) g->fw[g->nw++] = carry;
    int wi = g->F >> 6, sb = g->F & 63;
    uint64_t q = 0;
    if (wi < g->nw) {
        q = g->fw[wi] >> sb;
        if (sb && wi + 1 < g->nw) q |= g->fw[wi + 1] << (64 - sb);
        g->fw[wi] &= sb ? (1ULL << sb) - 1 : 0;
        if (g->nw > wi + 1) g->nw = wi + 1;
    }
    while (g->nw && !g->fw[g->nw - 1]) g->nw--;
    for (int i = 18; i >= 0; i--) { g->ch[i] = (unsigned char)(q % 10); q /= 10; }
    g->cl = 19;
    g->cp = 0;
}

static int gnext(struct gen *g) {
    if (g->vz) { g->vz--; return 0; }
    if (g->cp == g->cl) {
        if (g->il >= 0) limb_chunk(g);
        else if (!g->nw) return 0;          // the expansion has ended
        else frac_chunk(g);
    }
    return g->ch[g->cp++];
}

static int gdone(const struct gen *g) {
    return !g->vz && g->cp == g->cl && g->il < 0 && !g->nw;
}

static int gtail_zero(const struct gen *g) {
    for (int i = g->cp; i < g->cl; i++) if (g->ch[i]) return 0;
    for (int i = g->il; i >= 0; i--) if (g->lb[i]) return 0;
    return !g->nw;
}

// Positions the stream on the first SIGNIFICANT digit and returns its
// decimal place (the %e exponent); zero is one 0 at place 0.
static long seek_first(struct gen *g, const struct num *n) {
    if (n->kind == K_ZERO) { g->vz = 1; return 0; }
    if (g->nint) return g->nint - 1;
    long z = 0;
    while (!gnext(g)) z++;
    g->cp--;                                // un-read it: it came from the chunk
    return -z - 1;
}

// What rounding to `n` digits does: `up` adds one at the last digit,
// `ovf` means it carried through every digit (999 -> 1000), and
// `last_nz` is the last nonzero digit of the rounded result (-1: none).
struct plan { int up, ovf; long last_nz; };

static void plan_run(struct gen *g, long n, struct plan *p) {
    long last_n9 = -1, last_nz = -1, k;
    int lastd = 0;
    for (k = 0; k < n; k++) {
        if (gdone(g)) break;                // the rest are zeros
        int d = gnext(g);
        if (d != 9) last_n9 = k;
        if (d) last_nz = k;
        lastd = d;
    }
    int up = 0;
    if (k < n) {
        last_n9 = n - 1;
    } else {
        // The tail against one half of the last digit's unit: the next
        // digit decides unless it is a 5, and then whatever follows it.
        int d = gnext(g);
        int rest = d > 5 ? 1 : d < 5 ? -1 : !gtail_zero(g);
        up = rest > 0 || (rest == 0 && (lastd & 1));
    }
    p->up = up;
    p->ovf = up && last_n9 < 0;
    p->last_nz = !up ? last_nz : p->ovf ? 0 : last_n9;
}

// The write pass's view: the same digits with the plan applied.
struct rstream { struct gen *g; const struct plan *p; long k; };

static int rnext(struct rstream *r) {
    long k = r->k++;
    const struct plan *p = r->p;
    if (p->ovf) return k == 0;
    if (p->up && k > p->last_nz) return 0;  // carried away: the 9s became 0s
    int d = gnext(r->g);
    return p->up && k == p->last_nz ? d + 1 : d;
}

// --- the field -----------------------------------------------------------

struct out { k_fmt_putc put; void *ctx; };

static void emit(struct out *o, char c) { o->put(o->ctx, c); }
static void emit_n(struct out *o, char c, long n) { while (n-- > 0) emit(o, c); }

// [spaces][sign][prefix][zeros] before the body; the left-justified
// spaces after it come from field_close(). `body` is the length the body
// WILL have, known before it is written.
static long field_open(struct out *o, const struct k_fmt_float_spec *sp, char sign,
                       const char *prefix, long body, int zero_ok) {
    long len = body + (sign != 0);
    for (const char *q = prefix; *q; q++) len++;
    long pad = (long)sp->width > len ? (long)sp->width - len : 0;
    int zeros = sp->zero && !sp->left && zero_ok;
    if (!sp->left && !zeros) emit_n(o, ' ', pad);
    if (sign) emit(o, sign);
    for (const char *q = prefix; *q; q++) emit(o, *q);
    if (zeros) emit_n(o, '0', pad);
    return sp->left ? pad : 0;
}

static void put_exp(struct out *o, long x, int min_digits) {
    char b[24];
    int n = 0;
    unsigned long a = x < 0 ? (unsigned long)-x : (unsigned long)x;
    emit(o, x < 0 ? '-' : '+');
    do { b[n++] = (char)('0' + a % 10); a /= 10; } while (a);
    while (n < min_digits) b[n++] = '0';
    while (n) emit(o, b[--n]);
}

static int exp_len(long x, int min_digits) {
    unsigned long a = x < 0 ? (unsigned long)-x : (unsigned long)x;
    int n = 1;
    while (a >= 10) { a /= 10; n++; }
    return 1 + (n > min_digits ? n : min_digits);
}

// f style: `ints` digits from the stream before the point (0: a lone
// "0"), then `fc` after it, the first `lz` of them zeros not taken from
// the stream.
static long len_f(long ints, long fc, int alt) {
    return (ints ? ints : 1) + (fc > 0 || alt) + fc;
}

static void body_f(struct out *o, struct rstream *r, long ints, long lz, long fc, int alt) {
    if (!ints) emit(o, '0');
    for (long i = 0; i < ints; i++) emit(o, (char)('0' + rnext(r)));
    if (fc > 0 || alt) emit(o, '.');
    for (long i = 0; i < fc; i++) emit(o, i < lz ? '0' : (char)('0' + rnext(r)));
}

static long len_e(long fc, int alt, long x) {
    return 1 + (fc > 0 || alt) + fc + 1 + exp_len(x, 2);
}

static void body_e(struct out *o, struct rstream *r, long fc, int alt, long x, int upper) {
    emit(o, (char)('0' + rnext(r)));
    if (fc > 0 || alt) emit(o, '.');
    for (long i = 0; i < fc; i++) emit(o, (char)('0' + rnext(r)));
    emit(o, upper ? 'E' : 'e');
    put_exp(o, x, 2);
}

// %f %e %g.
static void conv_dec(struct out *o, const struct k_fmt_float_spec *sp, const struct num *n,
                     char sign, uint32_t *lb, uint64_t *fw) {
    char c = (char)(sp->conv | 0x20);
    int upper = sp->conv != c, alt = sp->alt;
    long P = sp->prec < 0 ? 6 : sp->prec;
    struct gen g;
    struct plan pl;
    struct rstream r = { &g, &pl, 0 };

    if (c == 'f') {
        gen_init(&g, n, lb, fw);
        g.vz = !g.nint;                     // "0." before a pure fraction
        long ints = g.nint ? g.nint : 1;
        plan_run(&g, ints + P, &pl);
        ints += pl.ovf;
        long pad = field_open(o, sp, sign, "", len_f(ints, P, alt), 1);
        gen_init(&g, n, lb, fw);
        g.vz = !g.nint;
        body_f(o, &r, ints, 0, P, alt);
        emit_n(o, ' ', pad);
        return;
    }

    gen_init(&g, n, lb, fw);
    long X = seek_first(&g, n);
    long nd = c == 'e' ? P + 1 : (P ? P : 1);
    plan_run(&g, nd, &pl);
    X += pl.ovf;
    gen_init(&g, n, lb, fw);
    seek_first(&g, n);

    if (c == 'e') {
        long pad = field_open(o, sp, sign, "", len_e(P, alt, X), 1);
        body_e(o, &r, P, alt, X, upper);
        emit_n(o, ' ', pad);
        return;
    }
    // %g: C's rule. The exponent X is the one %e WOULD print, after
    // rounding to `nd` significant digits; the fixed form is used when
    // -4 <= X < nd, and either form drops trailing zeros (and a bare
    // point) unless `#` asks for them.
    long keep = alt ? nd : pl.last_nz + 1 > 0 ? pl.last_nz + 1 : 1;
    if (X >= -4 && X < nd) {
        long ints = X >= 0 ? X + 1 : 0;
        long lz = X >= 0 ? 0 : -X - 1;
        long fc = X >= 0 ? (keep > X + 1 ? keep - X - 1 : 0) : lz + keep;
        long pad = field_open(o, sp, sign, "", len_f(ints, fc, alt), 1);
        body_f(o, &r, ints, lz, fc, alt);
        emit_n(o, ' ', pad);
    } else {
        long pad = field_open(o, sp, sign, "", len_e(keep - 1, alt, X), 1);
        body_e(o, &r, keep - 1, alt, X, upper);
        emit_n(o, ' ', pad);
    }
}

// %a: the significand in hex, exactly as glibc lays it out -- a double
// as 0x1.<13 digits> (0x0.<13> when subnormal, at p-1022), a long double
// as its explicit integer bit and the next three in the first digit
// (1.0L is 0x8p-3). A precision rounds to even on the dropped bits; with
// none the digits stop at the last nonzero one.
static void conv_hex(struct out *o, const struct k_fmt_float_spec *sp, const struct num *n, char sign) {
    int upper = sp->conv == 'A';
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned lead = 0;
    uint64_t frac = 0;
    int nf = 0;
    long x = 0;
    if (n->kind == K_FIN && !n->is_long) {
        lead = (unsigned)(n->m >> 52);
        frac = n->m & ((1ULL << 52) - 1);
        nf = 13;
        x = n->bexp ? (long)n->bexp - 1023 : -1022;
    } else if (n->kind == K_FIN) {
        lead = (unsigned)(n->m >> 60);
        frac = n->m & ((1ULL << 60) - 1);
        nf = 15;
        x = (long)(n->bexp ? n->bexp : 1) - 16383 - 3;
    }
    long P = sp->prec;
    if (P < 0) {
        P = nf;
        while (P > 0 && !((frac >> (4 * (nf - P))) & 15)) P--;
    } else if (P < nf) {
        int drop = 4 * (nf - (int)P);
        uint64_t rem = frac & ((1ULL << drop) - 1), half = 1ULL << (drop - 1);
        frac >>= drop;
        if (rem > half || (rem == half && ((P ? frac : lead) & 1))) {
            frac++;
            if (P == 0 || frac >> (4 * P)) { frac = 0; lead++; }
            if (lead == 16) { lead = 1; x += 4; }
        }
        frac <<= drop;
    }
    long body = 1 + (P > 0 || sp->alt) + P + 1 + exp_len(x, 1);
    long pad = field_open(o, sp, sign, upper ? "0X" : "0x", body, 1);
    emit(o, hex[lead]);
    if (P > 0 || sp->alt) emit(o, '.');
    for (long i = 0; i < P; i++)
        emit(o, i < nf ? hex[(frac >> (4 * (nf - 1 - i))) & 15] : '0');
    emit(o, upper ? 'P' : 'p');
    put_exp(o, x, 1);
    emit_n(o, ' ', pad);
}

int k_fmt_float(const struct k_fmt_float_spec *sp, va_list ap, k_fmt_putc put, void *ctx) {
    struct num n;
    if (sp->is_long) decode_ld(va_arg(ap, long double), &n);
    else             decode_d(va_arg(ap, double), &n);
    struct out o = { put, ctx };
    char sign = n.neg ? '-' : sp->plus ? '+' : sp->space ? ' ' : 0;
    int upper = sp->conv >= 'A' && sp->conv <= 'Z';

    if (n.kind == K_INF || n.kind == K_NAN) {
        // A NaN keeps its sign bit, as glibc prints it ("-nan"); '0' pads
        // with spaces here, since zeros would read as digits.
        const char *w = n.kind == K_NAN ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        long pad = field_open(&o, sp, sign, "", 3, 0);
        for (; *w; w++) emit(&o, *w);
        emit_n(&o, ' ', pad);
        return 1;
    }
    if ((sp->conv | 0x20) == 'a') { conv_hex(&o, sp, &n, sign); return 1; }

    // The two arrays, sized from the exponent: the integer part's limbs
    // when e >= 0 (a long double's 2^16384 is 4933 digits), the fraction's
    // words when e < 0 (its numerator runs to 2^16445).
    int e = n.kind == K_FIN ? n.e : 0;
    long nlimbs = e >= 0 ? (64L + e) * 30103 / 100000 / 9 + 3 : 3;
    long nwords = e < 0 ? -(long)e / 64 + 3 : 1;
    uint32_t lb[nlimbs];
    uint64_t fw[nwords];
    conv_dec(&o, sp, &n, sign, lb, fw);
    return 1;
}
