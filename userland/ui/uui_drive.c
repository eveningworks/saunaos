// See ui/uui_drive.h.
#include "ui/uui_drive.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "lib/udrive.h"
#include "lib/human.h"
#include <stdio.h>

static int pad(void)    { return ugfx_char_w(); }
static int line_h(void) { return ugfx_char_h() + 2; }
static int icon_px(void) { return ugfx_char_h() * 3 / 2; }

int uui_drive_row_h(void) { return 2 * line_h() + ugfx_char_h() / 2 + 4; }

void uui_drive_init(struct uui_drive *l, struct uui_drive_row *rows, int count,
                    const char *empty) {
    *l = (struct uui_drive){ .rows = rows, .count = count, .empty = empty,
                             .hover_row = -1, .hover_btn = -1, .press_row = -1,
                             .press_btn = -1, .fired_row = -1, .fired_btn = -1 };
}

void uui_drive_fill(struct uui_drive_row *r, const struct udrive *d) {
    snprintf(r->title, sizeof r->title, "%s", d->name);
    udrive_describe(d, r->sub, sizeof r->sub);
    r->icon = 0;
    r->can_open = d->point[0] != 0;
    r->can_eject = 1;
    r->indent = 0;
    r->busy = 0;
}

void uui_drive_fill_stick(struct uui_drive_row *r, const struct udrive *d, int volumes) {
    char size[24];
    human_size_iec(size, sizeof size, d->disk_size);
    snprintf(r->title, sizeof r->title, "%s", d->model[0] ? d->model : "USB drive");
    snprintf(r->sub, sizeof r->sub, "%s \xb7 %d volumes", size, volumes);
    r->icon = 0;
    r->can_open = 0;
    r->can_eject = 1;
    r->indent = 0;
    r->busy = 0;
}

void uui_drive_fill_volume(struct uui_drive_row *r, const struct udrive *d) {
    if (d->labeled) {
        snprintf(r->title, sizeof r->title, "%s", d->name);
        udrive_describe(d, r->sub, sizeof r->sub);
    } else {
        snprintf(r->title, sizeof r->title, "%s volume", udrive_fs_name(d));
        udrive_describe_space(d, r->sub, sizeof r->sub);
    }
    r->icon = 0;
    r->can_open = d->point[0] != 0;
    r->can_eject = 0;
    r->indent = 1;
    r->busy = 0;
}

// Where a row's text starts: past the icon, and an indented row's under
// its stick's title rather than under its icon.
static int text_x(const struct uui_drive *l) { return l->x + pad() + icon_px() + pad(); }

static const char OPEN[] = "Open";

int uui_drive_button_rect(const struct uui_drive *l, int row, int btn,
                          int *x, int *y, int *w, int *h) {
    if (row < 0 || row >= l->count || btn < 0 || btn >= UUI_DRIVE_BTNS) return 0;
    int bh = utheme_control_h();
    int by = l->y + row * uui_drive_row_h() + (uui_drive_row_h() - bh) / 2;
    // Eject outermost, square; Open beside it.
    int right = l->x + l->w - pad();
    if (btn == UUI_DRIVE_EJECT) {
        if (!l->rows[row].can_eject) return 0;
        *x = right - bh; *y = by; *w = bh; *h = bh;
        return 1;
    }
    if (!l->rows[row].can_open) return 0;
    int ow = ugfx_text_width(OPEN) + 2 * ugfx_char_w();
    // Under a stick, Open stands inside its card, which ends where Eject does.
    int gap = l->rows[row].can_eject ? bh + pad() / 2 : pad();
    *x = right - gap - ow; *y = by; *w = ow; *h = bh;
    return 1;
}

static enum uui_state state_of(const struct uui_drive *l, int row, int btn) {
    if (btn == UUI_DRIVE_EJECT && l->rows[row].busy) return UUI_STATE_DISABLED;
    if (l->press_row == row && l->press_btn == btn) return UUI_STATE_PRESSED;
    if (l->hover_row == row && l->hover_btn == btn) return UUI_STATE_HOVER;
    return UUI_STATE_REST;
}

