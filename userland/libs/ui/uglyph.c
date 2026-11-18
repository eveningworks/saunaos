// Any glyph of a font file, cached (ui/uglyph.h).
#include "ui/uglyph.h"
#include "base/ufile.h"
#include "base/ulru.h"
#include "base/ulog.h"
#include "base/uunicode.h"
#include "base/ucompose.h"
#include "kgrapheme.h"

#include <stdlib.h>
#include <string.h>

#define FACE_MAX  (8u << 20)
// Rendered glyphs kept, BY BYTES, least recently used out. A page in the
// browser is a few faces at a few sizes, ~1 KB a glyph at text sizes;
// a Character Map grid is a few hundred glyphs of a few KB. Eviction is
// ordinary and said once.
#define CACHE_BUDGET (4ul << 20)

struct cached {
    struct ulru_node node;        // first: a node IS its entry
    const struct uglyph_face *face;
    int gid;                      // KEYED BY GLYPH: a ligature has no character
    int px;
    int w, h;                     // the INK box
    int bx, by;                   // its top-left from the pen at the baseline
    int adv;                      // the pen's advance, pixels
    uint32_t *pix;                // w*h, alpha = coverage; NULL for a glyph with no ink
    int color;                    // pix is the glyph's own ARGB (straight): an emoji
};

static struct ulru g_lru;
static unsigned long g_bytes, g_evicted;
static struct ttf_scratch g_scratch;   // ~60 KB: static, never on the stack

int uglyph_open(struct uglyph_face *face, const char *path) {
    memset(face, 0, sizeof *face);
    if (ufile_slurp(path, FACE_MAX, &face->data, &face->len) != UFILE_OK) return 0;
    face->ok = ttf_open(&face->f, face->data, (uint32_t)face->len);
    if (!face->ok) { free(face->data); face->data = 0; }
    return face->ok;
}

static struct cached *render_gid(struct uglyph_face *face, int gid, int px);

// A colour glyph in its own colours, any other in `color`.
static void blit(struct ugfx_surface *s, const struct cached *c, int x, int y, uint32_t color) {
    if (c->color) ugfx_blit_alpha(s, x, y, c->w, c->h, c->pix, c->w);
    else ugfx_blit_tinted(s, x, y, c->w, c->h, c->pix, c->w, color);
}

// The glyphs grapheme cluster [p, p+n) is drawn with, into `g`: one when
// its accents compose (base/ucompose.h) or the face ligates it (its `ccmp`
// lookups: an emoji sequence in an emoji face), else one per character the
// face has. Returns the count; `*first` gets the cluster's first character,
// what it kerns as.
static int cluster_glyphs(const struct uglyph_face *face, const char *p, int n,
                          uint16_t *g, int cap, uint32_t *first) {
    uint32_t cps[32];
    int k = 0;
    for (int i = 0; i < n && k < 32;) {
        int len;
        cps[k++] = uunicode_utf8_decode(p + i, &len);
        i += len > 0 ? len : 1;
    }
    *first = cps[0];
    k = ucompose_cluster(cps, k);
    int m = 0;
    for (int i = 0; i < k && m < cap; i++) {
        int gid = ttf_glyph_index(&face->f, cps[i]);
        if (gid || i == 0) g[m++] = (uint16_t)gid;
    }
    if (m > 1) m = ttf_ligate(&face->f, TTF_SCRIPT_OTHER, TTF_FEATURE('c', 'c', 'm', 'p'), g, m);
    // What no ligature took up is the parts, minus the ones that only
    // join or choose (a selector, ZWJ): drawn, they would be a box or a gap.
    if (m > 1) {
        int vs15 = ttf_glyph_index(&face->f, 0xFE0E), vs16 = ttf_glyph_index(&face->f, 0xFE0F);
        int zwj = ttf_glyph_index(&face->f, 0x200D), o = 1;
        for (int i = 1; i < m; i++)
            if (!g[i] || (g[i] != vs15 && g[i] != vs16 && g[i] != zwj)) g[o++] = g[i];
        m = o;
    }
    return m;
}

int uglyph_text(struct ugfx_surface *s, struct uglyph_face *face, const char *utf8, int px,
                int x, int baseline, uint32_t color) {
    if (!face->ok) return 0;
    int pen = x, prev = 0;
    int len = (int)strlen(utf8);
    // A GRAPHEME CLUSTER at a time (api/kgrapheme.h), kerned between
    // clusters by glyph id in the script of the one on the right.
    for (int i = 0, nx; (nx = kgrapheme_next(utf8, len, i)) > i; i = nx) {
        uint16_t g[32];
        uint32_t first;
        int m = cluster_glyphs(face, utf8 + i, nx - i, g, 32, &first);
        if (prev && g[0]) pen += fx_round(fx_mul(fx_from_int(ttf_pair_kern_units(&face->f, ttf_script_for(first),
                                                                                prev, g[0])),
                                                  ttf_scale_for_px(&face->f, px)));
        prev = g[0];
        for (int j = 0; j < m; j++) {
            struct cached *c = g[j] ? render_gid(face, g[j], px) : 0;
            if (!c) continue;
            if (s && c->pix) blit(s, c, pen + c->bx, baseline + c->by, color);
            pen += c->adv;
        }
    }
    return pen - x;
}

