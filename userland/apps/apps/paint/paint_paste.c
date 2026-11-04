// Paint's Paste: the clipboard's picture -- or a copied picture file --
// placed at the view's top-left and FLOATING, with handles, until it is
// let go: Windows 11 Paint's and GIMP's floating paste (P1 from the
// mockups). Its frame moves it, a handle scales it (Shift keeps its
// shape), Enter or a click outside keeps it as one undo step, Esc drops
// it. One of Paint's units -- see paint.h.
//
// IT FLOATS OVER THE CANVAS, NOT IN THE LAYER, until it is placed: each
// frame draws it straight to the screen at the box's place and size
// (nearest neighbour, only what the view shows), and placing it writes it
// into the layer once, filtered -- GIMP's floating selection. Writing the
// layer on every motion (restore, rescale, blend, recomposite) made a big
// picture lag the pointer by seconds.
//
// A STEP BEFORE IT JOINS IT. Into a new layer (Ctrl+Shift+V, Paint.NET's)
// the layer is added first; bigger than the canvas and Expand chosen
// (P2), the canvas grows first. Either is joined to the paste
// (upixundo_join), so one Ctrl+Z takes both, and Esc takes that step back.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "keyboard.h"
#include "lib/uclip.h"
#include "kpath.h"

enum drag { D_NONE, D_MOVE, D_HANDLE };

static struct uimg g_src;            // the picture as pasted
static struct uimg g_scaled;         // ...at the box's size, made when it is placed
static char g_name[64];              // for the question and the note
static int g_new_layer;              // pending: into a new layer
static int g_open;
static int g_before;                 // a step was joined before the paste (a layer, a canvas): Esc undoes it
static int g_x, g_y, g_w, g_h;       // the box, picture pixels
static enum drag g_drag;
static int g_handle;
static float g_gx, g_gy;
static int g_ox, g_oy, g_ow, g_oh;
static uint32_t *g_row;              // one screen row of the overlay
static int *g_cols;                  // the source column for each screen column
static int g_row_cap;

int paste_active(void) { return g_open; }

static void forget(void) {
    uimg_free(&g_src);
    uimg_free(&g_scaled);
}

static void redraw(void) { paint_text_changed(); }

// Floating at the view's top-left (the picture's, when it is all in view).
static void place(void) {
    int vx = 0, vy = 0;
    uui_paintcanvas_to_picture(&g_canvas, g_canvas.x, g_canvas.y, &vx, &vy);
    g_x = vx > 0 ? vx : 0;
    g_y = vy > 0 ? vy : 0;
    if (g_x + g_src.w > g_layer->w) g_x = g_layer->w - g_src.w > 0 ? g_layer->w - g_src.w : 0;
    if (g_y + g_src.h > g_layer->h) g_y = g_layer->h - g_src.h > 0 ? g_layer->h - g_src.h : 0;
    g_w = g_src.w;
    g_h = g_src.h;
    g_open = 1;
    g_drag = D_NONE;
    char note[128];
    snprintf(note, sizeof note, "Pasted %s, %d \xd7 %d -- drag to move, a handle scales it, click away to place",
             g_name, g_src.w, g_src.h);
    paint_note(note);
    ulogf("paint: paste %dx%d at %d,%d new_layer %d\n", g_src.w, g_src.h, g_x, g_y, g_new_layer);
    redraw();
}

// Into a new layer first, when asked -- or when the current one is a text
// layer, whose words a picture would end.
static int prepare_layer(void) {
    if (!g_new_layer && !layers_text(layers_active())) return 1;
    if (layers_count() >= PAINT_LAYERS_MAX) { paint_note("No room for another layer"); return 0; }
    int was = layers_count();
    layers_add();
    if (layers_count() == was) { paint_note("Not enough memory for a layer"); return 0; }
    g_before = 1;
    upixundo_join(&g_undo);
    return 1;
}

static void start(void) {
    g_before = 0;
    if (g_src.w > g_layer->w || g_src.h > g_layer->h) {
        paint_ask_paste(g_name, g_src.w, g_src.h);
        return;
    }
    if (!prepare_layer()) { forget(); return; }
    place();
}

void paste_answer(enum paste_fit fit) {
    if (fit == PASTE_CANCEL || !g_src.px) { forget(); return; }
    g_before = 0;
    if (fit == PASTE_EXPAND) {
        int w = g_src.w > g_layer->w ? g_src.w : g_layer->w, h = g_src.h > g_layer->h ? g_src.h : g_layer->h;
        unsigned st = upixundo_state(&g_undo);
        resize_canvas_to(w, h, 0, 0);
        if (upixundo_state(&g_undo) == st) { forget(); return; }   // it said why
        g_before = 1;
        upixundo_join(&g_undo);
    }
    // The layer's step after the canvas's joins both: the second join
    // ties the layer to the canvas, the paste to the layer.
    if (!prepare_layer()) { if (g_before) paint_undo(); forget(); return; }
    place();
}

