// Paint's Paste: the clipboard's picture -- or a copied picture file --
// placed at the view's top-left and FLOATING, with handles, until it is
// let go: Windows 11 Paint's and GIMP's floating paste (P1 from the
// mockups). Its frame moves it, a handle scales it (Shift keeps its
// shape), Enter or a click outside keeps it as one undo step, Esc drops
// it. One of Paint's units -- see paint.h.
//
// DRAWN FROM THE UNDO COPY, as a shape is: what the last render covered
// goes back from the copy taken when the paste began, and the picture is
// laid over again at its new place and size.
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
static struct uimg g_scaled;         // ...at the box's size, when that differs
static int g_rough;                  // g_scaled is the drag's nearest-neighbour stand-in
static char g_name[64];              // for the question and the note
static int g_new_layer;              // pending: into a new layer
static int g_open;
static int g_before;                 // a step was joined before the paste (a layer, a canvas): Esc undoes it
static int g_x, g_y, g_w, g_h;       // the box, picture pixels
static enum drag g_drag;
static int g_handle;
static float g_gx, g_gy;
static int g_ox, g_oy, g_ow, g_oh;
static struct upix_rect g_drawn;

int paste_active(void) { return g_open; }

static void forget(void) {
    uimg_free(&g_src);
    uimg_free(&g_scaled);
}

static void restore(void) {
    const struct uimg *snap = &g_undo.snap;
    if (!snap->px || snap->w != g_layer->w || snap->h != g_layer->h) return;
    for (int j = 0; j < g_drawn.h; j++)
        memcpy(g_layer->px + (size_t)(g_drawn.y + j) * g_layer->w + g_drawn.x,
               snap->px + (size_t)(g_drawn.y + j) * snap->w + g_drawn.x, (size_t)g_drawn.w * 4);
}

// NEAREST NEIGHBOUR WHILE A HANDLE IS HELD, filtered once it is let go:
// uimg_scale() per motion of a big picture is slow enough to lose the
// motions themselves.
static int rough_scale(void) {
    uimg_free(&g_scaled);
    g_scaled.px = malloc((size_t)g_w * g_h * 4);
    if (!g_scaled.px) return -1;
    g_scaled.w = g_w;
    g_scaled.h = g_h;
    for (int y = 0; y < g_h; y++) {
        const uint32_t *row = g_src.px + (size_t)((long long)y * g_src.h / g_h) * g_src.w;
        uint32_t *out = g_scaled.px + (size_t)y * g_w;
        for (int x = 0; x < g_w; x++) out[x] = row[(long long)x * g_src.w / g_w];
    }
    return 0;
}

static const struct uimg *at_size(void) {
    if (g_w == g_src.w && g_h == g_src.h) return &g_src;
    int rough = g_drag == D_HANDLE;
    if (g_scaled.px && g_scaled.w == g_w && g_scaled.h == g_h && g_rough == rough) return &g_scaled;
    g_rough = rough;
    uimg_free(&g_scaled);
    if ((rough ? rough_scale() : uimg_scale(&g_src, g_w, g_h, &g_scaled)) != 0) return &g_src;
    return &g_scaled;
}

static void render(void) {
    if (!g_open) return;
    restore();
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    const struct uimg *im = at_size();
    upix_over_at(g_layer, g_x, g_y, im);
    upix_rect_add(&g_drawn, g_layer, g_x, g_y, im->w, im->h);
    struct upix_rect both = was;
    upix_rect_add(&both, g_layer, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    paint_changed(&both);
}

// Floating at the view's top-left (the picture's, when it is all in view).
static void place(void) {
    if (upixundo_begin(&g_undo, g_layer) != 0) {
        paint_note("Not enough memory to paste");
        if (g_before) paint_undo();
        forget();
        return;
    }
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
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    char note[128];
    snprintf(note, sizeof note, "Pasted %s, %d \xd7 %d -- drag to move, a handle scales it, click away to place",
             g_name, g_src.w, g_src.h);
    paint_note(note);
    ulogf("paint: paste %dx%d at %d,%d new_layer %d\n", g_src.w, g_src.h, g_x, g_y, g_new_layer);
    render();
    paint_text_changed();
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

void paste_commit(void) {
    if (!g_open) return;
    g_open = 0;
    g_drag = D_NONE;
    upixundo_commit(&g_undo, g_layer, &g_drawn);
    ulogf("paint: paste placed at %d,%d %dx%d\n", g_x, g_y, g_w, g_h);
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    forget();
    paint_note("");
    paint_text_changed();
}

static void drop(void) {
    if (!g_open) return;
    restore();
    upixundo_cancel(&g_undo);
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    g_open = 0;
    g_drag = D_NONE;
    forget();
    paint_changed(&was);
    if (g_before) paint_undo();   // the layer or the canvas it brought
    g_before = 0;
    paint_note("");
    paint_text_changed();
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
    render();
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
        // WHERE IT WAS LET GO, not the last motion that arrived; then
        // drawn again, filtered.
        if (g_drag != D_NONE) drag_to(x, y, mods);
        int scaled = g_drag == D_HANDLE;
        g_drag = D_NONE;
        if (scaled) render();
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

void paste_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    if (!g_open) return;
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
