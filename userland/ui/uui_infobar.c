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
    uui_button_init(&b->action, 0, 0, 0, 0, action_label, UUI_ROLE(accent), UUI_ROLE(field_bg), 0);
}

void uui_infobar_set_second(struct uui_infobar *b, const char *label) {
    uui_button_init(&b->second, 0, 0, 0, 0, label, UUI_COLOR_UNSET, UUI_COLOR_UNSET, 0);
    b->second.outlined = 1;
}

int uui_infobar_take(struct uui_infobar *b) {
    int c = b->clicked;
    b->clicked = 0;
    return c;
}

static int has_action(const struct uui_infobar *b) { return b->action.label != NULL; }
static int has_second(const struct uui_infobar *b) { return b->second.label != NULL; }

static void ops_natural_size(const void *w, int *ow, int *oh) {
    const struct uui_infobar *b = w;
    int bw = 0, bh = ugfx_char_h(), sw = 0, sh = 0;
    if (has_action(b)) uui_button_natural_size(&b->action, &bw, &bh);
    if (has_second(b)) uui_button_natural_size(&b->second, &sw, &sh);
    *ow = ugfx_text_width(b->text ? b->text : "") + bw + sw + ugfx_char_h() * 4;
    *oh = (bh > sh ? bh : sh) + pad() * 2;
}

static void ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_infobar *b = w;
    b->x = x; b->y = y; b->w = width; b->h = height;
    int right = x + width - pad() * 2;
    if (has_second(b)) {
        int bw, bh;
        uui_button_natural_size(&b->second, &bw, &bh);
        uui_button_set_geometry(&b->second, right - bw, y + (height - bh) / 2, bw, bh);
        right -= bw + pad();
    }
    if (has_action(b)) {
        int bw, bh;
        uui_button_natural_size(&b->action, &bw, &bh);
        uui_button_set_geometry(&b->action, right - bw, y + (height - bh) / 2, bw, bh);
    }
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_infobar *b = w;
    *x = b->x; *y = b->y; *ow = b->w; *oh = b->h;
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_infobar *b = w;
    int m = 4;                                   // inset from the view's edge
    uint32_t hue = b->error ? utheme_action(UTHEME_ACT_DANGER) : UTHEME_ACCENT;
    uint32_t tint = ugfx_blend(UTHEME_WHITE, hue, b->error ? 22 : 34);
    uui_fill_round_rect(s, b->x + m, b->y + m / 2, b->w - 2 * m, b->h - m, 6,
                        ugfx_blend(tint, hue, 70));
    uui_fill_round_rect(s, b->x + m + 1, b->y + m / 2 + 1, b->w - 2 * m - 2, b->h - m - 2, 5, tint);
    int px = ugfx_char_h(), tx = b->x + m + pad();
    const struct uimg *ico = b->icon ? icon_get(b->icon, px) : 0;
    if (ico) {
        ugfx_blit_alpha(s, tx, b->y + (b->h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
        tx += ico->w + pad();
    } else if (b->error) {
        // A warning sign drawn, so an error bar needs no icon file.
        int e = px - 2, cy = b->y + (b->h - e) / 2;
        for (int r = 0; r < e; r++) {
            int half = r / 2;
            ugfx_fill_rect(s, tx + e / 2 - half, cy + r, 2 * half + 1, 1, hue);
        }
        ugfx_fill_rect(s, tx + e / 2, cy + e / 3, 1, e / 3, tint);
        ugfx_fill_rect(s, tx + e / 2, cy + e - 3, 1, 1, tint);
        tx += e + pad();
    }
    int right = has_action(b) ? b->action.x - pad()
              : has_second(b) ? b->second.x - pad() : b->x + b->w - m - pad();
    ugfx_draw_string_clipped(s, tx, b->y + (b->h - ugfx_char_h()) / 2, right - tx,
                             b->text ? b->text : "", UTHEME_TEXT, tint);
    if (has_action(b)) uui_button_draw_one(s, &b->action);
    if (has_second(b)) uui_button_draw_one(s, &b->second);
}

// Which action (cx, cy) is on: 1, 2, or 0.
static int on_action(const struct uui_infobar *b, int cx, int cy) {
    if (has_action(b) && uui_hit(b->action.x, b->action.y, b->action.w, b->action.h, cx, cy)) return 1;
    if (has_second(b) && uui_hit(b->second.x, b->second.y, b->second.w, b->second.h, cx, cy)) return 2;
    return 0;
}

static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_infobar *b = w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_infobar *b = w;
    b->pressed = on_action(b, cx, cy);
    b->action.pressed = b->pressed == 1;
    b->second.pressed = b->pressed == 2;
    return 1;
}

// The action fires on RELEASE over it, so a press can be dragged off.
static int ops_release(void *w, int cx, int cy) {
    struct uui_infobar *b = w;
    if (b->pressed && on_action(b, cx, cy) == b->pressed) b->clicked = b->pressed;
    b->pressed = b->action.pressed = b->second.pressed = 0;
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
