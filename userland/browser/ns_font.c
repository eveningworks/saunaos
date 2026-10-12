// Text for NetSurf: the faces, the three measuring calls its layout makes
// (gui_layout_table), and drawing a run. See ns_font.h.
//
// **MEASURING NEVER RASTERISES.** Layout asks for widths thousands of
// times a page; uglyph caches RENDERED glyphs, so measuring through it
// would rasterise every character at every size just to read its
// advance. Widths come from the font's metrics (ttf_advance_px() plus
// kerning) here, and only drawing goes through uglyph -- and both walk
// a string the same way, so a run draws exactly as wide as it measured.
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "browser/ns_font.h"
#include "lib/uunicode.h"
#include "ui/uglyph.h"

#include "netsurf/layout.h"
#include "netsurf/plot_style.h"

// CSS's reference pixel: 96 to the inch. main() sets NetSurf's own DPI
// to the same (browser_set_dpi()), so a 12pt font NetSurf asks for is
// the 16 px it laid the page out with.
#define NS_DPI 96

enum { F_SANS, F_SANS_BOLD, F_MONO, F_MONO_BOLD, F_COUNT };

// Liberation Sans and DejaVu Sans Mono are what /usr/share/fonts ships;
// serif, italic and the rest map onto these until faces for them are
// added (docs/roadmap.md).
static const char *const FACE_PATH[F_COUNT] = {
    "/usr/share/fonts/liberation-sans.ttf",
    "/usr/share/fonts/liberation-sans-bold.ttf",
    "/usr/share/fonts/dejavu-sans-mono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-bold.ttf",
};

static struct uglyph_face g_face[F_COUNT];

int ns_font_init(void) {
    int ok = 0;
    for (int i = 0; i < F_COUNT; i++) ok += uglyph_open(&g_face[i], FACE_PATH[i]);
    // Whatever opened stands in for whatever did not: a missing bold is
    // regular text, never no text.
    for (int i = 0; i < F_COUNT; i++)
        if (!g_face[i].ok)
            for (int j = 0; j < F_COUNT; j++)
                if (g_face[j].ok) { g_face[i] = g_face[j]; break; }
    return ok > 0;
}

static struct uglyph_face *face_for(const plot_font_style_t *st) {
    int mono = st->family == PLOT_FONT_FAMILY_MONOSPACE;
    int bold = st->weight >= 600;
    return &g_face[(mono ? F_MONO : F_SANS) + bold];
}

int ns_font_px(const plot_font_style_t *st) {
    int px = (int)(((long)st->size * NS_DPI) / (72L * PLOT_STYLE_SCALE));
    return px < 1 ? 1 : px;
}

// The width a missing glyph takes: an em's half, so the text keeps its
// shape and a box can be drawn where the character should be.
static int missing_w(int px) { return px / 2 > 1 ? px / 2 : 1; }

// Walks `s[0..len)`, calling `each` with every character's byte offset,
// the pen before it and its advance (kerning against the previous one
// folded in). Stops early when `each` returns nonzero. Returns the pen.
typedef int (*char_fn)(void *ctx, size_t off, int pen, int adv, uint32_t cp, int gid);

static int walk(struct uglyph_face *f, int px, const char *s, size_t len, char_fn each, void *ctx) {
    int pen = 0, prev = 0;
    size_t i = 0;
    while (i < len) {
        int n;
        uint32_t cp = uunicode_utf8_decode(s + i, &n);
        if (n <= 0 || i + (size_t)n > len) n = 1;          // a truncated tail: one byte
        int gid = f->ok ? ttf_glyph_index(&f->f, cp) : 0;
        int kern = (prev && gid) ? ttf_kern_px(&f->f, prev, gid, px) : 0;
        int adv = gid ? ttf_advance_px(&f->f, gid, px) : missing_w(px);
        if (each && each(ctx, i, pen + kern, adv, cp, gid)) return pen + kern;
        pen += kern + adv;
        prev = gid;
        i += (size_t)n;
    }
    return pen;
}

