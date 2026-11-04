// colour square and hue strip -- see ui/uui_colourfield.h.
#include "ui/uui_colourfield.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include <stdlib.h>

void uui_colourfield_init(struct uui_colourfield *f, enum uui_colourfield_kind kind, struct ucolour_hsv *hsv) {
    *f = (struct uui_colourfield){ .kind = kind, .hsv = hsv, .cache_hue = -1 };
}

void uui_colourfield_free(struct uui_colourfield *f) {
    free(f->cache);
    f->cache = 0;
    f->cache_w = f->cache_h = 0;
}

// The marker's size, and how far the square keeps its picture in from
// the edge so the marker is never cut off: FONT-DERIVED.
static int marker(void) {
    int m = ugfx_char_h() * 2 / 3;
    return m < 8 ? 8 : m;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// --- the square's picture --------------------------------------------------

// Transparent outside a rounded corner of radius r, so ugfx_blit_alpha
// leaves what is under it.
static int outside_corner(int x, int y, int w, int h, int r) {
    if (r <= 0) return 0;
    int cx = x < r ? r : x >= w - r ? w - r - 1 : -1;
    int cy = y < r ? r : y >= h - r ? h - r - 1 : -1;
    if (cx < 0 || cy < 0) return 0;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy > r * r;
}

static void build_cache(struct uui_colourfield *f, int w, int h) {
    if (f->cache && f->cache_w == w && f->cache_h == h && f->cache_hue == f->hsv->h) return;
    if (!f->cache || f->cache_w != w || f->cache_h != h) {
        free(f->cache);
        f->cache = malloc((size_t)w * h * 4);
        if (!f->cache) { f->cache_w = f->cache_h = 0; return; }
        f->cache_w = w;
        f->cache_h = h;
    }
    f->cache_hue = f->hsv->h;
    int r = uui_control_radius(1);
    struct ucolour_hsv c = { f->hsv->h, 0, 0 };
    for (int y = 0; y < h; y++) {
        c.v = h > 1 ? 1.0f - (float)y / (float)(h - 1) : 1.0f;
        for (int x = 0; x < w; x++) {
            c.s = w > 1 ? (float)x / (float)(w - 1) : 0.0f;
            f->cache[(size_t)y * w + x] = outside_corner(x, y, w, h, r) ? 0 : ucolour_from_hsv(&c) | 0xFF000000u;
        }
    }
}

// --- geometry ----------------------------------------------------------------

static void cf_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_colourfield *f = w;
    int ch = ugfx_char_h();
    if (out_w) *out_w = ch * 10;
    if (out_h) *out_h = f->kind == UUI_CF_SQUARE ? ch * 6 : marker() + 4;
}

static void cf_geometry(void *w, int x, int y, int width, int height) {
    struct uui_colourfield *f = w;
    f->x = x; f->y = y; f->w = width; f->h = height;
}

static void cf_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_colourfield *f = w;
    if (x) *x = f->x;
    if (y) *y = f->y;
    if (out_w) *out_w = f->w;
    if (out_h) *out_h = f->h;
}

// The strip's thumb, and the band's run between its two half-thumbs, so
// the thumb at either end stays inside the control.
static int thumb_w(void) {
    int t = marker() / 2;
    return t < 6 ? 6 : t;
}
static int run_x0(const struct uui_colourfield *f) { return f->x + thumb_w() / 2 + 1; }
static int run_w(const struct uui_colourfield *f) { return f->w - 2 * (thumb_w() / 2 + 1); }

// The strip's band: a little less tall than the control, so its thumb
// stands out above and below it.
static void band(const struct uui_colourfield *f, int *by, int *bh) {
    *bh = f->h * 2 / 3;
    if (*bh < 4) *bh = f->h;
    *by = f->y + (f->h - *bh) / 2;
}

// --- drawing -------------------------------------------------------------------

static void ring(struct ugfx_surface *s, int cx, int cy, int d) {
    // White inside dark, so it reads on every colour under it.
    int r = d / 2;
    uui_stroke_round_rect(s, cx - r - 1, cy - r - 1, d + 2, d + 2, r + 1, 0x141414);
    uui_stroke_round_rect(s, cx - r, cy - r, d, d, r, 0xFFFFFF);
    uui_stroke_round_rect(s, cx - r + 1, cy - r + 1, d - 2, d - 2, r - 1 > 0 ? r - 1 : 0, 0xFFFFFF);
}

