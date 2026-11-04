// Bytes as a grid (ui/uui_hexview.h).
#include "ui/uui_hexview.h"
#include "ui/uui_primitives.h"
#include "ui/uui_ctxmenu.h"
#include "ui/utheme.h"
#include "keyboard.h"

#include <stdio.h>
#include <string.h>

// --- geometry, every number from the grid's face -----------------------------

struct geo {
    int cw, ch, row_h;
    int off_x, hex_x, text_x, digits;
    int rows_y, vis;                 // where the rows start, how many fit
    int bar_w;
};

static uint64_t nrows(const struct uui_hexview *v) {
    return v->size ? (v->size + (uint64_t)v->per_row - 1) / (uint64_t)v->per_row : 0;
}

static int offset_digits(uint64_t size) {
    int d = 8;
    while (d < 16 && size > 0 && ((size - 1) >> (4 * d)) != 0) d++;
    return d;
}

// Measured in the grid's face, the process's own face put back.
static void geo_of(const struct uui_hexview *v, int per_row, struct geo *g) {
    const struct ugfx_font *was = v->font ? ugfx_set_font(v->font) : 0;
    g->cw = ugfx_char_advance('0');
    g->ch = ugfx_char_h();
    if (was) ugfx_set_font(was);
    if (g->cw < 1) g->cw = 1;
    g->row_h = g->ch + ugfx_px(4);
    g->digits = offset_digits(v->size);
    g->off_x = v->x + g->cw;
    g->hex_x = g->off_x + (g->digits + 2) * g->cw;
    g->text_x = g->hex_x + per_row * 3 * g->cw + (per_row / 8 - 1) * g->cw + 2 * g->cw;
    g->rows_y = v->y + g->row_h;
    g->vis = v->h > g->row_h ? (v->h - g->row_h) / g->row_h : 0;
    if (g->vis < 1) g->vis = 1;
    g->bar_w = uui_sbar_width();
}

static int cell_x(const struct geo *g, int i) { return g->hex_x + i * 3 * g->cw + (i / 8) * g->cw; }

int uui_hexview_width_for(const struct uui_hexview *v, int per_row) {
    struct geo g;
    geo_of(v, per_row, &g);
    return g.text_x - v->x + per_row * g.cw + g.cw + g.bar_w;
}

int uui_hexview_visible_rows(const struct uui_hexview *v) {
    struct geo g;
    geo_of(v, v->per_row, &g);
    return g.vis;
}

static uint64_t max_top(const struct uui_hexview *v) {
    uint64_t n = nrows(v), vis = (uint64_t)uui_hexview_visible_rows(v);
    return n > vis ? n - vis : 0;
}

static void clamp_top(struct uui_hexview *v) {
    if (v->top > max_top(v)) v->top = max_top(v);
}

// The scrollbar in rows >> shift, so a file of any length fits its int.
static void place_bar(struct uui_hexview *v) {
    struct geo g;
    geo_of(v, v->per_row, &g);
    uint64_t n = nrows(v);
    int shift = 0;
    while ((n >> shift) > (1u << 30)) shift++;
    v->shift = shift;
    uui_sbar_place(&v->bar, v->x + v->w - g.bar_w, g.rows_y, v->h - (g.rows_y - v->y));
    int vis = (int)((uint64_t)g.vis >> shift);
    uui_sbar_set(&v->bar, (int)(n >> shift), vis > 0 ? vis : 1, (int)(v->top >> shift));
}

static void ensure_visible(struct uui_hexview *v, int centre) {
    if (!v->size) return;
    uint64_t row = v->cursor / (uint64_t)v->per_row, vis = (uint64_t)uui_hexview_visible_rows(v);
    if (row >= v->top && row < v->top + vis) return;
    if (centre) v->top = row > vis / 4 ? row - vis / 4 : 0;
    else if (row < v->top) v->top = row;
    else v->top = row - vis + 1;
    clamp_top(v);
}

// --- state ----------------------------------------------------------------------

void uui_hexview_init(struct uui_hexview *v) {
    memset(v, 0, sizeof *v);
    v->per_row = 16;
    v->hit_cur = -1;
    v->focused = 1;
    uui_sbar_init(&v->bar);
}

void uui_hexview_set_source(struct uui_hexview *v, uint64_t size, uui_hexview_read_fn read, void *ctx) {
    v->size = size;
    v->read = read;
    v->read_ctx = ctx;
    v->top = v->cursor = v->anchor = 0;
    v->has_sel = 0;
    v->dragging = 0;
    v->changed = 1;
}

