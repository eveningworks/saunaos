// A picture to paint on, zoomed and scrolled -- see ui/uui_paintcanvas.h.
#include "ui/uui_paintcanvas.h"
#include "ui/uapp.h"          // uapp_keyboard_mods()
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "syscall_abi.h"      // WIN_CURSOR_*
#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>

static const int ZOOMS[] = { 10, 25, 50, 75, 100, 150, 200, 300, 400, 600, 800, 1200, 1600, 2400, 3200 };
#define NZOOMS ((int)(sizeof ZOOMS / sizeof ZOOMS[0]))

static int zoomed(int n, int zoom) {
    long long v = (long long)n * zoom / 100;
    return v < 1 ? 1 : (int)v;
}

static int pic_w(const struct uui_paintcanvas *c) { return c->img && c->img->px ? zoomed(c->img->w, c->zoom) : 0; }
static int pic_h(const struct uui_paintcanvas *c) { return c->img && c->img->px ? zoomed(c->img->h, c->zoom) : 0; }

// What scrolls: the picture, and with edge handles a MARGIN past its
// right and bottom edges -- or a picture bigger than the view would put
// the handles where no scroll could reach them.
static int margin(const struct uui_paintcanvas *c) { return c->on_resize ? ugfx_char_h() * 2 : 0; }
static int cont_w(const struct uui_paintcanvas *c) { return pic_w(c) ? pic_w(c) + margin(c) : 0; }
static int cont_h(const struct uui_paintcanvas *c) { return pic_h(c) ? pic_h(c) + margin(c) : 0; }

