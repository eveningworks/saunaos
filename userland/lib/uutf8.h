#ifndef ULIB_UUTF8_H
#define ULIB_UUTF8_H

// UTF-8 stepping with the TRANSITION RULE: a byte that does not start a
// valid sequence is ONE character whose value is the byte -- its Latin-1
// letter -- so decoding never fails and text written before UTF-8 (every
// Latin-1 name and file on an old disk) reads as itself rather than as
// U+FFFD. docs/utf8-design.md, "Decided here", 2.
//
// THE DECODER IS api/kutf8.h's, under ring-3 names: the kernel console,
// the line editor and the terminal key decoder are compiled into both
// rings and need the same one, so it lives where both can reach it.
// lib/uunicode.h's uunicode_utf8_decode() is the strict one (U+FFFD on a
// bad byte) for code that wants to know.
#include <stdint.h>
#include "kutf8.h"

static inline uint32_t uutf8_decode(const char *s, int avail, int *len) {
    return kutf8_decode(s, avail, len);
}
static inline int uutf8_encode(uint32_t cp, char out[4]) { return kutf8_encode(cp, out); }

// Where the character ENDING at byte `i` begins. Through uutf8_decode()
// rather than kutf8_prev(), so a host check that swaps the decoder for a
// byte-at-a-time one (ugfx_text_hostcheck.py's control) swaps this too.
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
