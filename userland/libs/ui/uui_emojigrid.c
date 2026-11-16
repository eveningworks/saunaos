// A grid of emoji (ui/uui_emojigrid.h): a uui_grid whose cell is a colour glyph.
#include "ui/uui_emojigrid.h"
#include "base/uemojidata.h"
#include "base/ufontfallback.h"
#include "base/ulog.h"
#include "ui/utheme.h"

#include <string.h>

struct uglyph_face *uui_emojigrid_face(void) {
    static struct uglyph_face face;
    static int tried;
    if (tried) return &face;
    tried = 1;
    const struct ufont_fallback *fb = ufont_fallback();
    for (int i = 0; i < fb->n; i++)
        if (fb->faces[i].emoji && uglyph_open(&face, fb->faces[i].path)) return &face;
    ulogf("emojigrid: no emoji face in the fallback list\n");
    return &face;
}

int uui_emojigrid_draw_entry(struct ugfx_surface *s, struct uglyph_face *face, int entry, int px,
                             int x, int y, int w, int h) {
    const struct uemojidata *d = uemojidata();
    if (entry < 0 || entry >= d->count) return 0;
    if (!face) face = uui_emojigrid_face();
    if (!face->ok) return 0;
    return uglyph_draw(s, face, d->entries[entry].cp, px, x, y, w, h, UTHEME_TEXT) == 1;
}

static void cell(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)state;   // a colour glyph keeps its colours on the selection too
    struct uui_emojigrid *g = ctx;
    if (index < 0 || index >= g->count) return;
    uui_emojigrid_draw_entry(s, g->face, g->items[index], g->px, x, y, w, h);
}

void uui_emojigrid_init(struct uui_emojigrid *g, int cell_px, int px) {
    memset(g, 0, sizeof *g);
    uui_grid_init(&g->grid, cell_px, cell_px);
    g->px = px;
    uui_grid_set(&g->grid, 0, cell, g);
}

void uui_emojigrid_set(struct uui_emojigrid *g, const int *items, int count) {
    g->items = items;
    g->count = count;
    g->grid.scroll = 0;
    g->grid.selected = -1;
    uui_grid_set(&g->grid, count, cell, g);
    uui_grid_select(&g->grid, count ? 0 : -1);
    g->grid.scroll = 0;   // the first cell is at the top, whatever the grid's height was
}

int uui_emojigrid_focused(const struct uui_emojigrid *g) {
    int i = g->grid.selected;
    return i >= 0 && i < g->count ? g->items[i] : -1;
}

int uui_emojigrid_take(struct uui_emojigrid *g) {
    int i = uui_grid_take(&g->grid);
    return i >= 0 && i < g->count ? g->items[i] : -1;
}

// --- ops: the grid's, through the embedded struct ---------------------------

static void eg_natural(const void *w, int *ow, int *oh) { uui_grid_ops.natural_size(w, ow, oh); }
static void eg_geometry(void *w, int x, int y, int ww, int hh) { uui_grid_ops.set_geometry(w, x, y, ww, hh); }
static void eg_bounds(const void *w, int *x, int *y, int *ow, int *oh) { uui_grid_ops.bounds(w, x, y, ow, oh); }
static void eg_draw(struct ugfx_surface *s, const void *w) { uui_grid_ops.draw(s, w); }
static int  eg_hit(const void *w, int cx, int cy) { return uui_grid_ops.hit(w, cx, cy); }
static int  eg_key(void *w, int key, unsigned mods) { return uui_grid_ops.key(w, key, mods); }
static void eg_focused(void *w, int f) { uui_grid_ops.set_focused(w, f); }
static int  eg_accepts(const void *w) { return uui_grid_ops.accepts_focus(w); }
static int  eg_press(void *w, int cx, int cy, unsigned mods) { return uui_grid_ops.press(w, cx, cy, mods); }
static int  eg_motion(void *w, int cx, int cy, unsigned b) { return uui_grid_ops.motion(w, cx, cy, b); }
static int  eg_release(void *w, int cx, int cy) { return uui_grid_ops.release(w, cx, cy); }
static int  eg_wheel(void *w, int notches) { return uui_grid_ops.wheel(w, notches); }

static void eg_describe(const void *w, const struct uui_describe *d) {
    uui_grid_ops.describe(w, d);
    const struct uui_emojigrid *g = w;
    uui_describe_int(d, "count", g->count);
    uui_describe_int(d, "entry", uui_emojigrid_focused(g));
}

const struct uui_widget_ops uui_emojigrid_ops = {
    .natural_size = eg_natural,
    .set_geometry = eg_geometry,
    .bounds = eg_bounds,
    .draw = eg_draw,
    .hit = eg_hit,
    .key = eg_key,
    .set_focused = eg_focused,
    .accepts_focus = eg_accepts,
    .press = eg_press,
    .motion = eg_motion,
    .release = eg_release,
    .wheel = eg_wheel,
    .describe = eg_describe,
};