static int load(const char *path) {
    forget();
    if (uimg_load(path, &g_src) != 0) return 0;
    snprintf(g_name, sizeof g_name, "%s", k_path_basename(path));
    return 1;
}

void paste_from_file(const char *path, int new_layer) {
    if (g_open) paste_commit();
    g_new_layer = new_layer;
    if (!load(path)) { paint_note("That file is not a picture Paint can open"); return; }
    start();
}

void paste_from_clipboard(int new_layer) {
    static struct uclip c;   // a 64 KiB snapshot: never on the stack (lib/uclip.h)
    if (g_open) paste_commit();
    g_new_layer = new_layer;
    uclip_load(&c);
    const char *img = uclip_image(&c);
    if (img) {
        if (!load(img)) { paint_note("The copied picture could not be read"); return; }
        snprintf(g_name, sizeof g_name, "the copied picture");
        start();
        return;
    }
    if (uclip_op(&c) != UCLIP_NONE && uclip_kind(&c) == UCLIP_KIND_FILES) {
        // COPIED FILES: the first one that is a picture -- what
        // Screenshot's Copy and the File Manager's put there.
        for (int i = 0; i < uclip_count(&c); i++) {
            if (load(uclip_path(&c, i))) { start(); return; }
        }
        paint_note("None of the copied files is a picture");
        return;
    }
    paint_note(uclip_op(&c) == UCLIP_NONE ? "There is nothing on the clipboard to paste"
                                          : "The clipboard holds text, not a picture");
}

// A LIFTED SELECTION floats as a paste does, from where it was: the step
// that cleared it is joined to the one that places it.
void paste_float(struct uimg *im, int x, int y) {
    if (g_open) paste_commit();
    forget();
    g_src = *im;
    memset(im, 0, sizeof *im);
    snprintf(g_name, sizeof g_name, "the selection");
    g_before = 1;
    upixundo_join(&g_undo);
    g_x = x;
    g_y = y;
    g_w = g_src.w;
    g_h = g_src.h;
    g_open = 1;
    g_drag = D_NONE;
    redraw();
}

void paste_commit(void) {
    if (!g_open) return;
    g_open = 0;
    g_drag = D_NONE;
    const struct uimg *im = &g_src;
    if ((g_w != g_src.w || g_h != g_src.h) && uimg_scale(&g_src, g_w, g_h, &g_scaled) == 0) im = &g_scaled;
    else if (g_w != g_src.w || g_h != g_src.h) paint_note("Not enough memory to scale it; placed at its size");
    if (upixundo_begin(&g_undo, g_layer) != 0) {
        paint_note("Not enough memory to paste");
        if (g_before) paint_undo();
    } else {
        struct upix_rect r = { 0, 0, 0, 0 };
        upix_over_at(g_layer, g_x, g_y, im);
        upix_rect_add(&r, g_layer, g_x, g_y, im->w, im->h);
        upixundo_commit(&g_undo, g_layer, &r);
        paint_changed(&r);
        paint_note("");
    }
    ulogf("paint: paste placed at %d,%d %dx%d\n", g_x, g_y, g_w, g_h);
    forget();
    redraw();
}

static void drop(void) {
    if (!g_open) return;
    g_open = 0;
    g_drag = D_NONE;
    forget();
    upixundo_cancel(&g_undo);     // and the join it set: nothing is coming to join
    if (g_before) paint_undo();   // the layer or the canvas it brought
    g_before = 0;
    paint_note("");
    redraw();
}

int paste_key(int key) {
    if (!g_open) return 0;
    if (key == 0x1B) { drop(); return 1; }
    if (key == '\r' || key == '\n') { paste_commit(); return 1; }
    return 0;
}

// --- the box and its handles ---------------------------------------------

static int handles(struct upix_pt *out) {
    return paint_box_handles((float)g_x, (float)g_y, (float)(g_x + g_w), (float)(g_y + g_h), out);
}

static int inside(float x, float y) {
    float b = paint_handle_band() / 2;
    return x >= g_x - b && x <= g_x + g_w + b && y >= g_y - b && y <= g_y + g_h + b;
}

