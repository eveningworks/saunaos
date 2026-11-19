#ifndef TTF_INTERNAL_H
#define TTF_INTERNAL_H

// What ttf.c, ttf_hint.c and ttf_cff.c share and nobody else calls.
#include "ttf.h"

#ifdef __cplusplus
extern "C" {
#endif

// The file's bytes, read only through these: each answers 0 for a read
// that would leave the buffer (ttf.c's top comment says why there is no
// unchecked path). Written as `len - off` so an offset near 4 GiB taken
// from a hostile table cannot wrap the check.
static inline uint32_t rd_u8(const struct ttf_font *f, uint32_t off) {
    if (off >= f->len) return 0;
    return f->data[off];
}

static inline uint32_t rd_u16(const struct ttf_font *f, uint32_t off) {
    if (off >= f->len || f->len - off < 2) return 0;
    return ((uint32_t)f->data[off] << 8) | f->data[off + 1];
}

static inline int32_t rd_i16(const struct ttf_font *f, uint32_t off) {
    return (int32_t)(int16_t)(uint16_t)rd_u16(f, off);
}

static inline uint32_t rd_u32(const struct ttf_font *f, uint32_t off) {
    if (off >= f->len || f->len - off < 4) return 0;
    return ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16)
         | ((uint32_t)f->data[off + 2] << 8) | (uint32_t)f->data[off + 3];
}

static inline int in_range(const struct ttf_font *f, uint32_t off, uint32_t bytes) {
    return off <= f->len && bytes <= f->len - off;
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) \
                       | ((uint32_t)(c) << 8) | (uint32_t)(d))

// CFF OUTLINES (ttf_cff.c). Open: finds the INDEXes in f->cff and fills
// f->cff_*; 0 for a CFF this cannot read. Decode: glyph `gid`'s contours
// into `sc` as ttf.c's glyf decoder does -- scaled by `scale`, offset to
// (ox, oy), y flipped -- the curves as TTF_PT_CUBIC pairs. Box: its
// extent in font units (y up) from the outline's points, for the blue
// zones; 0 for an empty or malformed glyph.
int ttf_cff_open(struct ttf_font *f);
int ttf_cff_decode(const struct ttf_font *f, int gid, fx_t scale, fx_t ox, fx_t oy,
                   struct ttf_scratch *sc);
int ttf_cff_ybox(const struct ttf_font *f, int gid, int *ymin, int *ymax);

// Fits a decoded outline (sc->px/py, pixel space, y down, the baseline on
// row `baseline`) to the grid, vertically only. ttf_render_glyph() calls it
// between decoding and flattening; ttf.h's ttf_set_hinting() says what it does.
void ttf_hint_outline(const struct ttf_font *f, fx_t scale, fx_t baseline,
                      struct ttf_scratch *sc);

#ifdef __cplusplus
}
#endif

#endif
