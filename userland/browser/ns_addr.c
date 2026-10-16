// The address field's frame: security chip, the field, zoom badge, star.
// See ns_addr.h.
#include <stdio.h>
#include <string.h>

#include "browser/ns_addr.h"
#include "browser/ns_site.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "win_proto.h"   // WIN_CURSOR_*

static uint32_t mix(uint32_t a, uint32_t b, int pct_b) {
    int r = (int)((a >> 16) & 255) * (100 - pct_b) + (int)((b >> 16) & 255) * pct_b;
    int g = (int)((a >> 8) & 255) * (100 - pct_b) + (int)((b >> 8) & 255) * pct_b;
    int bl = (int)(a & 255) * (100 - pct_b) + (int)(b & 255) * pct_b;
    return ugfx_rgb(r / 100, g / 100, bl / 100);
}

static int pad(void) { return ugfx_char_advance('n') / 2 + 2; }
static int icon(void) { return ugfx_char_h() * 2 / 3; }

// What the chip says, by state: NULL text is an icon alone.
static const char *chip_text(int state) {
    switch (state) {
    case NS_SITE_INSECURE:
    case NS_SITE_OVERRIDE: return "Not secure";
    case NS_SITE_FILE:     return "File";
    case NS_SITE_INTERNAL: return "Web Browser";
    }
    return NULL;
}

static int chip_has_icon(int state) {
    return state == NS_SITE_SECURE || state == NS_SITE_ISSUES || state == NS_SITE_INSECURE ||
           state == NS_SITE_OVERRIDE;
}

void ns_addr_sync(struct ns_addr *a) {
    int p = pad(), gap = p / 2;
    memset(a->part_x, 0, sizeof a->part_x);
    memset(a->part_w, 0, sizeof a->part_w);
    int left = a->x + 3, right = a->x + a->w - 3;
    if (a->state != NS_SITE_NONE) {
        const char *t = chip_text(a->state);
        int w = 2 * p;
        if (chip_has_icon(a->state)) w += icon();
        if (t) w += ugfx_text_width(t) + (chip_has_icon(a->state) ? gap : 0);
        if (!t) w = icon() + 2 * gap;   // a lone lock is a small target, not a chip
        a->part_x[NS_ADDR_CHIP] = left;
        a->part_w[NS_ADDR_CHIP] = w;
        left += w + gap;
    }
    if (a->show_star) {
        int w = icon() + 2 * gap;
        right -= w;
        a->part_x[NS_ADDR_STAR] = right;
        a->part_w[NS_ADDR_STAR] = w;
        right -= gap;
    }
    if (a->zoom && a->zoom != 100) {
        char z[8];
        snprintf(z, sizeof z, "%d%%", a->zoom);
        int w = ugfx_text_width(z) + 2 * p;
        right -= w;
        a->part_x[NS_ADDR_ZOOM] = right;
        a->part_w[NS_ADDR_ZOOM] = w;
        right -= gap;
    }
    if (right < left + 1) right = left + 1;
    a->field.ops->set_geometry(a->field.widget, left, a->y + 1, right - left, a->h - 2);
}

void ns_addr_init(struct ns_addr *a, struct uui_textbox *field, int field_id, const char *field_name) {
    memset(a, 0, sizeof *a);
    field->bare = 1;
    field->frameless = 1;   // the frame and ground are this widget's, 2 px when focused
    a->field = (struct uui_item){ .ops = &uui_textbox_ops, .widget = field, .id = field_id,
                                  .name = field_name };
    a->zoom = 100;
}

int ns_addr_take(struct ns_addr *a) {
    int e = a->event;
    a->event = NS_ADDR_NONE;
    return e;
}

int ns_addr_part_rect(const struct ns_addr *a, int part, int *x, int *y, int *w, int *h) {
    if (part <= NS_ADDR_NONE || part >= NS_ADDR_PARTS || !a->part_w[part]) return 0;
    int ph = ugfx_char_h() + 4;
    *x = a->part_x[part];
    *y = a->y + (a->h - ph) / 2;
    *w = a->part_w[part];
    *h = ph;
    return 1;
}

static int part_at(const struct ns_addr *a, int cx, int cy) {
    if (cy < a->y || cy >= a->y + a->h) return NS_ADDR_NONE;
    for (int p = NS_ADDR_CHIP; p < NS_ADDR_PARTS; p++)
        if (a->part_w[p] && cx >= a->part_x[p] && cx < a->part_x[p] + a->part_w[p]) return p;
    return NS_ADDR_NONE;
}

