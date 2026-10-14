// A line that explains -- see ui/uui_infonote.h.
#include "ui/uui_infonote.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"

static int icon_px(void) { return ugfx_char_h(); }
static int gap(void)     { return ugfx_char_w() / 2 + 2; }

void uui_infonote_init(struct uui_infonote *n, const char *text) {
    *n = (struct uui_infonote){ .text = text };
}

static void in_natural(const void *w, int *ow, int *oh) {
    const struct uui_infonote *n = w;
    *ow = icon_px() + gap() + (n->text ? ugfx_text_width(n->text) : 0);
    *oh = ugfx_char_h() + 4;
}

static void in_geometry(void *w, int x, int y, int width, int height) {
    struct uui_infonote *n = w;
    n->x = x; n->y = y; n->w = width; n->h = height;
}

static void in_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_infonote *n = w;
    *x = n->x; *y = n->y; *ow = n->w; *oh = n->h;
}

static void in_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_infonote *n = w;
    int ix = n->x, iy = n->y + (n->h - icon_px()) / 2;
    const struct uimg *ico = icon_get("tb-info", icon_px());
    if (ico) ugfx_blit_tinted(s, ix, iy, ico->w, ico->h, ico->px, ico->w, UTHEME_ACCENT);
    if (!n->text) return;
    int tx = ix + icon_px() + gap();
    // MUTED: a note sits a step back from the controls it explains.
    ugfx_draw_string_clipped(s, tx, n->y + (n->h - ugfx_char_h()) / 2, n->x + n->w - tx, n->text,
                             ugfx_blend(UTHEME_TEXT, UTHEME_WHITE, 110), UGFX_TRANSPARENT);
}

const struct uui_widget_ops uui_infonote_ops = {
    .natural_size = in_natural,
    .set_geometry = in_geometry,
    .bounds       = in_bounds,
    .draw         = in_draw,
};
