// Paint's shapes: a line, a curve, a polygon or a figure (rectangle,
// rounded rectangle, ellipse, triangle, arrow, star), dragged out, then
// EDITABLE until it is let go -- eight handles size its box and its
// frame moves it (a line's and a curve's handles are their points), and
// the bar's outline, fill, width and smoothing restyle it -- Windows 11
// Paint's and Paint.NET's. One of Paint's units -- see paint.h.
//
// DRAWN FROM THE UNDO COPY, as the text box once was: the copy taken
// when the shape began goes back over what the last render covered, and
// the shape is drawn again (lib/upixel.h's masks). Letting it go is the
// undo step closing over that rect.
//
// A POLYGON IS CLICKED, KolourPaint's: the first drag is its first side,
// each click a corner, and a double click, Enter, or a click on the first
// corner closes it.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uapp.h"
#include "keyboard.h"
#include "lib/udclick.h"
#include "rt/sys.h"

#define HANDLE_PX 8                 // a handle, screen pixels
#define POLY_MAX 64
#define PATH_MAX_PTS 1024

int g_shape_kind = SHAPE_RECT;
int g_shape_outline = SHAPE_INK_1;
int g_shape_fill = SHAPE_INK_NONE;
int g_shape_width = 3;
int g_shape_aa = 1;

enum drag { D_NONE, D_CREATE, D_MOVE, D_HANDLE, D_POLY };

static int g_open;                   // a shape is on the picture, still editable
static int g_building;               // a polygon still taking corners
static float g_x0, g_y0, g_x1, g_y1; // a figure's box; a line's ends (x0,y0)-(x1,y1)
static struct upix_pt g_pts[POLY_MAX];   // a polygon's corners; a curve's four points
static int g_npts;
static enum drag g_drag;
static int g_handle;
static float g_gx, g_gy;             // where a drag took hold, picture pixels
static float g_ox0, g_oy0, g_ox1, g_oy1;   // the box when it did
static struct upix_pt g_opts[POLY_MAX];
static struct upix_rect g_drawn;
static unsigned long long g_last_click;

int shapes_active(void) { return g_open; }

static int is_line(void) { return g_shape_kind == SHAPE_LINE; }
static int is_curve(void) { return g_shape_kind == SHAPE_CURVE; }
static int is_poly(void) { return g_shape_kind == SHAPE_POLYGON; }
static int is_figure(void) { return !is_line() && !is_curve() && !is_poly(); }

static uint32_t ink_of(int which, int fill) {
    // An outline's Colour 1 and a fill's Colour 2 are each one's "own",
    // as Windows 11 Paint's are; the other is the swap.
    if (which == SHAPE_INK_NONE) return 0;
    int slot = fill ? (which == SHAPE_INK_1 ? 1 : 0) : (which == SHAPE_INK_1 ? 0 : 1);
    return g_colour[slot];
}

static void restore(void) {
    const struct uimg *snap = &g_undo.snap;
    if (!snap->px || snap->w != g_layer->w || snap->h != g_layer->h) return;
    for (int j = 0; j < g_drawn.h; j++)
        memcpy(g_layer->px + (size_t)(g_drawn.y + j) * g_layer->w + g_drawn.x,
               snap->px + (size_t)(g_drawn.y + j) * snap->w + g_drawn.x, (size_t)g_drawn.w * 4);
}

// The shape's path, and whether it is closed.
static int path(struct upix_pt *out, int max, int *closed) {
    *closed = 1;
    if (is_line()) {
        *closed = 0;
        out[0] = (struct upix_pt){ g_x0, g_y0 };
        out[1] = (struct upix_pt){ g_x1, g_y1 };
        return 2;
    }
    if (is_curve()) {
        *closed = 0;
        return upix_bezier(g_pts[0], g_pts[1], g_pts[2], g_pts[3], out, 64);
    }
    if (is_poly()) {
        *closed = !g_building;
        int n = g_npts < max ? g_npts : max;
        memcpy(out, g_pts, sizeof *out * (size_t)n);
        return n;
    }
    static const enum upix_figure FIG[] = {
        [SHAPE_RECT] = UPIX_FIG_RECT, [SHAPE_ROUNDED] = UPIX_FIG_ROUNDED, [SHAPE_ELLIPSE] = UPIX_FIG_ELLIPSE,
        [SHAPE_TRIANGLE] = UPIX_FIG_TRIANGLE, [SHAPE_ARROW] = UPIX_FIG_ARROW, [SHAPE_STAR] = UPIX_FIG_STAR,
    };
    return upix_figure(FIG[g_shape_kind], g_x0, g_y0, g_x1, g_y1, out, max);
}

