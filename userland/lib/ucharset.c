// lib/ucharset.h: Latin-1 and Windows-1252 <-> UTF-8.
#include "lib/ucharset.h"

// Windows-1252's 0x80-0x9F, where Latin-1 has C1 controls; 0 for the five
// bytes it leaves undefined.
static const unsigned short CP1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static unsigned from_byte(unsigned char c, int cp1252) {
    if (cp1252 && c >= 0x80 && c < 0xA0) return CP1252[c - 0x80];
    return c;
}

// A code point's byte, or -1 when the charset has none.
static int to_byte(unsigned cp, int cp1252) {
    if (cp1252 && cp >= 0x80 && cp < 0xA0) return -1;
    if (cp <= 0xFF) return (int)cp;
    for (int i = 0; cp1252 && i < 32; i++)
        if (CP1252[i] == cp) return 0x80 + i;
    return -1;
}

int ucharset_cp1252_byte(unsigned cp) { return to_byte(cp, 1); }

static long to_utf8(char *dst, size_t cap, const char *src, size_t n, int cp1252, char repl) {
    if (!cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = from_byte((unsigned char)src[i], cp1252);
        if (!src[i]) continue;
        if (!c) c = (unsigned char)repl;         // a byte the charset leaves undefined
        size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (o + need >= cap) { dst[0] = 0; return -1; }
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xC0 | c >> 6);
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | c >> 12);
            dst[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = 0;
    return (long)o;
}

static long from_utf8(char *dst, size_t cap, const char *src, size_t n, char repl, int cp1252) {
    if (!cap) return -1;
    const unsigned char *s = (const unsigned char *)src;
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned c = s[i], cp, more, min;
        if (c < 0x80)                { cp = c;        more = 0; min = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; more = 3; min = 0x10000; }
        else                         { cp = 0x110000; more = 0; min = 0; }   // stray byte
        size_t k = 1;
        for (; k <= more; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80) break;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (k <= more || cp < min) {   // truncated, or overlong: one byte replaced
            cp = 0x110000;
            k = 1;
        }
        i += k;
        if (!cp) continue;
        if (o + 1 >= cap) { dst[0] = 0; return -1; }
        int b = to_byte(cp, cp1252);
        dst[o++] = b >= 0 ? (char)b : repl;
    }
    dst[o] = 0;
    return (long)o;
}

long ucharset_latin1_to_utf8(char *dst, size_t cap, const char *src, size_t n) {
    return to_utf8(dst, cap, src, n, 0, '?');
}

long ucharset_utf8_to_latin1(char *dst, size_t cap, const char *src, size_t n, char repl) {
    return from_utf8(dst, cap, src, n, repl, 0);
}

long ucharset_cp1252_to_utf8(char *dst, size_t cap, const char *src, size_t n, char repl) {
    return to_utf8(dst, cap, src, n, 1, repl);
}

long ucharset_utf8_to_cp1252(char *dst, size_t cap, const char *src, size_t n, char repl) {
    return from_utf8(dst, cap, src, n, repl, 1);
}
