// Text MEASUREMENT: a string in, pixels or an index out.
//
// Split out of ugfx.c because these are the only text functions
// that touch no font state and no surface -- they stand entirely on
// ugfx_cp_advance(), ugfx_kern() and ugfx_font_codepage(). That is what
// makes them compilable on the host against a synthetic proportional
// face, which matters more here than for most code: this is the
// arithmetic that decides where a caret goes, and it was wrong in every
// widget for as long as the interface face was monospace and the error
// was invisible.
//
// THE INVARIANT THEY ALL SHARE: kerning is counted BETWEEN adjacent
// characters, before the advance of the second. So a measurement of a
// SLICE must be taken on that slice -- an offset into the whole string
// includes a kern pair the drawing of the slice never applies, which is
// a caret drifting by a pixel or two per scroll step.
//
// AND EVERY INDEX IS A BYTE OFFSET ON A CHARACTER BOUNDARY: the strings
// are UTF-8 (docs/utf8-design.md), decoded here one character at a time,
// and a caller slices with what these return rather than with `i + 1`.
#include "ui/ugfx.h"
#include "lib/uutf8.h"

int ugfx_text_decode(const char *str, int i, int end, uint32_t *cp) {
    *cp = 0;
    if (!str || i < 0 || (end >= 0 && i >= end) || !str[i]) return i < 0 ? 0 : i;
    if (ugfx_font_codepage()) {          // one byte, one character
        *cp = (unsigned char)str[i];
        return i + 1;
    }
    int n;
    *cp = uutf8_decode(str + i, end < 0 ? -1 : end - i, &n);
    return i + (n > 0 ? n : 1);
}

int ugfx_text_width_n(const char *str, int n) {
    if (!str) return 0;
    int w = 0, prev = 0;
    uint32_t cp;
    for (int i = 0, nx; (nx = ugfx_text_decode(str, i, n, &cp)) > i; i = nx) {
        w += ugfx_kern(prev, (int)cp) + ugfx_cp_advance(cp);
        prev = (int)cp;
    }
    return w;
}

int ugfx_text_width(const char *str) { return ugfx_text_width_n(str, -1); }

int ugfx_text_index_at_x(const char *str, int x) {
    if (!str || x <= 0) return 0;
    int w = 0, prev = 0, i = 0;
    uint32_t cp;
    for (int nx; (nx = ugfx_text_decode(str, i, -1, &cp)) > i; i = nx) {
        int adv = ugfx_kern(prev, (int)cp) + ugfx_cp_advance(cp);
        // Past a glyph's midpoint belongs to the boundary AFTER it, so
        // clicking the right half of a character puts the caret behind
        // it. Rounding by half the CELL instead lands a click on the
        // wrong character for every glyph narrower than the widest one.
        if (x < w + adv / 2) return i;
        w += adv;
        prev = (int)cp;
    }
    return i;
}

int ugfx_draw_string_elided(struct ugfx_surface *s, int x, int y, int max_w,
                            const char *str, uint32_t color, uint32_t bg) {
    if (!str) return 0;
    if (ugfx_text_width(str) <= max_w) {
        ugfx_draw_string_clipped(s, x, y, max_w, str, color, bg);
        return 0;
    }
    // ROOM FOR THE MARK FIRST, then as much text as is left. Measured
    // rather than counted: on a proportional face the two dots are not
    // two character widths.
    int mark = ugfx_text_width("..");
    int cut_w = max_w - mark;
    if (cut_w < 0) cut_w = 0;
    int n = ugfx_text_fit_chars(str, cut_w);
    char buf[256];
    if (n > (int)sizeof buf - 1) n = uutf8_cut(str, n, (int)sizeof buf - 1);
    for (int i = 0; i < n; i++) buf[i] = str[i];
    buf[n] = '\0';
    int drawn = ugfx_text_width(buf);
    ugfx_draw_string_clipped(s, x, y, cut_w, buf, color, bg);
    ugfx_draw_string_clipped(s, x + drawn, y, mark, "..", color, bg);
    return 1;
}

int ugfx_text_elide(char *dst, int cap, const char *src, int max_w) {
    if (!dst || cap <= 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    int len = 0;
    while (src[len]) len++;
    int n;
    if (len < cap && ugfx_text_width(src) <= max_w) {
        n = len;
    } else {
        // The mark's room first, as ugfx_draw_string_elided() takes it.
        if (cap < 3) return 1;   // no room for the mark: nothing, never a cut without it
        int cut_w = max_w - ugfx_text_width("..");
        n = ugfx_text_fit_chars(src, cut_w < 0 ? 0 : cut_w);
        if (n < 1) n = ugfx_text_next(src, 0);   // one letter and the mark beats the mark alone
        if (n > len) n = len;
        if (n > cap - 3) n = uutf8_cut(src, n, cap - 3);
    }
    for (int i = 0; i < n; i++) dst[i] = src[i];
    if (n == len) { dst[n] = '\0'; return 0; }
    dst[n] = '.'; dst[n + 1] = '.'; dst[n + 2] = '\0';
    return 1;
}

int ugfx_text_fit_chars(const char *str, int max_w) {
    if (!str || ugfx_char_w() <= 0) return 0;
    int i = 0, used = 0, prev = 0;
    uint32_t cp;
    for (int nx; (nx = ugfx_text_decode(str, i, -1, &cp)) > i; i = nx) {
        int adv = ugfx_kern(prev, (int)cp) + ugfx_cp_advance(cp);
        if (used + adv > max_w) break;
        used += adv;
        prev = (int)cp;
    }
    return i;
}

int ugfx_text_next(const char *str, int i) {
    if (!str || i < 0) return 0;
    uint32_t cp;
    return ugfx_text_decode(str, i, -1, &cp);
}

int ugfx_text_prev(const char *str, int i) {
    if (!str || i <= 0) return 0;
    if (ugfx_font_codepage()) return i - 1;
    return uutf8_prev(str, i);
}
