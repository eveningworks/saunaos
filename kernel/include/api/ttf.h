#ifndef TTF_H
#define TTF_H

// A TrueType/OpenType parser and scanline rasterizer, in fixed point.
//
// This is the runtime half of what tools/gen/genttf.py does offline: given
// the bytes of a .ttf, .otf or .ttc file it decodes glyph outlines
// (TrueType's quadratic contours from `glyf`, or CFF's cubic ones, which
// media/ttf_cff.c runs from their Type 2 charstrings) and fills them into
// an 8-bit coverage map, which is the
// exact format font_ttf.h's baked tables already use -- so a glyph this
// produces is a drop-in for a glyph that was baked at build time (see
// kernel/drivers/font_face.c, which builds one atlas from either).
//
// THREE PROPERTIES ARE LOAD-BEARING, and each of them is why this file
// looks the way it does rather than like stb_truetype.
//
// **No floating point.** This kernel builds with -mno-sse and has no FP
// at all, so everything scaled is Q16.16 (fixed.h). Font units are
// 16-bit integers and unitsPerEm is typically 1000-2048, so a
// coordinate scaled to pixels has plenty of headroom in Q16.16 -- the
// one place that is not true is fx_mul of two large intermediates,
// which is why the edge walk keeps coordinates in pixel space rather
// than accumulating in font units.
//
// **No allocation.** Everything transient lives in a caller-supplied
// struct ttf_scratch (~70 KB, far too big for either ring's stack --
// the kernel budget is 16 KiB per stack and USERLAND_CFLAGS carries a
// -Wframe-larger-than too). That is what lets one implementation serve
// the kernel, a ring-3 program and a KTEST, the same rule geom.c and
// klineedit.c follow: this file may not reference anything kernel-only.
//
// **Every read is bounds-checked, because a font file is untrusted
// input.** Windows put exactly this code in ring 0 (win32k.sys/GDI) and
// spent a decade of CVEs on it before Win10 moved font parsing out to a
// sandboxed user-mode host; Linux never had it in the kernel at all,
// and under Wayland/X11 the client rasterizes. SaunaOS deliberately
// differs (the console needs glyphs before any process exists, and
// WIN_REQ_FONT shares one atlas so client text cannot drift from the
// desktop's) -- so the mitigation has to be in here instead: there is
// no raw pointer arithmetic into the file anywhere below, only rd_u8/
// rd_u16/rd_i16/rd_u32 with an explicit length check, and a malformed
// table makes a function return 0 rather than read past the buffer.
// docs/decisions/drivers.md has the full reasoning; since /bin/fontd
// (2026-09-09) it runs in ring 3 only, sandboxed, and the checks stay.
#include <stdint.h>
#include "fixed.h"