void uui_hexview_set_per_row(struct uui_hexview *v, int n) {
    if (n < 8 || n > UUI_HEXVIEW_ROW_MAX || n % 8) return;
    v->per_row = n;
    // The cursor's row keeps its place on screen as far as it can.
    v->top = v->cursor / (uint64_t)n;
    uint64_t vis = (uint64_t)uui_hexview_visible_rows(v);
    v->top = v->top > vis / 4 ? v->top - vis / 4 : 0;
    clamp_top(v);
    v->changed = 1;
}

void uui_hexview_goto(struct uui_hexview *v, uint64_t off, uint64_t len) {
    if (!v->size) return;
    if (off >= v->size) off = v->size - 1;
    v->cursor = off;
    v->has_sel = len > 0;
    v->anchor = len > 0 ? (len - 1 > v->size - 1 - off ? v->size - 1 : off + len - 1) : off;
    ensure_visible(v, 1);
    v->changed = 1;
}

int uui_hexview_selection(const struct uui_hexview *v, uint64_t *lo, uint64_t *len) {
    if (!v->has_sel || !v->size) return 0;
    uint64_t a = v->anchor < v->cursor ? v->anchor : v->cursor;
    uint64_t b = v->anchor < v->cursor ? v->cursor : v->anchor;
    *lo = a;
    *len = b - a + 1;
    return 1;
}

int uui_hexview_take_cmd(struct uui_hexview *v) {
    int c = v->cmd;
    v->cmd = 0;
    return c;
}

int uui_hexview_take_changed(struct uui_hexview *v) {
    int c = v->changed;
    v->changed = 0;
    return c;
}

// --- where a byte is ---------------------------------------------------------------

int uui_hexview_byte_rect(const struct uui_hexview *v, uint64_t off, int text,
                          int *x, int *y, int *w, int *h) {
    if (off >= v->size) return 0;
    struct geo g;
    geo_of(v, v->per_row, &g);
    uint64_t row = off / (uint64_t)v->per_row;
    if (row < v->top || row >= v->top + (uint64_t)g.vis) return 0;
    int i = (int)(off % (uint64_t)v->per_row);
    *y = g.rows_y + (int)(row - v->top) * g.row_h;
    *h = g.row_h;
    *x = text ? g.text_x + i * g.cw : cell_x(&g, i);
    *w = text ? g.cw : 3 * g.cw;
    return 1;
}

// The byte at (cx, cy) and its column; -1 off the rows. `clamp` maps a
// point past an edge to the nearest byte (a drag), else it must be on one.
static int64_t byte_at(const struct uui_hexview *v, int cx, int cy, int clamp, int *text) {
    struct geo g;
    geo_of(v, v->per_row, &g);
    if (!v->size) return -1;
    int64_t r = cy < g.rows_y ? -1 : (cy - g.rows_y) / g.row_h;
    if (!clamp && (r < 0 || r >= g.vis)) return -1;
    int64_t row = (int64_t)v->top + r;
    if (row < 0) row = 0;
    int col;
    if (cx >= g.text_x - g.cw) {
        col = (cx - g.text_x) / g.cw;
        *text = 1;
    } else {
        int rel = cx - g.hex_x;
        col = rel < 0 ? 0 : rel / (3 * g.cw);
        // the gap between the groups belongs to the byte before it
        int k = col;
        while (k > 0 && cell_x(&g, k) > cx) k--;
        col = k;
        *text = 0;
    }
    if (col < 0) col = 0;
    if (col >= v->per_row) {
        if (!clamp && !*text) return -1;
        col = v->per_row - 1;
    }
    uint64_t off = (uint64_t)row * (uint64_t)v->per_row + (uint64_t)col;
    if (off >= v->size) {
        if (!clamp && (uint64_t)row * (uint64_t)v->per_row >= v->size) return -1;
        off = v->size - 1;
    }
    return (int64_t)off;
}

// --- drawing ------------------------------------------------------------------------

static uint32_t mid_accent(void) { return ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 130); }

static void draw_hex2(struct ugfx_surface *s, int x, int y, int cw, uint8_t b, uint32_t fg, uint32_t bg) {
    static const char d[] = "0123456789ABCDEF";
    // text-loop-ok: a hex digit from this table, ASCII
    ugfx_draw_char(s, x, y, d[b >> 4], fg, bg);
    // text-loop-ok: a hex digit from this table, ASCII
    ugfx_draw_char(s, x + cw, y, d[b & 15], fg, bg);
}

