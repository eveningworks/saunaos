// Installable things as cards or rows -- see ui/uui_appcard.h.
#include <string.h>

#include "ui/uui_appcard.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_label.h"        // uui_label_wrap_next(): the summary's two lines
#include "ui/uui_primitives.h"
#include "media/icon_cache.h"
#include "media/uimg.h"
#include "base/udclick.h"
#include "rt/sys.h"
#include "keyboard.h"
#include "win_proto.h"           // WIN_CURSOR_*

// --- metrics: the font's, and ugfx_px() for the fixed gaps -----------------

static int gap(void)       { return ugfx_px(12); }
static int pad(void)       { return ugfx_px(14); }
static int lh(void)        { return ugfx_char_h(); }
static int btn_h(void)     { return lh() + ugfx_px(10); }
static int tile_card(void) { return lh() * 2 + ugfx_px(4); }
static int tile_row(void)  { return lh() + ugfx_px(8); }
static int card_min_w(void) { return ugfx_char_w() * 20; }
static int card_h(void) { return pad() * 2 + tile_card() + ugfx_px(8) + lh() * 2 + ugfx_px(8) + btn_h(); }
static int row_h(void)  { return lh() + ugfx_px(18); }
static int head_h(const struct uui_appcards *v) { return v->layout == UUI_APPCARD_ROWS ? lh() + ugfx_px(10) : 0; }


static int bar_w(const struct uui_appcards *v) { return uui_sbar_shown(&v->sb) ? uui_sbar_width() : 0; }
static int inner_w(const struct uui_appcards *v) { return v->w - bar_w(v); }

static int cols_for(int iw) {
    int c = (iw - gap()) / (card_min_w() + gap());
    return c > 0 ? c : 1;
}

int uui_appcards_columns(const struct uui_appcards *v) {
    return v->layout == UUI_APPCARD_CARDS ? cols_for(inner_w(v)) : 1;
}

static int pitch_h(const struct uui_appcards *v) {
    return v->layout == UUI_APPCARD_CARDS ? card_h() + gap() : row_h();
}

static int total_for(const struct uui_appcards *v, int iw) {
    if (v->layout == UUI_APPCARD_ROWS) return v->count * row_h();
    int c = cols_for(iw), rows = (v->count + c - 1) / c;
    return rows ? gap() + rows * (card_h() + gap()) : 0;
}

// THE BAR IS RESERVED ONLY WHILE IT IS SHOWN, and whether it shows
// depends on the columns its own width leaves: asked at the full width
// first, then again with the strip taken.
static void sync_bar(struct uui_appcards *v) {
    int list_y = v->y + head_h(v), list_h = v->h - head_h(v);
    if (list_h < 0) list_h = 0;
    int total = total_for(v, v->w);
    if (total > list_h) total = total_for(v, v->w - uui_sbar_width());
    uui_sbar_place(&v->sb, v->x + v->w - uui_sbar_width(), list_y, list_h);
    uui_sbar_set(&v->sb, total, list_h, v->top);
    v->top = v->sb.top;
    v->sb.step = v->layout == UUI_APPCARD_ROWS ? row_h() : card_h() / 2;
}

void uui_appcards_init(struct uui_appcards *v, int layout) {
    memset(v, 0, sizeof *v);
    v->layout = layout;
    v->selected = v->hot = v->armed = v->pressed = v->activated = v->last_click = -1;
    uui_sbar_init(&v->sb);
}

void uui_appcards_set(struct uui_appcards *v, const struct uui_appcard *items, int count) {
    v->items = items;
    v->count = count;
    if (v->selected >= count) v->selected = count - 1;
    if (v->hot >= count) v->hot = -1;
    v->armed = -1;
    sync_bar(v);
}

void uui_appcards_set_layout(struct uui_appcards *v, int layout) {
    if (layout == v->layout) return;
    v->layout = layout;
    v->top = 0;
    v->hot = v->armed = -1;
    sync_bar(v);
    uui_appcards_select(v, v->selected);
}

