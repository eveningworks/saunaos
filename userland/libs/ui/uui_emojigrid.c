// A grid of emoji (ui/uui_emojigrid.h): a uui_grid whose cell is a colour glyph.
#include "ui/uui_emojigrid.h"
#include "base/uemojidata.h"
#include "base/ufontfallback.h"
#include "base/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "keyboard.h"

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

int uui_emojigrid_draw_toned(struct ugfx_surface *s, struct uglyph_face *face, int entry, int tone,
                             int px, int x, int y, int w, int h) {
    const struct uemojidata *d = uemojidata();
    if (entry < 0 || entry >= d->count) return 0;
    if (!face) face = uui_emojigrid_face();
    if (!face->ok) return 0;
    char seq[128];
    if (!uemojidata_toned(&d->entries[entry], tone, seq, sizeof seq)) return 0;
    // ONE GLYPH, a sequence included: the face's ligature (uglyph.h).
    return uglyph_draw_cluster(s, face, seq, px, x, y, w, h, UTHEME_TEXT) == 1;
}

int uui_emojigrid_draw_entry(struct ugfx_surface *s, struct uglyph_face *face, int entry, int px,
                             int x, int y, int w, int h) {
    return uui_emojigrid_draw_toned(s, face, entry, 0, px, x, y, w, h);
}

static void cell(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)state;   // a colour glyph keeps its colours on the selection too
    struct uui_emojigrid *g = ctx;
    if (index < 0 || index >= g->count) return;
    uui_emojigrid_draw_entry(s, g->face, g->items[index], g->px, x, y, w, h);
}

void uui_emojigrid_init(struct uui_emojigrid *g, int cell_px, int px) {
    memset(g, 0, sizeof *g);
    g->var_entry = g->took_entry = -1;
    uui_grid_init(&g->grid, cell_px, cell_px);
    g->px = px;
    uui_grid_set(&g->grid, 0, cell, g);
}