// One band behind bytes i..j of a row, in both columns.
static void band(struct ugfx_surface *s, const struct geo *g, int ry, int i, int j, uint32_t fill,
                 uint32_t edge) {
    int hx = cell_x(g, i), hw = cell_x(g, j) + 3 * g->cw - hx;
    int tx = g->text_x + i * g->cw, tw = (j - i + 1) * g->cw;
    int by = ry + 1, bh = g->row_h - 2, r = uui_select_radius(0);
    if (edge != UUI_HEXVIEW_NO_TINT) {
        uui_fill_round_rect(s, hx, by, hw, bh, r, edge);
        uui_fill_round_rect(s, hx + 1, by + 1, hw - 2, bh - 2, uui_select_radius(1), fill);
        uui_fill_round_rect(s, tx, by, tw, bh, r, edge);
        uui_fill_round_rect(s, tx + 1, by + 1, tw - 2, bh - 2, uui_select_radius(1), fill);
    } else {
        ugfx_fill_rect(s, hx, ry, hw, g->row_h, fill);
        ugfx_fill_rect(s, tx, ry, tw, g->row_h, fill);
    }
}

// The first hit that ends after `off`.
static int first_hit(const struct uui_hexview *v, uint64_t off) {
    int lo = 0, hi = v->nhits;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (v->hits[mid] + (uint64_t)v->hit_len <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void draw_row(struct ugfx_surface *s, const struct uui_hexview *v, const struct geo *g,
                     uint64_t row, int ry, uint32_t bg, uint32_t muted) {
    uint64_t off = row * (uint64_t)v->per_row;
    int n = v->size - off < (uint64_t)v->per_row ? (int)(v->size - off) : v->per_row;
    uint8_t buf[UUI_HEXVIEW_ROW_MAX];
    int got = v->read ? (int)v->read(v->read_ctx, off, buf, (size_t)n) : 0;
    uint32_t cbg[UUI_HEXVIEW_ROW_MAX];
    for (int i = 0; i < n; i++) cbg[i] = bg;

    // 1. what the bytes mean: a wash per run of one tint
    if (v->tint) {
        uint32_t t[UUI_HEXVIEW_ROW_MAX];
        for (int i = 0; i < n; i++) t[i] = v->tint(v->tint_ctx, off + (uint64_t)i);
        for (int i = 0; i < n;) {
            int j = i;
            while (j + 1 < n && t[j + 1] == t[i]) j++;
            if (t[i] != UUI_HEXVIEW_NO_TINT) {
                uint32_t wash = ugfx_blend(bg, t[i], 46);
                band(s, g, ry, i, j, wash, UUI_HEXVIEW_NO_TINT);
                for (int k = i; k <= j; k++) cbg[k] = wash;
            }
            i = j + 1;
        }
    }
    // 2. find hits
    for (int h = first_hit(v, off); h < v->nhits && v->hits[h] < off + (uint64_t)n; h++) {
        uint64_t a = v->hits[h] > off ? v->hits[h] : off;
        uint64_t b = v->hits[h] + (uint64_t)v->hit_len;
        if (b > off + (uint64_t)n) b = off + (uint64_t)n;
        if (b <= a) continue;
        uint32_t c = h == v->hit_cur ? utheme_current()->find_hit_current : utheme_current()->find_hit;
        band(s, g, ry, (int)(a - off), (int)(b - off - 1), c, UUI_HEXVIEW_NO_TINT);
        for (uint64_t k = a; k < b; k++) cbg[k - off] = c;
    }
    // 3. the selection, over both
    uint64_t lo, len;
    if (uui_hexview_selection(v, &lo, &len) && lo < off + (uint64_t)n && lo + len > off) {
        int i = lo > off ? (int)(lo - off) : 0;
        int j = lo + len < off + (uint64_t)n ? (int)(lo + len - 1 - off) : n - 1;
        band(s, g, ry, i, j, UTHEME_SELECTION, mid_accent());
        for (int k = i; k <= j; k++) cbg[k] = UTHEME_SELECTION;
    }
    // 4. the cursor's byte: the full accent edge in the caret's column, the mid one in the other
    if (v->size && v->cursor >= off && v->cursor < off + (uint64_t)n) {
        int i = (int)(v->cursor - off);
        int r = uui_select_radius(0);
        uint32_t on = v->focused ? UTHEME_ACCENT : mid_accent();
        uui_stroke_round_rect(s, cell_x(g, i), ry + 1, 3 * g->cw, g->row_h - 2, r, v->text_col ? mid_accent() : on);
        uui_stroke_round_rect(s, g->text_x + i * g->cw - 1, ry + 1, g->cw + 2, g->row_h - 2, r,
                              v->text_col ? on : mid_accent());
    }

    int ty = ry + (g->row_h - g->ch) / 2;
    char offs[20];
    snprintf(offs, sizeof offs, "%0*llX", g->digits, (unsigned long long)off);
    ugfx_draw_string(s, g->off_x, ty, offs, muted, bg);
    for (int i = 0; i < n; i++) {
        int x = cell_x(g, i) + g->cw / 2;
        if (i >= got) {   // the read came back short: the file changed, or a disk error
            ugfx_draw_char(s, x, ty, '?', muted, cbg[i]);
            ugfx_draw_char(s, x + g->cw, ty, '?', muted, cbg[i]);
            ugfx_draw_char(s, g->text_x + i * g->cw, ty, '?', muted, cbg[i]);
            continue;
        }
        draw_hex2(s, x, ty, g->cw, buf[i], buf[i] ? UTHEME_TEXT : muted, cbg[i]);
        int printable = buf[i] >= 0x20 && buf[i] < 0x7F;
        // text-loop-ok: ONE BYTE, ONE CELL -- the text column is printable ASCII, '.' for the rest
        ugfx_draw_char(s, g->text_x + i * g->cw, ty, printable ? (char)buf[i] : '.',
                       printable ? UTHEME_TEXT : muted, cbg[i]);
    }
}

static void hv_draw(struct ugfx_surface *s, const void *w) {
    struct uui_hexview *v = (struct uui_hexview *)(uintptr_t)w;
    struct geo g;
    geo_of(v, v->per_row, &g);
    uint32_t bg = UTHEME_WHITE, muted = utheme_current()->text_muted;
    uint32_t head = ugfx_blend(bg, UTHEME_CHROME, 150);
    ugfx_fill_rect(s, v->x, v->y, v->w, v->h, bg);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, v->x, v->y, v->w - (uui_sbar_shown(&v->bar) ? g.bar_w : 0), v->h);
    const struct ugfx_font *was = v->font ? ugfx_set_font(v->font) : 0;

    // The header: the column numbers, the cursor's in the accent.
    ugfx_fill_rect(s, v->x, v->y, v->w, g.row_h, head);
    ugfx_fill_rect(s, v->x, v->y + g.row_h - 1, v->w, 1, UTHEME_SEPARATOR);
    int ty = v->y + (g.row_h - g.ch) / 2;
    ugfx_draw_string(s, g.off_x, ty, "Offset", muted, head);
    int cur_col = v->size ? (int)(v->cursor % (uint64_t)v->per_row) : -1;
    for (int i = 0; i < v->per_row; i++)
        draw_hex2(s, cell_x(&g, i) + g.cw / 2, ty, g.cw, (uint8_t)i, i == cur_col ? UTHEME_ACCENT : muted, head);
    ugfx_draw_string(s, g.text_x, ty, "Text", muted, head);

    uint64_t rows = nrows(v);
    for (int k = 0; k <= g.vis && v->top + (uint64_t)k < rows; k++) {
        int ry = g.rows_y + k * g.row_h;
        if (ry >= v->y + v->h) break;
        draw_row(s, v, &g, v->top + (uint64_t)k, ry, bg, muted);
    }
    if (was) ugfx_set_font(was);
    ugfx_clip_restore(s, &saved);
    place_bar(v);
    uui_sbar_draw(&v->bar, s, bg, UTHEME_TEXT);
}

// --- input -----------------------------------------------------------------------------

static void moved(struct uui_hexview *v, uint64_t to, int extend) {
    if (!v->size) return;
    if (extend) {
        if (!v->has_sel) { v->anchor = v->cursor; v->has_sel = 1; }
    } else {
        v->has_sel = 0;
    }
    v->cursor = to;
    if (!extend) v->anchor = to;
    ensure_visible(v, 0);
    v->changed = 1;
}

int uui_hexview_key(struct uui_hexview *v, int key, unsigned mods) {
    if (!v->size) return 0;
    int ext = (mods & KEY_MOD_SHIFT) != 0, ctrl = (mods & KEY_MOD_CTRL) != 0;
    // Shift+arrow, Shift+Home/End and Ctrl+arrow arrive as KEY CODES of
    // their own (api/keyboard.h): back to the key and the modifier.
    switch (key) {
    case KEY_SHIFT_ARROW_LEFT:  key = KEY_ARROW_LEFT;  ext = 1; break;
    case KEY_SHIFT_ARROW_RIGHT: key = KEY_ARROW_RIGHT; ext = 1; break;
    case KEY_SHIFT_ARROW_UP:    key = KEY_ARROW_UP;    ext = 1; break;
    case KEY_SHIFT_ARROW_DOWN:  key = KEY_ARROW_DOWN;  ext = 1; break;
    case KEY_SHIFT_HOME:        key = KEY_HOME;        ext = 1; break;
    case KEY_SHIFT_END:         key = KEY_END;         ext = 1; break;
    case KEY_CTRL_ARROW_LEFT:   key = KEY_ARROW_LEFT;  break;
    case KEY_CTRL_ARROW_RIGHT:  key = KEY_ARROW_RIGHT; break;
    default: break;
    }
    uint64_t c = v->cursor, pr = (uint64_t)v->per_row, last = v->size - 1;
    uint64_t page = (uint64_t)uui_hexview_visible_rows(v) * pr;
    switch (key) {
    case KEY_ARROW_LEFT:  moved(v, c ? c - 1 : 0, ext); return 1;
    case KEY_ARROW_RIGHT: moved(v, c < last ? c + 1 : last, ext); return 1;
    case KEY_ARROW_UP:    moved(v, c >= pr ? c - pr : c, ext); return 1;
    case KEY_ARROW_DOWN:  moved(v, c + pr <= last ? c + pr : c, ext); return 1;
    case KEY_PAGE_UP:     moved(v, c >= page ? c - page : c % pr, ext); return 1;
    case KEY_PAGE_DOWN: {
        uint64_t to = c + page <= last ? c + page : last;
        moved(v, to, ext);
        // the view moves a page too, not only far enough to show the cursor
        v->top += (uint64_t)uui_hexview_visible_rows(v);
        clamp_top(v);
        ensure_visible(v, 0);
        return 1;
    }
    case KEY_HOME: moved(v, ctrl ? 0 : c - c % pr, ext); return 1;
    case KEY_END: {
        uint64_t end = c - c % pr + pr - 1;
        moved(v, ctrl || end > last ? last : end, ext);
        return 1;
    }
    case '\t':
        v->text_col = !v->text_col;
        v->changed = 1;
        return 1;
    default:
        return 0;
    }
}

static void hv_natural(const void *w, int *ow, int *oh) {
    const struct uui_hexview *v = w;
    struct geo g;
    geo_of(v, v->per_row, &g);
    *ow = uui_hexview_width_for(v, v->per_row);
    *oh = g.row_h * 12;
}

static void hv_geometry(void *w, int x, int y, int width, int height) {
    struct uui_hexview *v = w;
    v->x = x; v->y = y; v->w = width; v->h = height;
    clamp_top(v);
}

static void hv_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_hexview *v = w;
    *x = v->x; *y = v->y; *ow = v->w; *oh = v->h;
}