#ifdef __cplusplus
extern "C" {
#endif

// Caps on one glyph's complexity. A Latin glyph in DejaVu Sans Mono
// peaks around 120 points and 40 contours' worth of nothing; these are
// generous by an order of magnitude and are REFUSALS, not truncations
// -- a glyph past any of them renders as blank rather than as a
// partial outline, because half an outline fills wrongly (the scanline
// fill needs closed contours) and a wrong glyph is worse than a
// missing one.
#define TTF_MAX_POINTS   1024
#define TTF_MAX_CONTOURS 64
#define TTF_MAX_EDGES    2048
#define TTF_MAX_CELL_W   256  // widest cell the coverage accumulator holds

// Vertical supersampling: each pixel row is sampled at up to this many
// sub-scanlines and the results averaged, while horizontal coverage is
// computed exactly from the span ends. 16 gives a near-horizontal edge 17
// grey levels where 4 gave 5 -- visible at 9-14 px, where the autohinter
// leaves round tops and diagonals between grid lines. Above 32 px a glyph
// takes fewer (ttf_render_glyph()), since its rows cost per sub-scanline.
#define TTF_SUBSAMPLES   16

// The blue zones (struct ttf_font): BOTTOM zones hold letters' feet, TOP
// zones their tops.
enum { TTF_BLUE_BASE, TTF_BLUE_DESC, TTF_BLUE_X, TTF_BLUE_CAP, TTF_BLUE_ASC, TTF_BLUES };

// A CFF INDEX (an array of variable-length items): `count` items whose
// offsets, `osz` bytes each, start at `offs`; item offsets count from
// `data`. All zero for an absent one.
struct ttf_cff_index { uint32_t data, offs, count; uint8_t osz; };

struct ttf_font {
    const uint8_t *data;
    uint32_t len;

    // Table offsets into `data`, 0 when the table is absent.
    uint32_t glyf, loca, cmap, hmtx, head, hhea, maxp;
    uint32_t glyf_len, loca_len;

    // The format-0 horizontal `kern` subtable's PAIR ARRAY, not the
    // table's own offset -- ttf_open() walks past the headers once so
    // that a lookup is a binary search and nothing else. 0 (with
    // kern_pairs 0) when the face has no such table: a monospace face
    // has none by definition, and a modern proportional face keeps its
    // kerning in GPOS instead -- ttf_has_kerning() asks about both.
    uint32_t kern_pairs_off;
    int kern_pairs;

    int units_per_em;
    int loca_long;     // head.indexToLocFormat: 0 = u16 halves, 1 = u32
    int num_glyphs;
    int num_hmetrics;  // hhea.numberOfHMetrics
    int ascent;        // hhea, font units
    int descent;       // hhea, font units, POSITIVE (the file stores it negative)
    int line_gap;

    uint32_t name, name_len;   // `name` and `OS/2`, 0 when absent: ttf_family()
    uint32_t os2, os2_len;     // and ttf_win_metrics() read them
    uint32_t vdmx, vdmx_len;   // the device metrics Windows lays text out by,
    uint32_t hdmx, hdmx_len;   // 0 when absent: ttf_vdmx(), ttf_hdmx()
    uint32_t colr, colr_len;   // COLR v0 and CPAL, 0 when absent or another
    uint32_t cpal, cpal_len;   // version: ttf_render_color_glyph()
    uint32_t gpos, gpos_len;   // OpenType layout, 0 when absent:
    uint32_t gsub, gsub_len;   // ttf_pair_kern_units(), ttf_ligate()

    // CFF OUTLINES (an 'OTTO' font): the `CFF ` table, 0 for a TrueType
    // one -- whose outlines are glyf's. What ttf_cff.c reads each glyph
    // through, found once by ttf_open(). A CID-keyed font (CJK) keeps its
    // local subroutines per font DICT, picked by FDSelect, so `subrs` is
    // only the name-keyed font's.
    uint32_t cff, cff_len;
    struct ttf_cff_index cff_chars, cff_gsubrs, cff_subrs, cff_fdarray;
    uint32_t cff_fdselect;     // 0 = not CID-keyed

    // The autohinter's BLUE ZONES, measured by ttf_open() from the face's
    // own Latin letters' bounding boxes, font units, y up. `ref` is where
    // FLAT letters stop (the top of 'x', of 'H'), `shoot` where ROUND ones
    // overshoot it ('o', 'O'). A bit in blue_ok per zone found.
    int16_t blue_ref[TTF_BLUES], blue_shoot[TTF_BLUES];
    uint8_t blue_ok;
    uint8_t no_hint;           // ttf_set_hinting(f, 0): render the raw outline
};

struct ttf_edge { fx_t x0, y0, x1, y1; int dir; };

// What struct ttf_scratch's on[] says of a point. glyf's curves are
// quadratic (one OFF point between two ONs, consecutive OFFs implying an
// ON between them); CFF's are cubic, always two CUBIC points then an ON.
#define TTF_PT_OFF   0
#define TTF_PT_ON    1
#define TTF_PT_CUBIC 2

// One horizontal feature of an outline the autohinter fits to the grid: a
// flat run or a round extremum, in pixel space (y down). `ink` is +1 when
// the glyph's ink lies BELOW it (a top edge), -1 when above.
struct ttf_hedge { fx_t y, xmin, xmax, len; int8_t ink; uint8_t used; };
// Sized so it CANNOT fill: an outline point adds at most one flat run and
// one extremum, and an edge at most one anchor, so a glyph within
// TTF_MAX_POINTS never has an edge or an anchor dropped.
#define TTF_MAX_HEDGES (2 * TTF_MAX_POINTS)

struct ttf_scratch {
    // One glyph's outline, already scaled into pixel space.
    fx_t    px[TTF_MAX_POINTS];
    fx_t    py[TTF_MAX_POINTS];
    uint8_t on[TTF_MAX_POINTS];      // TTF_PT_*
    uint8_t flags[TTF_MAX_POINTS];   // the file's own point flags, repeat-expanded
    int     contour_end[TTF_MAX_CONTOURS];
    int     npoints, ncontours;

    struct ttf_edge edges[TTF_MAX_EDGES];
    int     nedges;

    fx_t    xs[TTF_MAX_EDGES];       // crossings of one sub-scanline
    int     xdir[TTF_MAX_EDGES];
    int32_t acc[TTF_MAX_CELL_W];     // coverage accumulator for one row

    // The autohinter's (ttf_hint.c): the edges it found, then the anchors
    // -- an original y and where it was fitted -- sorted by original y.
    struct ttf_hedge hedges[TTF_MAX_HEDGES];
    int     nhedges;
    fx_t    anc_o[TTF_MAX_HEDGES], anc_f[TTF_MAX_HEDGES];
    int     nanc;
};

// Parses the table directory and the handful of headers everything else
// needs. Returns 1 on success, 0 for anything it does not understand or
// cannot bounds-check -- a truncated file, CFF2 (a variable font), a CFF
// font whose charstrings are not Type 2. Outlines may be TrueType's
// (`glyf`) or CFF's (an 'OTTO' font, name-keyed or CID-keyed). `data`
// must stay alive and unmodified for as long as `f` is used; nothing is
// copied.
int ttf_open(struct ttf_font *f, const uint8_t *data, uint32_t len);

// The same for face `index` of a TrueType COLLECTION (`ttcf`, a .ttc:
// several faces sharing tables), as FreeType's face_index and fontconfig's
// `:index=` name one. A single font is a collection of one: index 0.
int ttf_open_index(struct ttf_font *f, const uint8_t *data, uint32_t len, int index);

// How many faces `data` holds: 1 for a font, the member count for a
// collection, 0 for neither.
int ttf_face_count(const uint8_t *data, uint32_t len);

// Unicode codepoint -> glyph id, through `cmap`. Understands format 4
// (the BMP subtable every real font has) and format 12; returns 0 (the
// .notdef glyph) for an unmapped codepoint, which is also what a font
// says for one it has no glyph for -- the caller cannot distinguish
// them and does not need to.
int ttf_glyph_index(const struct ttf_font *f, uint32_t codepoint);

// Advance width of `gid` in font units (hmtx). Glyphs past
// numberOfHMetrics all share the last entry's advance, which is how a
// monospace font stores one advance for thousands of glyphs.
int ttf_advance_units(const struct ttf_font *f, int gid);

// The scale factor taking font units to pixels for a `px` em size.
static inline fx_t ttf_scale_for_px(const struct ttf_font *f, int px) {
    if (!f->units_per_em) return 0;
    return fx_div(fx_from_int(px), fx_from_int(f->units_per_em));
}

// Rounded pixel advance of `gid` at `px`. This is the number that makes
// proportional text possible at all -- see gfx_char_advance().
int ttf_advance_px(const struct ttf_font *f, int gid, int px);

// The script a pair or a run is in, which picks the OpenType LangSys whose
// features apply. A script the face does not list falls back to DFLT, then
// latn, HarfBuzz's order.
enum ttf_script { TTF_SCRIPT_LATIN, TTF_SCRIPT_GREEK, TTF_SCRIPT_CYRILLIC, TTF_SCRIPT_OTHER };

// The script a character kerns in, by its block: Greek and Greek Extended,
// Cyrillic and its Supplement, else Latin -- the three this OS kerns.
static inline enum ttf_script ttf_script_for(uint32_t cp) {
    if ((cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x1F00 && cp <= 0x1FFF)) return TTF_SCRIPT_GREEK;
    if (cp >= 0x400 && cp <= 0x52F) return TTF_SCRIPT_CYRILLIC;
    return TTF_SCRIPT_LATIN;
}

// Kerning adjustment for the ordered pair (left, right) in font units, or 0
// when the font does not kern that pair. Almost always negative: kerning
// tucks a pair closer than their advances alone would put them ("AV", "To").
//
// From the GPOS `kern` feature's PairPos lookups (formats 1 and 2, the first
// glyph's XAdvance) when the face's GPOS has one for `script`, else the old
// format-0 `kern` table. That is ALL of OpenType positioning this reads: no
// contextual kerning, no mark attachment (docs/decisions/gui.md, "Grapheme
// clusters, pair kerning and emoji sequences").
int ttf_pair_kern_units(const struct ttf_font *f, enum ttf_script script,
                        int left_gid, int right_gid);

// ttf_pair_kern_units() for Latin -- what an atlas of Latin-1 is kerned by.
int ttf_kern_units(const struct ttf_font *f, int left_gid, int right_gid);

// Whether the face kerns at all, through GPOS or the `kern` table: what
// decides whether an atlas's kern matrix is worth filling.
static inline int ttf_has_kerning(const struct ttf_font *f) {
    return f && (f->kern_pairs || f->gpos);
}

// GSUB LIGATURES (lookup type 4) of `feature` -- TTF_FEATURE('c','c','m','p')
// for the emoji face's ZWJ sequences, flags, skin tones and keycaps -- applied
// in place to `gids[0..n)` in the font's lookup order. Returns the new count:
// a ligature replaces its components with one glyph. Other lookup types are
// skipped, not half-applied.
#define TTF_FEATURE(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) \
                               | ((uint32_t)(c) << 8) | (uint32_t)(d))