int uui_appcards_item_rect(const struct uui_appcards *v, int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= v->count) return 0;
    int list_y = v->y + head_h(v), list_h = v->h - head_h(v);
    if (v->layout == UUI_APPCARD_ROWS) {
        *x = v->x;
        *y = list_y + i * row_h() - v->top;
        *w = inner_w(v);
        *h = row_h();
    } else {
        int iw = inner_w(v), c = cols_for(iw), g = gap();
        int cw = (iw - g * (c + 1)) / c;
        *x = v->x + g + (i % c) * (cw + g);
        *y = list_y + g + (i / c) * (card_h() + g) - v->top;
        *w = cw;
        *h = card_h();
    }
    return *y + *h > list_y && *y < list_y + list_h;
}

// A button is as wide as its label, in the bold face it is drawn in.
static int label_w(const char *t) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int w = ugfx_text_width(t);
    ugfx_set_font(was);
    return w;
}

static int button_w(const struct uui_appcard *it) {
    return it->button ? label_w(it->button) + ugfx_px(26) : 0;
}

// The ROWS layout's button column: as wide as its widest button, so the
// header's columns line up with every row's.
static int rows_btn_col(const struct uui_appcards *v) {
    int m = 0;
    for (int i = 0; i < v->count; i++) {
        int b = button_w(&v->items[i]);
        if (b > m) m = b;
    }
    return m;
}

// THE ROWS LAYOUT'S COLUMNS, measured from what they hold: Status, Size
// and Licence right to left before the button, each as wide as its widest
// cell or its heading. A narrow view drops Size, then Licence, before the
// Name column falls under ten characters -- the details pane says both.
struct row_cols { int st_x, st_w, sz_x, sz_w, bd_x, bd_w, show_sz, show_bd; };

static int badge_w(const char *t) { return t ? ugfx_text_width(t) + ugfx_px(16) : 0; }

static void row_cols(const struct uui_appcards *v, int x, int w, struct row_cols *c) {
    int p = ugfx_px(10), bc = rows_btn_col(v);
    int right = x + w - p - (bc ? bc + p : 0);
    c->st_w = ugfx_text_width("Status");
    c->sz_w = ugfx_text_width("Size");
    c->bd_w = ugfx_text_width("Licence");
    for (int i = 0; i < v->count; i++) {
        const struct uui_appcard *it = &v->items[i];
        int sw = it->pending ? label_w(it->pending) : it->status ? ugfx_text_width(it->status) : 0;
        if (sw > c->st_w) c->st_w = sw;
        if (it->size && ugfx_text_width(it->size) > c->sz_w) c->sz_w = ugfx_text_width(it->size);
        if (badge_w(it->badge) > c->bd_w) c->bd_w = badge_w(it->badge);
    }
    c->st_w += p * 2;
    c->sz_w += p * 2;
    c->bd_w += p * 2;
    int name_min = x + p + tile_row() + ugfx_px(10) + ugfx_char_w() * 10;
    c->show_bd = right - c->st_w - c->bd_w >= name_min;
    c->show_sz = c->show_bd && right - c->st_w - c->bd_w - c->sz_w >= name_min;
    c->st_x = right - c->st_w;
    c->sz_x = c->st_x - (c->show_sz ? c->sz_w : 0);
    c->bd_x = c->sz_x - (c->show_bd ? c->bd_w : 0);
}

int uui_appcards_button_rect(const struct uui_appcards *v, int i, int *x, int *y, int *w, int *h) {
    int ix, iy, iw, ih;
    if (!uui_appcards_item_rect(v, i, &ix, &iy, &iw, &ih) || !v->items[i].button) return 0;
    *w = button_w(&v->items[i]);
    *h = btn_h();
    if (v->layout == UUI_APPCARD_ROWS) {
        *x = ix + iw - ugfx_px(10) - rows_btn_col(v);   // left-aligned in the column
        *y = iy + (ih - *h) / 2;
    } else {
        *x = ix + iw - pad() - *w;
        *y = iy + ih - pad() - *h;
    }
    return 1;
}