// The whole rect: the scrollbar is inside it and must stay live.
static int hv_hit(const void *w, int cx, int cy) {
    const struct uui_hexview *v = w;
    return uui_hit(v->x, v->y, v->w, v->h, cx, cy);
}

static int bar_moved(struct uui_hexview *v, int r) {
    if (r & UUI_SBAR_MOVED) {
        v->top = (uint64_t)v->bar.top << v->shift;
        clamp_top(v);
        v->changed = 1;
    }
    return r;
}

static int hv_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_hexview *v = w;
    place_bar(v);
    if (bar_moved(v, uui_sbar_press(&v->bar, cx, cy)) & UUI_SBAR_TOOK) return 1;
    int text;
    int64_t off = byte_at(v, cx, cy, 0, &text);
    if (off < 0) return 1;
    v->text_col = text;
    if (mods & KEY_MOD_SHIFT) {
        moved(v, (uint64_t)off, 1);
    } else {
        moved(v, (uint64_t)off, 0);
        v->dragging = 1;
    }
    return 1;
}

static int hv_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_hexview *v = w;
    place_bar(v);
    int r = bar_moved(v, uui_sbar_motion(&v->bar, cx, cy, buttons));
    if (r & UUI_SBAR_TOOK) return 1;
    if (!v->dragging || !buttons) return (r & UUI_SBAR_REDRAW) != 0;
    // past the top or the bottom, the view follows a row a motion
    struct geo g;
    geo_of(v, v->per_row, &g);
    if (cy < g.rows_y && v->top) v->top--;
    else if (cy >= g.rows_y + g.vis * g.row_h && v->top < max_top(v)) v->top++;
    int text;
    int64_t off = byte_at(v, cx, cy < g.rows_y ? g.rows_y : cy, 1, &text);
    if (off < 0) return 1;
    if ((uint64_t)off != v->cursor) {
        uint64_t keep = v->anchor;
        v->cursor = (uint64_t)off;
        v->anchor = keep;
        v->has_sel = 1;
        v->changed = 1;
    }
    return 1;
}

