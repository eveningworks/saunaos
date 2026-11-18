// The glyph cache: every character the session atlas lacks, rasterized
// on first use and kept per process (docs/utf8-design.md, "Decided
// here", 3 and 4). ugfx.c asks it for anything past U+00FF.
//
// THE SHAPE IS PANGO'S AND DIRECTWRITE'S: glyphs rendered on demand,
// keyed (face, size, character), the fallback list's faces in order when
// the chosen one has no glyph (base/ufontfallback.h -- Android's ordered
// list), an outlined box when nothing does. An emoji tries the list's
// emoji face first. Split from ugfx.c because
// the atlas is mapped and this is rendered -- one owns a shm mapping,
// the other a heap.
//
// **A CACHED GLYPH IS AN ATLAS CELL IN ALL BUT STORAGE**: rasterized at
// the font's own em size, with its baseline on the atlas's baseline row,
// into a cell exactly `char_h` rows tall -- so ugfx draws it with the
// same loop and it lines up with the letters beside it whatever face it
// came from. Only the width is its own: its advance, or its ink if that
// reaches further.
//
// BOUNDED IN BYTES, least recently used out, and hashed rather than
// scanned (base/ulru.h): this is on the path of every label in a Greek
// desktop, not a character map's grid.
#include "ui/ugfx_glyph.h"
#include "base/ufile.h"
#include "base/ulru.h"
#include "base/ulog.h"
#include "base/ufontfallback.h"
#include "ttf.h"
#include <stdlib.h>
#include <string.h>

#define FACE_FILE_MAX (8u << 20)

// The budget. A glyph at the default size is a couple of hundred bytes,
// so this holds a few thousand -- every character a page of any one
// script uses, many times over. Eviction is ordinary, not an error.
#define CACHE_BUDGET (512ul * 1024)

struct face {
    char *path;
    unsigned char *data;
    struct ttf_font t;
    int ok;                    // opened and parsed; 0 = tried and failed
};

struct gent {
    struct ulru_node node;     // first: a node IS its entry
    uint32_t cp;               // a code point, or a UGFX_CP_CLUSTER token
    int face, px, base, cell_h, smear, emoji;
    int mark;                  // a mark centred over a letter this wide, + 1; 0 otherwise
    int none;                  // a cluster no face draws as one glyph: draw its parts
    int w, adv;
    int color;                 // cov is premultiplied R,G,B,A per pixel (an emoji)
    unsigned char cov[];       // w * cell_h, times 4 when `color`
};

static struct face **g_faces;
static int g_nfaces, g_capfaces;

static struct ulru g_lru;
static unsigned long g_bytes, g_evicted;
static struct ttf_scratch *g_scratch;

void ugfx_glyph_cache_stats(int *entries, unsigned long *bytes,
                            unsigned long *budget, unsigned long *evicted) {
    if (entries) *entries = g_lru.count;
    if (bytes) *bytes = g_bytes;
    if (budget) *budget = CACHE_BUDGET;
    if (evicted) *evicted = g_evicted;
}

// The face behind `path`, opened on first use and REMEMBERED IF IT FAILS
// -- a missing file must not be retried on every character. -1 for none.
static int face_for(const char *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < g_nfaces; i++)
        if (!strcmp(g_faces[i]->path, path)) return g_faces[i]->ok ? i : -1;
    if (g_nfaces == g_capfaces) {
        int cap = g_capfaces ? g_capfaces * 2 : 4;
        struct face **nf = realloc(g_faces, (size_t)cap * sizeof *nf);
        if (!nf) return -1;
        g_faces = nf;
        g_capfaces = cap;
    }
    struct face *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->path = strdup(path);
    if (!f->path) { free(f); return -1; }
    size_t len = 0;
    // A sandboxed process that did not unveil /usr/share/fonts lands
    // here too, and draws boxes: said once, so it is not a mystery.
    if (ufile_slurp(path, FACE_FILE_MAX, &f->data, &len) == UFILE_OK &&
        ttf_open(&f->t, f->data, (uint32_t)len))
        f->ok = 1;
    else {
        free(f->data);
        f->data = 0;
        ulogf("ugfx: %s will not open; characters past the atlas fall back\n", path);
    }
    g_faces[g_nfaces++] = f;
    return f->ok ? g_nfaces - 1 : -1;
}

