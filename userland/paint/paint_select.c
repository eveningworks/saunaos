// Paint's selection: a rectangle dragged, a lasso drawn, or a magic wand's
// pick (L1 from the mockups), combined New / Add / Subtract as
// Paint.NET's are -- Shift adds and Alt subtracts whatever the bar says.
// What is selected can be moved by dragging it (lifted into a float, as
// a paste), cut, copied, deleted, cropped to, inverted; and while one is
// there, every tool paints inside it only. One of Paint's units -- see
// paint.h.
//
// THE MASK IS lib/upixel.h's: the picture's size, a byte a pixel. It
// belongs to the picture, not a layer, and goes when the picture's size
// changes. Changing it is not an undo step (MS Paint's, KolourPaint's).
//
// DRAWN EACH FRAME FROM THE MASK, only over the selection's box where
// the view shows it: a light accent wash inside, and marching ants
// wherever a screen pixel's neighbour differs -- so the ants sit on the
// edge at every zoom.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "keyboard.h"
#include "lib/uclip.h"

int g_sel_kind = SEL_RECT;
int g_sel_mode = UPIX_SEL_NEW;
int g_sel_tol = 32;

enum drag { D_NONE, D_RECT, D_LASSO, D_PRESSED_IN };

static struct upix_sel g_sel;
static enum drag g_drag;
static enum upix_sel_op g_op;              // this gesture's: the bar's, or Shift's / Alt's
static int g_ax, g_ay, g_bx, g_by;         // a rectangle's corners, or where a press inside landed
static struct upix_pt *g_pts;              // a lasso being drawn
static int g_npts, g_cap;
static uint8_t *g_rowa, *g_rowb;           // the overlay's mask rows, screen columns
static int *g_cols;
static int g_row_cap;

// The mask, at the picture's size: a picture of another size (a new one,
// a resize, a rotate) starts with nothing selected.
static struct upix_sel *mask(void) {
    if (!g_layer || !g_layer->px) return 0;
    if (!g_sel.a || g_sel.w != g_layer->w || g_sel.h != g_layer->h) {
        upix_sel_free(&g_sel);
        if (upix_sel_init(&g_sel, g_layer->w, g_layer->h) != 0) return 0;
    }
    return &g_sel;
}

int select_any(void) {
    return g_sel.a && g_layer && g_sel.w == g_layer->w && g_sel.h == g_layer->h && upix_sel_any(&g_sel);
}

const struct upix_sel *select_get(void) { return select_any() ? &g_sel : 0; }

static void changed(void) {
    struct upix_sel *s = &g_sel;
    ulogf("paint: selection %d,%d %dx%d\n", s->box.x, s->box.y, s->box.w, s->box.h);
    paint_text_changed();   // a redraw
}

void select_clip(const struct upix_rect *r) {
    if (!select_any() || !r) return;
    upix_sel_keep_outside(&g_sel, g_layer, &g_undo.snap, r);
}

void select_all(void) { struct upix_sel *s = mask(); if (s) { upix_sel_all(s); changed(); } }
void select_none(void) { if (g_sel.a) { upix_sel_clear(&g_sel); changed(); } }
void select_invert(void) { struct upix_sel *s = mask(); if (s) { upix_sel_invert(s); changed(); } }

// What a cleared pixel becomes: Colour 2 on the bottom layer, nothing on
// the ones above -- the eraser's rule.
static uint32_t room(void) { return layers_active() > 0 ? 0 : g_colour[1]; }

// A text layer is its words: ask to make it pixels before taking any.
static int on_pixels(void) {
    if (!layers_text(layers_active())) return 1;
    paint_ask_rasterize();
    return 0;
}

static int clear_selected(void) {
    if (upixundo_begin(&g_undo, g_layer) != 0) { paint_note("Not enough memory"); return 0; }
    struct upix_rect r = { 0, 0, 0, 0 };
    upix_sel_paint(&g_sel, g_layer, room(), &r);
    upixundo_commit(&g_undo, g_layer, &r);
    paint_changed(&r);
    return 1;
}