void uui_drive_draw(struct ugfx_surface *s, const struct uui_drive *l, uint32_t bg) {
    uint32_t fg = UTHEME_TEXT, dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    int rh = uui_drive_row_h();
    if (!l->count) {
        if (l->empty)
            ugfx_draw_string_clipped(s, l->x + pad(), l->y + (rh - ugfx_char_h()) / 2,
                                     l->w - 2 * pad(), l->empty, dim, bg);
        return;
    }
    for (int i = 0; i < l->count; i++) {
        const struct uui_drive_row *r = &l->rows[i];
        int ry = l->y + i * rh;
        uint32_t ground = bg;
        if (r->indent) {
            // A volume on a card of its own under its stick's title.
            int cx = text_x(l) - pad() / 2, cw = l->x + l->w - pad() - cx, inset = ugfx_px(2);
            uui_fill_round_rect(s, cx, ry + inset, cw, rh - 2 * inset, uui_control_radius(0),
                                UTHEME_SEPARATOR);
            uui_fill_round_rect(s, cx + 1, ry + inset + 1, cw - 2, rh - 2 * inset - 2,
                                uui_control_radius(0), UTHEME_WHITE);
            ground = UTHEME_WHITE;
        } else {
            if (i) ugfx_fill_rect(s, l->x + pad(), ry, l->w - 2 * pad(), 1, UTHEME_SEPARATOR);
            // ugfx_blit_alpha(): an icon's corners are transparent.
            int isz = icon_px(), ix = l->x + pad();
            const struct uimg *ic = icon_get(r->icon ? r->icon : "drive-usb", isz);
            if (ic) ugfx_blit_alpha(s, ix, ry + (rh - ic->h) / 2, ic->w, ic->h, ic->px, ic->w);
        }

        int tx = text_x(l) + (r->indent ? pad() / 2 : 0), right = l->x + l->w - pad();
        for (int b = 0; b < UUI_DRIVE_BTNS; b++) {
            int bx, by, bw, bh;
            if (uui_drive_button_rect(l, i, b, &bx, &by, &bw, &bh) && bx < right) right = bx;
        }
        int tw = right - pad() - tx, ty = ry + (rh - 2 * line_h()) / 2;
        if (tw > 0) {
            ugfx_draw_string_elided(s, tx, ty, tw, r->title, fg, ground);
            ugfx_draw_string_elided(s, tx, ty + line_h(), tw, r->sub, dim, ground);
        }

        int bx, by, bw, bh;
        if (uui_drive_button_rect(l, i, UUI_DRIVE_OPEN, &bx, &by, &bw, &bh)) {
            uint32_t face = uui_state_bg(UTHEME_WHITE, state_of(l, i, UUI_DRIVE_OPEN));
            uui_control_frame(s, bx, by, bw, bh, face, UTHEME_OUTLINE);
            uui_button_draw_label(s, bx, by, bw, bh, OPEN, fg, face, state_of(l, i, UUI_DRIVE_OPEN));
        }
        if (uui_drive_button_rect(l, i, UUI_DRIVE_EJECT, &bx, &by, &bw, &bh)) {
            // Only a wash under the pointer: the glyph alone is the button,
            // as a toolbar's is (Windows' tray list, Plasma's applet).
            enum uui_state st = state_of(l, i, UUI_DRIVE_EJECT);
            uint32_t ground = bg == UGFX_TRANSPARENT ? UTHEME_PANEL_BG : bg;
            if (st == UUI_STATE_HOVER || st == UUI_STATE_PRESSED)
                uui_fill_round_rect(s, bx, by, bw, bh, uui_control_radius(0), uui_state_bg(ground, st));
            const struct uimg *g = icon_get("tb-eject", ugfx_char_h());
            if (g) {
                uint32_t ink = st == UUI_STATE_DISABLED ? dim : fg;
                ugfx_blit_tinted(s, bx + (bw - g->w) / 2, by + (bh - g->h) / 2, g->w, g->h,
                                 g->px, g->w, ink);
            }
        }
    }
}

