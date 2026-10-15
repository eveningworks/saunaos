// Paint's gradients: the Gradient tool -- dragged from where Colour 1 is
// to where Colour 2 is, its two ends draggable until it is let go, as a
// shape's handles are (G1 from the mockups, Paint.NET's) -- and the
// bucket's Gradient kind, which fills the region a click would fill
// (G2, PhotoFiltre's). Both take the bar's shape, Colour or Transparency,
// and dithering. A selection keeps either inside it. One of Paint's
// units -- see paint.h.
//
// DRAWN FROM THE UNDO COPY, as a shape is: what the last render covered
// goes back from the copy taken when it began, and the gradient is laid
// over again (lib/upixel.h's upix_gradient_fill).
//
// ONCE A FRAME, NOT ONCE A MOTION: a gradient covers the whole layer, and
// rendering it per motion event made a drag land seconds behind the
// pointer (7.2 s for 48 steps at 800 x 600). A motion only moves the ends
// and asks for a frame; the frame renders (gradient_flush()).
#include "paint.h"
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uapp.h"
#include "ui/uui_primitives.h"
#include "ui/ulog.h"

int g_grad_shape = UPIX_GRAD_LINEAR;
int g_grad_clear;
int g_grad_dither = 1;
int g_fill_gradient;

enum drag { D_NONE, D_END, D_BUCKET };

static int g_open;                   // the tool's gradient, ends still draggable
static enum drag g_drag;
static int g_end;                    // which end a drag holds: 0 start, 1 end
static float g_ax, g_ay, g_bx, g_by; // its line, picture pixels
static struct upix_rect g_drawn;
static int g_owed;                   // the ends moved since the last render
static struct upix_sel g_region;     // the bucket's: what a fill there would cover
static int g_slot;                   // the bucket's colour slot

int gradient_active(void) { return g_open; }

static void restore(void) {
    const struct uimg *snap = &g_undo.snap;
    if (!snap->px || snap->w != g_layer->w || snap->h != g_layer->h) return;
    for (int j = 0; j < g_drawn.h; j++)
        memcpy(g_layer->px + (size_t)(g_drawn.y + j) * g_layer->w + g_drawn.x,
               snap->px + (size_t)(g_drawn.y + j) * snap->w + g_drawn.x, (size_t)g_drawn.w * 4);
}

// Its colours: Colour 1 to Colour 2, or Colour 1 to nothing. The bucket's
// right button starts from Colour 2.
static void colours(uint32_t *c0, uint32_t *c1, int slot) {
    *c0 = g_colour[slot];
    *c1 = g_grad_clear ? g_colour[slot] & 0x00FFFFFFu : g_colour[!slot];
}

