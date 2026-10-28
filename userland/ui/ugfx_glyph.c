// The glyph cache: every character the session atlas lacks, rasterized
// on first use and kept per process (docs/utf8-design.md, "Decided
// here", 3 and 4). ugfx.c asks it for anything past U+00FF.
//
// THE SHAPE IS PANGO'S AND DIRECTWRITE'S: glyphs rendered on demand,
// keyed (face, size, character), a fallback face when the chosen one has
// no glyph, an outlined box when nothing does. Split from ugfx.c because
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
// scanned: this is on the path of every label in a Greek desktop, not a
// character map's grid.
#include "ui/ugfx_glyph.h"
#include "lib/ufile.h"
#include "ui/ulog.h"
#include "ttf.h"
#include <stdlib.h>
#include <string.h>

#define FALLBACK_FACE "/usr/share/fonts/dejavu-sans-mono.ttf"
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
    struct gent *hnext;        // bucket chain
    struct gent *newer, *older;   // the LRU list, newest at g_head
    uint32_t cp;
    int face, px, base, cell_h, smear;
    int w, adv;
    unsigned char cov[];       // w * cell_h
};

static struct face **g_faces;
static int g_nfaces, g_capfaces;

static struct gent **g_buckets;
static unsigned g_nbuckets;    // a power of two, or 0 before first use
static int g_count;
static unsigned long g_bytes, g_evicted;
static struct gent *g_head, *g_tail;
static struct ttf_scratch *g_scratch;

void ugfx_glyph_cache_stats(int *entries, unsigned long *bytes,
                            unsigned long *budget, unsigned long *evicted) {
    if (entries) *entries = g_count;
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

static unsigned hash(uint32_t cp, int face, int px, int base, int cell_h, int smear) {
    unsigned h = cp * 2654435761u;
    h ^= (unsigned)(face + 1) * 40503u + (unsigned)px * 97u + (unsigned)base * 131u
       + (unsigned)cell_h * 193u + (unsigned)smear * 7u;
    return h ^ (h >> 15);
}

static void lru_unlink(struct gent *e) {
    if (e->newer) e->newer->older = e->older; else g_head = e->older;
    if (e->older) e->older->newer = e->newer; else g_tail = e->newer;
    e->newer = e->older = 0;
}

static void lru_push(struct gent *e) {
    e->newer = 0;
    e->older = g_head;
    if (g_head) g_head->newer = e;
    g_head = e;
    if (!g_tail) g_tail = e;
}

static size_t gent_bytes(const struct gent *e) {
    return sizeof *e + (size_t)e->w * (size_t)e->cell_h;
}

static void evict_one(void) {
    struct gent *e = g_tail;
    if (!e) return;
    lru_unlink(e);
    unsigned b = hash(e->cp, e->face, e->px, e->base, e->cell_h, e->smear) & (g_nbuckets - 1);
    for (struct gent **pp = &g_buckets[b]; *pp; pp = &(*pp)->hnext)
        if (*pp == e) { *pp = e->hnext; break; }
    g_bytes -= gent_bytes(e);
    g_count--;
    if (!g_evicted++)
        ulogf("ugfx: glyph cache full at %lu bytes; evicting least recently used\n",
              (unsigned long)CACHE_BUDGET);
    free(e);
}

// Grows the table so chains stay short: one entry per bucket on average.
static int buckets_fit(void) {
    if (g_nbuckets && (unsigned)g_count < g_nbuckets) return 1;
    unsigned n = g_nbuckets ? g_nbuckets * 2 : 256;
    struct gent **nb = calloc(n, sizeof *nb);
    if (!nb) return g_nbuckets != 0;   // keep the old table; longer chains, still right
    for (unsigned i = 0; i < g_nbuckets; i++)
        for (struct gent *e = g_buckets[i], *nx; e; e = nx) {
            nx = e->hnext;
            unsigned b = hash(e->cp, e->face, e->px, e->base, e->cell_h, e->smear) & (n - 1);
            e->hnext = nb[b];
            nb[b] = e;
        }
    free(g_buckets);
    g_buckets = nb;
    g_nbuckets = n;
    return 1;
}

// Rasterizes `cp` from face `fi` into a fresh entry, or NULL when the
// face has no glyph for it.
static struct gent *render(int fi, uint32_t cp, int px, int base, int cell_h, int smear) {
    struct face *f = g_faces[fi];
    int gid = ttf_glyph_index(&f->t, cp);
    if (!gid) return 0;
    if (!g_scratch && !(g_scratch = malloc(sizeof *g_scratch))) return 0;
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

int ugfx_glyph_get(const struct ugfx_font *f, uint32_t cp,
                   const unsigned char **cov, int *w, int *h, int *adv) {
    if (!f || f->char_h <= 0) return 0;
    int cell_h = f->char_h;
    int px = f->src_px > 0 ? f->src_px : f->line_h * 3 / 4;
    int base = f->src_baseline > 0 ? f->src_baseline : f->line_h * 4 / 5;
    if (px < 4) px = 4;
    if (base >= cell_h) base = cell_h - 1;
    int smear = f->src_smear;
    int fi = face_for(f->src_path);

    if (!buckets_fit()) return 0;
    unsigned b = hash(cp, fi, px, base, cell_h, smear) & (g_nbuckets - 1);
    for (struct gent *e = g_buckets[b]; e; e = e->hnext)
        if (e->cp == cp && e->face == fi && e->px == px && e->base == base &&
            e->cell_h == cell_h && e->smear == smear) {
            lru_unlink(e);
            lru_push(e);
            *cov = e->cov; *w = e->w; *h = e->cell_h; *adv = e->adv;
            return 1;
        }

    // The chain: the font's own face, DejaVu Sans Mono (the widest
    // coverage shipped), the box. Cached under the PRIMARY key either
    // way, so a character nothing has costs one lookup after the first.
    struct gent *e = fi >= 0 ? render(fi, cp, px, base, cell_h, smear) : 0;
    if (!e) {
        int fb = face_for(FALLBACK_FACE);
        if (fb >= 0 && fb != fi) e = render(fb, cp, px, base, cell_h, smear);
    }
    if (!e) e = box(px, base, cell_h);
    if (!e) return 0;
    e->cp = cp; e->face = fi; e->px = px; e->base = base;
    e->cell_h = cell_h; e->smear = smear;
    while (g_tail && g_bytes + gent_bytes(e) > CACHE_BUDGET) evict_one();
    e->hnext = g_buckets[b];
    g_buckets[b] = e;
    lru_push(e);
    g_bytes += gent_bytes(e);
    g_count++;
    *cov = e->cov; *w = e->w; *h = e->cell_h; *adv = e->adv;
    return 1;
}
