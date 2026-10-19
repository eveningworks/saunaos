#ifndef ULIB_WINFNT_H
#define ULIB_WINFNT_H

#include <stddef.h>
#include <stdint.h>

// Windows' bitmap fonts: a .FON -- an NE module whose RT_FONT resources
// are FNT 2.0 or 3.0 raster fonts, one face at one size each -- or a bare
// .FNT. What Windows 3.x drew its System, Fixedsys and MS Sans Serif text
// in; FreeType's `winfnt` driver reads the same format.
//
// PARSES UNTRUSTED INPUT: every read is bounds-checked against the bytes
// given, and it allocates nothing -- a font points into the caller's
// buffer, which must outlive it. A vector font (MODERN.FON's strokes) is
// counted but not drawn: `vector` says so.

struct winfnt {
    const uint8_t *fnt;        // its FNT bytes, inside the caller's buffer
    uint32_t len;
    int version;               // 0x200 or 0x300
    int vector;                // strokes, not bitmaps: winfnt_glyph() refuses it
    int points;
    int ascent;                // pixels above the baseline, within pix_height
    int internal_leading, external_leading;
    int italic, underline, strikeout;
    int weight;                // FW_*: 400 normal, 700 bold
    int charset;               // 0 ANSI, 255 OEM, 2 symbol
    int pitch_family;          // dfPitchAndFamily: bit 0 set = variable pitch
    int pix_width;             // a fixed font's cell width, 0 for a proportional one
    int pix_height;            // every glyph's rows
    int avg_width, max_width;
    int first, last, default_char;
    char face[32];
    uint32_t table;            // its character table
    int entry;                 // a table entry's bytes: 4 (2.0) or 6 (3.0)
};

// The fonts in a .FON, or 1 for a bare .FNT; -1 when it is neither.
int winfnt_count(const uint8_t *data, size_t n);

// Font `i` of the file. 0, or -1 when it is malformed.
int winfnt_get(const uint8_t *data, size_t n, int i, struct winfnt *out);

// Character `ch`'s advance in pixels: its own, else the default
// character's; -1 when the font has neither.
int winfnt_width(const struct winfnt *f, unsigned ch);

// Character `ch` as coverage, 255 inked and 0 not, `pix_height` rows of
// `stride` bytes at `out`; cut to `max_w` columns. Its advance, or -1
// (a vector font, or the glyph's bitmap lies outside the file).
int winfnt_glyph(const struct winfnt *f, unsigned ch, uint8_t *out, int stride, int max_w);

#endif