static void render(void) {
    if (!g_open || !g_layer) return;
    restore();
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    static struct upix_pt pts[PATH_MAX_PTS];
    int closed, n = path(pts, PATH_MAX_PTS, &closed);
    int exact = !g_shape_aa;
    struct upix_mask m;
    if (closed && n >= 3 && g_shape_fill != SHAPE_INK_NONE &&
        upix_mask_fill(&m, pts, n, g_layer->w, g_layer->h, g_shape_aa) == 0) {
        upix_mask_paint(g_layer, &m, ink_of(g_shape_fill, 1), exact, &g_drawn);
        upix_mask_free(&m);
    }
    // A line or a curve with no outline would be nothing: they always draw.
    int outline = g_shape_outline != SHAPE_INK_NONE || !closed;
    if (outline && n >= 1 &&
        upix_mask_stroke(&m, pts, n, closed, (float)g_shape_width, g_layer->w, g_layer->h, g_shape_aa) == 0) {
        upix_mask_paint(g_layer, &m, ink_of(g_shape_outline != SHAPE_INK_NONE ? g_shape_outline : SHAPE_INK_1, 0),
                        exact, &g_drawn);
        upix_mask_free(&m);
    }
    struct upix_rect both = was;
    upix_rect_add(&both, g_layer, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    paint_changed(&both);
}

void shapes_restyle(void) { render(); }

void shapes_commit(void) {
    if (!g_open) return;
    g_open = 0;
    g_building = 0;
    g_drag = D_NONE;
    upixundo_commit(&g_undo, g_layer, &g_drawn);
    ulogf("paint: shape %d committed at %d,%d %dx%d\n", g_shape_kind, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    paint_text_changed();
}

// --- the box and its handles ---------------------------------------------

static void box(float *x0, float *y0, float *x1, float *y1) {
    if (is_figure()) { *x0 = g_x0; *y0 = g_y0; *x1 = g_x1; *y1 = g_y1; }
    else if (is_line()) { *x0 = g_x0; *y0 = g_y0; *x1 = g_x1; *y1 = g_y1; }
    else {
        int n = is_curve() ? 4 : g_npts;
        *x0 = *x1 = g_pts[0].x;
        *y0 = *y1 = g_pts[0].y;
        for (int i = 1; i < n; i++) {
            if (g_pts[i].x < *x0) *x0 = g_pts[i].x;
            if (g_pts[i].x > *x1) *x1 = g_pts[i].x;
            if (g_pts[i].y < *y0) *y0 = g_pts[i].y;
            if (g_pts[i].y > *y1) *y1 = g_pts[i].y;
        }
        return;
    }
    if (*x1 < *x0) { float t = *x0; *x0 = *x1; *x1 = t; }
    if (*y1 < *y0) { float t = *y0; *y0 = *y1; *y1 = t; }
}

// Handle i's centre: a box's eight (corners, then the sides' middles), a
// line's two ends, a curve's four points.
static int handles(struct upix_pt *out) {
    if (is_line()) { out[0] = (struct upix_pt){ g_x0, g_y0 }; out[1] = (struct upix_pt){ g_x1, g_y1 }; return 2; }
    if (is_curve()) { memcpy(out, g_pts, sizeof *out * 4); return 4; }
    if (g_building) return 0;
    float x0, y0, x1, y1;
    box(&x0, &y0, &x1, &y1);
    float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    struct upix_pt h[8] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 },
                            { mx, y0 }, { x1, my }, { mx, y1 }, { x0, my } };
    memcpy(out, h, sizeof h);
    return 8;
}

static float band(void) {
    int z = g_canvas.zoom > 0 ? g_canvas.zoom : 100;
    return (float)HANDLE_PX * 100.0f / (float)z;
}

static int handle_at(float x, float y) {
    struct upix_pt h[8];
    int n = handles(h);
    float b = band() * 0.75f;
    for (int i = 0; i < n; i++)
        if (x >= h[i].x - b && x <= h[i].x + b && y >= h[i].y - b && y <= h[i].y + b) return i;
    return -1;
}