// The gradient over `mask` (NULL: everywhere) inside `box`.
static void render_in(const uint8_t *mask, struct upix_rect box, int slot) {
    restore();
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    struct upix_gradient gr = { (enum upix_grad)g_grad_shape, g_ax, g_ay, g_bx, g_by, 0, 0, g_grad_dither };
    colours(&gr.c0, &gr.c1, slot);
    upix_gradient_fill(g_layer, &gr, mask, &box, &g_drawn);
    struct upix_rect both = was;
    upix_rect_add(&both, g_layer, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    paint_changed(&both);
}

// The tool's: inside the selection, or over the whole layer.
static void render_now(void) {
    const struct upix_sel *s = select_get();
    struct upix_rect all = { 0, 0, g_layer->w, g_layer->h };
    render_in(s ? s->a : 0, s ? s->box : all, 0);
}

static void render(void) {
    if (!g_open && g_drag != D_BUCKET) return;
    g_owed = 1;
    paint_text_changed();   // a frame, which renders
}

void gradient_flush(void) {
    if (!g_owed) return;
    g_owed = 0;
    if (g_drag == D_BUCKET) { if (g_region.box.w > 0) render_in(g_region.a, g_region.box, g_slot); }
    else if (g_open) render_now();
}

void gradient_restyle(void) { render(); }

void gradient_commit(void) {
    if (!g_open) return;
    gradient_flush();
    g_open = 0;
    g_drag = D_NONE;
    upixundo_commit(&g_undo, g_layer, &g_drawn);
    ulogf("paint: gradient %d from %d,%d to %d,%d\n", g_grad_shape, (int)g_ax, (int)g_ay, (int)g_bx, (int)g_by);
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    paint_text_changed();
}

static void drop(void) {
    if (!g_open) return;
    g_owed = 0;
    restore();
    upixundo_cancel(&g_undo);
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    g_open = 0;
    g_drag = D_NONE;
    paint_changed(&was);
}

int gradient_key(int key) {
    if (!g_open) return 0;
    if (key == 0x1B) { drop(); return 1; }
    if (key == '\r' || key == '\n') { gradient_commit(); return 1; }
    return 0;
}

static int handles(struct upix_pt *h) {
    h[0] = (struct upix_pt){ g_ax, g_ay };
    h[1] = (struct upix_pt){ g_bx, g_by };
    return 2;
}

void gradient_pointer(enum uui_paint_phase phase, int ix, int iy, unsigned mods) {
    (void)mods;
    float x = (float)ix + 0.5f, y = (float)iy + 0.5f;
    struct upix_pt h[2];
    if (phase == UUI_PAINT_HOVER) {
        g_canvas.cursor = g_open && paint_handle_at(h, handles(h), x, y) >= 0 ? WIN_CURSOR_MOVE : WIN_CURSOR_CROSSHAIR;
        return;
    }
    if (phase == UUI_PAINT_MOVE || phase == UUI_PAINT_UP) {
        if (g_drag != D_END) return;
        if (g_end) { g_bx = x; g_by = y; } else { g_ax = x; g_ay = y; }
        if (phase == UUI_PAINT_UP) {
            g_drag = D_NONE;
            // A CLICK, NOT A DRAG: no gradient, and only the last one let go.
            if (g_ax == g_bx && g_ay == g_by) { drop(); return; }
        }
        render();
        return;
    }
    // DOWN: an end of the open one, or anywhere else lets it go and the
    // same press starts the next.
    if (g_open) {
        int k = paint_handle_at(h, handles(h), x, y);
        if (k >= 0) { g_drag = D_END; g_end = k; return; }
        gradient_commit();
    }
    if (layers_text(layers_active())) { paint_ask_rasterize(); return; }
    if (upixundo_begin(&g_undo, g_layer) != 0) { paint_note("Not enough memory for a gradient"); return; }
    g_open = 1;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    g_ax = g_bx = x;
    g_ay = g_by = y;
    g_drag = D_END;
    g_end = 1;
    paint_colour_used(0);
    paint_colour_used(1);
}

// --- the bucket's ------------------------------------------------------------

void gradient_bucket(enum uui_paint_phase phase, int ix, int iy, int slot) {
    float x = (float)ix + 0.5f, y = (float)iy + 0.5f;
    if (phase == UUI_PAINT_DOWN) {
        if (ix < 0 || iy < 0 || ix >= g_layer->w || iy >= g_layer->h) return;
        if (upixundo_begin(&g_undo, g_layer) != 0) { paint_note("Not enough memory for a gradient"); return; }
        upix_sel_free(&g_region);
        if (upix_sel_init(&g_region, g_layer->w, g_layer->h) != 0) { upixundo_cancel(&g_undo); return; }
        upix_sel_wand(&g_region, g_layer, ix, iy, 0, UPIX_SEL_NEW);
        // A SELECTION KEEPS IT INSIDE, as it keeps a solid fill.
        const struct upix_sel *s = select_get();
        if (s) {
            for (size_t i = 0; i < (size_t)g_region.w * g_region.h; i++) g_region.a[i] &= s->a[i];
            upix_sel_rect(&g_region, 0, 0, 0, 0, UPIX_SEL_ADD);   // the box again, after the AND
        }
        g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
        g_ax = g_bx = x;
        g_ay = g_by = y;
        g_slot = slot;
        g_drag = D_BUCKET;
        paint_colour_used(0);
        paint_colour_used(1);
        return;
    }
    if (g_drag != D_BUCKET || phase == UUI_PAINT_HOVER) return;
    g_bx = x;
    g_by = y;
    if (phase == UUI_PAINT_UP && g_ax == g_bx && g_ay == g_by) {
        // A CLICK: left to right across the region.
        const struct upix_rect *b = &g_region.box;
        g_ax = (float)b->x;
        g_bx = (float)(b->x + b->w);
        g_ay = g_by = (float)b->y + (float)b->h / 2;
    }
    render();
    if (phase == UUI_PAINT_UP) {
        gradient_flush();
        g_drag = D_NONE;
        upixundo_commit(&g_undo, g_layer, &g_drawn);
        ulogf("paint: bucket gradient %d over %d,%d %dx%d\n", g_grad_shape, g_region.box.x, g_region.box.y,
              g_region.box.w, g_region.box.h);
        g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
        upix_sel_free(&g_region);
    }
}

// --- drawing ---------------------------------------------------------------------

void gradient_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    if (!g_open) return;
    int ax, ay, bx, by;
    uui_paintcanvas_to_screen(c, (int)g_ax, (int)g_ay, &ax, &ay);
    uui_paintcanvas_to_screen(c, (int)g_bx, (int)g_by, &bx, &by);
    // The line, white under accent dashes, and an end in each colour.
    int n = abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay);
    for (int i = 0; i <= n; i++) {
        int px = ax + (n ? (bx - ax) * i / n : 0), py = ay + (n ? (by - ay) * i / n : 0);
        ugfx_fill_rect(s, px, py, 1, 1, (i / 4) & 1 ? UTHEME_WHITE : UTHEME_ACCENT);
    }
    uint32_t c0, c1;
    colours(&c0, &c1, 0);
    int r = PAINT_HANDLE_PX / 2 + 2;
    uui_fill_round_rect(s, ax - r - 1, ay - r - 1, 2 * r + 2, 2 * r + 2, r + 1, UTHEME_WHITE);
    uui_fill_round_rect(s, ax - r + 1, ay - r + 1, 2 * r - 2, 2 * r - 2, r - 1, c0 & 0xFFFFFF);
    uui_fill_round_rect(s, bx - r - 1, by - r - 1, 2 * r + 2, 2 * r + 2, r + 1, UTHEME_WHITE);
    uui_fill_round_rect(s, bx - r + 1, by - r + 1, 2 * r - 2, 2 * r - 2, r - 1, g_grad_clear ? UTHEME_WHITE : c1 & 0xFFFFFF);
}

void gradient_log_layout(const struct uui_paintcanvas *c) {
    uapp_logf_layout("paint: layout gradientstyle shape %d clear %d dither %d bucket %d\n", g_grad_shape, g_grad_clear,
                     g_grad_dither, g_fill_gradient);
    if (!g_open) { uapp_logf_layout("paint: layout gradient.open 0\n"); return; }
    uapp_logf_layout("paint: layout gradient.open 1 from %d %d to %d %d\n", (int)g_ax, (int)g_ay, (int)g_bx, (int)g_by);
    struct upix_pt h[2];
    paint_log_handles(c, "gradient", h, handles(h));
}
