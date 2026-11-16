#ifndef ULIB_UEMOJI_H
#define ULIB_UEMOJI_H

// Whether a character is drawn as an EMOJI or as text: its Emoji_Presentation
// property, from a table tools/gen/gen_emoji.py generates out of Unicode's
// emoji data, overridden by the variation selector that may follow it
// (U+FE0F asks for emoji, U+FE0E for text). The text path sends an emoji
// to the emoji face before the text faces (base/ufontfallback.h).
#include <stdint.h>
#include "base/uemoji_table.h"

#define UEMOJI_VS_TEXT  0xFE0Eu
#define UEMOJI_VS_EMOJI 0xFE0Fu

// 1 when `cp` defaults to emoji presentation.
static inline int uemoji_presentation(uint32_t cp) {
    if (cp < 0x231A) return 0;   // nothing below WATCH is
    int lo = 0, hi = (int)(sizeof UEMOJI_PRESENTATION / sizeof UEMOJI_PRESENTATION[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cp < UEMOJI_PRESENTATION[m][0]) hi = m - 1;
        else if (cp > UEMOJI_PRESENTATION[m][1]) lo = m + 1;
        else return 1;
    }
    return 0;
}

// A character the text path skips: the variation selectors, which only
// choose how the one before them looks.
static inline int uemoji_invisible(uint32_t cp) {
    return cp == UEMOJI_VS_TEXT || cp == UEMOJI_VS_EMOJI;
}

// THE ONE RULE, for every text path: a variation selector belongs to the
// character before it, WHATEVER that character is (a keycap is '1' then
// U+FE0F). This says whether the UTF-8 at `b` (up to `avail` bytes, < 0
// for a NUL-terminated string) starts with one: its code point, else 0.
// Reads no byte past `avail` or past a NUL.
static inline uint32_t uemoji_selector_utf8(const char *b, int avail) {
    const unsigned char *u = (const unsigned char *)b;
    if (avail >= 0 && avail < 3) return 0;
    if (u[0] != 0xEF || u[1] != 0xB8) return 0;
    if (u[2] == 0x8E) return UEMOJI_VS_TEXT;
    if (u[2] == 0x8F) return UEMOJI_VS_EMOJI;
    return 0;
}

#endif