struct gkey { uint32_t cp; int face, px, base, cell_h, smear, emoji, mark; };

static unsigned hash(const struct gkey *k) {
    unsigned h = k->cp * 2654435761u;
    h ^= (unsigned)(k->face + 1) * 40503u + (unsigned)k->px * 97u + (unsigned)k->base * 131u
       + (unsigned)k->cell_h * 193u + (unsigned)k->smear * 7u + (unsigned)k->emoji * 389u
       + (unsigned)k->mark * 2671u;
    return h ^ (h >> 15);
}

static int gent_is(const struct ulru_node *n, const void *key) {
    const struct gent *e = (const struct gent *)n;
    const struct gkey *k = key;
    return e->cp == k->cp && e->face == k->face && e->px == k->px && e->base == k->base &&
           e->cell_h == k->cell_h && e->smear == k->smear && e->emoji == k->emoji && e->mark == k->mark;
}

static size_t gent_bytes(const struct gent *e) {
    return sizeof *e + (size_t)e->w * (size_t)e->cell_h * (e->color ? 4u : 1u);
}

// A COLOUR glyph (COLR v0, ttf.h), or NULL when `gid` has none. Drawn at
// the text's size unless its layers would leave the cell, then a step
// smaller until they fit -- an emoji is taller than a capital in most
// emoji fonts, and the cell is the text's.
static struct gent *render_color(struct face *f, int gid, int px, int base, int cell_h) {
    int up, down, epx = px, room = cell_h - base;
    if (base < 1 || room < 0) return 0;   // no cell to fit into
    if (!ttf_color_glyph_extent(&f->t, gid, epx, &up, &down, g_scratch)) return 0;
    // The size that fits, estimated in one step (the layers scale with the
    // size), then re-measured until it does -- the extents round outward,
    // so the estimate can leave a row or two over. Never under two thirds
    // of the text's size; a glyph still over at that is clipped.
    if (up > base || down > room) {
        int a = up > base ? px * base / up : px;
        int b = down > room ? (room ? px * room / down : px * 2 / 3) : px;
        epx = a < b ? a : b;
        int floor = px * 2 / 3;
        if (epx < floor) epx = floor;
        for (int tries = 0; tries < 4; tries++) {
            ttf_color_glyph_extent(&f->t, gid, epx, &up, &down, g_scratch);
            if ((up <= base && down <= room) || epx <= floor) break;
            epx--;
        }
    }
    int adv = ttf_advance_px(&f->t, gid, epx);
    if (adv < 1) adv = epx;
    int cw = adv + epx;
    if (cw > TTF_MAX_CELL_W) cw = TTF_MAX_CELL_W;
    size_t n = (size_t)cw * (size_t)cell_h;
    unsigned char *rgba = calloc(n, 4), *cov = malloc(n);
    if (!rgba || !cov) { free(rgba); free(cov); return 0; }
    // Layers in "the text colour" are drawn black: the cache is keyed
    // without a colour, and the emoji fonts shipped use none.
    if (ttf_render_color_glyph(&f->t, gid, epx, rgba, cov, cw, cell_h, 0, base, 0, g_scratch) != 1) {
        free(rgba); free(cov);
        return 0;
    }
    free(cov);
    int last = -1;
    for (int y = 0; y < cell_h; y++)
        for (int x = cw - 1; x > last; x--)
            if (rgba[((size_t)y * cw + x) * 4 + 3]) { last = x; break; }
    int w = last + 1 > adv ? last + 1 : adv;
    if (w > cw) w = cw;
    struct gent *e = malloc(sizeof *e + (size_t)w * (size_t)cell_h * 4);
    if (!e) { free(rgba); return 0; }
    memset(e, 0, sizeof *e);
    for (int y = 0; y < cell_h; y++)
        memcpy(e->cov + (size_t)y * w * 4, rgba + (size_t)y * cw * 4, (size_t)w * 4);
    free(rgba);
    e->w = w;
    e->adv = adv;
    e->color = 1;
    return e;
}

static void evict_one(void) {
    struct gent *e = (struct gent *)ulru_oldest(&g_lru);
    if (!e) return;
    ulru_remove(&g_lru, &e->node);
    g_bytes -= gent_bytes(e);
    if (!g_evicted++)
        ulogf("ugfx: glyph cache full at %lu bytes; evicting least recently used\n",
              (unsigned long)CACHE_BUDGET);
    free(e);
}

