// A site's permission, as a row and as an ask: see uui_siteperm.h.
#include <stdio.h>
#include <string.h>

#include "ui/uui_siteperm.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"
#include "lib/usiteperm.h"
#include "win_proto.h"   // WIN_CURSOR_*

static int gap(void) { return ugfx_char_advance('n') + 4; }

// --- the row ------------------------------------------------------------

void uui_siteperm_init(struct uui_siteperm *p, const char *label, int value, int default_on) {
    memset(p, 0, sizeof *p);
    p->label = label;
    snprintf(p->dflt, sizeof p->dflt, "Default (%s)", default_on ? "On" : "Off");
    p->opts[USITEPERM_DEFAULT] = p->dflt;
    p->opts[USITEPERM_ALLOW] = "Allow";
    p->opts[USITEPERM_BLOCK] = "Block";
    uui_segmented_init(&p->seg, p->opts, 3, value);
}

int uui_siteperm_value(const struct uui_siteperm *p) {
    return p->seg.selected < 0 ? USITEPERM_DEFAULT : p->seg.selected;
}

static int caption_of(const struct uui_siteperm *p, int caption_w) {
    return caption_w > 0 ? caption_w : ugfx_text_width(p->label ? p->label : "") + gap();
}

void uui_siteperm_natural_size(const struct uui_siteperm *p, int caption_w, int *ow, int *oh) {
    int sw, sh;
    uui_segmented_natural_size(&p->seg, &sw, &sh);
    *ow = caption_of(p, caption_w) + sw;
    *oh = sh;
}

void uui_siteperm_set_geometry(struct uui_siteperm *p, int x, int y, int caption_w) {
    p->caption_w = caption_of(p, caption_w);
    uui_siteperm_natural_size(p, p->caption_w, &p->w, &p->h);
    p->x = x;
    p->y = y;
    uui_segmented_set_geometry(&p->seg, x + p->caption_w, y);
}

void uui_siteperm_draw(struct ugfx_surface *s, const struct uui_siteperm *p, uint32_t bg) {
    ugfx_draw_string_clipped(s, p->x, p->y + (p->h - ugfx_char_h()) / 2, p->caption_w - 4,
                             p->label ? p->label : "", utheme_current()->text_muted, bg);
    uui_segmented_draw(s, &p->seg);
}

static void r_natural(const void *w, int *ow, int *oh) { uui_siteperm_natural_size(w, 0, ow, oh); }
static void r_geometry(void *w, int x, int y, int width, int height) {
    (void)width; (void)height;   // its size is its own
    uui_siteperm_set_geometry(w, x, y, 0);
}
static void r_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_siteperm *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}
static void r_draw(struct ugfx_surface *s, const void *w) { uui_siteperm_draw(s, w, UTHEME_WINDOW_BG); }
static int r_hit(const void *w, int cx, int cy) {
    const struct uui_siteperm *p = w;
    return uui_segmented_at(&p->seg, cx, cy) >= 0;
}
static int r_press(void *w, int cx, int cy, unsigned mods) {
    return uui_segmented_ops.press(&((struct uui_siteperm *)w)->seg, cx, cy, mods);
}
static int r_motion(void *w, int cx, int cy, unsigned buttons) {
    return uui_segmented_ops.motion(&((struct uui_siteperm *)w)->seg, cx, cy, buttons);
}
static int r_release(void *w, int cx, int cy) {
    return uui_segmented_ops.release(&((struct uui_siteperm *)w)->seg, cx, cy);
}
static int r_key(void *w, int key, unsigned mods) {
    return uui_segmented_ops.key(&((struct uui_siteperm *)w)->seg, key, mods);
}
static void r_set_focused(void *w, int f) { ((struct uui_siteperm *)w)->seg.focused = f; }
static int r_accepts_focus(const void *w) {
    return uui_segmented_ops.accepts_focus(&((const struct uui_siteperm *)w)->seg);
}
static void r_describe(const void *w, const struct uui_describe *d) {
    uui_segmented_ops.describe(&((const struct uui_siteperm *)w)->seg, d);
}