int ttf_ligate(const struct ttf_font *f, enum ttf_script script, uint32_t feature,
               uint16_t *gids, int n);

// The same adjustment rounded to pixels at `px`. Rounded per pair
// rather than accumulated in font units, because the caller adds it to
// an already-rounded advance -- carrying sub-pixel positions would mean
// every drawing path in both rings tracking a fractional pen, which is
// a much larger change than kerning is worth here.
int ttf_kern_px(const struct ttf_font *f, int left_gid, int right_gid, int px);

// SYNTHETIC BOLD: thickens an already-rasterized coverage cell in
// place, by OR-ing each row with itself shifted right by 1..`strength`
// pixels (taking the maximum coverage, so an antialiased edge stays
// antialiased). `strength` is clamped to 1..4.
//
// This is what GDI does when a family has no bold face, and what Cairo
// and DirectWrite fall back to for the same reason -- it is visibly
// worse than a designed bold, especially at small sizes where the
// smear closes a counter, and it is the only way a face somebody just
// dropped into /usr/share/fonts can have a bold at all. font_face.c
// prefers a real `<name>-bold.ttf` and reaches for this only when
// there is not one.
//
// The caller must leave room: emboldening widens a glyph by `strength`
// pixels, so a cell sized to the unemboldened advance clips. font_face.c
// adds it to the cell width and to every advance.
void ttf_embolden(uint8_t *cov, int w, int h, int strength);