void select_delete(void) {
    if (!select_any() || !on_pixels()) return;
    clear_selected();
}

int select_copy(int cut) {
    if (!select_any()) return 0;
    if (cut && !on_pixels()) return 1;
    // WHAT SHOWS, every layer together, as Copy picture does; a cut then
    // clears it from the current layer.
    struct uimg out;
    if (upix_sel_copy(&g_sel, layers_view(), &out) != 0) { paint_note("Not enough memory to copy"); return 1; }
    int ok = uclip_set_image(&out);
    uimg_free(&out);
    if (!ok) { paint_note("The clipboard refused the selection"); return 1; }
    if (cut) clear_selected();
    paint_note(cut ? "Cut the selection" : "Copied the selection");
    return 1;
}

struct crop { int x, y, w, h; };

static int crop_one(const struct uimg *in, struct uimg *out, int index, void *ctx) {
    (void)index;
    const struct crop *c = ctx;
    out->px = malloc((size_t)c->w * c->h * 4);
    if (!out->px) return -1;
    out->w = c->w;
    out->h = c->h;
    for (int y = 0; y < c->h; y++)
        memcpy(out->px + (size_t)y * c->w, in->px + (size_t)(c->y + y) * in->w + c->x, (size_t)c->w * 4);
    return 0;
}

void select_crop(void) {
    if (!select_any()) return;
    struct crop c = { g_sel.box.x, g_sel.box.y, g_sel.box.w, g_sel.box.h };
    if (!layers_transform(crop_one, &c, "crop")) { paint_note("Not enough memory to crop"); return; }
    ulogf("paint: cropped to %d,%d %dx%d\n", c.x, c.y, c.w, c.h);
    upix_sel_free(&g_sel);   // the picture's size changed under it
    changed();
}

// LIFTED TO MOVE: the selected pixels float as a paste, the place they
// left cleared; the selection itself goes with them.
static void lift(int x, int y) {
    if (!on_pixels()) return;
    struct uimg im;
    if (upix_sel_copy(&g_sel, g_layer, &im) != 0) { paint_note("Not enough memory to move it"); return; }
    int bx = g_sel.box.x, by = g_sel.box.y;
    if (!clear_selected()) { uimg_free(&im); return; }
    upix_sel_clear(&g_sel);
    paste_float(&im, bx, by);
    // The press that began it, then where the pointer is now: the float's
    // frame takes the drag from here.
    paste_pointer(UUI_PAINT_DOWN, g_ax, g_ay, 0);
    paste_pointer(UUI_PAINT_MOVE, x, y, 0);
}

static void lasso_add(int x, int y) {
    if (g_npts && (int)g_pts[g_npts - 1].x == x && (int)g_pts[g_npts - 1].y == y) return;
    if (g_npts == g_cap) {
        int cap = g_cap ? g_cap * 2 : 256;
        struct upix_pt *p = realloc(g_pts, sizeof *p * (size_t)cap);
        if (!p) return;
        g_pts = p;
        g_cap = cap;
    }
    g_pts[g_npts++] = (struct upix_pt){ (float)x + 0.5f, (float)y + 0.5f };
}

