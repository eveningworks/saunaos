// Values at an offset, and the text forms around them. See base/ubyteval.h.
#include "base/ubyteval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; int width; } ROWS[UBV_ROWS] = {
    [UBV_BINARY] = { "Binary", 1 },   [UBV_INT8] = { "Int8", 1 },     [UBV_INT16] = { "Int16", 2 },
    [UBV_INT32] = { "Int32", 4 },     [UBV_INT64] = { "Int64", 8 },   [UBV_HEX64] = { "Hex 64", 8 },
    [UBV_FLOAT32] = { "Float32", 4 }, [UBV_FLOAT64] = { "Float64", 8 }, [UBV_UTF8] = { "UTF-8", 1 },
};

const char *ubyteval_name(int row) { return row >= 0 && row < UBV_ROWS ? ROWS[row].name : ""; }
int ubyteval_width(int row) { return row >= 0 && row < UBV_ROWS ? ROWS[row].width : 0; }

static uint64_t get(const uint8_t *b, int n, int be) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)b[be ? n - 1 - i : i] << (8 * i);
    return v;
}

static void integer(uint64_t u, int bits, char *out, size_t cap) {
    uint64_t sign = 1ull << (bits - 1);
    if (!(u & sign)) {
        snprintf(out, cap, "%llu", (unsigned long long)u);
        return;
    }
    // The two's-complement value, built so the most negative one does not overflow.
    long long s = bits == 64 ? (long long)u : (long long)u - (long long)(sign << 1);
    snprintf(out, cap, "%lld (unsigned %llu)", s, (unsigned long long)u);
}

// The SHORTEST text that reads back as the same value: "0.1", not
// "0.10000000000000001" -- what every inspector shows.
static void real(double v, int is32, char *out, size_t cap) {
    for (int p = 1; p <= 17; p++) {
        snprintf(out, cap, "%.*g", p, v);
        double back = strtod(out, NULL);
        if (is32 ? (float)back == (float)v : back == v) return;
    }
}

static void float_row(const uint8_t *b, int be, int is32, char *out, size_t cap) {
    uint64_t u = get(b, is32 ? 4 : 8, be);
    int ebits = is32 ? 8 : 11, mbits = is32 ? 23 : 52;
    uint64_t exp = (u >> mbits) & ((1ull << ebits) - 1), man = u & ((1ull << mbits) - 1);
    int neg = (int)(u >> (is32 ? 31 : 63)) & 1;
    if (exp == (1ull << ebits) - 1) {
        snprintf(out, cap, "%s", man ? "NaN" : neg ? "-infinity" : "+infinity");
        return;
    }
    double v;
    if (is32) {
        uint32_t w = (uint32_t)u;
        float f;
        memcpy(&f, &w, 4);
        v = f;
    } else {
        memcpy(&v, &u, 8);
    }
    real(v, is32, out, cap);
}

static void hexbytes(const uint8_t *b, int n, char *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < n && k + 3 < cap; i++)
        k += (size_t)snprintf(out + k, cap - k, "%s%02X", i ? " " : "", b[i]);
}

static void utf8_row(const uint8_t *b, int avail, char *out, size_t cap) {
    unsigned c0 = b[0];
    int need;
    uint32_t cp;
    unsigned lo = 0x80, hi = 0xBF;
    if (c0 < 0x80) { need = 1; cp = c0; }
    else if (c0 >= 0xC2 && c0 <= 0xDF) { need = 2; cp = c0 & 0x1F; }
    else if (c0 >= 0xE0 && c0 <= 0xEF) {
        need = 3; cp = c0 & 0x0F;
        if (c0 == 0xE0) lo = 0xA0;
        if (c0 == 0xED) hi = 0x9F;
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
        need = 4; cp = c0 & 0x07;
        if (c0 == 0xF0) lo = 0x90;
        if (c0 == 0xF4) hi = 0x8F;
    } else {
        snprintf(out, cap, "not a character (%02X)", c0);
        return;
    }
    char hex[16];
    for (int i = 1; i < need; i++) {
        if (i >= avail) {
            hexbytes(b, avail, hex, sizeof hex);
            snprintf(out, cap, "cut short (%s)", hex);
            return;
        }
        unsigned ci = b[i];
        if (ci < (i == 1 ? lo : 0x80) || ci > (i == 1 ? hi : 0xBF)) {
            hexbytes(b, i + 1, hex, sizeof hex);
            snprintf(out, cap, "not a character (%s)", hex);
            return;
        }
        cp = cp << 6 | (ci & 0x3F);
    }
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
        snprintf(out, cap, "U+%04X (control)", (unsigned)cp);
        return;
    }
    char ch[5] = { 0 };
    memcpy(ch, b, (size_t)need);
    snprintf(out, cap, "%s  U+%04X", ch, (unsigned)cp);
}