int uglyph_text_width(struct uglyph_face *face, const char *utf8, int px) {
    return uglyph_text(0, face, utf8, px, 0, 0, 0);
}

int uglyph_ascent(const struct uglyph_face *face, int px) {
    if (!face->ok || !face->f.units_per_em) return px;
    return face->f.ascent * px / face->f.units_per_em;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// The `name` table, read with every offset checked against the file:
// nameID 1 (family), Windows Unicode (UTF-16BE, ASCII kept) first, the
// Macintosh Roman record (8-bit) if there is no Windows one.
int uglyph_family(const struct uglyph_face *face, char *out, int cap) {
    if (!face->ok || cap < 2) return 0;
    const uint8_t *d = face->data;
    size_t len = face->len;
    if (len < 12) return 0;
    int ntab = be16(d + 4);
    uint32_t off = 0;
    for (int i = 0; i < ntab && 12 + 16 * (size_t)(i + 1) <= len; i++)
        if (!memcmp(d + 12 + 16 * i, "name", 4)) off = be32(d + 12 + 16 * i + 8);
    if (!off || off + 6 > len) return 0;
    int count = be16(d + off + 2);
    uint32_t strings = off + be16(d + off + 4);
    int best = -1, best_kind = 0;
    for (int i = 0; i < count && off + 6 + 12 * (size_t)(i + 1) <= len; i++) {
        const uint8_t *r = d + off + 6 + 12 * i;
        if (be16(r + 6) != 1) continue;
        int kind = be16(r) == 3 ? 2 : be16(r) == 1 ? 1 : 0;
        if (kind > best_kind) { best = i; best_kind = kind; }
    }
    if (best < 0) return 0;
    const uint8_t *r = d + off + 6 + 12 * best;
    uint32_t at = strings + be16(r + 10), n = be16(r + 8);
    if (at + n > len) return 0;
    int o = 0;
    for (uint32_t k = 0; k < n && o < cap - 1; k += best_kind == 2 ? 2 : 1) {
        uint32_t ch = best_kind == 2 ? be16(d + at + k) : d[at + k];
        out[o++] = ch >= 32 && ch < 127 ? (char)ch : '?';
    }
    out[o] = '\0';
    return o > 0;
}

static size_t cached_bytes(const struct cached *c) {
    return sizeof *c + (size_t)c->w * (size_t)c->h * sizeof *c->pix;
}

static void drop(struct cached *c) {
    ulru_remove(&g_lru, &c->node);
    g_bytes -= cached_bytes(c);
    free(c->pix);
    free(c);
}

void uglyph_forget(const struct uglyph_face *face) {
    for (struct ulru_node *n = ulru_oldest(&g_lru), *next; n; n = next) {
        next = n->newer;
        if (((struct cached *)n)->face == face) drop((struct cached *)n);
    }
}

void uglyph_close(struct uglyph_face *face) {
    uglyph_forget(face);
    free(face->data);
    memset(face, 0, sizeof *face);
}

int uglyph_has(const struct uglyph_face *face, uint32_t cp) {
    return face->ok && ttf_glyph_index(&face->f, cp) != 0;
}

// Rasterizes into a cell twice the em, then keeps only the ink's box.
struct ckey { const struct uglyph_face *face; int gid; int px; };

static int cached_is(const struct ulru_node *n, const void *key) {
    const struct cached *c = (const struct cached *)n;
    const struct ckey *k = key;
    return c->face == k->face && c->gid == k->gid && c->px == k->px;
}

static unsigned ckey_hash(const struct uglyph_face *face, int gid, int px) {
    unsigned h = (unsigned)gid * 2654435761u ^ (unsigned)px * 40503u ^ (unsigned)((uintptr_t)face >> 4) * 97u;
    return h ^ (h >> 15);
}

// The cached glyph, rasterized on a miss. A glyph with no ink (a space)
// is cached too, as an entry with no pixels: its advance is the answer.
static struct cached *render_gid(struct uglyph_face *face, int gid, int px) {
    if (gid <= 0) return 0;
    unsigned hv = ckey_hash(face, gid, px);
    struct ckey key = { face, gid, px };
    struct cached *hit = (struct cached *)ulru_find(&g_lru, hv, cached_is, &key);
    if (hit) return hit;
    int cw = px * 2, ch = px * 2;
    if (cw > TTF_MAX_CELL_W) cw = ch = TTF_MAX_CELL_W;
    uint8_t *cov = calloc((size_t)cw * (size_t)ch, 1);
    if (!cov) return 0;
    // A COLOUR glyph (COLR v0, ttf.h) when the face has one for `gid`:
    // premultiplied RGBA, kept below as straight ARGB for ugfx_blit_alpha().
    uint8_t *rgba = face->f.colr ? calloc((size_t)cw * (size_t)ch, 4) : 0;
    if (rgba && ttf_render_color_glyph(&face->f, gid, px, rgba, cov, cw, ch, cw / 4, ch * 2 / 3,
                                       0, &g_scratch) != 1) {
        free(rgba);
        rgba = 0;
        for (size_t k = 0; k < (size_t)cw * (size_t)ch; k++) cov[k] = 0;
    }
    if (rgba) {
        for (size_t k = 0; k < (size_t)cw * (size_t)ch; k++) cov[k] = rgba[k * 4 + 3];
    } else if (!ttf_render_glyph(&face->f, gid, px, cov, cw, ch, cw / 4, ch * 2 / 3, &g_scratch)) {
        free(cov);
        return 0;
    }
    int x0 = cw, y0 = ch, x1 = -1, y1 = -1;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            if (cov[y * cw + x]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    struct cached *slot = calloc(1, sizeof *slot);
    if (!slot) { free(cov); free(rgba); return 0; }
    slot->color = rgba != 0;
    slot->face = face;
    slot->gid = gid;
    slot->px = px;
    slot->adv = ttf_advance_px(&face->f, gid, px);
    slot->bx = x0 - cw / 4;
    slot->by = y0 - ch * 2 / 3;
    if (x1 >= 0) {                 // else a space: no ink, but a glyph
        slot->w = x1 - x0 + 1;
        slot->h = y1 - y0 + 1;
        slot->pix = malloc((size_t)slot->w * (size_t)slot->h * sizeof *slot->pix);
        if (!slot->pix) { free(cov); free(rgba); free(slot); return 0; }
        for (int y = 0; y < slot->h; y++)
            for (int x = 0; x < slot->w; x++) {
                size_t k = (size_t)(y0 + y) * cw + x0 + x;
                uint32_t a = cov[k];
                if (!rgba) { slot->pix[y * slot->w + x] = a << 24; continue; }
                // Premultiplied to straight: each channel back over its alpha.
                const uint8_t *p = rgba + k * 4;
                uint32_t r = a ? p[0] * 255u / a : 0, g = a ? p[1] * 255u / a : 0, b = a ? p[2] * 255u / a : 0;
                slot->pix[y * slot->w + x] = a << 24 | (r > 255 ? 255 : r) << 16 |
                                             (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
    }
    free(cov);
    free(rgba);
    struct ulru_node *old;
    while ((old = ulru_oldest(&g_lru)) && g_bytes + cached_bytes(slot) > CACHE_BUDGET) {
        if (!g_evicted++)
            ulogf("uglyph: glyph cache full at %lu bytes; evicting least recently used\n", CACHE_BUDGET);
        drop((struct cached *)old);
    }
    if (!ulru_insert(&g_lru, &slot->node, hv)) { free(slot->pix); free(slot); return 0; }
    g_bytes += cached_bytes(slot);
    return slot;
}

// Glyph `gid` centred across the rect on the face's baseline (uglyph_draw()).
static int draw_gid(struct ugfx_surface *s, struct uglyph_face *face, int gid, int px,
                    int x, int y, int w, int h, uint32_t color) {
    if (!face->ok || px <= 0) return 0;
    struct cached *c = render_gid(face, gid, px);
    if (!c) return 0;
    if (!c->pix) return UGLYPH_NO_INK;
    int em = face->f.units_per_em > 0 ? face->f.units_per_em : 1;
    int asc = face->f.ascent * px / em, desc = face->f.descent * px / em;
    int top = y + (h - asc - desc) / 2 + asc + c->by;
    if (top + c->h > y + h) top = y + h - c->h;
    if (top < y) top = y;
    blit(s, c, x + (w - c->w) / 2, top, color);
    return 1;
}

int uglyph_draw(struct ugfx_surface *s, struct uglyph_face *face, uint32_t cp, int px,
                int x, int y, int w, int h, uint32_t color) {
    if (!face->ok) return 0;
    return draw_gid(s, face, ttf_glyph_index(&face->f, cp), px, x, y, w, h, color);
}

int uglyph_draw_cluster(struct ugfx_surface *s, struct uglyph_face *face, const char *utf8, int px,
                        int x, int y, int w, int h, uint32_t color) {
    if (!face->ok || !utf8 || !utf8[0]) return 0;
    uint16_t g[32];
    uint32_t first;
    int len = (int)strlen(utf8);
    int m = cluster_glyphs(face, utf8, kgrapheme_next(utf8, len, 0), g, 32, &first);
    return m == 1 ? draw_gid(s, face, g[0], px, x, y, w, h, color) : 0;
}