const struct uui_widget_ops uui_siteperm_ops = {
    .natural_size  = r_natural,
    .set_geometry  = r_geometry,
    .bounds        = r_bounds,
    .draw          = r_draw,
    .hit           = r_hit,
    .press         = r_press,
    .motion        = r_motion,
    .release       = r_release,
    .key           = r_key,
    .set_focused   = r_set_focused,
    .accepts_focus = r_accepts_focus,
    .describe      = r_describe,
};

// --- the ask ------------------------------------------------------------

static int line_h(void) { return ugfx_char_h() + 6; }

void uui_permask_open(struct uui_permask *a, const char *title, const char *line1,
                      const char *line2, const char *secondary, const char *primary,
                      int right, int top, int bound_w, int bound_h) {
    memset(a, 0, sizeof *a);
    snprintf(a->title, sizeof a->title, "%s", title ? title : "");
    snprintf(a->line[0], sizeof a->line[0], "%s", line1 ? line1 : "");
    snprintf(a->line[1], sizeof a->line[1], "%s", line2 ? line2 : "");
    snprintf(a->secondary, sizeof a->secondary, "%s", secondary ? secondary : "");
    snprintf(a->primary, sizeof a->primary, "%s", primary ? primary : "");
    int p = gap(), w = ugfx_text_width(a->title);
    for (int i = 0; i < 2; i++)
        if (ugfx_text_width(a->line[i]) > w) w = ugfx_text_width(a->line[i]);
    a->bw[0] = a->secondary[0] ? ugfx_text_width(a->secondary) + 2 * p : 0;
    a->bw[1] = ugfx_text_width(a->primary) + 2 * p;
    a->bh = line_h() + 4;
    int buttons = a->bw[0] + (a->bw[0] ? p / 2 : 0) + a->bw[1];
    if (buttons > w) w = buttons;
    int min = ugfx_char_advance('n') * 30;
    if (w < min) w = min;
    a->w = w + 2 * p;
    if (a->w > bound_w - 8) a->w = bound_w - 8;
    int lines = 1 + (a->line[0][0] != 0) + (a->line[1][0] != 0);
    a->h = p + lines * line_h() + p / 2 + a->bh + p;
    a->x = right - a->w;
    a->y = top;
    if (a->x + a->w > bound_w - 4) a->x = bound_w - 4 - a->w;
    if (a->y + a->h > bound_h - 4) a->y = bound_h - 4 - a->h;
    if (a->x < 4) a->x = 4;
    if (a->y < 4) a->y = 4;
    a->by = a->y + a->h - p - a->bh;
    a->bx[1] = a->x + a->w - p - a->bw[1];
    a->bx[0] = a->bx[1] - p / 2 - a->bw[0];
    a->open = 1;
}

void uui_permask_close(struct uui_permask *a) {
    if (a->open && !a->event) a->event = UUI_PERMASK_CLOSED;
    a->open = 0;
}

int uui_permask_take(struct uui_permask *a) {
    int e = a->event;
    a->event = UUI_PERMASK_NONE;
    return e;
}

// The button under (cx, cy): 1 secondary, 2 primary, 0 none.
static int button_at(const struct uui_permask *a, int cx, int cy) {
    for (int i = 0; i < 2; i++)
        if (a->bw[i] && uui_hit(a->bx[i], a->by, a->bw[i], a->bh, cx, cy)) return i + 1;
    return 0;
}

static void a_natural(const void *w, int *ow, int *oh) { (void)w; *ow = 0; *oh = 0; }
// Placed by uui_permask_open(), never by a layout.
static void a_geometry(void *w, int x, int y, int width, int height) {
    (void)w; (void)x; (void)y; (void)width; (void)height;
}
static void a_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_permask *a = w;
    *x = a->x; *y = a->y;
    *ow = a->open ? a->w : 0;
    *oh = a->open ? a->h : 0;
}