static void cf_draw(struct ugfx_surface *s, const void *w) {
    struct uui_colourfield *f = (struct uui_colourfield *)w;   // the cache is ours to fill
    if (f->w < 4 || f->h < 4) return;
    if (f->kind == UUI_CF_SQUARE) {
        int iw = f->w - 2, ih = f->h - 2;
        build_cache(f, iw, ih);
        uui_stroke_round_rect(s, f->x, f->y, f->w, f->h, uui_control_radius(0), UTHEME_OUTLINE);
        if (f->cache) ugfx_blit_alpha(s, f->x + 1, f->y + 1, iw, ih, f->cache, iw);
        int mx = f->x + 1 + (int)(clamp01(f->hsv->s) * (float)(iw - 1) + 0.5f);
        int my = f->y + 1 + (int)((1.0f - clamp01(f->hsv->v)) * (float)(ih - 1) + 0.5f);
        ring(s, mx, my, marker());
        if (f->focused) uui_focus_ring(s, f->x - 2, f->y - 2, f->w + 4, f->h + 4);
        return;
    }
    int by, bh;
    band(f, &by, &bh);
    // One column per pixel: the hue at that x, fully saturated and bright.
    int x0 = run_x0(f), rw = run_w(f);
    if (rw < 2) return;
    struct ucolour_hsv c = { 0, 1, 1 };
    for (int x = 0; x < rw; x++) {
        c.h = 359.9f * (float)x / (float)(rw - 1);
        int inset = 0;
        // Round ends: a half-disc's worth trimmed off each end column.
        int r = bh / 2, d = x < r ? r - x : x >= rw - r ? x - (rw - r - 1) : 0;
        if (d > 0) {
            int k = r * r - d * d;
            int half = 0;
            while ((half + 1) * (half + 1) <= k) half++;
            inset = r - half;
        }
        if (bh - 2 * inset > 0) ugfx_fill_rect(s, x0 + x, by + inset, 1, bh - 2 * inset, ucolour_from_hsv(&c));
    }
    int tx = x0 + (int)(f->hsv->h / 359.9f * (float)(rw - 1) + 0.5f), tw = thumb_w();
    uui_fill_round_rect(s, tx - tw / 2 - 1, f->y, tw + 2, f->h, uui_control_radius(0), 0x141414);
    uui_fill_round_rect(s, tx - tw / 2, f->y + 1, tw, f->h - 2, uui_control_radius(1), 0xFFFFFF);
    if (f->focused) uui_focus_ring(s, f->x - 2, f->y - 2, f->w + 4, f->h + 4);
}

// --- input ------------------------------------------------------------------------

static int set_at(struct uui_colourfield *f, int cx, int cy) {
    struct ucolour_hsv was = *f->hsv;
    if (f->kind == UUI_CF_SQUARE) {
        int iw = f->w - 2, ih = f->h - 2;
        f->hsv->s = iw > 1 ? clamp01((float)(cx - f->x - 1) / (float)(iw - 1)) : 0;
        f->hsv->v = ih > 1 ? 1.0f - clamp01((float)(cy - f->y - 1) / (float)(ih - 1)) : 1;
    } else {
        int rw = run_w(f);
        f->hsv->h = rw > 1 ? 359.9f * clamp01((float)(cx - run_x0(f)) / (float)(rw - 1)) : 0;
    }
    return was.h != f->hsv->h || was.s != f->hsv->s || was.v != f->hsv->v;
}

static int cf_hit(const void *w, int cx, int cy) {
    const struct uui_colourfield *f = w;
    return uui_hit(f->x, f->y, f->w, f->h, cx, cy);
}

static int cf_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_colourfield *f = w;
    if (!cf_hit(f, cx, cy)) return 0;
    f->dragging = 1;
    set_at(f, cx, cy);
    return 1;
}

static int cf_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_colourfield *f = w;
    // A DRAG NEEDS THE BUTTON STILL DOWN, uui_scale's rule: the grab
    // outlives the button by a motion or two.
    if (!f->dragging || !(buttons & 0x1)) return 0;
    return set_at(f, cx, cy);
}

static int cf_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    ((struct uui_colourfield *)w)->dragging = 0;
    return 1;   // so the router names this widget to the app
}

// Arrows step one percent, or one degree along the strip; with nothing
// else to do, Page keys step ten.
static int cf_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_colourfield *f = w;
    float d = 0;
    int vertical = 0;
    switch (key) {
    case KEY_ARROW_LEFT:  d = -1; break;
    case KEY_ARROW_RIGHT: d = 1; break;
    case KEY_ARROW_UP:    d = 1; vertical = 1; break;
    case KEY_ARROW_DOWN:  d = -1; vertical = 1; break;
    case KEY_PAGE_UP:     d = 10; vertical = 1; break;
    case KEY_PAGE_DOWN:   d = -10; vertical = 1; break;
    default: return 0;
    }
    if (f->kind == UUI_CF_HUE) {
        float h = f->hsv->h + d;
        f->hsv->h = h < 0 ? 0 : h > 359.9f ? 359.9f : h;
    } else if (vertical) {
        f->hsv->v = clamp01(f->hsv->v + d / 100);
    } else {
        f->hsv->s = clamp01(f->hsv->s + d / 100);
    }
    return 1;
}

static int cf_accepts_focus(const void *w) { (void)w; return 1; }
static void cf_set_focused(void *w, int focused) { ((struct uui_colourfield *)w)->focused = focused; }

const struct uui_widget_ops uui_colourfield_ops = {
    .natural_size  = cf_natural,
    .set_geometry  = cf_geometry,
    .bounds        = cf_bounds,
    .draw          = cf_draw,
    .hit           = cf_hit,
    .press         = cf_press,
    .motion        = cf_motion,
    .release       = cf_release,
    .key           = cf_key,
    .accepts_focus = cf_accepts_focus,
    .set_focused   = cf_set_focused,
};