// --- the icons -------------------------------------------------------------

static void draw_lock(struct ugfx_surface *s, int x, int cy, int sz, uint32_t c) {
    int bh = sz * 3 / 5, by = cy + sz / 2 - bh;
    ugfx_fill_rect(s, x, by, sz, bh, c);
    int t = sz >= 12 ? 2 : 1, inset = sz / 5, top = by - sz * 2 / 5;
    ugfx_fill_rect(s, x + inset, top, t, by - top, c);
    ugfx_fill_rect(s, x + sz - inset - t, top, t, by - top, c);
    ugfx_fill_rect(s, x + inset, top, sz - 2 * inset, t, c);
}

static void draw_warning(struct ugfx_surface *s, int x, int cy, int sz, uint32_t c, uint32_t bg) {
    int xs[3] = { x + sz / 2, x + sz, x }, ys[3] = { cy - sz / 2, cy + sz / 2, cy + sz / 2 };
    ugfx_fill_polygon(s, xs, ys, 3, c);
    int t = sz >= 12 ? 2 : 1;
    ugfx_fill_rect(s, x + sz / 2 - t / 2, cy - sz / 6, t, sz / 3, bg);
    ugfx_fill_rect(s, x + sz / 2 - t / 2, cy + sz / 3 - t, t, t, bg);
}

// A five-pointed star in a 20-unit box, scaled to `sz`.
static void draw_star(struct ugfx_surface *s, int x, int cy, int sz, uint32_t c, int filled) {
    static const int P[10][2] = { { 10, 1 }, { 13, 7 }, { 19, 8 }, { 14, 12 }, { 16, 19 },
                                  { 10, 15 }, { 4, 19 }, { 6, 12 }, { 1, 8 }, { 7, 7 } };
    int xs[10], ys[10], y0 = cy - sz / 2;
    for (int i = 0; i < 10; i++) {
        xs[i] = x + P[i][0] * sz / 20;
        ys[i] = y0 + P[i][1] * sz / 20;
    }
    if (filled) ugfx_fill_polygon(s, xs, ys, 10, c);
    else ugfx_draw_polyline(s, xs, ys, 10, 1, c, GEOM_AA);
}

// --- the widget ----------------------------------------------------------------

static void a_natural(const void *w, int *ow, int *oh) {
    const struct ns_addr *a = w;
    a->field.ops->natural_size(a->field.widget, ow, oh);
    *ow = 0;
    *oh += 2;
}

static void a_geometry(void *w, int x, int y, int width, int height) {
    struct ns_addr *a = w;
    a->x = x;
    a->y = y;
    a->w = width;
    a->h = height;
    ns_addr_sync(a);
}

static void a_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct ns_addr *a = w;
    *x = a->x;
    *y = a->y;
    *ow = a->w;
    *oh = a->h;
}

static int a_hit(const void *w, int cx, int cy) {
    const struct ns_addr *a = w;
    return uui_hit(a->x, a->y, a->w, a->h, cx, cy);
}

static struct uui_item *a_children(void *w, int *count) {
    *count = 1;
    return &((struct ns_addr *)w)->field;
}

static void draw_part_bg(struct ugfx_surface *s, const struct ns_addr *a, int part, uint32_t fill) {
    int x, y, w, h;
    if (!ns_addr_part_rect(a, part, &x, &y, &w, &h)) return;
    uint32_t bg = fill;
    if (a->hot == part || a->armed == part) bg = mix(fill, UTHEME_TEXT, a->armed == part ? 16 : 8);
    if (bg != UTHEME_WHITE) uui_fill_round_rect(s, x, y, w, h, h / 2, bg);
}