int ubyteval_row(const uint8_t *b, int avail, int be, int row, char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = '\0';
    if (row < 0 || row >= UBV_ROWS) return 0;
    int w = ROWS[row].width;
    if (avail < w) {
        if (avail <= 0) snprintf(out, cap, "past the end");
        else snprintf(out, cap, "needs %d bytes, %d left", w, avail);
        return 0;
    }
    if (avail > 8) avail = 8;
    uint64_t u = get(b, w, be);
    switch (row) {
    case UBV_BINARY: {   // "0101 1010", the nibbles apart
        char bits[10];
        for (int i = 0, k = 0; i < 8; i++) {
            if (i == 4) bits[k++] = ' ';
            bits[k++] = (b[0] >> (7 - i)) & 1 ? '1' : '0';
        }
        bits[9] = '\0';
        snprintf(out, cap, "%s", bits);
        break;
    }
    case UBV_INT8:  integer(u, 8, out, cap); break;
    case UBV_INT16: integer(u, 16, out, cap); break;
    case UBV_INT32: integer(u, 32, out, cap); break;
    case UBV_INT64: integer(u, 64, out, cap); break;
    case UBV_HEX64: snprintf(out, cap, "0x%016llX", (unsigned long long)u); break;
    case UBV_FLOAT32: float_row(b, be, 1, out, cap); break;
    case UBV_FLOAT64: float_row(b, be, 0, out, cap); break;
    case UBV_UTF8: utf8_row(b, avail, out, cap); break;
    }
    return 1;
}

int ubyteval_as_offset(const uint8_t *b, int avail, int be, uint64_t size, uint64_t *out) {
    static const int widths[] = { 8, 4, 2 };
    for (int i = 0; i < 3; i++) {
        if (avail < widths[i]) continue;
        uint64_t v = get(b, widths[i], be);
        if (v < size) { *out = v; return 1; }
    }
    return 0;
}

// --- what a person types ----------------------------------------------------

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int ubyteval_parse_offset(const char *s, uint64_t cur, uint64_t size, uint64_t *out, const char **why) {
    static const char *const USAGE = "Type an offset: 0x2BC0, 11200, +16 or -0x10.";
    while (*s == ' ' || *s == '\t') s++;
    int rel = 0;
    if (*s == '+' || *s == '-') rel = *s++ == '+' ? 1 : -1;
    while (*s == ' ') s++;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (base == 10 && end > s && (end[-1] == 'h' || end[-1] == 'H')) { base = 16; end--; }
    if (end == s) { *why = USAGE; return 0; }
    uint64_t v = 0;
    for (const char *p = s; p < end; p++) {
        int d = base == 16 ? hexval(*p) : (*p >= '0' && *p <= '9' ? *p - '0' : -1);
        if (d < 0) { *why = USAGE; return 0; }
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) { *why = "That number is too large."; return 0; }
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (rel > 0) {
        if (v > UINT64_MAX - cur) { *why = "That is past the end of the file."; return 0; }
        v = cur + v;
    } else if (rel < 0) {
        if (v > cur) { *why = "That is before the start of the file."; return 0; }
        v = cur - v;
    }
    if (!size) { *why = "The file is empty."; return 0; }
    if (v >= size) { *why = "That is past the end of the file."; return 0; }
    *out = v;
    return 1;
}

int ubyteval_parse_hex(const char *s, uint8_t *out, int cap) {
    int n = 0;
    while (*s) {
        if (*s == ' ' || *s == ',' || *s == '\t') { s++; continue; }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && hexval(s[2]) >= 0) s += 2;
        const char *t = s;
        while (hexval(*t) >= 0) t++;
        if (t == s || (t - s) % 2 || (*t && *t != ' ' && *t != ',' && *t != '\t')) return -1;
        for (; s < t; s += 2) {
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
        }
    }
    return n;
}

// --- copied text ---------------------------------------------------------------

size_t ubyteval_hex_size(size_t n) { return n ? n * 3 : 1; }

int ubyteval_hex_text(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (cap < ubyteval_hex_size(n)) return -1;
    static const char digits[] = "0123456789ABCDEF";
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) out[k++] = ' ';
        out[k++] = digits[b[i] >> 4];
        out[k++] = digits[b[i] & 15];
    }
    out[k] = '\0';
    return (int)k;
}

int ubyteval_ascii_text(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (cap < n + 1) return -1;
    for (size_t i = 0; i < n; i++) out[i] = b[i] >= 0x20 && b[i] < 0x7F ? (char)b[i] : '.';
    out[n] = '\0';
    return (int)n;
}

#define C_PER_LINE 12

size_t ubyteval_c_size(size_t n) {
    return 48 + n * 6 + (n / C_PER_LINE + 1) * 5 + 4;
}

int ubyteval_c_array(const uint8_t *b, size_t n, char *out, size_t cap) {
    if (cap < ubyteval_c_size(n)) return -1;
    size_t k = (size_t)snprintf(out, cap, "unsigned char data[%llu] = {\n", (unsigned long long)n);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        if (i % C_PER_LINE == 0) { memcpy(out + k, "    ", 4); k += 4; }
        out[k++] = '0';
        out[k++] = 'x';
        out[k++] = digits[b[i] >> 4];
        out[k++] = digits[b[i] & 15];
        out[k++] = ',';
        out[k++] = i % C_PER_LINE == C_PER_LINE - 1 || i == n - 1 ? '\n' : ' ';
    }
    memcpy(out + k, "};\n", 4);
    return (int)(k + 3);
}
