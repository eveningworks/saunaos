// iconv() -- see <iconv.h> for the charsets and the contract.
//
// EVERY CONVERSION GOES THROUGH A CODE POINT: decode one character from
// the source, encode it into the target. N decoders and N encoders
// rather than N*N tables, which is musl's shape and the reason adding a
// charset is two functions.
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum cs { CS_UTF8, CS_ASCII, CS_LATIN1, CS_CP1252,
          CS_UTF16, CS_UTF16LE, CS_UTF16BE, CS_UTF32, CS_UTF32LE, CS_UTF32BE };

enum { ON_BAD_FAIL, ON_BAD_TRANSLIT, ON_BAD_IGNORE };

struct __iconv {
    enum cs from, to;
    int on_bad;
    int in_be;         // UTF-16/32 input: the order its mark said (BE if none)
    int in_started;    // ...and whether the mark has been looked for
    int out_started;   // UTF-16/32 output: whether the mark has been written
};

static const struct { const char *name; enum cs cs; } NAMES[] = {
    { "UTF8", CS_UTF8 },
    { "ASCII", CS_ASCII }, { "USASCII", CS_ASCII }, { "ANSIX3.41968", CS_ASCII },
    { "ISO88591", CS_LATIN1 }, { "ISO8859-1", CS_LATIN1 }, { "LATIN1", CS_LATIN1 },
    { "L1", CS_LATIN1 }, { "CP819", CS_LATIN1 },
    { "WINDOWS1252", CS_CP1252 }, { "CP1252", CS_CP1252 },
    { "UTF16", CS_UTF16 }, { "UTF16LE", CS_UTF16LE }, { "UTF16BE", CS_UTF16BE },
    { "UTF32", CS_UTF32 }, { "UTF32LE", CS_UTF32LE }, { "UTF32BE", CS_UTF32BE },
    { "UCS4", CS_UTF32BE }, { "UCS4LE", CS_UTF32LE }, { "UCS4BE", CS_UTF32BE },
    { "WCHART", CS_UTF32LE },     // wchar_t is 32-bit here, in host (LE) order
};

// The name, uppercased with '-', '_' and ' ' dropped, up to any "//".
// `suffix` gets what follows the first "//", or "".
static int lookup(const char *name, enum cs *out, const char **suffix) {
    char norm[32];
    size_t n = 0;
    const char *p = name;
    for (; *p && !(p[0] == '/' && p[1] == '/'); p++) {
        char c = *p;
        if (c == '-' || c == '_' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (n + 1 >= sizeof norm) return 0;
        norm[n++] = c;
    }
    norm[n] = '\0';
    *suffix = (p[0] == '/' && p[1] == '/') ? p + 2 : "";
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        // ISO88591 vs ISO8859-1: the table is written without dashes too,
        // so compare against the same normalisation.
        char t[32];
        size_t m = 0;
        for (const char *q = NAMES[i].name; *q; q++)
            if (*q != '-') t[m++] = *q;
        t[m] = '\0';
        if (!strcmp(norm, t)) { *out = NAMES[i].cs; return 1; }
    }
    return 0;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    enum cs to, from;
    const char *tsuf, *fsuf;
    if (!tocode || !fromcode || !lookup(tocode, &to, &tsuf) || !lookup(fromcode, &from, &fsuf)) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    int on_bad = ON_BAD_FAIL;
    if (!strcmp(tsuf, "TRANSLIT")) on_bad = ON_BAD_TRANSLIT;
    else if (!strcmp(tsuf, "IGNORE")) on_bad = ON_BAD_IGNORE;
    else if (*tsuf) { errno = EINVAL; return (iconv_t)-1; }
    iconv_t cd = malloc(sizeof *cd);
    if (!cd) { errno = ENOMEM; return (iconv_t)-1; }
    memset(cd, 0, sizeof *cd);
    cd->from = from;
    cd->to = to;
    cd->on_bad = on_bad;
    cd->in_be = 1;
    return cd;
}

int iconv_close(iconv_t cd) {
    if (!cd || cd == (iconv_t)-1) { errno = EINVAL; return -1; }
    free(cd);
    return 0;
}

