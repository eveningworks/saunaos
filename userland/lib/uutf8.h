#ifndef ULIB_UUTF8_H
#define ULIB_UUTF8_H

// UTF-8 stepping with the TRANSITION RULE: a byte that does not start a
// valid sequence is ONE character whose value is the byte -- its Latin-1
// letter -- so decoding never fails and text written before UTF-8 (every
// Latin-1 name and file on an old disk) reads as itself rather than as
// U+FFFD. docs/utf8-design.md, "Decided here", 2.
//
// Header-only and allocation-free, so a host harness and every ring-3
// caller share one decoder. lib/uunicode.h's uunicode_utf8_decode() is
// the strict one (U+FFFD on a bad byte) for code that wants to know.
//
// Valid means what RFC 3629 says: no overlong forms, no surrogates,
// nothing past U+10FFFF, every continuation byte present.
#include <stdint.h>

// The character at `s`, and its length in bytes in `*len` (1..4).
// `avail` bounds the read (bytes left in the buffer); a negative value
// means the string is NUL-terminated, which is safe because a NUL is
// never a continuation byte. An empty input (avail 0, or a NUL when the
// string is NUL-terminated) is U+0000 with length 0; in a COUNTED buffer
// a NUL is an ordinary character of length 1.
static inline uint32_t uutf8_decode(const char *s, int avail, int *len) {
    const unsigned char *p = (const unsigned char *)s;
    if (avail == 0 || (avail < 0 && !p[0])) { if (len) *len = 0; return 0; }
    unsigned c = p[0];
    int n;
    uint32_t cp, min;
    if (c < 0x80)              { if (len) *len = 1; return c; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
    else                       { if (len) *len = 1; return c; }   // C0/C1/F5+: a stray byte
    if (avail > 0 && avail < n) { if (len) *len = 1; return c; }
    for (int k = 1; k < n; k++) {
        unsigned b = p[k];
        if ((b & 0xC0) != 0x80) { if (len) *len = 1; return c; }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        if (len) *len = 1;
        return c;
    }
    if (len) *len = n;
    return cp;
}

// `cp` as UTF-8 into `out` (no NUL); the byte count, 0 for a value that
// is not a scalar value (a surrogate, past U+10FFFF).
static inline int uutf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Where the character ENDING at byte `i` begins -- the inverse step of
// uutf8_decode(), agreeing with it on every input: a run of continuation
// bytes that is not the tail of one valid sequence steps back ONE byte,
// because forward decoding read each of them as a character of its own.
static inline int uutf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    const unsigned char *p = (const unsigned char *)s;
    int j = i - 1;
    while (j > 0 && i - j < 4 && (p[j] & 0xC0) == 0x80) j--;
    if (j < i - 1) {
        int n;
        uutf8_decode(s + j, i - j, &n);
        if (n == i - j) return j;
    }
    return i - 1;
}

// Characters in the first `n` bytes (`n < 0`: up to the NUL).
static inline int uutf8_count(const char *s, int n) {
    int k = 0;
    for (int i = 0; n < 0 ? s[i] != 0 : i < n; k++) {
        int len;
        uutf8_decode(s + i, n < 0 ? -1 : n - i, &len);
        if (len <= 0) break;
        i += len;
    }
    return k;
}

// The longest prefix of `s` that is at most `cap` bytes and ends on a
// character boundary -- where to cut a string so no sequence is split.
static inline int uutf8_cut(const char *s, int len, int cap) {
    if (len <= cap) return len;
    int i = 0;
    while (i < cap) {
        int n;
        uutf8_decode(s + i, len - i, &n);
        if (n <= 0 || i + n > cap) break;
        i += n;
    }
    return i;
}

#endif