static int find(const struct uui_drive *l, int cx, int cy, int *row, int *btn) {
    for (int i = 0; i < l->count; i++)
        for (int b = 0; b < UUI_DRIVE_BTNS; b++) {
            int x, y, w, h;
            if (uui_drive_button_rect(l, i, b, &x, &y, &w, &h) && uui_hit(x, y, w, h, cx, cy)) {
                *row = i;
                *btn = b;
                return 1;
            }
        }
    *row = *btn = -1;
    return 0;
}

int uui_drive_hover(struct uui_drive *l, int cx, int cy) {
    int r, b;
    find(l, cx, cy, &r, &b);
    if (r == l->hover_row && b == l->hover_btn) return 0;
    l->hover_row = r;
    l->hover_btn = b;
    return 1;
}

int uui_drive_press(struct uui_drive *l, int cx, int cy) {
    int r, b;
    if (!find(l, cx, cy, &r, &b)) return 0;
    if (b == UUI_DRIVE_EJECT && l->rows[r].busy) return 0;
    l->press_row = r;
    l->press_btn = b;
    return 1;
}

int uui_drive_release(struct uui_drive *l, int cx, int cy) {
    int r, b;
    int was_r = l->press_row, was_b = l->press_btn;
    l->press_row = l->press_btn = -1;
    if (was_r < 0) return 0;
    // Fires only over the button that was pressed, and only if its row is
    // still there: the caller may have rewritten the list meanwhile.
    if (!find(l, cx, cy, &r, &b) || r != was_r || b != was_b || r >= l->count) return 1;
    l->fired_row = r;
    l->fired_btn = b;
    return 2;
}

// --- the ops ---------------------------------------------------------------

// As wide as the widest row's text beside its buttons, so a free-space
// line is not cut short.
static void natural(const void *w, int *ow, int *oh) {
    const struct uui_drive *l = w;
    int text = l->empty && !l->count ? ugfx_text_width(l->empty) : 0;
    for (int i = 0; i < l->count; i++) {
        int t = ugfx_text_width(l->rows[i].title), u = ugfx_text_width(l->rows[i].sub);
        if (t > text) text = t;
        if (u > text) text = u;
    }
    int buttons = utheme_control_h() + pad() / 2 + ugfx_text_width(OPEN) + 2 * ugfx_char_w();
    *ow = pad() + icon_px() + pad() + text + pad() + buttons + pad();
    *oh = (l->count ? l->count : 1) * uui_drive_row_h();
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_drive *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_drive *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}

// Through the ops, on a card of its own -- uui_actionlist's look, so a
// list of drives sits among setting rows as one more card.
static void draw(struct ugfx_surface *s, const void *w) {
    const struct uui_drive *l = w;
    uui_fill_round_rect(s, l->x, l->y, l->w, l->h, 6, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, l->x + 1, l->y + 1, l->w - 2, l->h - 2, 5, UTHEME_WHITE);
    uui_drive_draw(s, w, UTHEME_WHITE);
}

static int hit(const void *w, int cx, int cy) {
    const struct uui_drive *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}

static int press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    return uui_drive_press(w, cx, cy);
}

static int motion(void *w, int cx, int cy, unsigned buttons) {
    if (buttons) return 0;   // a drag off the button just cancels at release
    return uui_drive_hover(w, cx, cy);
}

static int release(void *w, int cx, int cy) { return uui_drive_release(w, cx, cy) != 0; }

const struct uui_widget_ops uui_drive_ops = {
    .natural_size = natural,
    .set_geometry = geometry,
    .bounds       = bounds,
    .draw         = draw,
    .hit          = hit,
    .press        = press,
    .motion       = motion,
    .release      = release,
};