static void a_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_permask *a = w;
    if (!a->open) return;
    int r = uui_popup_radius(), p = gap();
    uint32_t bg = uui_popup_bg();
    uui_fill_round_rect(s, a->x, a->y, a->w, a->h, r, uui_popup_border());
    uui_fill_round_rect(s, a->x + 1, a->y + 1, a->w - 2, a->h - 2, r > 1 ? r - 1 : 0, bg);
    int x = a->x + p, y = a->y + p + 3, max = a->w - 2 * p;
    ugfx_draw_string_elided(s, x, y, max, a->title, UTHEME_TEXT, bg);
    ugfx_draw_string_elided(s, x + 1, y, max - 1, a->title, UTHEME_TEXT, bg);   // bold, as the card's host
    for (int i = 0; i < 2; i++) {
        if (!a->line[i][0]) continue;
        y += line_h();
        ugfx_draw_string_elided(s, x, y, max, a->line[i], utheme_current()->text_muted, bg);
    }
    for (int i = 0; i < 2; i++) {
        if (!a->bw[i]) continue;
        uint32_t base = i ? UTHEME_ACCENT : UTHEME_BUTTON_BG;
        enum uui_state st = a->armed == i + 1 ? UUI_STATE_PRESSED
                          : a->hot == i + 1 ? UUI_STATE_HOVER : UUI_STATE_REST;
        uint32_t fill = st == UUI_STATE_REST ? base : uui_state_bg(base, st);
        uui_fill_round_rect(s, a->bx[i], a->by, a->bw[i], a->bh, uui_control_radius(4), fill);
        const char *t = i ? a->primary : a->secondary;
        ugfx_draw_string(s, a->bx[i] + p, a->by + (a->bh - ugfx_char_h()) / 2, t,
                         i ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, fill);
    }
}

static int a_hit(const void *w, int cx, int cy) {
    const struct uui_permask *a = w;
    return a->open && uui_hit(a->x, a->y, a->w, a->h, cx, cy);
}

// Offered EVERY press while open (overlay_active). One outside closes the
// ask and is declined, so it reaches what is under it.
static int a_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_permask *a = w;
    if (!a->open) return 0;
    if (!a_hit(a, cx, cy)) {
        uui_permask_close(a);
        return 0;
    }
    a->armed = button_at(a, cx, cy);
    return 1;
}

static int a_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_permask *a = w;
    int hot = a->open ? button_at(a, cx, cy) : 0;
    if (hot == a->hot) return 0;
    a->hot = hot;
    return 1;
}

static int a_release(void *w, int cx, int cy) {
    struct uui_permask *a = w;
    int b = a->armed && button_at(a, cx, cy) == a->armed ? a->armed : 0;
    a->armed = 0;
    if (b) {
        a->event = b == 1 ? UUI_PERMASK_SECONDARY : UUI_PERMASK_PRIMARY;
        a->open = 0;
    }
    return 1;
}

static int a_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_permask *a = w;
    if (!a->open || key != 0x1B) return 0;
    uui_permask_close(a);
    return 1;
}

static int a_accepts_focus(const void *w) { (void)w; return 0; }
static int a_overlay(const void *w) { return ((const struct uui_permask *)w)->open; }
static int a_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

static void a_describe(const void *w, const struct uui_describe *d) {
    const struct uui_permask *a = w;
    uui_describe_int(d, "open", a->open);
    if (!a->open) return;
    if (a->bw[0]) uui_describe_rect(d, "secondary", a->bx[0], a->by, a->bw[0], a->bh);
    uui_describe_rect(d, "primary", a->bx[1], a->by, a->bw[1], a->bh);
}

const struct uui_widget_ops uui_permask_ops = {
    .natural_size   = a_natural,
    .set_geometry   = a_geometry,
    .bounds         = a_bounds,
    .draw           = a_draw,
    .hit            = a_hit,
    .press          = a_press,
    .motion         = a_motion,
    .release        = a_release,
    .key            = a_key,
    .accepts_focus  = a_accepts_focus,
    .overlay_active = a_overlay,
    .cursor         = a_cursor,
    .describe       = a_describe,
};