static int hv_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_hexview *v = w;
    v->dragging = 0;
    uui_sbar_release(&v->bar);
    return 1;
}

static int hv_wheel(void *w, int notches) {
    struct uui_hexview *v = w;
    uint64_t before = v->top, step = (uint64_t)(notches < 0 ? -notches : notches) * 3;
    if (notches > 0) v->top = v->top > step ? v->top - step : 0;
    else v->top += step;
    clamp_top(v);
    if (v->top != before) v->changed = 1;
    return v->top != before;
}

static int hv_cursor(const void *w, int cx, int cy) {
    const struct uui_hexview *v = w;
    int text;
    if (uui_sbar_hit(&v->bar, cx, cy)) return WIN_CURSOR_DEFAULT;
    return byte_at(v, cx, cy, 0, &text) >= 0 ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT;
}

// --- the right-click menu: the caller's rows ---------------------------------------------

static unsigned ctx_flags(void *w, int code) {
    struct uui_hexview *v = w;
    return v->menu_flags ? v->menu_flags(code) : 0;
}

static int ctx_run(void *w, int code) {
    struct uui_hexview *v = w;
    v->cmd = code;
    return 1;
}

static int hv_context_menu(void *w, int cx, int cy, struct uui_ctxmenu_src *out) {
    struct uui_hexview *v = w;
    if (!v->menu || !v->menu_count) return 0;
    int text;
    int64_t off = byte_at(v, cx, cy, 0, &text);
    if (off < 0) return 0;
    // A click OUTSIDE the selection moves to the byte clicked, as Explorer's does to a file.
    uint64_t lo, len;
    if (!uui_hexview_selection(v, &lo, &len) || (uint64_t)off < lo || (uint64_t)off >= lo + len) {
        v->text_col = text;
        moved(v, (uint64_t)off, 0);
    }
    uui_sbar_leave(&v->bar);
    out->w = v;
    out->items = v->menu;
    out->count = v->menu_count;
    out->flags = ctx_flags;
    out->run = ctx_run;
    return 1;
}

