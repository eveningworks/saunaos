// The Downloads flyout. See ns_dlpanel.h.
#include <string.h>

#include "browser/ns_dlpanel.h"
#include "browser/ns_download.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "win_proto.h"   // WIN_CURSOR_*

static const char *const LINK[3] = { 0, "Open Downloads", "Clear finished" };

static int pad(void) { return ugfx_char_advance('n') + 4; }
static int title_h(void) { return ugfx_char_h() + 2 * pad() / 2 + 6; }
static int foot_h(void) { return ugfx_char_h() + 14; }

void ns_dlpanel_init(struct ns_dlpanel *p, int list_id) {
    memset(p, 0, sizeof *p);
    uui_transfers_init(&p->list);
    p->child = (struct uui_item){ .ops = &uui_transfers_ops, .widget = &p->list, .id = list_id,
                                  .name = "downloads" };
}

static void place(struct ns_dlpanel *p, int right, int top, int bw, int bh) {
    p->list.items = ns_downloads(&p->list.count);
    int w = ugfx_char_advance('n') * 44, rows = p->list.count ? p->list.count : 1;
    int h = title_h() + rows * uui_transfers_row_h() + foot_h() + 2;
    if (w > bw - 8) w = bw - 8;
    if (h > bh - top - 4) h = bh - top - 4;
    p->w = w;
    p->h = h;
    p->x = right - w < 4 ? 4 : right - w;
    p->y = top;
    p->list.x = p->x + 1;
    p->list.y = p->y + title_h();
    p->list.w = w - 2;
    p->list.h = h - title_h() - foot_h() - 2;
    p->link_h = foot_h() - 8;
    p->link_y = p->y + h - foot_h() + 4;
    p->link_x[1] = p->x + pad();
    p->link_w[1] = ugfx_text_width(LINK[1]);
    p->link_w[2] = ugfx_text_width(LINK[2]);
    p->link_x[2] = p->x + w - pad() - p->link_w[2];
}

void ns_dlpanel_open(struct ns_dlpanel *p, int ax, int ay, int aw, int bw, int bh) {
    p->open = 1;
    p->hot = p->armed = 0;
    place(p, ax + aw, ay, bw, bh);
}

void ns_dlpanel_sync(struct ns_dlpanel *p, int bw, int bh) {
    if (p->open) place(p, p->x + p->w, p->y, bw, bh);
    else p->list.items = ns_downloads(&p->list.count);
}

void ns_dlpanel_close(struct ns_dlpanel *p) { p->open = 0; }

int ns_dlpanel_take(struct ns_dlpanel *p) {
    int e = p->event;
    p->event = NS_DLP_NONE;
    return e;
}

static int link_at(const struct ns_dlpanel *p, int cx, int cy) {
    for (int i = 1; i <= 2; i++)
        if (uui_hit(p->link_x[i], p->link_y, p->link_w[i], p->link_h, cx, cy)) return i;
    return 0;
}

static void d_natural(const void *w, int *ow, int *oh) { (void)w; *ow = 0; *oh = 0; }
// Placed by ns_dlpanel_open(), never by a layout.
static void d_geometry(void *w, int x, int y, int width, int height) {
    (void)w; (void)x; (void)y; (void)width; (void)height;
}

static void d_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct ns_dlpanel *p = w;
    *x = p->x;
    *y = p->y;
    *ow = p->open ? p->w : 0;
    *oh = p->open ? p->h : 0;
}

static int d_hit(const void *w, int cx, int cy) {
    const struct ns_dlpanel *p = w;
    return p->open && uui_hit(p->x, p->y, p->w, p->h, cx, cy);
}

static struct uui_item *d_children(void *w, int *count) {
    struct ns_dlpanel *p = w;
    *count = p->open ? 1 : 0;
    return &p->child;
}

static void d_begin(struct ugfx_surface *s, void *w) {
    struct ns_dlpanel *p = w;
    if (!p->open) return;
    int r = uui_popup_radius();
    uui_fill_round_rect(s, p->x, p->y, p->w, p->h, r, uui_popup_border());
    uui_fill_round_rect(s, p->x + 1, p->y + 1, p->w - 2, p->h - 2, r > 1 ? r - 1 : 0, UTHEME_WHITE);
    ugfx_draw_string(s, p->x + pad(), p->y + (title_h() - ugfx_char_h()) / 2, "Downloads", UTHEME_TEXT,
                     UTHEME_WHITE);
    if (!p->list.count)
        ugfx_draw_string(s, p->x + pad(), p->list.y + (uui_transfers_row_h() - ugfx_char_h()) / 2,
                         "Nothing downloaded yet", utheme_current()->text_muted, UTHEME_WHITE);
    ugfx_fill_rect(s, p->x + 1, p->y + p->h - foot_h(), p->w - 2, 1, UTHEME_SEPARATOR);
    uint32_t link = utheme_action(UTHEME_ACT_NAV);
    for (int i = 1; i <= 2; i++) {
        int ty = p->link_y + (p->link_h - ugfx_char_h()) / 2;
        ugfx_draw_string(s, p->link_x[i], ty, LINK[i], link, UTHEME_WHITE);
        if (p->hot == i) ugfx_fill_rect(s, p->link_x[i], ty + ugfx_char_h(), p->link_w[i], 1, link);
    }
}

// Offered EVERY press while open: one outside closes the flyout and is
// declined, so it reaches what is under it. One on the list went to
// the list first (a child), so a press here is the frame's own.
static int d_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct ns_dlpanel *p = w;
    if (!p->open) return 0;
    if (!d_hit(p, cx, cy)) {
        p->open = 0;
        return 0;
    }
    p->armed = link_at(p, cx, cy);
    return 1;
}

static int d_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct ns_dlpanel *p = w;
    int hot = p->open ? link_at(p, cx, cy) : 0;
    if (hot == p->hot) return 0;
    p->hot = hot;
    return 1;
}

static int d_release(void *w, int cx, int cy) {
    struct ns_dlpanel *p = w;
    if (p->armed && link_at(p, cx, cy) == p->armed)
        p->event = p->armed == 1 ? NS_DLP_OPEN_FOLDER : NS_DLP_CLEAR;
    p->armed = 0;
    return 1;
}

static int d_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct ns_dlpanel *p = w;
    if (!p->open || key != 0x1B) return 0;
    p->open = 0;
    return 1;
}

static int d_accepts_focus(const void *w) { (void)w; return 0; }
static int d_overlay(const void *w) { return ((const struct ns_dlpanel *)w)->open; }

static int d_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

static void d_describe(const void *w, const struct uui_describe *d) {
    const struct ns_dlpanel *p = w;
    uui_describe_int(d, "open", p->open);
    if (!p->open) return;
    uui_describe_rect(d, "folder", p->link_x[1], p->link_y, p->link_w[1], p->link_h);
    uui_describe_rect(d, "clear", p->link_x[2], p->link_y, p->link_w[2], p->link_h);
}

const struct uui_widget_ops ns_dlpanel_ops = {
    .natural_size   = d_natural,
    .set_geometry   = d_geometry,
    .bounds         = d_bounds,
    .hit            = d_hit,
    .press          = d_press,
    .motion         = d_motion,
    .release        = d_release,
    .key            = d_key,
    .accepts_focus  = d_accepts_focus,
    .overlay_active = d_overlay,
    .cursor         = d_cursor,
    .children       = d_children,
    .children_begin = d_begin,
    .describe       = d_describe,
};