// The font's family name (name ID 1: "Arial", "Liberation Sans") into
// `out`, Windows' English record preferred, as ASCII ('?' for anything
// past it). Its length, or -1 when the font names none.
int ttf_family(const struct ttf_font *f, char *out, int cap);

// head.macStyle: TTF_BOLD and TTF_ITALIC.
#define TTF_BOLD   1
#define TTF_ITALIC 2
int ttf_style(const struct ttf_font *f);

// The cell Windows lays text out in, font units: OS/2's usWinAscent and
// usWinDescent, hhea's ascent and descent when there is no OS/2.
void ttf_win_metrics(const struct ttf_font *f, int *ascent, int *descent);

// WINDOWS' OWN DEVICE METRICS, which a hinted font carries for the sizes it
// was tuned at: the cell at `ppem` (VDMX's group for a square-pixel screen),
// and a glyph's advance (hdmx). Each 1 with the answer, 0 when the font has
// none for that size -- the caller then scales the outline's metrics.
int ttf_vdmx(const struct ttf_font *f, int ppem, int *ymax, int *ymin);
int ttf_hdmx(const struct ttf_font *f, int ppem, int gid, int *advance);

// LIGHT AUTOHINTING, on by default: ttf_render_glyph() fits a glyph's
// horizontal features to the pixel grid VERTICALLY ONLY -- the x-height
// rounded (up from 3/8 of a pixel), flat and round tops and feet onto
// their blue zone, horizontal stems a whole number of pixels thick on
// whole rows, everything between interpolated. Advances and x are never
// touched, so layout is identical hinted or not. FreeType's "light" mode
// and DirectWrite's vertical-only hinting have this shape; the font's own
// TrueType instructions are not run. A face with no Latin x-height (a
// symbol font) is drawn unhinted. `on` 0 turns it off for `f`.
void ttf_set_hinting(struct ttf_font *f, int on);