// The view: the widget less whichever bars the picture needs. A bar can
// make the other necessary, so the two are decided together.
static void view(const struct uui_paintcanvas *c, int *vw, int *vh, int *need_h, int *need_v) {
    int sb = uui_sbar_width(), dw = cont_w(c), dh = cont_h(c);
    int nh = dw > c->w, nv = dh > c->h;
    if (nh && !nv) nv = dh > c->h - sb;
    if (nv && !nh) nh = dw > c->w - sb;
    *vw = c->w - (nv ? sb : 0);
    *vh = c->h - (nh ? sb : 0);
    if (need_h) *need_h = nh;
    if (need_v) *need_v = nv;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Scroll back in range, and the bars told where everything is.
static void layout(struct uui_paintcanvas *c) {
    int vw, vh, nh, nv;
    view(c, &vw, &vh, &nh, &nv);
    int dw = cont_w(c), dh = cont_h(c);
    c->sx = dw > vw ? clampi(c->sx, 0, dw - vw) : 0;
    c->sy = dh > vh ? clampi(c->sy, 0, dh - vh) : 0;
    uui_sbar_place(&c->vbar, c->x + c->w - uui_sbar_width(), c->y, nv ? vh : 0);
    uui_sbar_place_h(&c->hbar, c->x, c->y + c->h - uui_sbar_width(), nh ? vw : 0);
    uui_sbar_set(&c->vbar, dh, vh, c->sy);
    uui_sbar_set(&c->hbar, dw, vw, c->sx);
}

// The picture's top-left on screen: centred when it is smaller than the
// view, scrolled when it is not.
static void origin(const struct uui_paintcanvas *c, int *ox, int *oy) {
    int vw, vh;
    view(c, &vw, &vh, 0, 0);
    // Centred while the picture and its margin fit; scrolled from the
    // picture's top-left once they do not.
    *ox = c->x + (cont_w(c) <= vw ? (vw - pic_w(c)) / 2 : -c->sx);
    *oy = c->y + (cont_h(c) <= vh ? (vh - pic_h(c)) / 2 : -c->sy);
}

void uui_paintcanvas_init(struct uui_paintcanvas *c, struct uimg *img) {
    *c = (struct uui_paintcanvas){ 0 };
    c->img = img;
    c->zoom = 100;
    c->cursor = WIN_CURSOR_CROSSHAIR;
    uui_sbar_init(&c->vbar);
    uui_sbar_init(&c->hbar);
}

void uui_paintcanvas_picture_changed(struct uui_paintcanvas *c) { layout(c); }

static long long floor_div(long long a, long long b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int uui_paintcanvas_to_picture(const struct uui_paintcanvas *c, int cx, int cy, int *x, int *y) {
    int ox, oy;
    origin(c, &ox, &oy);
    int px = (int)floor_div((long long)(cx - ox) * 100, c->zoom);
    int py = (int)floor_div((long long)(cy - oy) * 100, c->zoom);
    if (x) *x = px;
    if (y) *y = py;
    return c->img && c->img->px && px >= 0 && py >= 0 && px < c->img->w && py < c->img->h;
}

void uui_paintcanvas_to_screen(const struct uui_paintcanvas *c, int x, int y, int *cx, int *cy) {
    int ox, oy;
    origin(c, &ox, &oy);
    if (cx) *cx = ox + (int)floor_div((long long)x * c->zoom, 100);
    if (cy) *cy = oy + (int)floor_div((long long)y * c->zoom, 100);
}

void uui_paintcanvas_picture_rect(const struct uui_paintcanvas *c, int *x, int *y, int *w, int *h) {
    origin(c, x, y);
    *w = pic_w(c);
    *h = pic_h(c);
}

void uui_paintcanvas_set_zoom(struct uui_paintcanvas *c, int zoom, int ax, int ay) {
    zoom = clampi(zoom, UUI_PAINT_ZOOM_MIN, UUI_PAINT_ZOOM_MAX);
    if (!c->img || !c->img->px || zoom == c->zoom) { c->zoom = zoom; layout(c); return; }
    int vw, vh;
    view(c, &vw, &vh, 0, 0);
    if (ax < 0) { ax = c->x + vw / 2; ay = c->y + vh / 2; }
    // The picture point under the anchor, in 1/100 pixels so a zoom out
    // and back lands where it began.
    int ox, oy;
    origin(c, &ox, &oy);
    long long fx = (long long)(ax - ox) * 10000 / c->zoom, fy = (long long)(ay - oy) * 10000 / c->zoom;
    c->zoom = zoom;
    c->sx = (int)(fx * zoom / 10000) - (ax - c->x);
    c->sy = (int)(fy * zoom / 10000) - (ay - c->y);
    layout(c);
}

int uui_paintcanvas_zoom_step(int zoom, int dir) {
    if (dir > 0) {
        for (int i = 0; i < NZOOMS; i++) if (ZOOMS[i] > zoom) return ZOOMS[i];
        return ZOOMS[NZOOMS - 1];
    }
    for (int i = NZOOMS - 1; i >= 0; i--) if (ZOOMS[i] < zoom) return ZOOMS[i];
    return ZOOMS[0];
}

int uui_paintcanvas_fit_zoom(const struct uui_paintcanvas *c) {
    if (!c->img || !c->img->px || c->w <= 0 || c->h <= 0) return 100;
    int zw = (int)((long long)(c->w - 8 - margin(c)) * 100 / c->img->w);
    int zh = (int)((long long)(c->h - 8 - margin(c)) * 100 / c->img->h);
    int z = zw < zh ? zw : zh;
    return clampi(z, UUI_PAINT_ZOOM_MIN, 100);
}

// --- the edge handles ------------------------------------------------------

static int handle_px(void) { int n = ugfx_char_h() / 2; return n < 6 ? 6 : n; }

// Where handle `h` (1 right, 2 bottom, 3 corner) sits, centred on its
// edge just outside the picture. 0 when the canvas has no handles.
static int handle_rect(const struct uui_paintcanvas *c, int h, int *x, int *y) {
    if (!c->on_resize || !c->img || !c->img->px) return 0;
    int ox, oy, n = handle_px();
    origin(c, &ox, &oy);
    int dw = pic_w(c), dh = pic_h(c);
    *x = h == 2 ? ox + dw / 2 - n / 2 : ox + dw + 1;
    *y = h == 1 ? oy + dh / 2 - n / 2 : oy + dh + 1;
    return 1;
}

static int handle_at(const struct uui_paintcanvas *c, int cx, int cy) {
    for (int h = 3; h >= 1; h--) {
        int x, y, n = handle_px();
        // A little slack round each: a 6 px square is a small target.
        if (handle_rect(c, h, &x, &y) && uui_hit(x - 3, y - 3, n + 6, n + 6, cx, cy)) return h;
    }
    return 0;
}

// The size a handle dragged to (cx, cy) makes, in picture pixels.
static void handle_size(const struct uui_paintcanvas *c, int cx, int cy, int *w, int *h) {
    int ox, oy;
    origin(c, &ox, &oy);
    *w = c->img->w;
    *h = c->img->h;
    // From the GRAB, not the pointer: the handle sits past the edge, and
    // a size read off the pointer jumped by that much on the first move.
    if (c->handle != 2) *w = (int)((long long)(cx - c->grab_dx - ox) * 100 / c->zoom);
    if (c->handle != 1) *h = (int)((long long)(cy - c->grab_dy - oy) * 100 / c->zoom);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (*w > 8192) *w = 8192;
    if (*h > 8192) *h = 8192;
}

static void draw_handles(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    int n = handle_px();
    for (int h = 1; h <= 3; h++) {
        int x, y;
        if (!handle_rect(c, h, &x, &y)) return;
        ugfx_fill_rect(s, x, y, n, n, UTHEME_ACCENT);
        ugfx_fill_rect(s, x + 1, y + 1, n - 2, n - 2, UTHEME_WHITE);
    }
    if (!c->handle) return;
    // THE NEW SIZE, dashed, with its numbers beside the corner.
    int ox, oy;
    origin(c, &ox, &oy);
    int w = zoomed(c->drag_w, c->zoom), h = zoomed(c->drag_h, c->zoom);
    for (int i = 0; i < w; i += 6) {
        ugfx_fill_rect(s, ox + i, oy - 1, i + 3 < w ? 3 : w - i, 1, UTHEME_ACCENT);
        ugfx_fill_rect(s, ox + i, oy + h, i + 3 < w ? 3 : w - i, 1, UTHEME_ACCENT);
    }
    for (int i = 0; i < h; i += 6) {
        ugfx_fill_rect(s, ox - 1, oy + i, 1, i + 3 < h ? 3 : h - i, UTHEME_ACCENT);
        ugfx_fill_rect(s, ox + w, oy + i, 1, i + 3 < h ? 3 : h - i, UTHEME_ACCENT);
    }
    char t[32];
    snprintf(t, sizeof t, "%d \xd7 %d px", c->drag_w, c->drag_h);
    int tw = ugfx_text_width(t) + 12, th = ugfx_char_h() + 6;
    int bx = ox + w + 6, by = oy + h + 6;
    uui_fill_round_rect(s, bx, by, tw, th, uui_control_radius(0), UTHEME_ACCENT);
    ugfx_draw_string_clipped(s, bx + 6, by + 3, tw - 12, t, UTHEME_ACCENT_TEXT, UTHEME_ACCENT);
}

// --- drawing -------------------------------------------------------------

static uint32_t ground(void) { return ugfx_blend(UTHEME_PANEL_BG, UTHEME_TEXT, 28); }

// One screen row of the picture, built then blitted: the column map is
// worked out once per paint, and each row is a lookup and, only where a
// pixel is not opaque, a blend over the checkerboard.
static uint32_t *g_row;
static int *g_cols;
static int g_cap;

static int grow(int n) {
    if (n <= g_cap) return 1;
    uint32_t *r = realloc(g_row, sizeof *r * (size_t)n);
    if (!r) return 0;
    g_row = r;
    int *k = realloc(g_cols, sizeof *k * (size_t)n);
    if (!k) return 0;
    g_cols = k;
    g_cap = n;
    return 1;
}

static uint32_t over_check(uint32_t px, int sx, int sy) {
    unsigned a = px >> 24;
    uint32_t chk = ((sx >> 3) + (sy >> 3)) & 1 ? 0xCCCCCC : 0xFFFFFF;
    if (a == 255) return px & 0xFFFFFF;
    if (a == 0) return chk;
    return ugfx_blend(chk, px & 0xFFFFFF, (uint8_t)a);
}

static void pc_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_paintcanvas *c = w;
    int vw, vh;
    view(c, &vw, &vh, 0, 0);
    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, ground());
    const struct uimg *im = c->img;
    if (im && im->px) {
        int ox, oy, dw = pic_w(c), dh = pic_h(c);
        origin(c, &ox, &oy);
        int c0 = ox > c->x ? ox : c->x, c1 = ox + dw < c->x + vw ? ox + dw : c->x + vw;
        int r0 = oy > c->y ? oy : c->y, r1 = oy + dh < c->y + vh ? oy + dh : c->y + vh;
        if (c1 > c0 && r1 > r0 && grow(c1 - c0)) {
            for (int i = c0; i < c1; i++) {
                int px = (int)((long long)(i - ox) * 100 / c->zoom);
                g_cols[i - c0] = px < im->w ? px : im->w - 1;
            }
            int last_py = -1;
            for (int r = r0; r < r1; r++) {
                int py = (int)((long long)(r - oy) * 100 / c->zoom);
                if (py >= im->h) py = im->h - 1;
                // A row that maps to the same picture row as the last, at
                // a zoom over 100%, is the same pixels -- unless a
                // checkerboard square changes under it.
                if (py != last_py || ((r - oy) & 7) == 0) {
                    const uint32_t *src = im->px + (size_t)py * im->w;
                    for (int i = 0; i < c1 - c0; i++)
                        g_row[i] = over_check(src[g_cols[i]], i + c0 - ox, r - oy);
                    last_py = py;
                }
                ugfx_blit(s, c0, r, c1 - c0, 1, g_row, c1 - c0);
            }
        }
        // An edge, so a white picture reads against a light ground.
        if (ox - 1 >= c->x) ugfx_fill_rect(s, ox - 1, r0, 1, r1 - r0, UTHEME_OUTLINE);
        if (ox + dw < c->x + vw) ugfx_fill_rect(s, ox + dw, r0, 1, r1 - r0, UTHEME_OUTLINE);
        if (oy - 1 >= c->y) ugfx_fill_rect(s, c0, oy - 1, c1 - c0, 1, UTHEME_OUTLINE);
        if (oy + dh < c->y + vh) ugfx_fill_rect(s, c0, oy + dh, c1 - c0, 1, UTHEME_OUTLINE);
    }
    ugfx_set_clip_rect(s, c->x, c->y, vw, vh);
    if (c->draw_over) c->draw_over(c->ctx, s, c);
    draw_handles(s, c);
    ugfx_clear_clip_rect(s);
    uui_sbar_draw(&c->vbar, s, ground(), UTHEME_TEXT);
    uui_sbar_draw(&c->hbar, s, ground(), UTHEME_TEXT);
}

