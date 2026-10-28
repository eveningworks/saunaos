#ifndef KUTF8_H
#define KUTF8_H

// UTF-8 for BOTH RINGS: decode and encode with the TRANSITION RULE, and
// the little Unicode knowledge a line editor and a keyboard layout need
// (case and "is this a letter"). Header-only and allocation-free, so the
// kernel console, klineedit.c and termkey.c (each compiled into the
// kernel AND libuapp.a) and every ring-3 caller share one decoder.
// userland/lib/uutf8.h is this under its ring-3 names.
//
// THE TRANSITION RULE: a byte that does not start a valid sequence is
// ONE character whose value is the byte -- its Latin-1 letter -- so
// decoding never fails and text written before UTF-8 (every Latin-1
// name and file on an old disk) reads as itself rather than as U+FFFD.
// docs/utf8-design.md, "Decided here", 2.
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
static inline uint32_t kutf8_decode(const char *s, int avail, int *len) {
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

// How many bytes the sequence a lead byte starts SHOULD have, 1 for an
// ASCII byte and for anything that cannot lead one (a stray byte is a
// character of its own). For a decoder fed one byte at a time.
static inline int kutf8_seq_len(unsigned char c) {
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 1;
}

// `cp` as UTF-8 into `out` (no NUL); the byte count, 0 for a value that
// is not a scalar value (a surrogate, past U+10FFFF).
static inline int kutf8_encode(uint32_t cp, char out[4]) {
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
// kutf8_decode(), agreeing with it on every input: a run of continuation
// bytes that is not the tail of one valid sequence steps back ONE byte,
// because forward decoding read each of them as a character of its own.
static inline int kutf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    const unsigned char *p = (const unsigned char *)s;
    int j = i - 1;
    while (j > 0 && i - j < 4 && (p[j] & 0xC0) == 0x80) j--;
    if (j < i - 1) {
        int n;
        kutf8_decode(s + j, i - j, &n);
        if (n == i - j) return j;
    }
    return i - 1;
}

// Where the character STARTING at byte `i` of an `n`-byte buffer ends.
static inline int kutf8_next(const char *s, int n, int i) {
    if (i >= n) return n;
    int len;
    kutf8_decode(s + i, n - i, &len);
    return i + (len > 0 ? len : 1);
}

// Characters in the first `n` bytes of `s` -- a cell count wherever
// every character is one cell (the kernel console).
static inline int kutf8_count(const char *s, int n) {
    int k = 0;
    for (int i = 0; i < n; k++) i = kutf8_next(s, n, i);
    return k;
}

// A UTF-8 decoder FED ONE BYTE AT A TIME, for a stream that arrives in
// pieces -- a terminal's reads split a sequence wherever they like, and
// a sequence split across two writes must still be ONE character. Zero
// it before use. Each byte completes 0, 1 or 2 characters (written to
// `out`, the count returned): 2 when a byte breaks a sequence off -- the
// lead is its own Latin-1 letter (the transition rule), then the byte
// itself. The middle of a malformed sequence is dropped.
struct kutf8_stream {
    char b[4];
    int  n, want;
};

static inline int kutf8_stream_feed(struct kutf8_stream *st, unsigned char c, uint32_t out[2]) {
    int k = 0;
    if (st->n) {
        if ((c & 0xC0) == 0x80) {
            st->b[st->n++] = (char)c;
            if (st->n < st->want) return 0;
            int len;
            uint32_t cp = kutf8_decode(st->b, st->n, &len);
            out[0] = len == st->n ? cp : (uint32_t)(unsigned char)st->b[0];
            st->n = 0;
            return 1;
        }
        out[k++] = (unsigned char)st->b[0];   // broken off: the lead, as itself
        st->n = 0;
    }
    int want = kutf8_seq_len(c);
    if (want > 1) {
        st->b[0] = (char)c;
        st->n = 1;
        st->want = want;
        return k;
    }
    out[k++] = c;
    return k;
}

// --- what a line editor and a layout need to know about a character -----
//
// NOT Unicode's full tables, and said so: case pairs for ASCII, Latin-1,
// Latin Extended-A and the Latin Extended-B letters a European layout
// types (Romanian's comma-below), Greek and Cyrillic -- every letter a
// /usr/share/kbs layout produces. A character outside them has no case
// here and comes back unchanged, which is the safe answer.

static inline uint32_t kutf8_toupper(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 0x20 : c;
    if (c < 0x100) {
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
        if (c == 0xFF) return 0x178;
        if (c == 0xB5) return 0x39C;
        return c;
    }
    if (c >= 0x100 && c <= 0x137) return (c & 1) && c != 0x131 ? c - 1 : (c == 0x131 ? 'I' : c);
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c : c - 1;
    if (c >= 0x14A && c <= 0x177) return (c & 1) ? c - 1 : c;
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c : c - 1;
    if (c >= 0x200 && c <= 0x21F) return (c & 1) ? c - 1 : c;
    if (c >= 0x222 && c <= 0x233) return (c & 1) ? c - 1 : c;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
    if (c == 0x3C2) return 0x3A3;                       // final sigma
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 0x25;
    if (c == 0x3CC) return 0x38C;
    if (c >= 0x3CD && c <= 0x3CE) return c - 0x3F;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) ||
        (c >= 0x4D0 && c <= 0x4FF))
        return (c & 1) ? c - 1 : c;
    return c;
}

static inline uint32_t kutf8_tolower(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 0x20 : c;
    if (c < 0x100) return (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 0x20 : c;
    if (c == 0x130) return 'i';
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x137) return (c & 1) || c == 0x130 ? c : c + 1;
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return (c & 1) ? c : c + 1;
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
    if (c >= 0x200 && c <= 0x21F) return (c & 1) ? c : c + 1;
    if (c >= 0x222 && c <= 0x233) return (c & 1) ? c : c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 0x25;
    if (c == 0x38C) return 0x3CC;
    if (c >= 0x38E && c <= 0x38F) return c + 0x3F;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) ||
        (c >= 0x4D0 && c <= 0x4FF))
        return (c & 1) ? c : c + 1;
    return c;
}

// A letter or digit, for WORD motion: ASCII alphanumerics, the Latin-1
// letters, and past Latin-1 everything outside the blocks that hold
// punctuation, symbols and the Private Use Area (where api/keyboard.h's
// special keys live) -- so `päivä`, `ąż` and `слово` are one word each,
// and a dash, an arrow or a box-drawing line still separates.
static inline int kutf8_isalnum(uint32_t c) {
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (c < 0x100)
        return (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xB5 || c == 0xBA;
    if (c >= 0x2000 && c <= 0x2BFF) return 0;   // punctuation, symbols, arrows, boxes
    if (c >= 0x2E00 && c <= 0x2E7F) return 0;   // supplemental punctuation
    if (c >= 0x3000 && c <= 0x303F) return 0;   // CJK punctuation
    if (c >= 0xE000 && c <= 0xF8FF) return 0;   // Private Use: KEY_* specials
    if (c >= 0xFE30 && c <= 0xFE4F) return 0;
    if (c >= 0xFF00 && c <= 0xFF0F) return 0;
    if (c >= 0xFFF0) return c > 0xFFFF;         // specials; the planes above are letters
    return c != 0x37E && c != 0x387;            // Greek's question mark and ano teleia
}

#endif