void uui_emojigrid_set(struct uui_emojigrid *g, const int *items, int count) {
    g->var_entry = -1;
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

int uui_emojigrid_take_seq(struct uui_emojigrid *g, char *out, int cap) {
    int e = g->took_entry, tone = g->took_tone;
    g->took_entry = -1;
    if (e < 0) e = uui_emojigrid_take(g), tone = 0;
    if (e < 0) return -1;
    if (out && cap > 0 && !uemojidata_toned(&uemojidata()->entries[e], tone, out, cap)) out[0] = '\0';
    return e;
}

int uui_emojigrid_index_at(const struct uui_emojigrid *g, int cx, int cy) {
    for (int i = 0; i < g->count; i++) {
        int x, y, w, h;
        if (uui_grid_cell_rect(&g->grid, i, &x, &y, &w, &h) && cx >= x && cx < x + w && cy >= y && cy < y + h)
            return i;
    }
    return -1;
}

int uui_emojigrid_variants(struct uui_emojigrid *g, int index) {
    if (index < 0) index = g->grid.selected;
    if (index < 0 || index >= g->count) return 0;
    int e = g->items[index];
    if (!uemojidata()->entries[e].toned) return 0;
    uui_grid_select(&g->grid, index);
    g->var_entry = e;
    g->var_index = index;
    g->var_sel = 0;
    return 1;
}

int  uui_emojigrid_variants_open(const struct uui_emojigrid *g) { return g->var_entry >= 0; }
void uui_emojigrid_variants_close(struct uui_emojigrid *g) { g->var_entry = -1; }

// The strip: six of the cell's own size in a card, above the cell when it
// fits inside the grid, else below; slid sideways to stay inside it.
static int strip_rect(const struct uui_emojigrid *g, int *sx, int *sy, int *cw, int *ch) {
    int x, y, w, h;
    if (g->var_entry < 0 || !uui_grid_cell_rect(&g->grid, g->var_index, &x, &y, &w, &h)) return 0;
    int pad = ugfx_px(4), sw = 6 * w + 2 * pad, sh = h + 2 * pad;
    int left = x - pad;
    if (left + sw > g->grid.x + g->grid.w) left = g->grid.x + g->grid.w - sw;
    if (left < g->grid.x) left = g->grid.x;
    int top = y - sh >= g->grid.y ? y - sh : y + h;
    *sx = left; *sy = top; *cw = w; *ch = h;
    return 1;
}

static int strip_at(const struct uui_emojigrid *g, int cx, int cy) {
    int sx, sy, cw, chh;
    if (!strip_rect(g, &sx, &sy, &cw, &chh)) return -1;
    int pad = ugfx_px(4);
    if (cy < sy + pad || cy >= sy + pad + chh || cx < sx + pad || cx >= sx + pad + 6 * cw) return -1;
    return (cx - sx - pad) / cw;
}

static void take_variant(struct uui_emojigrid *g) {
    g->took_entry = g->var_entry;
    g->took_tone = g->var_sel;
    g->var_entry = -1;
}

// --- ops: the grid's, through the embedded struct ---------------------------

static void eg_natural(const void *w, int *ow, int *oh) { uui_grid_ops.natural_size(w, ow, oh); }
static void eg_geometry(void *w, int x, int y, int ww, int hh) { uui_grid_ops.set_geometry(w, x, y, ww, hh); }
static void eg_bounds(const void *w, int *x, int *y, int *ow, int *oh) { uui_grid_ops.bounds(w, x, y, ow, oh); }

static void eg_draw(struct ugfx_surface *s, const void *w) {
    uui_grid_ops.draw(s, w);
    const struct uui_emojigrid *g = w;
    int sx, sy, cw, ch;
    if (!strip_rect(g, &sx, &sy, &cw, &ch)) return;
    int pad = ugfx_px(4);
    uui_control_frame(s, sx, sy, 6 * cw + 2 * pad, ch + 2 * pad, UTHEME_WHITE, UTHEME_BORDER);
    for (int t = 0; t < 6; t++) {
        int x = sx + pad + t * cw, y = sy + pad;
        if (t == g->var_sel) uui_fill_round_rect(s, x, y, cw, ch, ugfx_px(4), UTHEME_SELECTION);
        uui_emojigrid_draw_toned(s, g->face, g->var_entry, t, g->px, x, y, cw, ch);
    }
}

static int eg_hit(const void *w, int cx, int cy) { return uui_grid_ops.hit(w, cx, cy); }

static int eg_key(void *w, int key, unsigned mods) {
    struct uui_emojigrid *g = w;
    if (g->var_entry >= 0) {
        switch (key) {
        case KEY_ARROW_LEFT: if (g->var_sel > 0) g->var_sel--; return 1;
        case KEY_ARROW_RIGHT: if (g->var_sel < 5) g->var_sel++; return 1;
        case KEY_HOME: g->var_sel = 0; return 1;
        case KEY_END: g->var_sel = 5; return 1;
        case '\n': case '\r': case ' ': take_variant(g); return 1;
        case 0x1B: g->var_entry = -1; return 1;
        default: g->var_entry = -1; break;     // anything else shuts it and goes on
        }
    } else if (key == KEY_MENU || (key == KEY_F10 && (mods & KEY_MOD_SHIFT))) {
        return uui_emojigrid_variants(g, -1);
    }
    return uui_grid_ops.key(w, key, mods);
}

static void eg_focused(void *w, int f) {
    struct uui_emojigrid *g = w;
    if (!f) g->var_entry = -1;
    uui_grid_ops.set_focused(w, f);
}

static int  eg_accepts(const void *w) { return uui_grid_ops.accepts_focus(w); }

static int eg_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_emojigrid *g = w;
    if (g->var_entry >= 0) {
        int t = strip_at(g, cx, cy);
        if (t >= 0) { g->var_sel = t; take_variant(g); }
        else g->var_entry = -1;                // a click elsewhere only shuts it
        return 1;
    }
    return uui_grid_ops.press(w, cx, cy, mods);
}

static int eg_motion(void *w, int cx, int cy, unsigned b) {
    struct uui_emojigrid *g = w;
    if (g->var_entry >= 0) {
        int t = strip_at(g, cx, cy);
        if (t >= 0 && t != g->var_sel) { g->var_sel = t; return 1; }
        return 0;
    }
    return uui_grid_ops.motion(w, cx, cy, b);
}

static int  eg_release(void *w, int cx, int cy) { return uui_grid_ops.release(w, cx, cy); }
static int  eg_wheel(void *w, int notches) {
    ((struct uui_emojigrid *)w)->var_entry = -1;
    return uui_grid_ops.wheel(w, notches);
}

static void eg_describe(const void *w, const struct uui_describe *d) {
    uui_grid_ops.describe(w, d);
    const struct uui_emojigrid *g = w;
    uui_describe_int(d, "count", g->count);
    uui_describe_int(d, "entry", uui_emojigrid_focused(g));
    uui_describe_int(d, "variants", g->var_entry >= 0);
    uui_describe_int(d, "variant", g->var_sel);
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