// --- input -----------------------------------------------------------------

static int in_view(const struct uui_paintcanvas *c, int cx, int cy) {
    int vw, vh;
    view(c, &vw, &vh, 0, 0);
    return uui_hit(c->x, c->y, vw, vh, cx, cy);
}

static int bars(struct uui_paintcanvas *c, int r) {
    if (r & UUI_SBAR_MOVED) { c->sy = c->vbar.top; c->sx = c->hbar.top; layout(c); }
    return r;
}

static void report(struct uui_paintcanvas *c, enum uui_paint_phase ph, int cx, int cy) {
    int x, y;
    c->hovering = uui_paintcanvas_to_picture(c, cx, cy, &x, &y) || ph != UUI_PAINT_HOVER;
    c->hover_x = x;
    c->hover_y = y;
    if (c->on_pointer) c->on_pointer(c->ctx, ph, x, y, uapp_keyboard_mods());
}

static int pc_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_paintcanvas *c = w;
    layout(c);
    int r = bars(c, uui_sbar_press(&c->vbar, cx, cy));
    if (!r) r = bars(c, uui_sbar_press(&c->hbar, cx, cy));
    if (r & UUI_SBAR_TOOK) { c->bar_held = 1; return 1; }
    if (!in_view(c, cx, cy)) return 1;
    if ((c->handle = handle_at(c, cx, cy)) != 0) {
        int ox, oy;
        origin(c, &ox, &oy);
        c->grab_dx = cx - (ox + pic_w(c));
        c->grab_dy = cy - (oy + pic_h(c));
        handle_size(c, cx, cy, &c->drag_w, &c->drag_h);
        return 1;
    }
    c->painting = 1;
    report(c, UUI_PAINT_DOWN, cx, cy);
    return 1;
}