// The whole rows `gid` reaches at `px` above (*up) and below (*down) the
// baseline, AS ttf_render_glyph() DRAWS IT -- hinted, which can lift an
// accent past the font's own ascent. Rounded outward from the outline's
// points (off-curve ones too, so never short). 1, or 0 for a malformed
// glyph; a space reports 0 and 0. A cell sized from these never clips.
int ttf_glyph_extent(const struct ttf_font *f, int gid, int px, int *up, int *down,
                     struct ttf_scratch *sc);

// COLOUR GLYPHS, COLR version 0 (the emoji shape Windows' Segoe UI Emoji
// and Firefox's Twemoji use): a glyph is a stack of ordinary outline
// glyphs, each filled with one colour from the font's CPAL palette 0, or
// with `fg` where the font says "the text colour". Renders `gid` into
// `rgba` (w*h*4 bytes, PREMULTIPLIED R,G,B,A per pixel) as
// ttf_render_glyph() would place it; `cov` is w*h bytes of the caller's
// scratch for one layer at a time. Like ttf_render_glyph() it does not
// clear `rgba`. Returns 1, 0 when `gid` has no colour layers (draw it with
// ttf_render_glyph()), -1 for a malformed table.
int ttf_render_color_glyph(const struct ttf_font *f, int gid, int px,
                           uint8_t *rgba, uint8_t *cov, int w, int h,
                           int x_origin, int baseline, uint32_t fg,
                           struct ttf_scratch *sc);

// The rows a colour glyph's layers reach, as ttf_glyph_extent() measures
// one outline. 0 when `gid` has no colour layers.
int ttf_color_glyph_extent(const struct ttf_font *f, int gid, int px,
                           int *up, int *down, struct ttf_scratch *sc);

// Rasterizes `gid` into `cov` (w*h bytes, row-major, 0 = background,
// 255 = fully ink), with the glyph origin at (x_origin, baseline) in
// that bitmap's own coordinates and y growing DOWNWARD (the font's y
// grows upward; the flip happens here).
//
// Returns 1 on success -- including for a glyph with no outline at all,
// which is what a space is, and which leaves `cov` untouched. Returns 0
// only for a malformed or too-complex glyph. Does NOT clear `cov`: the
// caller owns it and usually wants to clear a whole atlas once.
int ttf_render_glyph(const struct ttf_font *f, int gid, int px,
                     uint8_t *cov, int w, int h,
                     int x_origin, int baseline,
                     struct ttf_scratch *sc);

#ifdef __cplusplus
}
#endif

#endif