void select_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods) {
    struct upix_sel *s = mask();
    if (!s) return;
    if (phase == UUI_PAINT_HOVER) {
        g_canvas.cursor = select_any() && upix_sel_at(s, x, y) && !(mods & (KEY_MOD_SHIFT | KEY_MOD_ALT))
                              ? WIN_CURSOR_MOVE : WIN_CURSOR_CROSSHAIR;
        return;
    }
    if (phase == UUI_PAINT_DOWN) {
        g_op = mods & KEY_MOD_SHIFT ? UPIX_SEL_ADD : mods & KEY_MOD_ALT ? UPIX_SEL_SUB : (enum upix_sel_op)g_sel_mode;
        g_ax = g_bx = x;
        g_ay = g_by = y;
        // INSIDE WHAT IS SELECTED, with nothing held: a drag moves it, and
        // a click without one starts afresh (below, on the release).
        if (select_any() && upix_sel_at(s, x, y) && !(mods & (KEY_MOD_SHIFT | KEY_MOD_ALT))) {
            g_drag = D_PRESSED_IN;
            return;
        }
        if (g_sel_kind == SEL_WAND) {
            int n = upix_sel_wand(s, g_layer, x, y, g_sel_tol * 255 / 100, g_op);
            ulogf("paint: wand at %d,%d tolerance %d%% -> %d pixels\n", x, y, g_sel_tol, n);
            changed();
            return;
        }
        g_drag = g_sel_kind == SEL_LASSO ? D_LASSO : D_RECT;
        g_npts = 0;
        if (g_drag == D_LASSO) lasso_add(x, y);
        paint_text_changed();
        return;
    }
    if (phase == UUI_PAINT_MOVE) {
        if (g_drag == D_PRESSED_IN) {
            if (abs(x - g_ax) + abs(y - g_ay) >= 2) { g_drag = D_NONE; lift(x, y); }
            return;
        }
        if (g_drag == D_RECT) { g_bx = x; g_by = y; paint_text_changed(); }
        else if (g_drag == D_LASSO) { lasso_add(x, y); paint_text_changed(); }
        return;
    }
    // UP.
    enum drag d = g_drag;
    g_drag = D_NONE;
    if (d == D_PRESSED_IN) {
        // A CLICK INSIDE: the wand picks again; the others let it go.
        if (g_sel_kind == SEL_WAND) upix_sel_wand(s, g_layer, x, y, g_sel_tol * 255 / 100, UPIX_SEL_NEW);
        else upix_sel_clear(s);
        changed();
    } else if (d == D_RECT) {
        g_bx = x;
        g_by = y;
        // A click is not a rectangle: with New it lets the selection go.
        if (g_ax == g_bx || g_ay == g_by) { if (g_op == UPIX_SEL_NEW) upix_sel_clear(s); }
        else upix_sel_rect(s, g_ax, g_ay, g_bx, g_by, g_op);
        changed();
    } else if (d == D_LASSO) {
        lasso_add(x, y);
        if (g_npts >= 3) upix_sel_poly(s, g_pts, g_npts, g_op);
        else if (g_op == UPIX_SEL_NEW) upix_sel_clear(s);
        g_npts = 0;
        changed();
    }
}

int select_key(int key, unsigned mods) {
    (void)mods;
    if (g_drag != D_NONE && key == 0x1B) { g_drag = D_NONE; g_npts = 0; paint_text_changed(); return 1; }
    if (!select_any()) return 0;
    if (key == 0x1B) { select_none(); return 1; }
    if (key == KEY_DELETE) { select_delete(); return 1; }
    return 0;
}

// --- drawing -----------------------------------------------------------------

static int grow(int n) {
    if (n <= g_row_cap) return 1;
    uint8_t *a = realloc(g_rowa, (size_t)n), *b = 0;
    if (a) g_rowa = a;
    if (a) b = realloc(g_rowb, (size_t)n);
    if (b) g_rowb = b;
    int *c = b ? realloc(g_cols, sizeof *c * (size_t)n) : 0;
    if (!c) return 0;
    g_cols = c;
    g_row_cap = n;
    return 1;
}

// The mask under screen row `r`, one byte a column from c0 (the table
// maps columns to picture x; -1 is outside the picture).
static void mask_row(const struct uui_paintcanvas *c, int r, int oy, int c0, int n, uint8_t *out) {
    int py = (int)(((long long)(r - oy) * 100) / c->zoom);
    if (r < oy || py < 0 || py >= g_sel.h) { memset(out, 0, (size_t)n); return; }
    const uint8_t *row = g_sel.a + (size_t)py * g_sel.w;
    for (int i = 0; i < n; i++) out[i] = g_cols[i] >= 0 && row[g_cols[i]] ? 1 : 0;
    (void)c0;
}