static int pc_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_paintcanvas *c = w;
    if (c->bar_held) {
        bars(c, uui_sbar_motion(&c->vbar, cx, cy, buttons));
        bars(c, uui_sbar_motion(&c->hbar, cx, cy, buttons));
        return 1;
    }
    if (c->handle) { handle_size(c, cx, cy, &c->drag_w, &c->drag_h); return 1; }
    if (c->painting) { report(c, UUI_PAINT_MOVE, cx, cy); return 1; }
    int r = bars(c, uui_sbar_motion(&c->vbar, cx, cy, buttons)) |
            bars(c, uui_sbar_motion(&c->hbar, cx, cy, buttons));
    int was = c->hovering, ox = c->hover_x, oy = c->hover_y;
    if (in_view(c, cx, cy) && !(r & UUI_SBAR_TOOK)) report(c, UUI_PAINT_HOVER, cx, cy);
    else if (c->hovering) { c->hovering = 0; if (c->on_pointer) c->on_pointer(c->ctx, UUI_PAINT_HOVER, ox, oy, 0); }
    return (r & UUI_SBAR_REDRAW) || was != c->hovering || ox != c->hover_x || oy != c->hover_y;
}

static int pc_release(void *w, int cx, int cy) {
    struct uui_paintcanvas *c = w;
    if (c->bar_held) {
        c->bar_held = 0;
        bars(c, uui_sbar_release(&c->vbar));
        bars(c, uui_sbar_release(&c->hbar));
        return 1;
    }
    if (c->handle) {
        handle_size(c, cx, cy, &c->drag_w, &c->drag_h);
        c->handle = 0;
        if (c->img && (c->drag_w != c->img->w || c->drag_h != c->img->h))
            c->on_resize(c->ctx, c->drag_w, c->drag_h);
        return 1;
    }
    if (!c->painting) return 0;
    c->painting = 0;
    report(c, UUI_PAINT_UP, cx, cy);
    return 1;
}