static int inside_frame(float x, float y) {
    float x0, y0, x1, y1, b = band() / 2;
    box(&x0, &y0, &x1, &y1);
    return x >= x0 - b && x <= x1 + b && y >= y0 - b && y <= y1 + b;
}

// --- the pointer -----------------------------------------------------------

static void begin(float x, float y) {
    if (upixundo_begin(&g_undo, g_layer) != 0) { paint_note("Not enough memory for a shape"); return; }
    g_open = 1;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    g_x0 = g_x1 = x;
    g_y0 = g_y1 = y;
    for (int i = 0; i < 4; i++) g_pts[i] = (struct upix_pt){ x, y };
    g_npts = 2;
    g_building = is_poly();
    g_drag = D_CREATE;
    paint_colour_used(0);
}

// Where the end being dragged goes: with Shift, a square box or a line at
// a multiple of 45 degrees, as every editor's.
static void constrain(float ax, float ay, float *x, float *y, unsigned mods) {
    if (!(mods & KEY_MOD_SHIFT)) return;
    float dx = *x - ax, dy = *y - ay, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (is_line() || is_poly()) {
        if (adx > 2 * ady) *y = ay;
        else if (ady > 2 * adx) *x = ax;
        else { float d = adx > ady ? adx : ady; *x = ax + (dx < 0 ? -d : d); *y = ay + (dy < 0 ? -d : d); }
        return;
    }
    float d = adx > ady ? adx : ady;
    *x = ax + (dx < 0 ? -d : d);
    *y = ay + (dy < 0 ? -d : d);
}

static void drag_to(float x, float y, unsigned mods) {
    if (g_drag == D_CREATE) {
        constrain(g_x0, g_y0, &x, &y, mods);
        g_x1 = x;
        g_y1 = y;
        if (is_curve()) {
            // A straight start: the controls a third and two thirds along.
            g_pts[0] = (struct upix_pt){ g_x0, g_y0 };
            g_pts[3] = (struct upix_pt){ x, y };
            g_pts[1] = (struct upix_pt){ g_x0 + (x - g_x0) / 3, g_y0 + (y - g_y0) / 3 };
            g_pts[2] = (struct upix_pt){ g_x0 + (x - g_x0) * 2 / 3, g_y0 + (y - g_y0) * 2 / 3 };
        }
        if (is_poly()) g_pts[1] = (struct upix_pt){ x, y };
    } else if (g_drag == D_POLY) {
        constrain(g_pts[g_npts - 2].x, g_pts[g_npts - 2].y, &x, &y, mods);
        g_pts[g_npts - 1] = (struct upix_pt){ x, y };
    } else if (g_drag == D_MOVE) {
        float dx = x - g_gx, dy = y - g_gy;
        g_x0 = g_ox0 + dx; g_y0 = g_oy0 + dy; g_x1 = g_ox1 + dx; g_y1 = g_oy1 + dy;
        for (int i = 0; i < (is_curve() ? 4 : g_npts); i++) g_pts[i] = (struct upix_pt){ g_opts[i].x + dx, g_opts[i].y + dy };
    } else if (g_drag == D_HANDLE) {
        if (is_line()) {
            if (g_handle == 0) { constrain(g_x1, g_y1, &x, &y, mods); g_x0 = x; g_y0 = y; }
            else { constrain(g_x0, g_y0, &x, &y, mods); g_x1 = x; g_y1 = y; }
        } else if (is_curve()) {
            g_pts[g_handle] = (struct upix_pt){ x, y };
        } else {
            // A corner moves two sides, a middle one; the box may turn inside out.
            float x0 = g_ox0, y0 = g_oy0, x1 = g_ox1, y1 = g_oy1;
            int h = g_handle;
            if (h == 0 || h == 3 || h == 7) x0 = x;
            if (h == 1 || h == 2 || h == 5) x1 = x;
            if (h == 0 || h == 1 || h == 4) y0 = y;
            if (h == 2 || h == 3 || h == 6) y1 = y;
            if (is_poly()) {
                // Every corner scaled from the old box into the new one.
                float sw = g_ox1 - g_ox0, sh = g_oy1 - g_oy0;
                for (int i = 0; i < g_npts; i++)
                    g_pts[i] = (struct upix_pt){ x0 + (sw ? (g_opts[i].x - g_ox0) / sw : 0) * (x1 - x0),
                                                 y0 + (sh ? (g_opts[i].y - g_oy0) / sh : 0) * (y1 - y0) };
            } else {
                g_x0 = x0; g_y0 = y0; g_x1 = x1; g_y1 = y1;
            }
        }
    }
    render();
}