// What a test drives the grid by: its columns, the rows, the cursor's cells.
static void hv_describe(const void *w, const struct uui_describe *d) {
    const struct uui_hexview *v = w;
    struct geo g;
    geo_of(v, v->per_row, &g);
    uui_describe_int(d, "per_row", v->per_row);
    uui_describe_int(d, "row_h", g.row_h);
    uui_describe_int(d, "cell_w", g.cw);
    uui_describe_int(d, "hex_x", g.hex_x);
    uui_describe_int(d, "text_x", g.text_x);
    uui_describe_int(d, "rows_y", g.rows_y);
    uui_describe_int(d, "visible", g.vis);
    uui_describe_int(d, "top", v->top > 0x7FFFFFFF ? 0x7FFFFFFF : (int)v->top);
    int x, y, cw, ch;
    if (uui_hexview_byte_rect(v, v->cursor, 0, &x, &y, &cw, &ch)) uui_describe_rect(d, "cursor_hex", x, y, cw, ch);
    if (uui_hexview_byte_rect(v, v->cursor, 1, &x, &y, &cw, &ch)) uui_describe_rect(d, "cursor_text", x, y, cw, ch);
    if (uui_sbar_shown(&v->bar)) uui_describe_rect(d, "bar", v->bar.x, v->bar.y, v->bar.w, v->bar.h);
}

const struct uui_widget_ops uui_hexview_ops = {
    .natural_size = hv_natural,
    .set_geometry = hv_geometry,
    .bounds = hv_bounds,
    .draw = hv_draw,
    .hit = hv_hit,
    .press = hv_press,
    .motion = hv_motion,
    .release = hv_release,
    .wheel = hv_wheel,
    .cursor = hv_cursor,
    .context_menu = hv_context_menu,
    .describe = hv_describe,
};