static int pc_wheel(void *w, int notches) {
    struct uui_paintcanvas *c = w;
    unsigned mods = uapp_keyboard_mods();
    if (mods & KEY_MOD_CTRL) {
        int ax = -1, ay = -1;
        if (c->hovering) uui_paintcanvas_to_screen(c, c->hover_x, c->hover_y, &ax, &ay);
        uui_paintcanvas_set_zoom(c, uui_paintcanvas_zoom_step(c->zoom, notches > 0 ? 1 : -1), ax, ay);
        if (c->on_pointer) c->on_pointer(c->ctx, UUI_PAINT_HOVER, c->hover_x, c->hover_y, mods);
        return 1;
    }
    int step = ugfx_char_h() * 3 * notches;
    if (mods & KEY_MOD_SHIFT) c->sx -= step;
    else c->sy -= step;
    layout(c);
    return 1;
}

static int pc_cursor(const void *w, int cx, int cy) {
    const struct uui_paintcanvas *c = w;
    int h = c->handle ? c->handle : handle_at(c, cx, cy);
    if (h) return h == 1 ? WIN_CURSOR_RESIZE_H : h == 2 ? WIN_CURSOR_RESIZE_V : WIN_CURSOR_MOVE;
    return in_view(c, cx, cy) && !uui_sbar_hit(&c->vbar, cx, cy) && !uui_sbar_hit(&c->hbar, cx, cy)
               ? c->cursor : WIN_CURSOR_DEFAULT;
}

static void pc_natural(const void *w, int *ow, int *oh) { (void)w; *ow = 0; *oh = 0; }

static void pc_geometry(void *w, int x, int y, int width, int height) {
    struct uui_paintcanvas *c = w;
    c->x = x; c->y = y; c->w = width; c->h = height;
    layout(c);
}

static void pc_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_paintcanvas *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

// A WIDGET WITH SCROLLBARS ANSWERS ITS WHOLE RECT (userland/CLAUDE.md).
static int pc_hit(const void *w, int cx, int cy) {
    const struct uui_paintcanvas *c = w;
    return uui_hit(c->x, c->y, c->w, c->h, cx, cy);
}

static void pc_describe(const void *w, const struct uui_describe *d) {
    const struct uui_paintcanvas *c = w;
    int x, y, pw, ph;
    uui_paintcanvas_picture_rect(c, &x, &y, &pw, &ph);
    uui_describe_rect(d, "picture", x, y, pw, ph);
    uui_describe_int(d, "zoom", c->zoom);
    for (int h = 1; h <= 3; h++) {
        int hx, hy;
        if (handle_rect(c, h, &hx, &hy)) uui_describe_rect_i(d, "handle", h, hx, hy, handle_px(), handle_px());
    }
}

const struct uui_widget_ops uui_paintcanvas_ops = {
    .natural_size = pc_natural,
    .set_geometry = pc_geometry,
    .bounds       = pc_bounds,
    .draw         = pc_draw,
    .hit          = pc_hit,
    .press        = pc_press,
    .motion       = pc_motion,
    .release      = pc_release,
    .wheel        = pc_wheel,
    .cursor       = pc_cursor,
    .describe     = pc_describe,
};
