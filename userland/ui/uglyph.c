// Any glyph of a font file, cached (ui/uglyph.h).
#include "ui/uglyph.h"
#include "lib/ufile.h"
#include "lib/ulru.h"
#include "ui/ulog.h"
#include "lib/uunicode.h"

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
    uint32_t cp;
    int px;
    int w, h;                     // the INK box
    int bx, by;                   // its top-left from the pen at the baseline
    int adv;                      // the pen's advance, pixels
    uint32_t *pix;                // w*h, alpha = coverage; NULL for a glyph with no ink
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

static struct cached *render(struct uglyph_face *face, uint32_t cp, int px);

int uglyph_text(struct ugfx_surface *s, struct uglyph_face *face, const char *utf8, int px,
                int x, int baseline, uint32_t color) {
    if (!face->ok) return 0;
    int pen = x, prev = 0;
    for (const char *p = utf8; *p;) {
        int n;
        uint32_t cp = uunicode_utf8_decode(p, &n);
        p += n;
        int gid = ttf_glyph_index(&face->f, cp);
        if (prev && gid) pen += ttf_kern_px(&face->f, prev, gid, px);
        prev = gid;
        struct cached *c = gid ? render(face, cp, px) : 0;
        if (!c) continue;
        if (s && c->pix) ugfx_blit_tinted(s, pen + c->bx, baseline + c->by, c->w, c->h, c->pix, c->w, color);
        pen += c->adv;
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
struct ckey { const struct uglyph_face *face; uint32_t cp; int px; };

static int cached_is(const struct ulru_node *n, const void *key) {
    const struct cached *c = (const struct cached *)n;
    const struct ckey *k = key;
    return c->face == k->face && c->cp == k->cp && c->px == k->px;
}

static unsigned ckey_hash(const struct uglyph_face *face, uint32_t cp, int px) {
    unsigned h = cp * 2654435761u ^ (unsigned)px * 40503u ^ (unsigned)((uintptr_t)face >> 4) * 97u;
    return h ^ (h >> 15);
}

// The cached glyph, rasterized on a miss. A glyph with no ink (a space)
// is cached too, as an entry with no pixels: its advance is the answer.
static struct cached *render(struct uglyph_face *face, uint32_t cp, int px) {
    unsigned hv = ckey_hash(face, cp, px);
    struct ckey key = { face, cp, px };
    struct cached *hit = (struct cached *)ulru_find(&g_lru, hv, cached_is, &key);
    if (hit) return hit;
    int gid = ttf_glyph_index(&face->f, cp);
    if (!gid) return 0;
    int cw = px * 2, ch = px * 2;
    if (cw > TTF_MAX_CELL_W) cw = ch = TTF_MAX_CELL_W;
    uint8_t *cov = calloc((size_t)cw * (size_t)ch, 1);
    if (!cov) return 0;
    if (!ttf_render_glyph(&face->f, gid, px, cov, cw, ch, cw / 4, ch * 2 / 3, &g_scratch)) { free(cov); return 0; }
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
    if (!slot) { free(cov); return 0; }
    slot->face = face;
    slot->cp = cp;
    slot->px = px;
    slot->adv = ttf_advance_px(&face->f, gid, px);
    slot->bx = x0 - cw / 4;
    slot->by = y0 - ch * 2 / 3;
    if (x1 >= 0) {                 // else a space: no ink, but a glyph
        slot->w = x1 - x0 + 1;
        slot->h = y1 - y0 + 1;
        slot->pix = malloc((size_t)slot->w * (size_t)slot->h * sizeof *slot->pix);
        if (!slot->pix) { free(cov); free(slot); return 0; }
        for (int y = 0; y < slot->h; y++)
            for (int x = 0; x < slot->w; x++)
                slot->pix[y * slot->w + x] = (uint32_t)cov[(y0 + y) * cw + x0 + x] << 24;
    }
    free(cov);
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

int uglyph_draw(struct ugfx_surface *s, struct uglyph_face *face, uint32_t cp, int px,
                int x, int y, int w, int h, uint32_t color) {
    if (!face->ok || px <= 0) return 0;
    struct cached *c = render(face, cp, px);
    if (!c) return 0;
    if (!c->pix) return UGLYPH_NO_INK;
    int em = face->f.units_per_em > 0 ? face->f.units_per_em : 1;
    int asc = face->f.ascent * px / em, desc = face->f.descent * px / em;
    int top = y + (h - asc - desc) / 2 + asc + c->by;
    if (top + c->h > y + h) top = y + h - c->h;
    if (top < y) top = y;
    ugfx_blit_tinted(s, x + (w - c->w) / 2, top, c->w, c->h, c->pix, c->w, color);
    return 1;
}