void uui_appcards_select(struct uui_appcards *v, int index) {
    if (index < -1 || index >= v->count) return;
    v->selected = index;
    if (index < 0) return;
    sync_bar(v);
    int x, y, w, h;
    uui_appcards_item_rect(v, index, &x, &y, &w, &h);
    int list_y = v->y + head_h(v), list_h = v->h - head_h(v);
    int g = v->layout == UUI_APPCARD_CARDS ? gap() : 0;
    if (y - g < list_y) v->top -= list_y - (y - g);
    else if (y + h + g > list_y + list_h) v->top += y + h + g - (list_y + list_h);
    sync_bar(v);
}

int uui_appcards_take_button(struct uui_appcards *v) {
    int i = v->pressed;
    v->pressed = -1;
    return i;
}

int uui_appcards_take_activate(struct uui_appcards *v) {
    int i = v->activated;
    v->activated = -1;
    return i;
}

// The item under a point, and whether it is on its button.
static int item_at(const struct uui_appcards *v, int cx, int cy, int *on_button) {
    *on_button = 0;
    int list_y = v->y + head_h(v);
    if (!uui_hit(v->x, list_y, inner_w(v), v->h - head_h(v), cx, cy)) return -1;
    for (int i = 0; i < v->count; i++) {
        int x, y, w, h;
        if (!uui_appcards_item_rect(v, i, &x, &y, &w, &h) || !uui_hit(x, y, w, h, cx, cy)) continue;
        int bx, by, bw, bh;
        if (uui_appcards_button_rect(v, i, &bx, &by, &bw, &bh)) *on_button = uui_hit(bx, by, bw, bh, cx, cy);
        return i;
    }
    return -1;
}

// --- painting ----------------------------------------------------------------

static uint32_t muted(void) { return utheme_current()->text_muted; }
static uint32_t danger(void) { return utheme_action(UTHEME_ACT_DANGER); }

// The licence badge, a capsule: warm for a copyleft licence, green else.
static int draw_badge(struct ugfx_surface *s, int x, int y, int max_w, const struct uui_appcard *it) {
    if (!it->badge || max_w <= 0) return 0;
    uint32_t hue = it->badge_warm ? utheme_severity(UTHEME_SEV_WARNING) : utheme_action(UTHEME_ACT_CREATE);
    uint32_t bg = ugfx_blend(UTHEME_WHITE, hue, 0x38), fg = ugfx_blend(hue, UTHEME_TEXT, 0x90);
    int tw = ugfx_text_width(it->badge), bw = tw + ugfx_px(16), bh = lh() + ugfx_px(4);
    if (bw > max_w) bw = max_w;
    uui_fill_round_rect(s, x, y, bw, bh, UUI_CAPSULE, bg);
    ugfx_draw_string_clipped(s, x + ugfx_px(8), y + ugfx_px(2), bw - ugfx_px(16), it->badge, fg, bg);
    return bw;
}

static void draw_button(struct ugfx_surface *s, const struct uui_appcards *v, int i, uint32_t ground) {
    int x, y, w, h;
    if (!uui_appcards_button_rect(v, i, &x, &y, &w, &h)) return;
    const struct uui_appcard *it = &v->items[i];
    enum uui_state st = it->button_greyed ? UUI_STATE_DISABLED
                      : v->armed == i && v->armed_button && v->hot == i && v->hot_button ? UUI_STATE_PRESSED
                      : v->hot == i && v->hot_button ? UUI_STATE_HOVER : UUI_STATE_REST;
    uint32_t fill, fg, edge;
    if (it->button_kind == UUI_APPCARD_BTN_ADD) {
        fill = UTHEME_ACCENT; fg = UTHEME_ACCENT_TEXT; edge = UTHEME_ACCENT;
    } else if (it->button_kind == UUI_APPCARD_BTN_REMOVE) {
        fill = ground; fg = danger(); edge = ugfx_blend(ground, danger(), 0x70);
    } else {
        fill = UTHEME_BUTTON_BG; fg = UTHEME_TEXT; edge = UTHEME_BORDER;
    }
    if (st != UUI_STATE_REST) fill = uui_state_bg(fill, st);
    if (st == UUI_STATE_DISABLED) fg = uui_state_bg(fg, UUI_STATE_DISABLED);
    uui_fill_round_rect(s, x, y, w, h, UUI_CAPSULE, edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, UUI_CAPSULE, fill);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    uui_button_draw_label(s, x, y, w, h, it->button, fg, fill, st);
    ugfx_set_font(was);
}