// Rasterizes glyph `gid` of face `fi` into a fresh entry.
static struct gent *render_gid(int fi, int gid, int px, int base, int cell_h, int smear) {
    struct face *f = g_faces[fi];
    if (!g_scratch && !(g_scratch = malloc(sizeof *g_scratch))) return 0;
    if (f->t.colr) {
        struct gent *c = render_color(f, gid, px, base, cell_h);
        if (c) return c;
    }
    int adv = ttf_advance_px(&f->t, gid, px);
    if (adv < 0) adv = 0;
    if (adv > 0) adv += smear;
    // Room for ink past the advance (an italic's overhang, a wide symbol
    // in a narrow cell); trimmed to what was actually inked below.
    int cw = adv + px + smear;
    if (cw < 1) cw = 1;
    if (cw > TTF_MAX_CELL_W) cw = TTF_MAX_CELL_W;
    unsigned char *cell = calloc((size_t)cw * (size_t)cell_h, 1);
    if (!cell) return 0;
    if (!ttf_render_glyph(&f->t, gid, px, cell, cw, cell_h, 0, base, g_scratch)) {
        free(cell);
        return 0;
    }
    if (smear) ttf_embolden(cell, cw, cell_h, smear);
    int last = -1;
    for (int y = 0; y < cell_h; y++)
        for (int x = cw - 1; x > last; x--)
            if (cell[y * cw + x]) { last = x; break; }
    int w = last + 1 > adv ? last + 1 : adv;
    if (w < 1) w = 1;
    if (w > cw) w = cw;
    struct gent *e = malloc(sizeof *e + (size_t)w * (size_t)cell_h);
    if (!e) { free(cell); return 0; }
    memset(e, 0, sizeof *e);
    for (int y = 0; y < cell_h; y++) memcpy(e->cov + y * w, cell + y * cw, (size_t)w);
    free(cell);
    e->w = w;
    e->adv = adv;
    return e;
}

// Rasterizes `cp` from face `fi`, or NULL when the face has no glyph for it.
static struct gent *render(int fi, uint32_t cp, int px, int base, int cell_h, int smear) {
    int gid = ttf_glyph_index(&g_faces[fi]->t, cp);
    return gid ? render_gid(fi, gid, px, base, cell_h, smear) : 0;
}

// NOTHING HAS IT: an outlined box the advance wide, cap-height tall on
// the baseline -- "tofu", what every toolkit draws, so a missing glyph
// reads as missing rather than as a space.
static struct gent *box(int px, int base, int cell_h) {
    int adv = px * 3 / 5;
    if (adv < 4) adv = 4;
    struct gent *e = calloc(1, sizeof *e + (size_t)adv * (size_t)cell_h);
    if (!e) return 0;
    e->w = e->adv = adv;
    int bottom = base < cell_h ? base : cell_h;   // the row after the box
    int top = bottom - px * 7 / 10;
    if (top < 0) top = 0;
    for (int y = top; y < bottom; y++)
        for (int x = 1; x < adv - 1; x++)
            if (y == top || y == bottom - 1 || x == 1 || x == adv - 2)
                e->cov[y * adv + x] = 255;
    return e;
}

// Tries the fallback list's faces whose emoji flag is `emoji`, in order;
// the first with the character draws it.
static struct gent *from_list(uint32_t cp, int emoji, int skip, int px, int base,
                              int cell_h, int smear) {
    const struct ufont_fallback *fb = ufont_fallback();
    for (int i = 0; i < fb->n; i++) {
        if (fb->faces[i].emoji != emoji) continue;
        int fi = face_for(fb->faces[i].path);
        if (fi < 0 || fi == skip) continue;
        struct gent *e = render(fi, cp, px, base, cell_h, smear);
        if (e) return e;
    }
    return 0;
}

// The font's cache key parts; -1 face when it names none.
static void key_for(const struct ugfx_font *f, struct gkey *k) {
    k->cell_h = f->char_h;
    k->px = f->src_px > 0 ? f->src_px : f->line_h * 3 / 4;
    k->base = f->src_baseline > 0 ? f->src_baseline : f->line_h * 4 / 5;
    if (k->px < 4) k->px = 4;
    if (k->base >= k->cell_h) k->base = k->cell_h - 1;
    k->smear = f->src_smear;
    k->face = face_for(f->src_path);
    k->emoji = 0;
    k->mark = 0;
}