static void grab(float x, float y, enum drag d, int h) {
    g_drag = d;
    g_handle = h;
    g_gx = x;
    g_gy = y;
    box(&g_ox0, &g_oy0, &g_ox1, &g_oy1);
    if (is_figure() || is_line()) { g_ox0 = g_x0; g_oy0 = g_y0; g_ox1 = g_x1; g_oy1 = g_y1; }
    memcpy(g_opts, g_pts, sizeof g_opts);
    if (is_figure() && d == D_HANDLE) box(&g_ox0, &g_oy0, &g_ox1, &g_oy1);
}

static void close_polygon(void) {
    if (!g_building) return;
    g_building = 0;
    if (g_npts > 2 && g_pts[g_npts - 1].x == g_pts[g_npts - 2].x && g_pts[g_npts - 1].y == g_pts[g_npts - 2].y)
        g_npts--;   // the double click's second press added nothing
    render();
}

void shapes_pointer(enum uui_paint_phase phase, int ix, int iy, unsigned mods) {
    float x = (float)ix + 0.5f, y = (float)iy + 0.5f;
    if (phase == UUI_PAINT_HOVER) {
        if (g_building) { g_canvas.cursor = WIN_CURSOR_CROSSHAIR; return; }
        int h = g_open ? handle_at(x, y) : -1;
        g_canvas.cursor = h >= 0 ? (is_line() || is_curve() ? WIN_CURSOR_MOVE : h >= 4 && h % 2 ? WIN_CURSOR_RESIZE_H
                                    : h >= 4 ? WIN_CURSOR_RESIZE_V : WIN_CURSOR_MOVE)
                        : g_open && inside_frame(x, y) && !is_line() && !is_curve() ? WIN_CURSOR_MOVE
                        : WIN_CURSOR_CROSSHAIR;
        return;
    }
    if (phase == UUI_PAINT_MOVE) {
        if (g_drag != D_NONE) drag_to(x, y, mods);
        return;
    }
    if (phase == UUI_PAINT_UP) {
        if (g_drag == D_CREATE && is_poly()) {
            // The first side is down; each click from here is a corner.
            g_drag = D_NONE;
            return;
        }
        if (g_drag == D_POLY) { g_drag = D_NONE; return; }
        if (g_drag == D_CREATE && g_x0 == g_x1 && g_y0 == g_y1) {
            // A CLICK, NOT A DRAG: no shape -- Windows 11 Paint's. It only
            // let the last one go.
            restore();
            upixundo_cancel(&g_undo);
            struct upix_rect was = g_drawn;
            g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
            g_open = 0;
            g_drag = D_NONE;
            paint_changed(&was);
            return;
        }
        g_drag = D_NONE;
        return;
    }
    // DOWN.
    unsigned long long now = sys_monotonic_ns();
    int dbl = now - g_last_click < udclick_ns();
    g_last_click = now;
    if (g_building) {
        float b = band();
        int on_first = x >= g_pts[0].x - b && x <= g_pts[0].x + b && y >= g_pts[0].y - b && y <= g_pts[0].y + b;
        if (dbl || on_first || g_npts >= POLY_MAX) { close_polygon(); return; }
        g_pts[g_npts++] = (struct upix_pt){ x, y };
        g_drag = D_POLY;
        drag_to(x, y, mods);
        return;
    }
    if (g_open) {
        int h = handle_at(x, y);
        if (h >= 0) { grab(x, y, D_HANDLE, h); return; }
        if (inside_frame(x, y) && !is_line() && !is_curve()) { grab(x, y, D_MOVE, 0); return; }
        // ANYWHERE ELSE lets it go, and the same press starts the next one.
        shapes_commit();
    }
    if (ix < 0 || iy < 0 || ix >= g_layer->w || iy >= g_layer->h) return;
    begin(x, y);
    render();
}