static void draw_tile(struct ugfx_surface *s, int x, int y, int size, const struct uui_appcard *it) {
    const struct uimg *im = it->icon && it->icon[0] ? icon_get(it->icon, size) : 0;
    if (im) {
        ugfx_blit_alpha(s, x, y, im->w, im->h, im->px, im->w);
        return;
    }
    uui_fill_round_rect(s, x, y, size, size, ugfx_px(8), it->tile ? it->tile : UTHEME_ACCENT);
    const char *g = it->glyph ? it->glyph : "";
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int tw = ugfx_text_width(g);
    ugfx_draw_string_clipped(s, x + (size - tw) / 2, y + (size - lh()) / 2, size, g, UTHEME_ACCENT_TEXT,
                             it->tile ? it->tile : UTHEME_ACCENT);
    ugfx_set_font(was);
}

// Selection outranks hover; a marked item wears its pending colour on the edge.
static void item_ground(const struct uui_appcards *v, int i, uint32_t base, uint32_t *fill, uint32_t *edge) {
    const struct uui_appcard *it = &v->items[i];
    *fill = base;
    *edge = UTHEME_SEPARATOR;
    if (i == v->selected) {
        *fill = UTHEME_SELECTION;
        *edge = v->focused ? UTHEME_ACCENT : ugfx_blend(UTHEME_SELECTION, UTHEME_ACCENT, 0x80);
    } else if (i == v->hot && !v->hot_button) {
        *fill = uui_state_bg(base, UUI_STATE_HOVER);
    }
    if (it->pending && i != v->selected)
        *edge = ugfx_blend(base, it->pending_danger ? danger() : UTHEME_ACCENT, 0xA0);
}

static void draw_card(struct ugfx_surface *s, const struct uui_appcards *v, int i) {
    const struct uui_appcard *it = &v->items[i];
    int x, y, w, h;
    uui_appcards_item_rect(v, i, &x, &y, &w, &h);
    uint32_t fill, edge;
    item_ground(v, i, UTHEME_WHITE, &fill, &edge);
    int r = utheme_radius(UTHEME_R_POPUP);
    uui_fill_round_rect(s, x, y, w, h, r, edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill);

    int p = pad(), t = tile_card(), tx = x + p, ty = y + p;
    draw_tile(s, tx, ty, t, it);
    int nx = tx + t + ugfx_px(10), nw = x + w - p - nx;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, nx, ty + t / 2 - lh(), nw, it->name ? it->name : "", UTHEME_TEXT, fill);
    ugfx_set_font(was);
    if (it->kind) ugfx_draw_string_elided(s, nx, ty + t / 2, nw, it->kind, muted(), fill);

    // The summary: two lines at most, the second elided.
    int sy = ty + t + ugfx_px(8), sw = w - 2 * p;
    if (it->summary) {
        char line[160];
        const char *rest = uui_label_wrap_next(it->summary, sw, line, (int)sizeof line);
        ugfx_draw_string_clipped(s, x + p, sy, sw, line, UTHEME_TEXT, fill);
        while (rest && *rest == ' ') rest++;
        if (rest && *rest) ugfx_draw_string_elided(s, x + p, sy + lh(), sw, rest, UTHEME_TEXT, fill);
    }

    // The footer: badge, size (or the pending state), the button.
    int fy = y + h - p - btn_h(), bx = x + p;
    int by = fy + (btn_h() - (lh() + ugfx_px(4))) / 2;
    int bw0 = button_w(it), right = x + w - p - (bw0 ? bw0 + ugfx_px(8) : 0);
    bx += draw_badge(s, bx, by, right - bx, it) + ugfx_px(8);
    const char *note = it->pending ? it->pending : it->size;
    uint32_t nc = it->pending ? (it->pending_danger ? danger() : UTHEME_ACCENT) : muted();
    if (note && right > bx) {
        if (it->pending) was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, bx, fy + (btn_h() - lh()) / 2, right - bx, note, nc, fill);
        if (it->pending) ugfx_set_font(was);
    }
    draw_button(s, v, i, fill);
}

