// NetSurf's bitmaps, as toy-os pixels (ns_bitmap.h).
//
// **THE FORMAT IS DECLARED, NOT CONVERTED.** bitmap_set_format() tells
// NetSurf's decoders to write 0xAARRGGBB words, straight alpha -- what
// ugfx's blits and lib/uimg.h use -- so a decoded image is drawn without
// a pass over its pixels.
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "browser/ns_bitmap.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"

static void *bm_create(int w, int h, enum gui_bitmap_flags flags) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    struct bitmap *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->w = w;
    b->h = h;
    b->opaque = (flags & BITMAP_OPAQUE) != 0;
    size_t n = (size_t)w * (size_t)h * sizeof(uint32_t);
    b->px = (flags & BITMAP_CLEAR) ? calloc(1, n) : malloc(n);
    if (!b->px) { free(b); return NULL; }
    return b;
}

static void drop_scaled(struct bitmap *b) {
    free(b->scaled);
    b->scaled = NULL;
    b->sw = b->sh = 0;
}

static void bm_destroy(void *v) {
    struct bitmap *b = v;
    if (!b) return;
    drop_scaled(b);
    free(b->px);
    free(b);
}

static void bm_set_opaque(void *v, bool opaque) { ((struct bitmap *)v)->opaque = opaque; }
static bool bm_get_opaque(void *v) { return ((struct bitmap *)v)->opaque; }
static unsigned char *bm_get_buffer(void *v) { return (unsigned char *)((struct bitmap *)v)->px; }
static size_t bm_get_rowstride(void *v) { return (size_t)((struct bitmap *)v)->w * 4; }
static int bm_get_width(void *v) { return ((struct bitmap *)v)->w; }
static int bm_get_height(void *v) { return ((struct bitmap *)v)->h; }
// The pixels changed under a cached scaled copy, which is now stale.
static void bm_modified(void *v) { drop_scaled(v); }

// Thumbnails of a page (history, the tab hover) are a later stage.
static nserror bm_render(struct bitmap *b, struct hlcache_handle *content) {
    (void)b; (void)content;
    return NSERROR_NOT_IMPLEMENTED;
}

static struct gui_bitmap_table g_table = {
    .create = bm_create,
    .destroy = bm_destroy,
    .set_opaque = bm_set_opaque,
    .get_opaque = bm_get_opaque,
    .get_buffer = bm_get_buffer,
    .get_rowstride = bm_get_rowstride,
    .get_width = bm_get_width,
    .get_height = bm_get_height,
    .modified = bm_modified,
    .render = bm_render,
};
struct gui_bitmap_table *ns_bitmap_table = &g_table;

void ns_bitmap_set_format(void) {
    bitmap_fmt_t fmt = { .layout = BITMAP_LAYOUT_ARGB8888, .pma = false };
    bitmap_set_format(&fmt);
}

const uint32_t *ns_bitmap_at_size(struct bitmap *b, int w, int h) {
    if (w == b->w && h == b->h) return b->px;
    if (b->scaled && b->sw == w && b->sh == h) return b->scaled;
    drop_scaled(b);
    struct uimg src = { .w = b->w, .h = b->h, .px = b->px, .has_alpha = !b->opaque };
    struct uimg out;
    if (uimg_scale(&src, w, h, &out) != 0) return NULL;
    b->scaled = out.px;
    b->sw = w;
    b->sh = h;
    return b->scaled;
}
