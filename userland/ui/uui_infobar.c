// See uui_infobar.h.
#include "ui/uui_infobar.h"
#include <string.h>
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"
#include "lib/icon_cache.h"

static int pad(void) { return ugfx_char_h() / 2; }

void uui_infobar_init(struct uui_infobar *b, const char *icon, const char *text,
                      const char *action_label) {
    memset(b, 0, sizeof *b);
    b->icon = icon;
    b->text = text;
    uui_button_init(&b->action, 0, 0, 0, 0, action_label, UTHEME_ACCENT, UTHEME_WHITE, 0);
}

int uui_infobar_take(struct uui_infobar *b) {
    int c = b->clicked;
    b->clicked = 0;
    return c;
}

static int has_action(const struct uui_infobar *b) { return b->action.label != NULL; }

static void ops_natural_size(const void *w, int *ow, int *oh) {
    const struct uui_infobar *b = w;
    int bw = 0, bh = ugfx_char_h();
    if (has_action(b)) uui_button_natural_size(&b->action, &bw, &bh);
    *ow = ugfx_text_width(b->text ? b->text : "") + bw + ugfx_char_h() * 3;
    *oh = bh + pad() * 2;
}

static void ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_infobar *b = w;
    b->x = x; b->y = y; b->w = width; b->h = height;
    if (has_action(b)) {
        int bw, bh;
        uui_button_natural_size(&b->action, &bw, &bh);
        uui_button_set_geometry(&b->action, x + width - bw - pad() * 2, y + (height - bh) / 2, bw, bh);
    }
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_infobar *b = w;
    *x = b->x; *y = b->y; *ow = b->w; *oh = b->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_infobar *b = w;
    int m = 4;                                   // inset from the view's edge
    uint32_t tint = ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 34);
    uui_fill_round_rect(s, b->x + m, b->y + m / 2, b->w - 2 * m, b->h - m, 6,
                        ugfx_blend(tint, UTHEME_ACCENT, 70));
    uui_fill_round_rect(s, b->x + m + 1, b->y + m / 2 + 1, b->w - 2 * m - 2, b->h - m - 2, 5, tint);
    int px = ugfx_char_h(), tx = b->x + m + pad();
    const struct uimg *ico = b->icon ? icon_get(b->icon, px) : 0;
    if (ico) {
        ugfx_blit_alpha(s, tx, b->y + (b->h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
        tx += ico->w + pad();
    }
    int right = has_action(b) ? b->action.x - pad() : b->x + b->w - m - pad();
    ugfx_draw_string_clipped(s, tx, b->y + (b->h - ugfx_char_h()) / 2, right - tx,
                             b->text ? b->text : "", UTHEME_TEXT, tint);
    if (has_action(b)) uui_button_draw_one(s, &b->action);
}

static int on_action(const struct uui_infobar *b, int cx, int cy) {
    return has_action(b) && uui_hit(b->action.x, b->action.y, b->action.w, b->action.h, cx, cy);
}

static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_infobar *b = w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_infobar *b = w;
    b->pressed = on_action(b, cx, cy);
    b->action.pressed = b->pressed;
    return 1;
}

// The action fires on RELEASE over it, so a press can be dragged off.
static int ops_release(void *w, int cx, int cy) {
    struct uui_infobar *b = w;
    if (b->pressed && on_action(b, cx, cy)) b->clicked = 1;
    b->pressed = b->action.pressed = 0;
    return 1;
}

const struct uui_widget_ops uui_infobar_ops = {
    .natural_size = ops_natural_size,
    .set_geometry = ops_set_geometry,
    .bounds       = ops_bounds,
    .draw         = ops_draw,
    .hit          = ops_hit,
    .press        = ops_press,
    .release      = ops_release,
};
