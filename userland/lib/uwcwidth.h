#ifndef ULIB_UWCWIDTH_H
#define ULIB_UWCWIDTH_H

// How many CELLS a character takes in a fixed grid: 2 for East Asian
// Wide and Fullwidth (CJK, Hangul, fullwidth forms), 1 for everything
// else -- wcwidth()'s rule, from a table tools/gen_wcwidth.py generates
// out of Unicode's EastAsianWidth property (docs/utf8-design.md,
// "Decided here", 6). utext's columns use it; the terminal's will.
//
// A combining mark is 1, not 0: SaunaOS composes nothing (the design's
// non-goals), so a mark is drawn as a character of its own and needs its
// own cell.
#include <stdint.h>
#include "lib/uwcwidth_table.h"

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

#endif