// The frame, chip and badges; the field draws itself over the middle.
static void a_begin(struct ugfx_surface *s, void *w) {
    struct ns_addr *a = w;
    const struct uui_textbox *f = a->field.widget;
    int r = uui_control_radius(4);
    uui_fill_round_rect(s, a->x, a->y, a->w, a->h, r, f->active ? UTHEME_ACCENT : UTHEME_OUTLINE);
    int t = f->active ? 2 : 1;
    uui_fill_round_rect(s, a->x + t, a->y + t, a->w - 2 * t, a->h - 2 * t, r > t ? r - t : 0, UTHEME_WHITE);

    int p = pad(), gap = p / 2, sz = icon(), cy = a->y + a->h / 2;
    uint32_t danger = utheme_action(UTHEME_ACT_DANGER);
    if (a->part_w[NS_ADDR_CHIP]) {
        int warn = a->state == NS_SITE_INSECURE || a->state == NS_SITE_OVERRIDE;
        uint32_t fill = warn ? mix(UTHEME_WHITE, danger, 12)
                      : chip_text(a->state) ? UTHEME_PANEL_BG : UTHEME_WHITE;
        uint32_t ink = warn ? danger : chip_text(a->state) ? utheme_current()->text_muted : UTHEME_TEXT;
        draw_part_bg(s, a, NS_ADDR_CHIP, fill);
        uint32_t under = (a->hot == NS_ADDR_CHIP || a->armed == NS_ADDR_CHIP)
                       ? mix(fill, UTHEME_TEXT, a->armed == NS_ADDR_CHIP ? 16 : 8) : fill;
        int x = a->part_x[NS_ADDR_CHIP] + (chip_text(a->state) ? p : gap);
        if (a->state == NS_SITE_SECURE) draw_lock(s, x, cy, sz, ink);
        else if (chip_has_icon(a->state)) draw_warning(s, x, cy, sz, warn ? danger : ink, under);
        if (chip_has_icon(a->state)) x += sz + gap;
        const char *text = chip_text(a->state);
        if (text) ugfx_draw_string(s, x, cy - ugfx_char_h() / 2, text, ink, under);
    }
    if (a->part_w[NS_ADDR_ZOOM]) {
        int x, y, w, h;
        ns_addr_part_rect(a, NS_ADDR_ZOOM, &x, &y, &w, &h);
        draw_part_bg(s, a, NS_ADDR_ZOOM, UTHEME_PANEL_BG);
        char z[8];
        snprintf(z, sizeof z, "%d%%", a->zoom);
        ugfx_draw_string(s, x + p, cy - ugfx_char_h() / 2, z, UTHEME_TEXT,
                         a->hot == NS_ADDR_ZOOM ? mix(UTHEME_PANEL_BG, UTHEME_TEXT, 8) : UTHEME_PANEL_BG);
    }
    if (a->part_w[NS_ADDR_STAR]) {
        draw_part_bg(s, a, NS_ADDR_STAR, UTHEME_WHITE);
        draw_star(s, a->part_x[NS_ADDR_STAR] + gap, cy, sz,
                  a->starred ? UTHEME_ACCENT : utheme_current()->text_muted, a->starred);
    }
}

static int a_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct ns_addr *a = w;
    a->armed = part_at(a, cx, cy);
    return a->armed != NS_ADDR_NONE;
}

static int a_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct ns_addr *a = w;
    int hot = part_at(a, cx, cy);
    if (hot == a->hot) return 0;
    a->hot = hot;
    return 1;
}

static int a_release(void *w, int cx, int cy) {
    struct ns_addr *a = w;
    int armed = a->armed;
    a->armed = NS_ADDR_NONE;
    if (armed && part_at(a, cx, cy) == armed) a->event = armed;
    return armed != NS_ADDR_NONE;
}

static int a_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

// The parts' rects, for a test that clicks the chip or reads its state.
static void a_describe(const void *w, const struct uui_describe *d) {
    const struct ns_addr *a = w;
    static const char *const NAMES[NS_ADDR_PARTS] = { 0, "chip", "zoom", "star" };
    int x, y, wd, h;
    for (int p = NS_ADDR_CHIP; p < NS_ADDR_PARTS; p++)
        if (ns_addr_part_rect(a, p, &x, &y, &wd, &h)) uui_describe_rect(d, NAMES[p], x, y, wd, h);
    uui_describe_int(d, "state", a->state);
}

const struct uui_widget_ops ns_addr_ops = {
    .natural_size   = a_natural,
    .set_geometry   = a_geometry,
    .bounds         = a_bounds,
    .hit            = a_hit,
    .press          = a_press,
    .motion         = a_motion,
    .release        = a_release,
    .cursor         = a_cursor,
    .children       = a_children,
    .children_begin = a_begin,
    .describe       = a_describe,
};