static void drag_to(float x, float y, unsigned mods) {
    int dx = (int)(x - g_gx), dy = (int)(y - g_gy);
    if (g_drag == D_MOVE) {
        g_x = g_ox + dx;
        g_y = g_oy + dy;
    } else if (g_drag == D_HANDLE) {
        int x0 = g_ox, y0 = g_oy, x1 = g_ox + g_ow, y1 = g_oy + g_oh, h = g_handle;
        if (h == 0 || h == 3 || h == 7) x0 += dx;
        if (h == 1 || h == 2 || h == 5) x1 += dx;
        if (h == 0 || h == 1 || h == 4) y0 += dy;
        if (h == 2 || h == 3 || h == 6) y1 += dy;
        // Never inside out, never nothing.
        if (x1 - x0 < 1) { if (h == 0 || h == 3 || h == 7) x0 = x1 - 1; else x1 = x0 + 1; }
        if (y1 - y0 < 1) { if (h == 0 || h == 1 || h == 4) y0 = y1 - 1; else y1 = y0 + 1; }
        // SHIFT KEEPS ITS SHAPE on a corner: the height follows the width.
        if ((mods & KEY_MOD_SHIFT) && h < 4 && g_ow > 0) {
            int nh = (int)((long long)(x1 - x0) * g_oh / g_ow);
            if (nh < 1) nh = 1;
            if (h == 0 || h == 1) y0 = y1 - nh; else y1 = y0 + nh;
        }
        g_x = x0; g_y = y0; g_w = x1 - x0; g_h = y1 - y0;
    }
    redraw();
}

void paste_pointer(enum uui_paint_phase phase, int ix, int iy, unsigned mods) {
    float x = (float)ix + 0.5f, y = (float)iy + 0.5f;
    struct upix_pt h[8];
    int n = handles(h);
    if (phase == UUI_PAINT_HOVER) {
        int k = paint_handle_at(h, n, x, y);
        g_canvas.cursor = k >= 0 ? paint_handle_cursor(k) : inside(x, y) ? WIN_CURSOR_MOVE : WIN_CURSOR_CROSSHAIR;
        return;
    }
    if (phase == UUI_PAINT_MOVE) { if (g_drag != D_NONE) drag_to(x, y, mods); return; }
    if (phase == UUI_PAINT_UP) {
        // WHERE IT WAS LET GO, not the last motion that arrived.
        if (g_drag != D_NONE) drag_to(x, y, mods);
        g_drag = D_NONE;
        return;
    }
    int k = paint_handle_at(h, n, x, y);
    if (k >= 0 || inside(x, y)) {
        g_drag = k >= 0 ? D_HANDLE : D_MOVE;
        g_handle = k;
        g_gx = x;
        g_gy = y;
        g_ox = g_x; g_oy = g_y; g_ow = g_w; g_oh = g_h;
        return;
    }
    // A CLICK OUTSIDE places it, and does nothing else -- the next press
    // is the tool's.
    paste_commit();
}

// The picture at the box's place and size, row by row, only where the
// view shows it (the canvas has clipped to its view).
static void draw_picture(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    int x0, y0, x1, y1;
    uui_paintcanvas_to_screen(c, g_x, g_y, &x0, &y0);
    uui_paintcanvas_to_screen(c, g_x + g_w, g_y + g_h, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) return;
    int c0 = x0 > c->x ? x0 : c->x, c1 = x1 < c->x + c->w ? x1 : c->x + c->w;
    int r0 = y0 > c->y ? y0 : c->y, r1 = y1 < c->y + c->h ? y1 : c->y + c->h;
    if (c1 <= c0 || r1 <= r0) return;
    int n = c1 - c0;
    if (n > g_row_cap) {
        uint32_t *row = realloc(g_row, (size_t)n * 4);
        if (!row) return;
        g_row = row;
        int *cols = realloc(g_cols, (size_t)n * sizeof *cols);
        if (!cols) return;
        g_cols = cols;
        g_row_cap = n;
    }
    for (int i = 0; i < n; i++) g_cols[i] = (int)((long long)(c0 + i - x0) * g_src.w / bw);
    int last = -1;
    for (int r = r0; r < r1; r++) {
        int sy = (int)((long long)(r - y0) * g_src.h / bh);
        if (sy != last) {
            const uint32_t *src = g_src.px + (size_t)sy * g_src.w;
            for (int i = 0; i < n; i++) g_row[i] = src[g_cols[i]];
            last = sy;
        }
        ugfx_blit_alpha(s, c0, r, n, 1, g_row, n);
    }
}

void paste_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    if (!g_open) return;
    draw_picture(s, c);
    paint_draw_frame(s, c, (float)g_x, (float)g_y, (float)(g_x + g_w), (float)(g_y + g_h));
    struct upix_pt h[8];
    paint_draw_handles(s, c, h, handles(h));
}

void paste_log_layout(const struct uui_paintcanvas *c) {
    if (!g_open) { uapp_logf_layout("paint: layout paste.open 0\n"); return; }
    uapp_logf_layout("paint: layout paste.open 1 box %d %d %d %d\n", g_x, g_y, g_w, g_h);
    struct upix_pt h[8];
    paint_log_handles(c, "paste", h, handles(h));
}