static void draw_mask(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    int ox, oy;
    uui_paintcanvas_to_screen(c, 0, 0, &ox, &oy);
    // The box on screen, one pixel bigger all round for the ants outside it.
    int x0, y0, x1, y1;
    uui_paintcanvas_to_screen(c, g_sel.box.x, g_sel.box.y, &x0, &y0);
    uui_paintcanvas_to_screen(c, g_sel.box.x + g_sel.box.w, g_sel.box.y + g_sel.box.h, &x1, &y1);
    x0--; y0--; x1++; y1++;
    if (x0 < c->x) x0 = c->x;
    if (y0 < c->y) y0 = c->y;
    if (x1 > c->x + c->w) x1 = c->x + c->w;
    if (y1 > c->y + c->h) y1 = c->y + c->h;
    int n = x1 - x0 + 1;
    if (x1 <= x0 || y1 <= y0 || !grow(n)) return;
    for (int i = 0; i < n; i++) {
        int sx = x0 + i;
        int px = sx < ox ? -1 : (int)(((long long)(sx - ox) * 100) / c->zoom);
        g_cols[i] = px >= 0 && px < g_sel.w ? px : -1;
    }
    uint32_t wash = UTHEME_ACCENT;
    mask_row(c, y0, oy, x0, n, g_rowa);
    for (int r = y0; r < y1; r++) {
        mask_row(c, r + 1, oy, x0, n, g_rowb);
        // The wash: runs of selected pixels in this row.
        for (int i = 0; i < n - 1;) {
            if (!g_rowa[i]) { i++; continue; }
            int j = i;
            while (j < n - 1 && g_rowa[j]) j++;
            ugfx_blend_hspan(s, x0 + i, r, j - i, wash, 0, 40);
            i = j;
        }
        // The ants: where this pixel's mask differs from the next one's,
        // across or down -- black and white in fours, Paint.NET's.
        for (int i = 0; i < n - 1; i++) {
            if (g_rowa[i] == g_rowa[i + 1] && g_rowa[i] == g_rowb[i]) continue;
            int sx = x0 + i;
            ugfx_fill_rect(s, sx, r, 1, 1, ((sx + r) >> 2) & 1 ? 0x141414 : 0xFFFFFF);
        }
        uint8_t *t = g_rowa; g_rowa = g_rowb; g_rowb = t;
    }
}

static void ant_line(struct ugfx_surface *s, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        ugfx_fill_rect(s, x0, y0, 1, 1, ((x0 + y0) >> 2) & 1 ? 0x141414 : 0xFFFFFF);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void select_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    if (select_any()) draw_mask(s, c);
    if (g_drag == D_RECT) {
        int ax, ay, bx, by;
        uui_paintcanvas_to_screen(c, g_ax < g_bx ? g_ax : g_bx, g_ay < g_by ? g_ay : g_by, &ax, &ay);
        uui_paintcanvas_to_screen(c, g_ax > g_bx ? g_ax : g_bx, g_ay > g_by ? g_ay : g_by, &bx, &by);
        ant_line(s, ax, ay, bx, ay); ant_line(s, bx, ay, bx, by);
        ant_line(s, bx, by, ax, by); ant_line(s, ax, by, ax, ay);
    } else if (g_drag == D_LASSO && g_npts > 1) {
        int px, py;
        uui_paintcanvas_to_screen(c, (int)g_pts[0].x, (int)g_pts[0].y, &px, &py);
        for (int i = 1; i < g_npts; i++) {
            int qx, qy;
            uui_paintcanvas_to_screen(c, (int)g_pts[i].x, (int)g_pts[i].y, &qx, &qy);
            ant_line(s, px, py, qx, qy);
            px = qx;
            py = qy;
        }
    }
}

void select_log_layout(const struct uui_paintcanvas *c) {
    (void)c;
    uapp_logf_layout("paint: layout selection any %d box %d %d %d %d kind %d mode %d tol %d\n", select_any(),
                     g_sel.box.x, g_sel.box.y, g_sel.box.w, g_sel.box.h, g_sel_kind, g_sel_mode, g_sel_tol);
}