static void draw_row(struct ugfx_surface *s, const struct uui_appcards *v, int i) {
    const struct uui_appcard *it = &v->items[i];
    int x, y, w, h;
    uui_appcards_item_rect(v, i, &x, &y, &w, &h);
    uint32_t fill, edge;
    item_ground(v, i, UTHEME_WHITE, &fill, &edge);
    int inset = ugfx_px(4);
    if (fill != UTHEME_WHITE || edge != UTHEME_SEPARATOR) {
        int r = uui_select_radius(0);
        uui_fill_round_rect(s, x + inset, y + 1, w - 2 * inset, h - 2,
                            r, edge != UTHEME_SEPARATOR ? edge : fill);
        uui_fill_round_rect(s, x + inset + 1, y + 2, w - 2 * inset - 2, h - 4, r > 1 ? r - 1 : r, fill);
    }
    int p = ugfx_px(10), t = tile_row(), cx = x + p;
    draw_tile(s, cx, y + (h - t) / 2, t, it);
    int ty = y + (h - lh()) / 2;
    struct row_cols c;
    row_cols(v, x, w, &c);
    int nx = cx + t + ugfx_px(10);
    // Name, then the kind in the muted colour after it.
    const char *name = it->name ? it->name : "";
    int used = label_w(name);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, nx, ty, c.bd_x - p - nx, name, UTHEME_TEXT, fill);
    ugfx_set_font(was);
    if (it->kind && nx + used + p + ugfx_char_w() * 4 < c.bd_x - p)   // a kind cut to ".." says nothing
        ugfx_draw_string_elided(s, nx + used + p, ty, c.bd_x - p - (nx + used + p), it->kind, muted(), fill);
    if (c.show_bd) draw_badge(s, c.bd_x + p, y + (h - (lh() + ugfx_px(4))) / 2, c.bd_w - 2 * p, it);
    if (c.show_sz && it->size) ugfx_draw_string_elided(s, c.sz_x + p, ty, c.sz_w - 2 * p, it->size, muted(), fill);
    const char *st = it->pending ? it->pending : it->status;
    uint32_t sc = it->pending ? (it->pending_danger ? danger() : UTHEME_ACCENT) : UTHEME_TEXT;
    if (st) {
        if (it->pending) was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, c.st_x + p, ty, c.st_w - 2 * p, st, sc, fill);
        if (it->pending) ugfx_set_font(was);
    }
    draw_button(s, v, i, fill);
}

static void draw_header(struct ugfx_surface *s, const struct uui_appcards *v) {
    int hh = head_h(v), w = inner_w(v), p = ugfx_px(10);
    uint32_t bg = UTHEME_PANEL_BG;
    ugfx_fill_rect(s, v->x, v->y, v->w, hh, bg);
    ugfx_fill_rect(s, v->x, v->y + hh - 1, v->w, 1, UTHEME_SEPARATOR);
    int ty = v->y + (hh - lh()) / 2;
    struct row_cols c;
    row_cols(v, v->x, w, &c);
    int nx = v->x + p + tile_row() + ugfx_px(10);
    ugfx_draw_string_clipped(s, nx, ty, c.bd_x - nx, "Name", muted(), bg);
    if (c.show_bd) ugfx_draw_string_clipped(s, c.bd_x + p, ty, c.bd_w - p, "Licence", muted(), bg);
    if (c.show_sz) ugfx_draw_string_clipped(s, c.sz_x + p, ty, c.sz_w - p, "Size", muted(), bg);
    ugfx_draw_string_clipped(s, c.st_x + p, ty, c.st_w - p, "Status", muted(), bg);
}