static int found(struct gent *e, const unsigned char **cov, int *w, int *h, int *adv, int *color) {
    if (e->none) return 0;
    *cov = e->cov; *w = e->w; *h = e->cell_h; *adv = e->adv; *color = e->color;
    return 1;
}

// Files `e` under `k` and answers from it.
static int keep(struct gent *e, const struct gkey *k, unsigned hv,
                const unsigned char **cov, int *w, int *h, int *adv, int *color) {
    e->cp = k->cp; e->face = k->face; e->px = k->px; e->base = k->base;
    e->cell_h = k->cell_h; e->smear = k->smear; e->emoji = k->emoji; e->mark = k->mark;
    while (ulru_oldest(&g_lru) && g_bytes + gent_bytes(e) > CACHE_BUDGET) evict_one();
    if (!ulru_insert(&g_lru, &e->node, hv)) { free(e); return 0; }
    g_bytes += gent_bytes(e);
    return found(e, cov, w, h, adv, color);
}

int ugfx_glyph_get(const struct ugfx_font *f, uint32_t cp, int emoji,
                   const unsigned char **cov, int *w, int *h, int *adv, int *color) {
    if (!f || f->char_h <= 0) return 0;
    struct gkey k;
    key_for(f, &k);
    k.cp = cp;
    k.emoji = emoji ? 1 : 0;
    int fi = k.face, px = k.px, base = k.base, cell_h = k.cell_h, smear = k.smear;
    unsigned hv = hash(&k);
    struct gent *hit = (struct gent *)ulru_find(&g_lru, hv, gent_is, &k);
    if (hit) return found(hit, cov, w, h, adv, color);

    // The chain: an emoji's emoji face first; the font's own face; the
    // fallback list's text faces in order; an emoji face last for a
    // character only it has; the box. Cached under the PRIMARY key either
    // way, so a character nothing has costs one lookup after the first.
    struct gent *e = emoji ? from_list(cp, 1, fi, px, base, cell_h, smear) : 0;
    if (!e && fi >= 0) e = render(fi, cp, px, base, cell_h, smear);
    if (!e) e = from_list(cp, 0, fi, px, base, cell_h, smear);
    if (!e && !emoji) e = from_list(cp, 1, fi, px, base, cell_h, smear);
    if (!e) e = box(px, base, cell_h);
    if (!e) return 0;
    return keep(e, &k, hv, cov, w, h, adv, color);
}

int ugfx_glyph_cluster(const struct ugfx_font *f, uint32_t token,
                       const unsigned char **cov, int *w, int *h, int *adv, int *color) {
    const uint32_t *cps;
    int n = ugfx_cluster_cps(token, &cps);
    if (!f || f->char_h <= 0 || n < 2) return 0;
    struct gkey k;
    key_for(f, &k);
    k.cp = token;
    k.emoji = 2;
    unsigned hv = hash(&k);
    struct gent *hit = (struct gent *)ulru_find(&g_lru, hv, gent_is, &k);
    if (hit) return found(hit, cov, w, h, adv, color);

    // The EMOJI faces' own ligatures (GSUB `ccmp`): a ZWJ sequence, a flag,
    // a skin tone or a keycap is one glyph there, every part mapped --
    // selectors and joiners included, as the face lists them.
    struct gent *e = 0;
    uint16_t *g = malloc((size_t)n * sizeof *g);
    const struct ufont_fallback *fb = ufont_fallback();
    for (int i = 0; g && i < fb->n && !e; i++) {
        if (!fb->faces[i].emoji) continue;
        int fi = face_for(fb->faces[i].path);
        if (fi < 0) continue;
        int all = 1;
        for (int j = 0; j < n && all; j++) all = (g[j] = (uint16_t)ttf_glyph_index(&g_faces[fi]->t, cps[j])) != 0;
        if (all && ttf_ligate(&g_faces[fi]->t, TTF_SCRIPT_OTHER, TTF_FEATURE('c', 'c', 'm', 'p'), g, n) == 1)
            e = render_gid(fi, g[0], k.px, k.base, k.cell_h, 0);
    }
    free(g);
    if (!e && !(e = calloc(1, sizeof *e))) return 0;
    if (!e->w) e->none = 1;       // remembered: the parts are drawn, every time without a search
    return keep(e, &k, hv, cov, w, h, adv, color);
}

