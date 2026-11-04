// See uui_themepreview.h.
#include "ui/uui_themepreview.h"
#include "ui/uui_primitives.h"
#include <stddef.h>

void uui_themepreview_init(struct uui_themepreview *p, const struct utheme *t) {
    p->x = p->y = p->w = p->h = 0;
    p->theme = t;
    p->taskbar_light = 0;
    p->corner = 8;
    p->desk = ugfx_rgb(24, 60, 90);   // the plain desktop's default blue
}

// A line of text where it fits, a bar where it does not: a card is too
// small for letters, and a bar in the text's colour is what the eye
// reads there anyway.
static void line(struct ugfx_surface *s, int x, int y, int w, int h, const char *text,
                 uint32_t fg, uint32_t bg, int words) {
    if (words) {
        ugfx_draw_string_clipped(s, x, y + (h - ugfx_char_h()) / 2, w, text, fg, bg);
        return;
    }
    int bh = h / 3 > 1 ? h / 3 : 1;
    ugfx_fill_rect(s, x, y + (h - bh) / 2, w, bh, fg);
}

static void boxed(struct ugfx_surface *s, int x, int y, int w, int h, int r,
                  uint32_t edge, uint32_t fill) {
    uui_fill_round_rect(s, x, y, w, h, r, edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
}

void uui_themepreview_draw(struct ugfx_surface *s, const struct uui_themepreview *p) {
    const struct utheme *t = p->theme ? p->theme : utheme_current();
    int X = p->x, Y = p->y, W = p->w, H = p->h;
    if (W < 24 || H < 16) return;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, X, Y, W, H);

    // Scale: a "unit" is a line's height at this size; words once a
    // line is as tall as the font.
    int u = H / 14 > 2 ? H / 14 : 2;
    int words = u >= ugfx_char_h();
    int r = p->corner * u / (ugfx_char_h() > 0 ? ugfx_char_h() : 14);

    ugfx_fill_rect(s, X, Y, W, H, p->desk);
    int tb = u * 3 / 2;
    ugfx_fill_rect(s, X, Y + H - tb, W, tb, p->taskbar_light ? ugfx_rgb(236, 236, 236)
                                                              : ugfx_rgb(30, 30, 34));
    // A light bar's top edge, as the real one draws it -- without it the
    // strip melts into whatever ground is around the preview.
    if (p->taskbar_light) ugfx_fill_rect(s, X, Y + H - tb, W, 1, ugfx_rgb(205, 205, 210));
    ugfx_fill_rect(s, X + u, Y + H - tb + u / 4, tb, tb - u / 2, t->accent);

    // The window.
    int wx = X + W / 12, wy = Y + H / 12, ww = W - W / 6, wh = H - tb - H / 12 - H / 14;
    boxed(s, wx, wy, ww, wh, r, t->border, t->window_bg);
    int th = u + u / 2 + t->titlebar_pad * u / 28;
    uui_fill_round_rect(s, wx + 1, wy + 1, ww - 2, th, r > 1 ? r - 1 : 0, t->titlebar);
    ugfx_fill_rect(s, wx + 1, wy + 1 + th / 2, ww - 2, th - th / 2, t->titlebar);

    // Caption buttons: discs on their face, or flat cells on the bar.
    int cb = th - u / 2, cy = wy + 1 + (th - cb) / 2;
    for (int i = 0; i < 3; i++) {
        if (t->caption_style == UTHEME_CAPTION_RECT) {
            // Flat cells: minimise, maximise and close as hairline glyphs.
            int cw = cb * 3 / 2, cx = wx + ww - 1 - (3 - i) * cw;
            int g = cb / 4 > 1 ? cb / 4 : 1, m = cx + cw / 2, my = wy + 1 + th / 2;
            if (i == 0) ugfx_fill_rect(s, m - g, my, 2 * g + 1, 1, t->titlebar_text);
            else if (i == 1) ugfx_draw_rect(s, m - g, my - g, 2 * g + 1, 2 * g + 1, t->titlebar_text);
            else {
                ugfx_draw_line(s, m - g, my - g, m + g, my + g, t->titlebar_text, 0);
                ugfx_draw_line(s, m + g, my - g, m - g, my + g, t->titlebar_text, 0);
            }
        } else {
            int cx = wx + ww - 1 - (3 - i) * (cb + u / 3) + cb / 2;
            ugfx_fill_circle(s, cx, cy + cb / 2, cb / 2, t->caption_bg);
        }
    }
    int tw = ww / 3;
    int tx = t->title_align == UTHEME_TITLE_CENTER ? wx + (ww - tw) / 2 : wx + u;
    line(s, tx, wy + 1, tw, th, "File Manager", t->titlebar_text, t->titlebar, words);

    // A command bar, then the body: a sidebar with a selected row, and a
    // page with a field, a control and the accent button.
    int by = wy + 1 + th;
    ugfx_fill_rect(s, wx + 1, by, ww - 2, u + u / 2, t->chrome);
    for (int i = 0; i < 3; i++)
        ugfx_fill_rect(s, wx + u + i * u * 3 / 2, by + u / 2, u / 2 + 1, u / 2 + 1,
                       t->action[i == 2 ? UTHEME_ACT_DANGER : i == 1 ? UTHEME_ACT_CREATE : UTHEME_ACT_NAV]);
    int top = by + u + u / 2;
    ugfx_fill_rect(s, wx + 1, top, ww - 2, 1, t->separator);
    int bottom = wy + wh - 1, side = ww / 3;
    ugfx_fill_rect(s, wx + 1, top + 1, side, bottom - top - 1, t->panel_bg);
    static const char *const PLACES[] = { "Home", "Documents", "Pictures", "System" };
    for (int i = 0; i < 4; i++) {
        int ry = top + u / 2 + i * u * 3 / 2;
        if (ry + u > bottom) break;
        if (i == 3) boxed(s, wx + u / 2, ry - u / 4, side - u, u + u / 2, r / 2, t->accent, t->selection_bg);
        line(s, wx + u, ry, side - 2 * u, u, PLACES[i], t->text, i == 3 ? t->selection_bg : t->panel_bg, words);
    }
    int px = wx + 1 + side, pw = ww - 2 - side;
    ugfx_fill_rect(s, px, top + 1, pw, bottom - top - 1, t->field_bg);
    ugfx_fill_rect(s, px, top + 1, 1, bottom - top - 1, t->separator);
    int fy = top + u;
    boxed(s, px + u, fy, pw - 2 * u, u + u / 2, r / 2, t->outline, t->field_bg);
    line(s, px + u + u / 2, fy, pw / 2, u + u / 2, "/usr/share/themes", t->text, t->field_bg, words);
    int ly = fy + 2 * u + u / 2;
    line(s, px + u, ly, pw * 2 / 3, u, "4 items", t->text_muted, t->field_bg, words);
    int bw = pw / 3, bh = u + u / 2, bx = px + pw - u - bw, byy = bottom - u - bh;
    if (byy > ly + u) {
        boxed(s, bx, byy, bw, bh, r / 2, t->accent, t->accent);
        line(s, bx + u / 2, byy, bw - u, bh, "OK", t->accent_text, t->accent, words);
        boxed(s, bx - bw - u / 2, byy, bw, bh, r / 2, t->outline, t->control_bg);
        line(s, bx - bw, byy, bw - u, bh, "Cancel", t->text, t->control_bg, words);
    }
    ugfx_clip_restore(s, &saved);
}

static void tp_natural(const void *w, int *out_w, int *out_h) {
    (void)w;
    int ch = ugfx_char_h() > 0 ? ugfx_char_h() : 14;
    if (out_w) *out_w = ch * 16;
    if (out_h) *out_h = ch * 10;
}

static void tp_geometry(void *w, int x, int y, int width, int height) {
    struct uui_themepreview *p = w;
    p->x = x; p->y = y; p->w = width; p->h = height;
}

static void tp_draw(struct ugfx_surface *s, const void *w) {
    uui_themepreview_draw(s, (const struct uui_themepreview *)w);
}

static void tp_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_themepreview *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}

const struct uui_widget_ops uui_themepreview_ops = {
    .natural_size = tp_natural,
    .set_geometry = tp_geometry,
    .draw         = tp_draw,
    .bounds       = tp_bounds,
};