static void a_draw(struct ugfx_surface *s, const void *w) {
    struct uui_appcards *v = (struct uui_appcards *)w;
    sync_bar(v);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, v->x, v->y, v->w, v->h);
    uint32_t ground = v->layout == UUI_APPCARD_CARDS ? UTHEME_WINDOW_BG : UTHEME_WHITE;
    ugfx_fill_rect(s, v->x, v->y, v->w, v->h, ground);
    if (v->layout == UUI_APPCARD_ROWS) draw_header(s, v);
    int list_y = v->y + head_h(v);
    ugfx_clip_intersect(s, v->x, list_y, v->w, v->h - head_h(v));
    if (!v->count && v->empty)
        ugfx_draw_string_elided(s, v->x + pad(), list_y + pad(), inner_w(v) - 2 * pad(), v->empty, muted(), ground);
    for (int i = 0; i < v->count; i++) {
        int x, y, iw, ih;
        if (!uui_appcards_item_rect(v, i, &x, &y, &iw, &ih)) continue;
        if (v->layout == UUI_APPCARD_ROWS) draw_row(s, v, i);
        else draw_card(s, v, i);
    }
    uui_sbar_draw(&v->sb, s, ground, UTHEME_TEXT);
    ugfx_clip_restore(s, &saved);
}

// --- ops -------------------------------------------------------------------

static void a_natural(const void *w, int *ow, int *oh) {
    const struct uui_appcards *v = w;
    *ow = v->layout == UUI_APPCARD_CARDS ? card_min_w() + gap() * 2 + uui_sbar_width() : ugfx_char_w() * 70;
    *oh = v->layout == UUI_APPCARD_CARDS ? card_h() + gap() * 2 : head_h(v) + row_h() * 4;
}

static void a_geometry(void *w, int x, int y, int width, int height) {
    struct uui_appcards *v = w;
    v->x = x; v->y = y; v->w = width; v->h = height;
    sync_bar(v);
}

static void a_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_appcards *v = w;
    *x = v->x; *y = v->y; *ow = v->w; *oh = v->h;
}

// The whole rect: the bar is part of the view (userland/CLAUDE.md).
static int a_hit(const void *w, int cx, int cy) {
    const struct uui_appcards *v = w;
    return uui_hit(v->x, v->y, v->w, v->h, cx, cy);
}

static int bar_moved(struct uui_appcards *v, int r) {
    if (r & UUI_SBAR_MOVED) v->top = v->sb.top;
    return r != 0;
}

static int a_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_appcards *v = w;
    sync_bar(v);
    if (uui_sbar_hit(&v->sb, cx, cy)) return bar_moved(v, uui_sbar_press(&v->sb, cx, cy)) | 1;
    int on;
    v->armed = item_at(v, cx, cy, &on);
    v->armed_button = on && v->armed >= 0 && !v->items[v->armed].button_greyed;
    if (v->armed >= 0 && !v->armed_button) v->selected = v->armed;
    return 1;
}

static int a_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_appcards *v = w;
    if (v->sb.grab >= 0) return bar_moved(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
    int changed = bar_moved(v, uui_sbar_motion(&v->sb, cx, cy, buttons));
    int on;
    int i = uui_sbar_hit(&v->sb, cx, cy) ? -1 : item_at(v, cx, cy, &on);
    if (i < 0) on = 0;
    if (i != v->hot || on != v->hot_button) {
        v->hot = i;
        v->hot_button = on;
        changed = 1;
    }
    return changed;
}

