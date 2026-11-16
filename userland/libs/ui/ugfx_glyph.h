#ifndef UUI_UGFX_GLYPH_H
#define UUI_UGFX_GLYPH_H

// ugfx's glyph cache for characters past the session atlas (U+0100 and
// up): rasterized from the font's own .ttf, then the fallback list's faces
// (base/ufontfallback.h), then an outlined box, per process, bounded in bytes. INTERNAL to ugfx --
// a client draws through ugfx_draw_cp()/ugfx_draw_string() and never
// sees a cached glyph.
#include <stdint.h>
#include "ui/ugfx.h"

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

#endif