int ugfx_glyph_mark(const struct ugfx_font *f, uint32_t cp, int over,
                    const unsigned char **cov, int *w, int *h, int *adv, int *color) {
    if (!f || f->char_h <= 0 || over < 0) return 0;
    struct gkey k;
    key_for(f, &k);
    k.cp = cp;
    k.mark = over + 1;
    unsigned hv = hash(&k);
    struct gent *hit = (struct gent *)ulru_find(&g_lru, hv, gent_is, &k);
    if (hit) return found(hit, cov, w, h, adv, color);

    // Rendered with its origin mid-cell, so ink on either side of it lands
    // (a mark's outline sits left of its origin in one face and centred on
    // it in another), then moved so the ink's middle is the letter's.
    int fi = k.face >= 0 && ttf_glyph_index(&g_faces[k.face]->t, cp) ? k.face : -1;
    const struct ufont_fallback *fb = ufont_fallback();
    for (int i = 0; i < fb->n && fi < 0; i++) {
        int c = fb->faces[i].emoji ? -1 : face_for(fb->faces[i].path);
        if (c >= 0 && ttf_glyph_index(&g_faces[c]->t, cp)) fi = c;
    }
    if (fi < 0 || !(g_scratch || (g_scratch = malloc(sizeof *g_scratch)))) return 0;
    int gid = ttf_glyph_index(&g_faces[fi]->t, cp);
    int cw = 3 * k.px, ox = cw / 2;
    if (cw > TTF_MAX_CELL_W) { cw = TTF_MAX_CELL_W; ox = cw / 2; }
    unsigned char *cell = calloc((size_t)cw * (size_t)k.cell_h, 1);
    if (!cell) return 0;
    ttf_render_glyph(&g_faces[fi]->t, gid, k.px, cell, cw, k.cell_h, ox, k.base, g_scratch);
    int l = cw, r = -1;
    for (int y = 0; y < k.cell_h; y++)
        for (int x = 0; x < cw; x++)
            if (cell[y * cw + x]) { if (x < l) l = x; if (x > r) r = x; }
    int ink = r >= l ? r - l + 1 : 0;
    int ew = over > ink ? over : (ink ? ink : 1);
    struct gent *e = calloc(1, sizeof *e + (size_t)ew * (size_t)k.cell_h);
    if (!e) { free(cell); return 0; }
    int shift = (ew - ink) / 2 - l;          // the ink's left edge to its centred column
    for (int y = 0; y < k.cell_h && ink; y++)
        for (int x = l; x <= r; x++) e->cov[y * ew + x + shift] = cell[y * cw + x];
    free(cell);
    e->w = ew;
    e->adv = 0;
    return keep(e, &k, hv, cov, w, h, adv, color);
}

// A small direct-mapped memo: kerning is asked for every pair drawn.
// Keyed by the font AND its size: a private font reloads at another size
// in the same struct.
static struct { const struct ugfx_font *f; uint32_t l, r; int size, px; } g_kern[256];

int ugfx_glyph_kern(const struct ugfx_font *f, uint32_t l, uint32_t r) {
    if (!f || !l || !r) return 0;
    unsigned slot = ((l * 31u) ^ (r * 2654435761u)) & 255u;
    int size = f->src_px > 0 ? f->src_px : f->line_h * 3 / 4;
    if (g_kern[slot].f == f && g_kern[slot].l == l && g_kern[slot].r == r && g_kern[slot].size == size)
        return g_kern[slot].px;
    int fi = face_for(f->src_path), px = 0;
    if (fi >= 0) {
        struct ttf_font *t = &g_faces[fi]->t;
        int gl = ttf_glyph_index(t, l), gr = ttf_glyph_index(t, r);
        int units = gl && gr ? ttf_pair_kern_units(t, ttf_script_for(r), gl, gr) : 0;
        if (units) px = fx_round(fx_mul(fx_from_int(units), ttf_scale_for_px(t, size)));
    }
    g_kern[slot].f = f; g_kern[slot].l = l; g_kern[slot].r = r; g_kern[slot].size = size; g_kern[slot].px = px;
    return px;
}