static int a_release(void *w, int cx, int cy) {
    struct uui_appcards *v = w;
    if (v->sb.grab >= 0) return bar_moved(v, uui_sbar_release(&v->sb)) | 1;
    int on;
    int i = item_at(v, cx, cy, &on);
    if (v->armed >= 0 && i == v->armed) {
        if (v->armed_button && on) {
            v->pressed = i;                  // commit on release: a press dragged off is cancelled
        } else if (!v->armed_button) {
            uint64_t now = sys_monotonic_ns();
            if (i == v->last_click && now - v->last_click_ns < udclick_ns()) {
                v->activated = i;
                v->last_click = -1;
            } else {
                v->last_click = i;
                v->last_click_ns = now;
            }
        }
    }
    v->armed = -1;
    v->armed_button = 0;
    return 1;
}

static int a_wheel(void *w, int notches) {
    struct uui_appcards *v = w;
    int old = v->top;
    sync_bar(v);
    uui_sbar_set(&v->sb, v->sb.total, v->sb.visible, v->top - notches * pitch_h(v) / 2);
    v->top = v->sb.top;
    return v->top != old;
}

static int a_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_appcards *v = w;
    if (!v->count) return 0;
    int c = uui_appcards_columns(v), s = v->selected < 0 ? 0 : v->selected;
    int page = ((v->h - head_h(v)) / pitch_h(v)) * c;
    if (page < c) page = c;
    switch (key) {
    case KEY_ARROW_LEFT:  if (c == 1) return 0; s = s > 0 ? s - 1 : 0; break;
    case KEY_ARROW_RIGHT: if (c == 1) return 0; s = s < v->count - 1 ? s + 1 : s; break;
    case KEY_ARROW_UP:    s = s - c >= 0 ? s - c : s; break;
    case KEY_ARROW_DOWN:  s = s + c < v->count ? s + c : s; break;
    case KEY_PAGE_UP:     s = s - page >= 0 ? s - page : s % c; break;
    case KEY_PAGE_DOWN:   s = s + page < v->count ? s + page : v->count - 1; break;
    case KEY_HOME:        s = 0; break;
    case KEY_END:         s = v->count - 1; break;
    case '\n': case '\r':
        if (v->selected >= 0) v->activated = v->selected;
        return v->selected >= 0;
    case ' ':
        if (v->selected >= 0 && v->items[v->selected].button && !v->items[v->selected].button_greyed)
            v->pressed = v->selected;
        return v->selected >= 0;
    default: return 0;
    }
    uui_appcards_select(v, s);
    return 1;
}

static int a_accepts(const void *w) { return ((const struct uui_appcards *)w)->count > 0; }
static void a_focused(void *w, int f) { ((struct uui_appcards *)w)->focused = f; }

static int a_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_DEFAULT;
}

// Each visible item and its button, for a test that clicks one.
static void a_describe(const void *w, const struct uui_describe *d) {
    const struct uui_appcards *v = w;
    uui_describe_int(d, "count", v->count);
    uui_describe_int(d, "selected", v->selected);
    uui_describe_int(d, "columns", uui_appcards_columns(v));
    uui_describe_int(d, "layout", v->layout);
    uui_describe_int(d, "top", v->top);
    if (v->layout == UUI_APPCARD_ROWS) uui_describe_rect(d, "header", v->x, v->y, v->w, head_h(v));
    for (int i = 0; i < v->count; i++) {
        int x, y, iw, ih;
        if (!uui_appcards_item_rect(v, i, &x, &y, &iw, &ih)) continue;
        uui_describe_rect_i(d, "item", i, x, y, iw, ih);
        if (uui_appcards_button_rect(v, i, &x, &y, &iw, &ih)) uui_describe_rect_i(d, "button", i, x, y, iw, ih);
    }
}

const struct uui_widget_ops uui_appcards_ops = {
    .natural_size  = a_natural,
    .set_geometry  = a_geometry,
    .bounds        = a_bounds,
    .draw          = a_draw,
    .hit           = a_hit,
    .key           = a_key,
    .set_focused   = a_focused,
    .accepts_focus = a_accepts,
    .press         = a_press,
    .motion        = a_motion,
    .release       = a_release,
    .wheel         = a_wheel,
    .cursor        = a_cursor,
    .describe      = a_describe,
};