// Windows-1252's 0x80-0x9F. Zero marks the five bytes it leaves
// undefined, which decode as EILSEQ.
static const uint16_t CP1252_HI[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

#define DEC_BAD   (-1)   // not a character
#define DEC_SHORT (-2)   // the input ends inside one
#define NO_CHAR   0xFFFFFFFFu   // *cp for bytes that were a mark, not a character

// One character from `s` (`n` bytes) into *cp; the bytes it used, or
// DEC_BAD / DEC_SHORT.
static int decode(iconv_t cd, const uint8_t *s, size_t n, uint32_t *cp) {
    enum cs cs = cd->from;
    if (cs == CS_UTF16 || cs == CS_UTF32) {
        int unit = cs == CS_UTF16 ? 2 : 4;
        if (!cd->in_started) {
            if (n < (size_t)unit) return DEC_SHORT;
            cd->in_started = 1;
            if (unit == 2 && s[0] == 0xFF && s[1] == 0xFE) { cd->in_be = 0; *cp = NO_CHAR; return 2; }
            if (unit == 2 && s[0] == 0xFE && s[1] == 0xFF) { cd->in_be = 1; *cp = NO_CHAR; return 2; }
            if (unit == 4 && s[0] == 0xFF && s[1] == 0xFE && !s[2] && !s[3]) { cd->in_be = 0; *cp = NO_CHAR; return 4; }
            if (unit == 4 && !s[0] && !s[1] && s[2] == 0xFE && s[3] == 0xFF) { cd->in_be = 1; *cp = NO_CHAR; return 4; }
        }
        cs = unit == 2 ? (cd->in_be ? CS_UTF16BE : CS_UTF16LE)
                       : (cd->in_be ? CS_UTF32BE : CS_UTF32LE);
    }
    switch (cs) {
    case CS_ASCII:
        if (s[0] > 0x7F) return DEC_BAD;
        *cp = s[0];
        return 1;
    case CS_LATIN1:
        *cp = s[0];
        return 1;
    case CS_CP1252:
        if (s[0] >= 0x80 && s[0] <= 0x9F) {
            if (!CP1252_HI[s[0] - 0x80]) return DEC_BAD;
            *cp = CP1252_HI[s[0] - 0x80];
        } else {
            *cp = s[0];
        }
        return 1;
    case CS_UTF8: {
        uint8_t b = s[0];
        int len;
        uint32_t c, min;
        if (b < 0x80) { *cp = b; return 1; }
        if (b >= 0xC2 && b <= 0xDF) { len = 2; c = b & 0x1F; min = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { len = 3; c = b & 0x0F; min = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { len = 4; c = b & 0x07; min = 0x10000; }
        else return DEC_BAD;
        for (int i = 1; i < len; i++) {
            if ((size_t)i >= n) return DEC_SHORT;
            if ((s[i] & 0xC0) != 0x80) return DEC_BAD;
            c = c << 6 | (s[i] & 0x3F);
        }
        // Overlong, a surrogate, or past Unicode: not a character.
        if (c < min || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return DEC_BAD;
        *cp = c;
        return len;
    }
    case CS_UTF16LE:
    case CS_UTF16BE: {
        int be = cs == CS_UTF16BE;
        if (n < 2) return DEC_SHORT;
        uint32_t u = be ? (uint32_t)(s[0] << 8 | s[1]) : (uint32_t)(s[1] << 8 | s[0]);
        if (u >= 0xDC00 && u <= 0xDFFF) return DEC_BAD;     // a lone low surrogate
        if (u < 0xD800 || u > 0xDBFF) { *cp = u; return 2; }
        if (n < 4) return DEC_SHORT;
        uint32_t l = be ? (uint32_t)(s[2] << 8 | s[3]) : (uint32_t)(s[3] << 8 | s[2]);
        if (l < 0xDC00 || l > 0xDFFF) return DEC_BAD;
        *cp = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
        return 4;
    }
    case CS_UTF32LE:
    case CS_UTF32BE: {
        if (n < 4) return DEC_SHORT;
        uint32_t c = cs == CS_UTF32BE
            ? (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3]
            : (uint32_t)s[3] << 24 | (uint32_t)s[2] << 16 | (uint32_t)s[1] << 8 | s[0];
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return DEC_BAD;
        *cp = c;
        return 4;
    }
    default:
        return DEC_BAD;
    }
}

#define ENC_UNREP  (-1)   // the target cannot hold this character
#define ENC_NOROOM (-2)

static void put16(uint8_t *o, uint32_t u, int be) {
    o[be ? 0 : 1] = (uint8_t)(u >> 8);
    o[be ? 1 : 0] = (uint8_t)u;
}

static void put32(uint8_t *o, uint32_t c, int be) {
    for (int i = 0; i < 4; i++) o[be ? i : 3 - i] = (uint8_t)(c >> (24 - 8 * i));
}

// `c` into `o` (`n` bytes of room); the bytes written, or ENC_*.
static int encode(enum cs cs, uint32_t c, uint8_t *o, size_t n) {
    switch (cs) {
    case CS_ASCII:
    case CS_LATIN1:
    case CS_CP1252: {
        uint32_t limit = cs == CS_ASCII ? 0x7F : 0xFF;
        uint8_t b;
        if (cs == CS_CP1252 && c >= 0x80 && c <= 0x9F) return ENC_UNREP;  // not in it
        if (c <= limit) {
            b = (uint8_t)c;
        } else if (cs == CS_CP1252) {
            int i = 0;
            while (i < 32 && CP1252_HI[i] != c) i++;
            if (i == 32) return ENC_UNREP;
            b = (uint8_t)(0x80 + i);
        } else {
            return ENC_UNREP;
        }
        if (n < 1) return ENC_NOROOM;
        o[0] = b;
        return 1;
    }
    case CS_UTF8: {
        int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n < (size_t)len) return ENC_NOROOM;
        if (len == 1) { o[0] = (uint8_t)c; return 1; }
        static const uint8_t LEAD[5] = { 0, 0, 0xC0, 0xE0, 0xF0 };
        for (int i = len - 1; i > 0; i--) { o[i] = (uint8_t)(0x80 | (c & 0x3F)); c >>= 6; }
        o[0] = (uint8_t)(LEAD[len] | c);
        return len;
    }
    case CS_UTF16:
    case CS_UTF16LE:
    case CS_UTF16BE: {
        int be = cs != CS_UTF16LE;
        if (c < 0x10000) {
            if (n < 2) return ENC_NOROOM;
            put16(o, c, be);
            return 2;
        }
        if (n < 4) return ENC_NOROOM;
        c -= 0x10000;
        put16(o, 0xD800 + (c >> 10), be);
        put16(o + 2, 0xDC00 + (c & 0x3FF), be);
        return 4;
    }
    case CS_UTF32:
    case CS_UTF32LE:
    case CS_UTF32BE:
        if (n < 4) return ENC_NOROOM;
        put32(o, c, cs != CS_UTF32LE);
        return 4;
    }
    return ENC_UNREP;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
    if (!cd || cd == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }
    if (!inbuf || !*inbuf) {
        cd->in_started = 0;
        cd->in_be = 1;
        cd->out_started = 0;
        return 0;
    }
    const uint8_t *in = (const uint8_t *)*inbuf;
    size_t inleft = *inbytesleft;
    uint8_t *out = (uint8_t *)*outbuf;
    size_t outleft = *outbytesleft;
    size_t irreversible = 0;
    int err = 0;

    while (inleft) {
        if (!cd->out_started && (cd->to == CS_UTF16 || cd->to == CS_UTF32)) {
            int w = encode(cd->to, 0xFEFF, out, outleft);
            if (w < 0) { err = E2BIG; break; }
            out += w;
            outleft -= (size_t)w;
            cd->out_started = 1;
        }
        uint32_t cp = 0;
        int r = decode(cd, in, inleft, &cp);
        if (r == DEC_SHORT) { err = EINVAL; break; }
        if (r == DEC_BAD) { err = EILSEQ; break; }
        if (cp == NO_CHAR) {             // a byte-order mark: consumed, nothing to write
            in += r;
            inleft -= (size_t)r;
            continue;
        }
        int w = encode(cd->to, cp, out, outleft);
        if (w == ENC_UNREP) {
            if (cd->on_bad == ON_BAD_FAIL) { err = EILSEQ; break; }
            irreversible++;
            if (cd->on_bad == ON_BAD_TRANSLIT) {
                w = encode(cd->to, '?', out, outleft);
                if (w == ENC_NOROOM) { err = E2BIG; break; }
            } else {
                w = 0;
            }
        }
        if (w == ENC_NOROOM) { err = E2BIG; break; }
        in += r;
        inleft -= (size_t)r;
        out += w;
        outleft -= (size_t)w;
    }

    *inbuf = (char *)in;
    *inbytesleft = inleft;
    *outbuf = (char *)out;
    *outbytesleft = outleft;
    if (err) { errno = err; return (size_t)-1; }
    return irreversible;
}