int shapes_key(int key) {
    if (!g_open) return 0;
    if (g_building && (key == '\r' || key == '\n')) { close_polygon(); return 1; }
    if (key == 0x1B || key == '\r' || key == '\n') { close_polygon(); shapes_commit(); return 1; }
    return 0;
}

// --- the frame and handles, over the picture ---------------------------------------

void shapes_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    if (!g_open) return;
    if (!is_line() && !is_curve() && !g_building) {
        float x0, y0, x1, y1;
        box(&x0, &y0, &x1, &y1);
        int sx0, sy0, sx1, sy1;
        uui_paintcanvas_to_screen(c, (int)x0, (int)y0, &sx0, &sy0);
        uui_paintcanvas_to_screen(c, (int)(x1 + 0.999f), (int)(y1 + 0.999f), &sx1, &sy1);
        for (int i = sx0; i < sx1; i += 6) {
            ugfx_fill_rect(s, i, sy0 - 1, i + 3 < sx1 ? 3 : sx1 - i, 1, UTHEME_ACCENT);
            ugfx_fill_rect(s, i, sy1, i + 3 < sx1 ? 3 : sx1 - i, 1, UTHEME_ACCENT);
        }
        for (int i = sy0; i < sy1; i += 6) {
            ugfx_fill_rect(s, sx0 - 1, i, 1, i + 3 < sy1 ? 3 : sy1 - i, UTHEME_ACCENT);
            ugfx_fill_rect(s, sx1, i, 1, i + 3 < sy1 ? 3 : sy1 - i, UTHEME_ACCENT);
        }
    }
    if (is_curve()) {
        // The controls' arms, dotted, from each end.
        for (int k = 0; k < 2; k++) {
            int ax, ay, bx, by;
            uui_paintcanvas_to_screen(c, (int)g_pts[k ? 3 : 0].x, (int)g_pts[k ? 3 : 0].y, &ax, &ay);
            uui_paintcanvas_to_screen(c, (int)g_pts[k ? 2 : 1].x, (int)g_pts[k ? 2 : 1].y, &bx, &by);
            int steps = (abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay)) / 4;
            for (int i = 0; i <= steps; i += 2)
                ugfx_fill_rect(s, ax + (bx - ax) * i / (steps ? steps : 1), ay + (by - ay) * i / (steps ? steps : 1), 1, 1,
                               UTHEME_ACCENT);
        }
    }
    struct upix_pt h[8];
    int n = handles(h);
    for (int i = 0; i < n; i++) {
        int hx, hy;
        uui_paintcanvas_to_screen(c, (int)h[i].x, (int)h[i].y, &hx, &hy);
        hx -= HANDLE_PX / 2;
        hy -= HANDLE_PX / 2;
        ugfx_fill_rect(s, hx, hy, HANDLE_PX, HANDLE_PX, UTHEME_ACCENT);
        ugfx_fill_rect(s, hx + 1, hy + 1, HANDLE_PX - 2, HANDLE_PX - 2, UTHEME_WHITE);
    }
}

// "paint: layout shape ...": open, kind, the box, the corners; each handle.
void shapes_log_layout(const struct uui_paintcanvas *c) {
    uapp_logf_layout("paint: layout shapestyle kind %d outline %d fill %d width %d smooth %d\n", g_shape_kind,
                     g_shape_outline, g_shape_fill, g_shape_width, g_shape_aa);
    if (!g_open) { uapp_logf_layout("paint: layout shape.open 0\n"); return; }
    float x0, y0, x1, y1;
    box(&x0, &y0, &x1, &y1);
    uapp_logf_layout("paint: layout shape.open 1 kind %d box %d %d %d %d building %d points %d\n", g_shape_kind,
                     (int)x0, (int)y0, (int)x1, (int)y1, g_building, g_npts);
    struct upix_pt h[8];
    int n = handles(h);
    for (int i = 0; i < n; i++) {
        int hx, hy;
        uui_paintcanvas_to_screen(c, (int)h[i].x, (int)h[i].y, &hx, &hy);
        uapp_logf_layout("paint: layout shape.handle %d %d %d %d %d\n", i, hx - HANDLE_PX / 2, hy - HANDLE_PX / 2,
                         HANDLE_PX, HANDLE_PX);
    }
}
