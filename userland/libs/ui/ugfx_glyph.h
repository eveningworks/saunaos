#ifndef UUI_UGFX_GLYPH_H
#define UUI_UGFX_GLYPH_H

// ugfx's glyph cache for characters past the session atlas (U+0100 and
// up): rasterized from the font's own .ttf, then the fallback list's faces
// (base/ufontfallback.h), then an outlined box, per process, bounded in bytes. INTERNAL to ugfx --
// a client draws through ugfx_draw_cp()/ugfx_draw_string() and never
// sees a cached glyph.
#include <stdint.h>
#include "ui/ugfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// The glyph for `cp` in font `f`: a coverage cell `*w` wide and `*h`
// (== f->char_h) tall with the baseline on the atlas's row, and the pen
// advance. `emoji` asks for emoji presentation (a variation selector's
// tag, else base/uemoji.h's default): the emoji face is tried first. `*color` 1 means a
// COLOUR glyph: `*cov` is then premultiplied R,G,B,A, four bytes a pixel,
// drawn as it is rather than in the text's colour. 1 on success, 0 only
// when memory ran out. The pointer is valid until the next call -- an
// eviction may free it.
int ugfx_glyph_get(const struct ugfx_font *f, uint32_t cp, int emoji,
                   const unsigned char **cov, int *w, int *h, int *adv, int *color);

// The ONE glyph an emoji face draws cluster `token` as (its `ccmp`
// ligature), or 0 when none does -- then the caller draws the parts. Both
// answers are cached.
int ugfx_glyph_cluster(const struct ugfx_font *f, uint32_t token,
                       const unsigned char **cov, int *w, int *h, int *adv, int *color);

// Combining mark `cp` with no precomposed form, as a cell `over` pixels wide
// (the letter it sits on) with its ink centred -- HarfBuzz's fallback
// placement. Advance 0.
int ugfx_glyph_mark(const struct ugfx_font *f, uint32_t cp, int over,
                    const unsigned char **cov, int *w, int *h, int *adv, int *color);

// Kerning in pixels between characters `l` and `r` of `f`'s own face, by
// glyph id from its GPOS or kern table (ttf_pair_kern_units()); 0 when either
// is not in the face.
int ugfx_glyph_kern(const struct ugfx_font *f, uint32_t l, uint32_t r);

#ifdef __cplusplus
}
#endif

#endif
