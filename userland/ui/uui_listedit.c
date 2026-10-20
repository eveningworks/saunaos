// uui_listedit.c -- a table with a column of buttons beside it (uui_listedit.h).
#include "ui/uui_listedit.h"

#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"

#define CARD_PAD 8

static const char *details_of(const struct uui_listedit *l) {
    const char *t = l->details ? l->details(l->ctx, l->table.selected) : 0;
    return t && t[0] ? t : 0;
}

// The card's height for this text: a line each, and its padding.
static int card_h(const char *t) {
    if (!t) return 0;
    int n = 0;
    for (const char *p = t; *p; p++) n += *p == '\n';
    if (t[0] && t[strlen(t) - 1] != '\n') n++;
    return n * (ugfx_char_h() + 4) + CARD_PAD * 2;
}

void uui_listedit_init(struct uui_listedit *l, const struct uui_table_column *cols, int ncols,
                       uui_table_cell_fn cell, void *ctx, int table_id, const char *table_name,
                       const char *const *labels, const int *codes, const char *const *names, int nbtn) {
    memset(l, 0, sizeof *l);
    if (nbtn > UUI_LISTEDIT_BUTTONS_MAX) nbtn = UUI_LISTEDIT_BUTTONS_MAX;
    uui_table_init(&l->table, 0, 0, 0, 0, cols, ncols, cell, ctx);
    l->items[0] = (struct uui_item){ .ops = &uui_table_ops, .widget = &l->table, .id = table_id, .name = table_name };
    for (int i = 0; i < nbtn; i++) {
        uui_button_init(&l->btn[i], 0, 0, 0, 0, labels[i], UUI_COLOR_UNSET, UUI_COLOR_UNSET, codes[i]);
        l->btn[i].outlined = 1;
        l->items[1 + i] = (struct uui_item){ .ops = &uui_button_ops, .widget = &l->btn[i], .id = codes[i],
                                             .name = names ? names[i] : 0 };
    }
    l->nbtn = nbtn;
    l->rows_shown = 6;
    l->ctx = ctx;
}

void uui_listedit_set_rows(struct uui_listedit *l, int rows) { uui_table_set_rows(&l->table, rows); }
int uui_listedit_selected(const struct uui_listedit *l) { return l->table.selected; }

void uui_listedit_select(struct uui_listedit *l, int row) {
    l->table.selected = row >= 0 && row < l->table.row_count ? row : -1;
}

static int buttons_w(const struct uui_listedit *l) {
    int w = 0;
    for (int i = 0; i < l->nbtn; i++) {
        int bw, bh;
        uui_button_natural_size(&l->btn[i], &bw, &bh);
        if (bw > w) w = bw;
    }
    return w;
}

static void natural(const void *w, int *ow, int *oh) {
    const struct uui_listedit *l = w;
    int lh = ugfx_char_h(), bw, bh = lh, gap = ugfx_char_w();
    if (l->nbtn) uui_button_natural_size(&l->btn[0], &bw, &bh);
    int table_h = uui_table_header_h(&l->table) + l->rows_shown * uui_table_row_h(&l->table) + 2;
    const char *t = details_of(l);
    if (t) table_h += gap + card_h(t);
    int col_h = l->nbtn * (bh + gap / 2);
    *ow = ugfx_char_w() * 40 + gap + buttons_w(l);
    *oh = table_h > col_h ? table_h : col_h;
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_listedit *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
    int gap = ugfx_char_w(), bw = buttons_w(l), bh = ugfx_char_h();
    if (l->nbtn) uui_button_natural_size(&l->btn[0], &(int){ 0 }, &bh);
    const char *t = details_of(l);
    int ch = t ? card_h(t) + gap : 0;
    uui_table_ops.set_geometry(&l->table, x, y, width - bw - gap, height - ch);
    for (int i = 0; i < l->nbtn; i++)
        uui_button_set_geometry(&l->btn[i], x + width - bw, y + i * (bh + gap / 2), bw, bh);
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_listedit *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}

// The details card, under the table: label, then value, a line each.
static void draw(struct ugfx_surface *s, const void *w) {
    const struct uui_listedit *l = w;
    const char *t = details_of(l);
    if (!t) return;
    int gap = ugfx_char_w(), ch = card_h(t);
    int cx = l->table.x, cy = l->y + l->h - ch, cw = l->table.w;
    int r = utheme_radius(UTHEME_R_CONTROL);
    uui_fill_round_rect(s, cx, cy, cw, ch, r, UTHEME_WHITE);
    uui_stroke_round_rect(s, cx, cy, cw, ch, r, UTHEME_OUTLINE);
    // The labels' column, as wide as the widest of them.
    int kw = 0;
    for (const char *p = t; *p;) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (tab && tab < end) {
            char k[64];
            int n = (int)(tab - p) < (int)sizeof k - 1 ? (int)(tab - p) : (int)sizeof k - 1;
            memcpy(k, p, (size_t)n);
            k[n] = 0;
            int tw = ugfx_text_width(k);
            if (tw > kw) kw = tw;
        }
        p = nl ? nl + 1 : end;
    }
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int y = cy + CARD_PAD, vx = cx + CARD_PAD + kw + gap * 2;
    for (const char *p = t; *p;) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        char k[64] = "", v[256];
        const char *val = p;
        if (tab && tab < end) {
            int n = (int)(tab - p) < (int)sizeof k - 1 ? (int)(tab - p) : (int)sizeof k - 1;
            memcpy(k, p, (size_t)n);
            k[n] = 0;
            val = tab + 1;
        }
        int vn = (int)(end - val) < (int)sizeof v - 1 ? (int)(end - val) : (int)sizeof v - 1;
        memcpy(v, val, (size_t)vn);
        v[vn] = 0;
        ugfx_draw_string_clipped(s, cx + CARD_PAD, y, kw + gap, k, dim, UTHEME_WHITE);
        ugfx_draw_string_clipped(s, vx, y, cx + cw - CARD_PAD - vx, v, UTHEME_TEXT, UTHEME_WHITE);
        y += ugfx_char_h() + 4;
        p = nl ? nl + 1 : end;
    }
}

static struct uui_item *children(void *w, int *n) {
    struct uui_listedit *l = w;
    *n = 1 + l->nbtn;
    return l->items;
}

// Before the buttons paint: which of them apply to the selected row now.
static void children_begin(struct ugfx_surface *s, void *w) {
    (void)s;
    struct uui_listedit *l = w;
    int row = l->table.selected;
    for (int i = 0; i < l->nbtn; i++)
        l->btn[i].disabled = l->enabled ? !l->enabled(l->ctx, l->btn[i].code, row) : i > 0 && row < 0;
}

const struct uui_widget_ops uui_listedit_ops = {
    .natural_size = natural,
    .set_geometry = geometry,
    .bounds = bounds,
    .draw = draw,
    .children = children,
    .children_begin = children_begin,
};
