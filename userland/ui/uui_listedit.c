// uui_listedit.c -- a table with a column of buttons beside it (uui_listedit.h).
#include "ui/uui_listedit.h"

#include <string.h>
#include "ui/ugfx.h"

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
    int col_h = l->nbtn * (bh + gap / 2);
    *ow = ugfx_char_w() * 40 + gap + buttons_w(l);
    *oh = table_h > col_h ? table_h : col_h;
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct uui_listedit *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
    int gap = ugfx_char_w(), bw = buttons_w(l), bh = ugfx_char_h();
    if (l->nbtn) uui_button_natural_size(&l->btn[0], &(int){ 0 }, &bh);
    uui_table_ops.set_geometry(&l->table, x, y, width - bw - gap, height);
    for (int i = 0; i < l->nbtn; i++)
        uui_button_set_geometry(&l->btn[i], x + width - bw, y + i * (bh + gap / 2), bw, bh);
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_listedit *l = w;
    *x = l->x; *y = l->y; *ow = l->w; *oh = l->h;
}

static void draw(struct ugfx_surface *s, const void *w) { (void)s; (void)w; }   // the children are all of it

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
