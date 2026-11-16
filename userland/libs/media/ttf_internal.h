#ifndef TTF_INTERNAL_H
#define TTF_INTERNAL_H

// What ttf.c and ttf_hint.c share and nobody else calls.
#include "ttf.h"

// Fits a decoded outline (sc->px/py, pixel space, y down, the baseline on
// row `baseline`) to the grid, vertically only. ttf_render_glyph() calls it
// between decoding and flattening; ttf.h's ttf_set_hinting() says what it does.
void ttf_hint_outline(const struct ttf_font *f, fx_t scale, fx_t baseline,
                      struct ttf_scratch *sc);

#endif