static nserror nf_width(const plot_font_style_t *st, const char *s, size_t len, int *width) {
    *width = walk(face_for(st), ns_font_px(st), s, len, 0, 0);
    return NSERROR_OK;
}

// position(): the character whose box holds x -- its offset, and the
// nearer of its two edges.
struct pos { int x; size_t off; int at; int found; };

static int pos_each(void *ctx, size_t off, int pen, int adv, uint32_t cp, int gid) {
    (void)cp; (void)gid;
    struct pos *p = ctx;
    if (p->x < pen + adv) {
        int right = p->x >= pen + adv / 2;
        p->off = off;
        p->at = right ? pen + adv : pen;
        p->found = right ? 2 : 1;
        return 1;
    }
    return 0;
}

static nserror nf_position(const plot_font_style_t *st, const char *s, size_t len,
                           int x, size_t *off, int *actual_x) {
    struct pos p = { x, len, 0, 0 };
    int end = walk(face_for(st), ns_font_px(st), s, len, pos_each, &p);
    if (!p.found) {
        *off = len;
        *actual_x = end;
    } else if (p.found == 2) {
        // The right edge: the offset is of the NEXT character.
        int n;
        uunicode_utf8_decode(s + p.off, &n);
        *off = p.off + (size_t)(n > 0 ? n : 1);
        if (*off > len) *off = len;
        *actual_x = p.at;
    } else {
        *off = p.off;
        *actual_x = p.at;
    }
    return NSERROR_OK;
}

// split(): the last space whose left edge still fits in x; failing
// that, the first space after it; failing that, the whole string.
struct split { int x; size_t fit_off; int fit_x; size_t over_off; int over_x; int have_fit; };

static int split_each(void *ctx, size_t off, int pen, int adv, uint32_t cp, int gid) {
    (void)adv; (void)gid;
    struct split *p = ctx;
    if (cp != ' ' || off == 0) return 0;
    if (pen <= p->x) {
        p->fit_off = off;
        p->fit_x = pen;
        p->have_fit = 1;
        return 0;
    }
    p->over_off = off;
    p->over_x = pen;
    return 1;
}

static nserror nf_split(const plot_font_style_t *st, const char *s, size_t len,
                        int x, size_t *off, int *actual_x) {
    struct split p = { x, 0, 0, len, 0, 0 };
    int end = walk(face_for(st), ns_font_px(st), s, len, split_each, &p);
    if (p.have_fit) {
        *off = p.fit_off;
        *actual_x = p.fit_x;
    } else if (p.over_off < len) {
        *off = p.over_off;
        *actual_x = p.over_x;
    } else {
        *off = len;
        *actual_x = end;
    }
    return NSERROR_OK;
}

static struct gui_layout_table g_layout = {
    .width = nf_width,
    .position = nf_position,
    .split = nf_split,
};
struct gui_layout_table *ns_layout_table = &g_layout;

// --- drawing ------------------------------------------------------------

struct draw { struct ugfx_surface *s; struct uglyph_face *f; int px, x, baseline; uint32_t color; };

static int draw_each(void *ctx, size_t off, int pen, int adv, uint32_t cp, int gid) {
    (void)off;
    struct draw *d = ctx;
    if (gid) {
        char one[5];
        int n = uunicode_utf8(cp, one);
        if (n > 0) {
            one[n] = '\0';
            uglyph_text(d->s, d->f, one, d->px, d->x + pen, d->baseline, d->color);
        }
    } else if (cp != ' ' && cp != 0xA0) {
        // A character no shipped face has: an outlined box where it goes.
        int h = d->px * 2 / 3;
        ugfx_draw_rect(d->s, d->x + pen + 1, d->baseline - h, adv - 2 > 1 ? adv - 2 : 1, h, d->color);
    }
    return 0;
}

int ns_font_draw(struct ugfx_surface *s, const plot_font_style_t *st, int x, int baseline,
                 const char *text, size_t len, uint32_t color) {
    struct draw d = { s, face_for(st), ns_font_px(st), x, baseline, color };
    return walk(d.f, d.px, text, len, draw_each, &d);
}
