#ifndef ULIB_UWCWIDTH_H
#define ULIB_UWCWIDTH_H

// How many CELLS a character takes in a fixed grid: 2 for East Asian
// Wide and Fullwidth (CJK, Hangul, fullwidth forms), 1 for everything
// else -- wcwidth()'s rule, from a table tools/gen/gen_wcwidth.py generates
// out of Unicode's EastAsianWidth property (docs/utf8-design.md,
// "Decided here", 6). utext's columns use it; the terminal's will.
//
// A GRAPHEME CLUSTER takes the cells uwcwidth_cluster() says: a letter's
// combining marks add none (they sit on it), and an emoji SEQUENCE is two
// (UTS #51) -- what the terminal's grid and the line editor's columns both
// count, so the caret the shell positions lands where the terminal drew.
#include <stdint.h>
#include "base/uwcwidth_table.h"
#include "base/uemoji.h"

static inline int uwcwidth(uint32_t cp) {
    if (cp < 0x1100) return 1;   // nothing below Hangul Jamo is wide
    int lo = 0, hi = (int)(sizeof UWCWIDTH_WIDE / sizeof UWCWIDTH_WIDE[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cp < UWCWIDTH_WIDE[m][0]) hi = m - 1;
        else if (cp > UWCWIDTH_WIDE[m][1]) lo = m + 1;
        else return 2;
    }
    return 1;
}

// The cells of one grapheme cluster, `cps[0..n)` (api/kgrapheme.h): TWO for
// an emoji -- an emoji-presentation first character, or one a joiner, a
// keycap, a skin tone, a flag's pair or a tag sequence makes one, or U+FE0F
// after a character past ASCII -- else its first character's width.
static inline int uwcwidth_cluster(const uint32_t *cps, int n) {
    if (n <= 0) return 0;
    if (n == 1) return uwcwidth(cps[0]);
    if (uemoji_presentation(cps[0])) return 2;
    if (cps[0] >= 0x1F1E6 && cps[0] <= 0x1F1FF) return 2;   // an indicator pair
    for (int i = 1; i < n; i++) {
        uint32_t c = cps[i];
        if (c == 0x200D || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) ||
            (c == UEMOJI_VS_EMOJI && cps[0] > 0x7F))
            return 2;
    }
    return uwcwidth(cps[0]);
}

#endif
