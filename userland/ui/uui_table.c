// table -- rows in columns, with a header. See ui/uui_table.h.
#include "ui/uui_table.h"
#include "lib/uimg.h"
#include "ui/uui_widget.h"
#include "ui/uui_route.h"   // UUI_NOWHERE
#include <string.h>
#include <stdlib.h>
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_ctxmenu.h" // the header's column chooser
#include "ui/uapp.h"        // uapp_keyboard_mods(): Shift+wheel scrolls sideways
#include "ui/uui_anim.h"    // the column glide: uui_anim_ms(), uui_anim_request()
#include "rt/sys.h"         // sys_monotonic_ns() -- the glide's clock
#include "lib/udclick.h"     // a divider's double click fits its column
#include <stdio.h>

// Left/right inset inside a column. Hoisted so natural_size() reserves
// exactly what the draw uses -- the "one geometry, shared" rule the
// scrollbar already follows.


// Longest cell text this widget will render. A cell is one column wide
// and gets clipped anyway, so this bounds the app's formatting rather
// than the layout.
#define UUI_TABLE_CELL_MAX 96

static int vcount(const struct uui_table *t);

// A fixed column's width, RESERVED IN DIGITS rather than in the widest
// glyph -- `uui_statusbar.c`'s `fixed_w()` in full, for the same reason
// and with the same consequence when it is got wrong: `char_w` is the
// widest advance in the face, so fixed columns over-reserve on a
// proportional one and eat the width the stretch columns needed.
// A column declaring a character count is nearly always reserving room
// for a number, and digits are one width in any sane face.
//
// One derivation, because column_rect() and natural_size() must agree
// or the table draws its cells outside the columns it measured.
static int col_fixed_w(int chars) {
    int per = ugfx_char_advance('0');
    if (per <= 0) per = ugfx_char_w();
    return chars * per;
}

// The narrowest a stretching column is squeezed: past it, the columns
// scroll sideways (hscroll) rather than lose one.
#define STRETCH_MIN() col_fixed_w(10)

void uui_table_init(struct uui_table *t, int x, int y, int w, int h,
                     const struct uui_table_column *cols, int col_count,
                     uui_table_cell_fn cell, void *ctx) {
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->cols = cols;
    t->col_count = col_count;
    t->row_count = 0;
    t->cell = cell;
    t->ctx = ctx;
    t->show_header = 1;
    t->selected = -1;
    t->hovered = -1;
    t->top = 0;
    uui_scrollanim_init(&t->anim);
    t->row_h = 0; // derive from the font
    uui_sbar_init(&t->sb);
    // Sorting off until an app supplies a comparator, so a table that
    // says nothing about sorting behaves exactly as it did before.
    t->compare = 0;
    t->tint = 0;
    t->fade = 0;
    t->icon = 0;
    t->heat = 0;
    t->meter = 0;
    t->dot = 0;
    t->group = 0;
    t->group_title = 0;
    t->parent = 0;
    t->collapsed = 0;
    t->toggled = -1;
    t->acted = t->acted_col = -1;
    t->structured = 0;
    t->view_count = 0;
    t->sort_col = UUI_TABLE_UNSORTED;
    t->sort_dir = 1;
    t->order_rows = 0;
    // See the header: column 0 by default, so a table that says nothing
    // about searching still answers the keyboard.
    t->seek_col = 0;
    uui_seek_reset(&t->seek);
    t->dbl_row = -1;

    // UNSET means "ask the theme when you draw" (utheme.h). An app
    // setting any of these still wins.
    t->bg = UUI_COLOR_UNSET;
    t->fg = UUI_COLOR_UNSET;
    t->sel_bg = UUI_COLOR_UNSET;
    t->sel_fg = UUI_COLOR_UNSET;
    t->head_bg = UUI_COLOR_UNSET;
    t->head_fg = UUI_COLOR_UNSET;
    t->grid = UUI_COLOR_UNSET;

    t->col_mode = UUI_TABLE_COLS_FIXED;
    t->cdrag.armed = t->cdrag.active = 0;
    t->rsz.col = t->rsz.last_col = -1;
    uui_sbar_init(&t->hsb);
    t->layout_changed = 0;
    memset(t->col_tw_on, 0, sizeof t->col_tw_on);
    uui_table_columns_reset(t);
    t->layout_changed = 0;
}

// --- the column layout ----------------------------------------------------
//
// A table with more columns than UUI_TABLE_MAX_COLS keeps its declared
// order and every column shown: the arrays below are fixed, as `order` is.
static int layout_ok(const struct uui_table *t) {
    return t->col_count > 0 && t->col_count <= UUI_TABLE_MAX_COLS;
}

static int col_locked(const struct uui_table *t, int c) {
    return c == 0 || (t->cols[c].flags & UUI_TCOL_LOCKED);
}

static const char *col_id(const struct uui_table *t, int c) {
    return t->cols[c].id ? t->cols[c].id : (t->cols[c].title ? t->cols[c].title : "");
}

void uui_table_columns_reset(struct uui_table *t) {
    if (!layout_ok(t)) return;
    for (int c = 0; c < t->col_count; c++) {
        t->col_order[c] = (unsigned char)c;
        t->col_hidden[c] = !col_locked(t, c) && (t->cols[c].flags & UUI_TCOL_HIDDEN);
        t->user_w[c] = 0;
    }
    t->layout_changed = 1;
}

void uui_table_set_columns_mode(struct uui_table *t, int mode) {
    t->col_mode = layout_ok(t) ? mode : UUI_TABLE_COLS_FIXED;
}

int uui_table_column_shown(const struct uui_table *t, int col) {
    if (col < 0 || col >= t->col_count) return 0;
    return !layout_ok(t) || !t->col_hidden[col];
}

void uui_table_column_show(struct uui_table *t, int col, int shown) {
    if (!layout_ok(t) || col < 0 || col >= t->col_count || col_locked(t, col)) return;
    if (t->col_hidden[col] == !shown) return;
    t->col_hidden[col] = !shown;
    t->layout_changed = 1;
}

int uui_table_column_at(const struct uui_table *t, int pos) {
    if (pos < 0 || pos >= t->col_count) return -1;
    return layout_ok(t) ? t->col_order[pos] : pos;
}

static int col_pos(const struct uui_table *t, int col) {
    if (!layout_ok(t)) return col;
    for (int p = 0; p < t->col_count; p++) if (t->col_order[p] == col) return p;
    return col;
}

void uui_table_column_move(struct uui_table *t, int col, int pos) {
    if (!layout_ok(t) || col < 0 || col >= t->col_count || col_locked(t, col)) return;
    if (pos < 1) pos = 1;
    if (pos >= t->col_count) pos = t->col_count - 1;
    int from = col_pos(t, col);
    if (from == pos) return;
    if (from < pos) for (int p = from; p < pos; p++) t->col_order[p] = t->col_order[p + 1];
    else            for (int p = from; p > pos; p--) t->col_order[p] = t->col_order[p - 1];
    t->col_order[pos] = (unsigned char)col;
    t->layout_changed = 1;
}

int uui_table_take_layout_changed(struct uui_table *t) {
    int c = t->layout_changed;
    t->layout_changed = 0;
    return c;
}

int uui_table_columns_save(const struct uui_table *t, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    out[0] = '\0';
    for (int p = 0; p < t->col_count; p++) {
        int c = uui_table_column_at(t, p);
        const char *id = col_id(t, c);
        char w[12] = "";
        if (t->user_w[c] > 0) snprintf(w, sizeof w, ":%d", t->user_w[c]);
        int len = (int)strlen(id) + (int)strlen(w) + (p ? 1 : 0) + (uui_table_column_shown(t, c) ? 0 : 1);
        if (n + len >= cap) { out[0] = '\0'; return 0; }   // refused, not truncated
        if (p) out[n++] = ',';
        if (!uui_table_column_shown(t, c)) out[n++] = '-';
        memcpy(out + n, id, strlen(id));
        n += (int)strlen(id);
        memcpy(out + n, w, strlen(w));
        n += (int)strlen(w);
        out[n] = '\0';
    }
    return n;
}

int uui_table_columns_load(struct uui_table *t, const char *s) {
    if (!layout_ok(t) || !s || !*s) return 0;
    unsigned char seen[UUI_TABLE_MAX_COLS] = { 0 };
    unsigned char order[UUI_TABLE_MAX_COLS], hidden[UUI_TABLE_MAX_COLS];
    int n = 0, any = 0;
    order[n++] = 0;
    seen[0] = 1;
    for (int c = 0; c < t->col_count; c++)
        hidden[c] = !col_locked(t, c) && (t->cols[c].flags & UUI_TCOL_HIDDEN);
    while (*s) {
        const char *e = strchr(s, ',');
        int len = e ? (int)(e - s) : (int)strlen(s);
        int hide = len > 0 && s[0] == '-';
        const char *name = s + hide;
        int nlen = len - hide, width = 0;
        // `id:width` -- a width the user dragged, in pixels.
        const char *colon = memchr(name, ':', (size_t)nlen);
        if (colon) {
            width = atoi(colon + 1);
            nlen = (int)(colon - name);
        }
        for (int c = 0; c < t->col_count; c++) {
            const char *id = col_id(t, c);
            if (seen[c] || (int)strlen(id) != nlen || strncmp(id, name, (size_t)nlen)) continue;
            seen[c] = 1;
            t->user_w[c] = (short)(width > 0 && width < 10000 ? width : 0);
            order[n++] = (unsigned char)c;
            if (!col_locked(t, c)) hidden[c] = (unsigned char)hide;
            any = 1;
            break;
        }
        // Column 0 named in the string is skipped above (seen): it is
        // first whatever the string says -- but its WIDTH is still the
        // string's, or a narrowed Name comes back full width.
        if (nlen == (int)strlen(col_id(t, 0)) && !strncmp(col_id(t, 0), name, (size_t)nlen)) {
            t->user_w[0] = (short)(width > 0 && width < 10000 ? width : 0);
            any = 1;
        }
        if (!e) break;
        s = e + 1;
    }
    // Columns the string does not name -- added since it was saved --
    // keep their declared order after it.
    for (int c = 0; c < t->col_count; c++) if (!seen[c]) order[n++] = (unsigned char)c;
    memcpy(t->col_order, order, (size_t)t->col_count);
    memcpy(t->col_hidden, hidden, (size_t)t->col_count);
    return any;
}

// The order a frame shows: the committed one, or mid-drag the dragged
// column lifted out and put back in the slot its centre is over -- the
// taskbar's live reorder (wm_taskbar.c's apply_drag()), with columns
// that are not all one width. Writes t->col_count entries.
static void live_order(const struct uui_table *t, unsigned char *out) {
    memcpy(out, t->col_order, (size_t)t->col_count);
    if (!t->cdrag.active) return;
    int d = t->cdrag.col, n = 0;
    unsigned char rest[UUI_TABLE_MAX_COLS];
    for (int p = 0; p < t->col_count; p++) if (t->col_order[p] != d) rest[n++] = t->col_order[p];
    // Where the dragged column's centre is, against the others' centres
    // laid out without it. Its own width is what it will take.
    int dx0, dw;
    uui_table_column_rect(t, d, &dx0, &dw);
    int centre = t->cdrag.x - t->cdrag.grab_dx + dw / 2;
    int x = t->x - t->hscroll, slot = n;   // where the columns start, scrolled
    for (int i = 0; i < n; i++) {
        int c = rest[i], cx, cw;
        uui_table_column_rect(t, c, &cx, &cw);
        if (i > 0 && cw > 0 && centre < x + cw / 2) { slot = i; break; }
        x += cw;
    }
    if (slot < 1) slot = 1;   // never before the locked first column
    // NOR AMONG THE HIDDEN ONES at the end: dropped past the last shown
    // column it lands right after it, so a hidden column chosen later
    // comes back after the one that was moved, not before it.
    int last_shown = 0;
    for (int i = 0; i < n; i++) if (uui_table_column_shown(t, rest[i])) last_shown = i;
    if (slot > last_shown + 1) slot = last_shown + 1;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i == slot) out[k++] = (unsigned char)d;
        out[k++] = rest[i];
    }
    if (slot >= n) out[k++] = (unsigned char)d;
}


// --- colours, resolved when we DRAW ----------------------------------
//
// Not at init: a widget built before a theme change would otherwise
// keep the old palette forever, and every app builds its widgets once.
// See utheme.h's UUI_COLOR.
uint32_t uui_table_c_bg(const struct uui_table *t)      { return UUI_COLOR(t->bg, UTHEME_WHITE); }
uint32_t uui_table_c_fg(const struct uui_table *t)      { return UUI_COLOR(t->fg, UTHEME_TEXT); }
uint32_t uui_table_c_sel_bg(const struct uui_table *t)  { return UUI_COLOR(t->sel_bg, UTHEME_SELECTION); }
uint32_t uui_table_c_sel_fg(const struct uui_table *t)  { return UUI_COLOR(t->sel_fg, UTHEME_TEXT); }
uint32_t uui_table_c_head_bg(const struct uui_table *t) { return UUI_COLOR(t->head_bg, UTHEME_BUTTON_BG); }
uint32_t uui_table_c_grid(const struct uui_table *t)    { return UUI_COLOR(t->grid, UTHEME_SEPARATOR); }
// A header's ink is a step lighter than body text: a third of the way
// to the muted text, (40,42,46) on Light.
uint32_t uui_table_c_head_fg(const struct uui_table *t) {
    return UUI_COLOR(t->head_fg, ugfx_blend(UTHEME_TEXT, utheme_current()->text_muted, 80));
}

int uui_table_row_h(const struct uui_table *t) {
    return t->row_h > 0 ? t->row_h : ugfx_char_h() + 4;
}

int uui_table_header_h(const struct uui_table *t) {
    return t->show_header ? uui_table_row_h(t) : 0;
}

void uui_table_set_seek_col(struct uui_table *t, int col) {
    t->seek_col = (col < t->col_count) ? col : -1;
    uui_seek_reset(&t->seek);
}

void uui_table_set_header(struct uui_table *t, int show) {
    t->show_header = show ? 1 : 0;
}

static int cols_total(const struct uui_table *t);

// The rows' height without a sideways bar: what decides whether a
// vertical one shows, and so how wide the columns' room is -- asked
// without the sideways bar, which depends on that width.
static int rows_fit(const struct uui_table *t, int hbar) {
    int rh = uui_table_row_h(t);
    int avail = t->h - uui_table_header_h(t) - (hbar ? uui_sbar_width() : 0);
    int n = rh > 0 ? avail / rh : 0;
    return n > 0 ? n : 1;
}

int uui_table_hbar_visible(const struct uui_table *t) {
    int vbar = vcount(t) > rows_fit(t, 0);
    return cols_total(t) > t->w - (vbar ? uui_sbar_width() : 0);
}

int uui_table_visible_rows(const struct uui_table *t) {
    return rows_fit(t, uui_table_hbar_visible(t));
}

int uui_table_scrollbar_visible(const struct uui_table *t) {
    return vcount(t) > uui_table_visible_rows(t);
}

// Clamps `top` so the view always fills when it can -- a table scrolled
// past its end shows blank rows under real ones, which reads as missing
// data rather than as the end of the list.
static void table_clamp(struct uui_table *t) {
    int vis = uui_table_visible_rows(t);
    int max_top = vcount(t) > vis ? vcount(t) - vis : 0;
    if (t->top > max_top) t->top = max_top;
    if (t->top < 0) t->top = 0;
}

// --- ordering: the sort, groups and the tree ------------------------
//
// See uui_table.h for why the app compares and the widget permutes.
// Everything below maintains `order`, a view-position -> app-row map;
// identity when there is no sort, so every caller can go through it
// unconditionally and there is no second, unsorted code path to keep
// in step.

// Rows on screen. The row count itself while the order is stale -- the
// same fallback source_row() takes -- so nothing indexes past it.
static int vcount(const struct uui_table *t) {
    return t->order_rows == t->row_count ? t->view_count : t->row_count;
}

int uui_table_view_count(const struct uui_table *t) { return vcount(t); }

// Scratch for a rebuild, shared by every table: a rebuild is never
// re-entered, and nothing survives the call. Grown with the largest
// table, like each table's own arrays (fit_rows()).
static int *s_sorted, *s_stk, *s_pos;
static unsigned char *s_grp;
static int s_cap;

#define GROW(p, n) do { void *q_ = realloc((p), sizeof *(p) * (size_t)(n)); \
                        if (!q_) return 0; (p) = q_; } while (0)

// Room for `n` rows in this table's arrays and the shared scratch; 0 when
// memory ran out, which leaves both at what they held.
static int fit_rows(struct uui_table *t, int n) {
    if (n > s_cap) {
        int cap = n + 64;
        GROW(s_sorted, cap); GROW(s_stk, cap); GROW(s_pos, cap); GROW(s_grp, cap);
        s_cap = cap;
    }
    if (n > t->rows_cap || !t->order) {
        int cap = n + 16;
        GROW(t->order, cap + UUI_TABLE_MAX_GROUPS);
        GROW(t->depth, cap); GROW(t->kids, cap); GROW(t->up, cap);
        t->rows_cap = cap;
    }
    return 1;
}

static int row_collapsed(const struct uui_table *t, int r) {
    return t->collapsed && t->collapsed(t->ctx, r);
}

// The app rows in comparator order, into s_sorted.
static void sort_rows(struct uui_table *t, int n) {
    for (int i = 0; i < n; i++) s_sorted[i] = i;
    // Not bounded by col_count: a column a narrow view HIDES can still be
    // what it is sorted by (uui_fileview drops trailing columns).
    if (!t->compare || t->sort_col < 0) return;

    // Insertion sort: STABLE, which is what makes a second sort on a
    // different column keep the previous column's order within ties --
    // the behaviour every desktop table has. Also correct at n = 0 and
    // 1 with no special cases. Quadratic in the worst case, which a few
    // hundred rows of a handful of comparisons each does not notice.
    for (int i = 1; i < n; i++) {
        int v = s_sorted[i];
        int j = i - 1;
        while (j >= 0) {
            int c = t->compare(t->ctx, s_sorted[j], v, t->sort_col) * t->sort_dir;
            if (c <= 0) break;   // <= keeps equal elements in place: stable
            s_sorted[j + 1] = s_sorted[j];
            j--;
        }
        s_sorted[j + 1] = v;
    }
}

// Each row's effective parent: in range, in the same group, and not on
// a cycle. Every member of a cycle walks back to itself within n steps,
// so the first one visited is cut and the rest then end at a root.
static void link_rows(struct uui_table *t, int n) {
    for (int r = 0; r < n; r++) {
        int p = t->parent ? t->parent(t->ctx, r) : -1;
        if (p < 0 || p >= n || p == r || s_grp[p] != s_grp[r]) p = -1;
        t->up[r] = p;
        t->kids[r] = 0;
        t->depth[r] = 0;
    }
    for (int r = 0; r < n; r++) {
        int x = t->up[r];
        for (int steps = 0; x >= 0 && steps <= n; steps++) {
            if (x == r) { t->up[r] = -1; break; }
            x = t->up[x];
        }
    }
    for (int r = 0; r < n; r++)
        if (t->up[r] >= 0) t->kids[t->up[r]] = 1;
}

// A root and every OPEN descendant, depth-first, children in sorted
// order. Iterative: a chain of thousands of nested rows is a legal input.
static int emit_subtree(struct uui_table *t, int n, int root, int v) {
    t->order[v++] = root;
    int sp = 0;
    if (t->kids[root] && !row_collapsed(t, root)) { s_stk[sp] = root; s_pos[sp++] = 0; }
    while (sp > 0) {
        int top = s_stk[sp - 1], found = -1, j;
        for (j = s_pos[sp - 1]; j < n; j++)
            if (t->up[s_sorted[j]] == top) { found = s_sorted[j]; break; }
        if (found < 0) { sp--; continue; }
        s_pos[sp - 1] = j + 1;
        int d = t->depth[top] + 1;
        t->depth[found] = (unsigned char)(d > 255 ? 255 : d);
        t->order[v++] = found;
        if (t->kids[found] && !row_collapsed(t, found)) { s_stk[sp] = found; s_pos[sp++] = 0; }
    }
    return v;
}

static void order_rebuild(struct uui_table *t) {
    // Out of memory: the app's own order, flat. order_rows that matches
    // no row count is what every reader takes as "identity".
    if (!fit_rows(t, t->row_count)) { t->order_rows = -1; t->structured = 0; return; }
    int n = t->row_count;
    sort_rows(t, n);
    t->order_rows = t->row_count;
    t->structured = t->group || t->parent;

    if (!t->structured) {
        for (int i = 0; i < n; i++) {
            t->order[i] = s_sorted[i];
            t->up[i] = -1; t->kids[i] = 0; t->depth[i] = 0;
        }
        t->view_count = t->row_count;
        return;
    }

    for (int r = 0; r < n; r++) {
        int g = t->group ? t->group(t->ctx, r) : 0;
        if (g < 0) g = 0;
        if (g >= UUI_TABLE_MAX_GROUPS) g = UUI_TABLE_MAX_GROUPS - 1;
        s_grp[r] = (unsigned char)g;
    }
    link_rows(t, n);

    int v = 0;
    for (int g = 0; g < UUI_TABLE_MAX_GROUPS; g++) {
        int captioned = 0;
        for (int i = 0; i < n; i++) {
            int r = s_sorted[i];
            if (s_grp[r] != g || t->up[r] >= 0) continue;
            if (t->group && !captioned) { t->order[v++] = UUI_TABLE_CAPTION(g); captioned = 1; }
            v = emit_subtree(t, n, r, v);
        }
    }
    t->view_count = v;

    // A selection folded away moves to the nearest ancestor on screen,
    // so an action never lands on a row the user cannot see.
    if (t->selected >= 0 && t->selected < n && uui_table_view_row(t, t->selected) < 0) {
        int x = t->up[t->selected];
        while (x >= 0 && uui_table_view_row(t, x) < 0) x = t->up[x];
        t->selected = x;
    }
    if (t->hovered >= 0 && uui_table_view_row(t, t->hovered) < 0) t->hovered = -1;
}

int uui_table_source_row(const struct uui_table *t, int view_row) {
    if (view_row < 0) return -1;
    // Built for a different row count: fall back to identity rather than
    // reading a stale slot. Showing the tail unsorted is a visible
    // oddity; indexing the wrong row is a silent one.
    if (t->order_rows != t->row_count) return view_row < t->row_count ? view_row : -1;
    if (view_row >= t->view_count) return -1;
    // Past the permutation's capacity, in plain order only.
    if (!t->structured && view_row >= t->rows_cap) return view_row;
    int e = t->order[view_row];
    return e >= 0 ? e : -1;   // a caption has no app row
}

int uui_table_view_row(const struct uui_table *t, int source_row) {
    if (source_row < 0 || source_row >= t->row_count) return -1;
    if (t->order_rows != t->row_count) return source_row;
    if (!t->structured && source_row >= t->rows_cap) return source_row;
    int n = t->view_count < t->rows_cap + UUI_TABLE_MAX_GROUPS ? t->view_count
                                                               : t->rows_cap + UUI_TABLE_MAX_GROUPS;
    for (int i = 0; i < n; i++) {
        if (t->order[i] == source_row) return i;
    }
    return -1;   // folded under a collapsed parent
}

int uui_table_caption_at(const struct uui_table *t, int view_row) {
    if (!t->structured || t->order_rows != t->row_count) return -1;
    if (view_row < 0 || view_row >= t->view_count) return -1;
    int e = t->order[view_row];
    return e <= UUI_TABLE_CAPTION(0) ? UUI_TABLE_CAPTION(0) - e : -1;
}

void uui_table_set_compare(struct uui_table *t, uui_table_cmp_fn compare) {
    t->compare = compare;
    order_rebuild(t);
}

void uui_table_set_icon(struct uui_table *t, uui_table_icon_fn icon) {
    t->icon = icon;
}

void uui_table_set_meter(struct uui_table *t, uui_table_meter_fn meter) {
    t->meter = meter;
}

void uui_table_set_dot(struct uui_table *t, uui_table_dot_fn dot) {
    t->dot = dot;
}

void uui_table_set_heat(struct uui_table *t, uui_table_heat_fn heat) {
    t->heat = heat;
}

void uui_table_set_fade(struct uui_table *t, uui_table_fade_fn fade) {
    t->fade = fade;
}

void uui_table_set_tint(struct uui_table *t, uui_table_tint_fn tint) {
    t->tint = tint;
}

void uui_table_set_groups(struct uui_table *t, uui_table_group_fn group,
                           uui_table_group_title_fn title) {
    t->group = group;
    t->group_title = title;
    order_rebuild(t);
    table_clamp(t);
}

void uui_table_set_tree(struct uui_table *t, uui_table_parent_fn parent,
                         uui_table_collapsed_fn collapsed) {
    t->parent = parent;
    t->collapsed = collapsed;
    order_rebuild(t);
    table_clamp(t);
}

int uui_table_take_toggled(struct uui_table *t) {
    int r = t->toggled;
    t->toggled = -1;
    return r;
}

int uui_table_take_action(struct uui_table *t, int *col) {
    int r = t->acted;
    if (col) *col = t->acted_col;
    t->acted = t->acted_col = -1;
    return r;
}

// Does `row` show a button in action column `col`? A row whose cell is
// empty has none, and a click or Enter there is no action.
static int has_action(const struct uui_table *t, int row, int col) {
    char buf[UUI_TABLE_CELL_MAX];
    buf[0] = 0;
    if (t->cell) t->cell(t->ctx, row, col, buf, (int)sizeof buf);
    return buf[0] != 0;
}

// The first shown action column, or -1: what Enter acts on.
static int first_action_col(const struct uui_table *t) {
    for (int p = 0; p < t->col_count; p++) {
        int c = uui_table_column_at(t, p);
        if (c >= 0 && (t->cols[c].flags & UUI_TCOL_ACTION) && uui_table_column_shown(t, c)) return c;
    }
    return -1;
}

void uui_table_set_sort(struct uui_table *t, int col, int dir) {
    if (col < UUI_TABLE_UNSORTED || col >= t->col_count) return;
    t->sort_col = col;
    t->sort_dir = dir < 0 ? -1 : 1;
    order_rebuild(t);
}

void uui_table_set_rows(struct uui_table *t, int row_count) {
    t->row_count = row_count;
    // Both of these move on every refresh in a task manager: processes
    // come and go under a live view. Clamping HERE is why the app never
    // has to think about it.
    if (t->selected >= row_count) t->selected = row_count > 0 ? row_count - 1 : -1;
    if (t->hovered >= row_count) t->hovered = -1;
    if (t->toggled >= row_count) t->toggled = -1;
    if (t->acted >= row_count) t->acted = t->acted_col = -1;
    // The app just told us its data changed, so the permutation is what
    // is stale. Rebuilding HERE is why an app that already calls this
    // after a refresh needs no sorting hook of its own -- Task Manager
    // re-reads the process table several times a second and says
    // nothing about sorting beyond supplying the comparator.
    order_rebuild(t);
    table_clamp(t);
}

// Width available to the columns -- the whole widget minus the
// scrollbar, when one is showing.
static int table_content_w(const struct uui_table *t) {
    return t->w - (uui_table_scrollbar_visible(t) ? uui_sbar_width() : 0);
}

// The shown columns' total width, as laid out.
static int cols_total(const struct uui_table *t) {
    int fixed = 0, stretch = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (!uui_table_column_shown(t, i)) continue;
        if (t->user_w[i] > 0) fixed += t->user_w[i];
        else if (t->cols[i].width_chars > 0) fixed += col_fixed_w(t->cols[i].width_chars);
        else stretch++;
    }
    int room = t->w - (vcount(t) > rows_fit(t, 0) ? uui_sbar_width() : 0) - fixed;
    int each = stretch > 0 ? room / stretch : 0;
    if (stretch && each < STRETCH_MIN()) each = STRETCH_MIN();
    return fixed + (stretch ? (each * stretch > room ? each * stretch : room) : 0);
}

static int hscroll_max(const struct uui_table *t) {
    int m = cols_total(t) - table_content_w(t);
    return m > 0 ? m : 0;
}

// The sideways bar, placed and set for this geometry; `hscroll` kept in range.
static struct uui_sbar *hbar(struct uui_table *t) {
    if (t->hscroll > hscroll_max(t)) t->hscroll = hscroll_max(t);
    if (t->hscroll < 0) t->hscroll = 0;
    int on = uui_table_hbar_visible(t);
    t->hsb.horiz = 1;
    uui_sbar_place_h(&t->hsb, t->x, t->y + t->h - uui_sbar_width(), on ? table_content_w(t) : 0);
    uui_sbar_set(&t->hsb, cols_total(t), table_content_w(t), t->hscroll);
    return &t->hsb;
}

// Every column's x and width for one display order: hidden ones are 0
// wide, at the x where they would be -- scrolled sideways by `hscroll`.
// The one geometry everything uses.
static void layout_cols(const struct uui_table *t, const unsigned char *order,
                        int *xs, int *ws) {
    int fixed = 0, stretch = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (!uui_table_column_shown(t, i)) continue;
        if (t->user_w[i] > 0) fixed += t->user_w[i];
        else if (t->cols[i].width_chars > 0) fixed += col_fixed_w(t->cols[i].width_chars);
        else stretch++;
    }

    int spare = table_content_w(t) - fixed;
    if (spare < 0) spare = 0;
    // Split evenly between stretch columns; the LAST stretch column
    // takes the rounding remainder so the columns exactly fill the
    // width rather than leaving a one-pixel gap that looks like a bug.
    // Never below STRETCH_MIN each: columns made wider than the table
    // scroll sideways instead of squeezing a stretching one to nothing.
    int each = stretch > 0 ? spare / stretch : 0;
    if (stretch && each < STRETCH_MIN()) { each = STRETCH_MIN(); spare = each * stretch; }

    int x = t->x - t->hscroll, seen_stretch = 0;
    for (int p = 0; p < t->col_count; p++) {
        int c = order ? order[p] : p;
        int width;
        if (!uui_table_column_shown(t, c)) width = 0;
        else if (t->user_w[c] > 0) width = t->user_w[c];
        else if (t->cols[c].width_chars > 0) width = col_fixed_w(t->cols[c].width_chars);
        else {
            seen_stretch++;
            width = (seen_stretch == stretch) ? spare - each * (stretch - 1) : each;
        }
        xs[c] = x;
        ws[c] = width;
        x += width;
    }
}

// The SETTLED place of app column `col`: the committed order, no drag,
// no glide -- what a hit test and a test tool use.
void uui_table_column_rect(const struct uui_table *t, int col,
                            int *out_x, int *out_w) {
    if (out_x) *out_x = t->x;
    if (out_w) *out_w = 0;
    if (col < 0 || col >= t->col_count) return;
    if (!layout_ok(t)) {
        // Past the fixed arrays: declared order, nothing hidden.
        int x = t->x, fixed = 0, stretch = 0;
        for (int i = 0; i < t->col_count; i++) {
            if (t->cols[i].width_chars > 0) fixed += col_fixed_w(t->cols[i].width_chars);
            else stretch++;
        }
        int spare = table_content_w(t) - fixed;
        if (spare < 0) spare = 0;
        int each = stretch > 0 ? spare / stretch : 0, seen = 0, width = 0;
        for (int i = 0; i <= col; i++) {
            if (t->cols[i].width_chars > 0) width = col_fixed_w(t->cols[i].width_chars);
            else { seen++; width = (seen == stretch) ? spare - each * (stretch - 1) : each; }
            if (i < col) x += width;
        }
        if (out_x) *out_x = x;
        if (out_w) *out_w = width;
        return;
    }
    int xs[UUI_TABLE_MAX_COLS], ws[UUI_TABLE_MAX_COLS];
    layout_cols(t, t->col_order, xs, ws);
    if (out_x) *out_x = xs[col];
    if (out_w) *out_w = ws[col];
}

// This frame's drawn rects, into t->hx/hw (header) and t->bx/bw (body):
// the live order eased by each column's glide, the dragged column at the
// pointer. In HEADER mode the body keeps the committed order until the
// drop. Called once at the top of draw; the glide is the draw's own state.
static void frame_cols(struct uui_table *t) {
    int n = t->col_count;
    if (!layout_ok(t)) {
        for (int c = 0; c < n && c < UUI_TABLE_MAX_COLS; c++) {
            int x, w;
            uui_table_column_rect(t, c, &x, &w);
            t->hx[c] = t->bx[c] = (short)x;
            t->hw[c] = t->bw[c] = (short)w;
        }
        return;
    }
    unsigned char live[UUI_TABLE_MAX_COLS];
    int lx[UUI_TABLE_MAX_COLS], lw[UUI_TABLE_MAX_COLS];
    int cx[UUI_TABLE_MAX_COLS], cw[UUI_TABLE_MAX_COLS];
    // HELD AT AN EDGE, the columns scroll under it -- Explorer's header,
    // and the only way to carry a column to one out of view. By the time
    // since the last frame, so the speed is the same at any frame rate.
    int edge_scroll = 0;
    if (t->cdrag.active && hscroll_max(t)) {
        unsigned long long t_ns = sys_monotonic_ns();
        int margin = ugfx_char_advance('n') * 3, dir = 0;
        if (t->cdrag.x < t->x + margin && t->hscroll > 0) dir = -1;
        if (t->cdrag.x > t->x + table_content_w(t) - margin && t->hscroll < hscroll_max(t)) dir = 1;
        if (dir) {
            unsigned long long dt = t->cdrag.scroll_ns ? t_ns - t->cdrag.scroll_ns : 16000000ULL;
            int step = (int)(dt / 2000000ULL);           // 500 px a second
            if (step < 1) step = 1;
            t->hscroll += dir * step;
            if (t->hscroll < 0) t->hscroll = 0;
            if (t->hscroll > hscroll_max(t)) t->hscroll = hscroll_max(t);
            t->cdrag.scroll_ns = t_ns;
            edge_scroll = 1;
        } else {
            t->cdrag.scroll_ns = 0;
        }
    }
    live_order(t, live);
    layout_cols(t, live, lx, lw);
    layout_cols(t, t->col_order, cx, cw);
    unsigned long long now = sys_monotonic_ns();
    int gliding = 0;
    // ONLY A DRAG GLIDES. A resize or a column chosen from the menu
    // moves the columns at once, as it always did; a glide there would
    // read as the table drifting on its own.
    // Nor while the columns scroll under a held one: they move with the
    // scroll at once, or they would trail it.
    int glide = (t->cdrag.active || t->cdrag.settle) && t->col_glide_ms && !edge_scroll;
    t->cdrag.drop_x = t->cdrag.active ? lx[t->cdrag.col] : -1;
    for (int c = 0; c < n; c++) {
        int target = lx[c], x;
        if (t->cdrag.active && c == t->cdrag.col) {
            // Under the pointer, exactly; recorded as the resting value so
            // the drop glides FROM here into its slot.
            // Between the locked first column's right edge and the last
            // column's -- the SCROLLED columns', not the table's own edges.
            int px = t->cdrag.x - t->cdrag.grab_dx;
            int lo = lx[0] + lw[0], hi = t->x - t->hscroll + cols_total(t) - lw[c];
            if (px > hi) px = hi;
            if (px < lo) px = lo;
            utween_start(&t->col_tw[c], px, px, 0, now);
            t->col_tw_to[c] = px;
            t->col_tw_on[c] = 1;
            x = px;
        } else if (!t->col_tw_on[c] || !glide) {
            utween_start(&t->col_tw[c], target, target, 0, now);
            t->col_tw_to[c] = target;
            t->col_tw_on[c] = 1;
            x = target;
        } else {
            if (t->col_tw_to[c] != target) {
                t->col_tw_to[c] = target;
                utween_retarget(&t->col_tw[c], target, t->col_glide_ms, now);
            }
            x = utween_value(&t->col_tw[c], now);
            if (utween_active(&t->col_tw[c])) gliding = 1;
        }
        t->hx[c] = (short)x;
        t->hw[c] = (short)lw[c];
        if (t->col_mode == UUI_TABLE_COLS_HEADER && t->cdrag.active) {
            t->bx[c] = (short)cx[c];
            t->bw[c] = (short)cw[c];
        } else {
            t->bx[c] = (short)x;
            t->bw[c] = (short)lw[c];
        }
    }
    if (gliding || edge_scroll) uui_anim_request();
    else t->cdrag.settle = 0;
}

// One tree level's indent, which is also the expander's box.
static int tree_indent(void) { return ugfx_char_h(); }

// Where row `row`'s expander sits, content-relative, for the hit test.
static void expander_rect(const struct uui_table *t, int row, int *x, int *w) {
    int cx;
    uui_table_column_rect(t, 0, &cx, 0);
    *x = cx + UUI_TABLE_PAD_X + t->depth[row] * tree_indent();
    *w = tree_indent();
}

// A triangle: pointing RIGHT when collapsed, DOWN when open -- KDE's and
// GTK's expander. Stacked spans rather than a glyph, like the sort
// arrow, so it stays font-derived.
static void draw_expander(struct ugfx_surface *s, int x, int y, int box, int h,
                          int collapsed, uint32_t c) {
    int a = box / 4 > 2 ? box / 4 : 2;
    int x0 = x + (box - (collapsed ? a : 2 * a + 1)) / 2;
    if (collapsed) {
        int y0 = y + (h - (2 * a + 1)) / 2;
        for (int r = 0; r <= 2 * a; r++) {
            int w = a + 1 - (r > a ? r - a : a - r);
            ugfx_fill_rect(s, x0, y0 + r, w, 1, c);
        }
    } else {
        int y0 = y + (h - (a + 1)) / 2;
        for (int r = 0; r <= a; r++)
            ugfx_fill_rect(s, x0 + r, y0 + r, 2 * (a - r) + 1, 1, c);
    }
}

// A group's caption row: its title in bold, in the accent, on the
// table's own ground -- a heading, so no hover and no selection.
static void draw_caption(struct ugfx_surface *s, const struct uui_table *t,
                         int group, int ry) {
    char buf[UUI_TABLE_CELL_MAX];
    buf[0] = '\0';
    if (t->group_title) t->group_title(t->ctx, group, buf, (int)sizeof buf);
    if (!buf[0]) return;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int ty = ry + (uui_table_row_h(t) - ugfx_char_h()) / 2;
    ugfx_draw_string_clipped(s, t->x + UUI_TABLE_PAD_X, ty,
                             t->w - 2 * UUI_TABLE_PAD_X, buf,
                             UTHEME_ACCENT, uui_table_c_bg(t));
    ugfx_set_font(was);
}

// One cell's text, drawn clipped to its column and aligned per the
// column's own rule.
static void draw_cell(struct ugfx_surface *s, const struct uui_table *t,
                       int col, int row, int ry, uint32_t fg, uint32_t bg) {
    char buf[UUI_TABLE_CELL_MAX];
    buf[0] = '\0';
    if (t->cell) t->cell(t->ctx, row, col, buf, (int)sizeof buf);
    if (!buf[0]) return;

    int cx = t->bx[col], cw = t->bw[col];
    int avail = cw - UUI_TABLE_PAD_X * 2;
    if (avail <= 0) return;

    int tx = cx + UUI_TABLE_PAD_X;
    if (col == 0 && t->structured && t->parent) {
        // The tree's indent and expander, before the icon: a row with no
        // children keeps the expander's gap so siblings line up.
        int ind = tree_indent(), d = t->depth[row];
        if (t->kids[row] && avail > (d + 1) * ind)
            draw_expander(s, tx + d * ind, ry, ind, uui_table_row_h(t),
                          row_collapsed(t, row), ugfx_blend(bg, fg, 150));
        tx += (d + 1) * ind;
        avail -= (d + 1) * ind;
        if (avail <= 0) return;
    }
    if (col == 0 && t->icon) {
        // The icon at the text's height, then the text after it; a row
        // with no icon keeps the gap, so the names still line up.
        int px = ugfx_char_h();
        const struct uimg *ico = t->icon(t->ctx, row, px);
        if (ico && avail > px) {
            int iy = ry + (uui_table_row_h(t) - ico->h) / 2;
            if (ico->has_alpha) ugfx_blit_alpha(s, tx, iy, ico->w, ico->h, ico->px, ico->w);
            else                ugfx_blit(s, tx, iy, ico->w, ico->h, ico->px, ico->w);
        }
        tx += px + UUI_TABLE_PAD_X;
        avail -= px + UUI_TABLE_PAD_X;
        if (avail <= 0) return;
    }
    uint32_t dot = t->dot ? t->dot(t->ctx, row, col) : 0;
    if (dot) {
        int r = ugfx_char_h() / 4 > 2 ? ugfx_char_h() / 4 : 2;
        if (avail <= 2 * r + UUI_TABLE_PAD_X) return;
        ugfx_fill_circle(s, tx + r, ry + uui_table_row_h(t) / 2, r, dot);
        tx += 2 * r + UUI_TABLE_PAD_X;
        avail -= 2 * r + UUI_TABLE_PAD_X;
    }
    if (t->cols[col].flags & UUI_TCOL_ACTION) {
        // A button's body inside the cell, its text centred: a control, so
        // it keeps its own colours on a selected row.
        int rh = uui_table_row_h(t), tw = ugfx_text_width(buf), bw = tw + 2 * ugfx_char_w();
        if (bw > avail) bw = avail;
        int bh = rh - 4 > ugfx_char_h() ? rh - 4 : ugfx_char_h();
        int bx = tx, by = ry + (rh - bh) / 2;
        uui_control_frame(s, bx, by, bw, bh, UTHEME_BUTTON_BG, UTHEME_OUTLINE);
        int ix = bx + (bw - tw) / 2 > bx + 1 ? bx + (bw - tw) / 2 : bx + 1;
        ugfx_draw_string_clipped(s, ix, ry + (rh - ugfx_char_h()) / 2, bw - 2, buf, UTHEME_TEXT, UTHEME_BUTTON_BG);
        return;
    }
    if (t->cols[col].align == UUI_TALIGN_RIGHT) {
        int tw = ugfx_text_width(buf);
        if (tw < avail) tx = cx + cw - UUI_TABLE_PAD_X - tw;
    }

    int ty = ry + (uui_table_row_h(t) - ugfx_char_h()) / 2;
    // Clipped, always: a column is a fixed box and ugfx_draw_string()
    // does not clip (docs/gui-guidelines.md's oldest trap, which has
    // produced the same overlap bug in two separate files).
    ugfx_draw_string_clipped(s, tx, ty, avail, buf, fg, bg);
}

// The header row: column titles, the sort arrow, and the rule under
// them. Split out because it is skipped ENTIRELY when the header is
// hidden (uui_table_set_header()) -- the rule below would otherwise
// land one pixel above the widget's own rect.
// One column's title and sort arrow, in a header cell at (cx, cw) drawn
// in `fg` on `bg` -- the strip's own, or a dragged column's lift.
static void header_cell(struct ugfx_surface *s, const struct uui_table *t, int c,
                        int cx, int cw, int hh, uint32_t fg, uint32_t bg);

static void table_draw_header(struct ugfx_surface *s, const struct uui_table *t, int hh) {
    ugfx_fill_rect(s, t->x, t->y, t->w, hh, uui_table_c_head_bg(t));
    int lifted = t->cdrag.active ? t->cdrag.col : -1;
    for (int c = 0; c < t->col_count && c < UUI_TABLE_MAX_COLS; c++) {
        if (c == lifted) continue;   // drawn last, over the others
        int cx = t->hx[c], cw = t->hw[c];
        if (cw <= 0) continue;
        header_cell(s, t, c, cx, cw, hh, uui_table_c_head_fg(t), uui_table_c_head_bg(t));
        // Column separator, header only -- a full grid turns a dense
        // table into graph paper, and every desktop table draws the
        // header rule and leaves the body clean.
        if (cx > t->x) ugfx_fill_rect(s, cx, t->y, 1, hh, uui_table_c_grid(t));
    }
    ugfx_fill_rect(s, t->x, t->y + hh - 1, t->w, 1, uui_table_c_grid(t));
}

// THE HELD COLUMN, over everything else: where it would land (a bar in
// the accent), then its header lifted -- an accent fill with a shadow --
// and in COLUMN mode its cells with it, on a tint of the accent. Drawn
// last so the gliding columns pass under it.
static void draw_lift(struct ugfx_surface *s, const struct uui_table *t, int hh,
                      int first, int count, int disp) {
    int d = t->cdrag.col, rh = uui_table_row_h(t);
    int cx = t->hx[d], cw = t->hw[d];
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, t->x, t->y, t->w, t->h);
    if (t->cdrag.drop_x >= 0)
        ugfx_fill_rect(s, t->cdrag.drop_x - 1, t->y, 2, t->h, UTHEME_ACCENT);
    int bottom = hh;
    uint32_t ground = ugfx_blend(uui_table_c_bg(t), UTHEME_ACCENT, 25);
    if (t->col_mode == UUI_TABLE_COLS_COLUMN) {
        int last = t->y + hh + (first - t->top + count) * rh + disp;
        bottom = (last < t->y + t->h ? last : t->y + t->h) - t->y;
        if (bottom < hh) bottom = hh;
    }
    uint32_t shadow = ugfx_blend(uui_table_c_bg(t), 0xFF000000u, 60);
    ugfx_fill_rect(s, cx + 3, t->y + 3, cw, bottom, shadow);
    ugfx_fill_rect(s, cx, t->y, cw, bottom, ground);
    ugfx_fill_rect(s, cx, t->y, cw, hh, UTHEME_ACCENT);
    header_cell(s, t, d, cx, cw, hh, UTHEME_ACCENT_TEXT, UTHEME_ACCENT);
    if (t->col_mode == UUI_TABLE_COLS_COLUMN) {
        ugfx_clip_intersect(s, cx, t->y + hh, cw, t->h - hh);
        for (int i = 0; i < count; i++) {
            int view = first + i;
            if (view < 0 || view >= vcount(t)) continue;
            int idx = uui_table_source_row(t, view);
            if (idx < 0) continue;
            draw_cell(s, t, d, idx, t->y + hh + (view - t->top) * rh + disp,
                      uui_table_c_fg(t), ground);
        }
    }
    ugfx_clip_restore(s, &saved);
}

static void header_cell(struct ugfx_surface *s, const struct uui_table *t, int c,
                        int cx, int cw, int hh, uint32_t fg, uint32_t bg) {
    {
        int avail = cw - UUI_TABLE_PAD_X * 2;
        if (avail <= 0) return;

        // The arrow's reservation is taken out FIRST, before the title
        // is positioned -- not after. Computing it afterwards is what
        // made a RIGHT-aligned title (PID, CPU, Memory) draw hard
        // against the column's right edge, which is exactly where the
        // arrow goes, so the arrow sat on top of the last character.
        // Reserving width for the clip is not enough on its own: a
        // right-aligned string is positioned from the right edge, so
        // the edge itself has to move.
        int arrow = 0;
        if (t->compare && c == t->sort_col) {
            arrow = ugfx_char_w() + UUI_TABLE_PAD_X; // glyph plus a gap
        }
        int text_avail = avail - arrow;
        if (text_avail <= 0) return; // no room for a title at all

        int tx = cx + UUI_TABLE_PAD_X;
        const char *title = t->cols[c].title ? t->cols[c].title : "";
        if (t->cols[c].align == UUI_TALIGN_RIGHT) {
            int tw = ugfx_text_width(title);
            if (tw < text_avail) tx = cx + cw - UUI_TABLE_PAD_X - arrow - tw;
        }
        ugfx_draw_string_clipped(s, tx, t->y + (hh - ugfx_char_h()) / 2,
                                  text_avail, title, fg, bg);
        if (arrow) {
            // Drawn as stacked rows rather than a glyph: the baked font
            // has no arrow character, and a triangle built from the
            // font's own cell size stays font-derived per
            // docs/gui-guidelines.md.
            int aw = ugfx_char_w();
            int ah = aw / 2 > 2 ? aw / 2 : 2;
            int ax = cx + cw - UUI_TABLE_PAD_X - aw;
            int ay = t->y + (hh - ah) / 2;
            for (int r = 0; r < ah; r++) {
                // Ascending points UP, which is the direction the
                // smallest value is: same as Windows and KDE.
                int row = (t->sort_dir > 0) ? r : ah - 1 - r;
                int inset = (ah - 1 - row) * aw / (2 * ah);
                int rw = aw - 2 * inset;
                if (rw < 1) rw = 1;
                ugfx_fill_rect(s, ax + inset, ay + r, rw, 1, fg);
            }
        }
    }
}

void uui_table_draw(struct ugfx_surface *s, const struct uui_table *t) {
    int rh = uui_table_row_h(t);
    int hh = uui_table_header_h(t);
    int vis = uui_table_visible_rows(t);
    int bar = uui_table_scrollbar_visible(t) ? uui_sbar_width() : 0;

    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, uui_table_c_bg(t));
    // This frame's column rects (the glide is the draw's own state, like
    // the scroll's below); every cell and title reads them.
    frame_cols((struct uui_table *)t);
    int lifted = t->cdrag.active ? t->cdrag.col : -1;

    hbar((struct uui_table *)t);                 // hscroll in range before any column is placed
    int hb = uui_table_hbar_visible(t) ? uui_sbar_width() : 0;
    int cw_all = table_content_w(t);

    // --- header ---
    struct ugfx_clip hsaved;
    ugfx_clip_save(s, &hsaved);
    ugfx_clip_intersect(s, t->x, t->y, cw_all, hh);
    if (hh > 0) table_draw_header(s, t, hh);
    ugfx_clip_restore(s, &hsaved);

    // --- rows ---
    // The glide (ui/uui_scrollanim.h): `top` has already moved, and the
    // rows are drawn `disp` px from where it puts them for a few frames,
    // plus the rows the displacement uncovers, clipped to the rows area
    // so a displaced row cannot paint over the header. The animation
    // state is the draw's own; the table is const to its caller.
    int disp = uui_scrollanim_sync((struct uui_scrollanim *)&t->anim, t->top * rh);
    int extra = uui_scrollanim_extra_rows(disp, rh);
    int first = disp > 0 ? t->top - extra : t->top;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, t->x, t->y + hh, cw_all, t->h - hh - hb);
    for (int i = 0; i < vis + 1 + extra; i++) {
        int view = first + i;
        if (view < 0) continue;
        if (view >= vcount(t)) break;
        // The app's row for this SCREEN position. `selected` and
        // `hovered` are app rows too, so the comparisons below are
        // apples to apples and a selection survives a re-sort.
        int ry = t->y + hh + (view - t->top) * rh + disp;
        int cap = uui_table_caption_at(t, view);
        if (cap >= 0) { draw_caption(s, t, cap, ry); continue; }
        int idx = uui_table_source_row(t, view);
        if (idx < 0) break;

        uint32_t rbg = uui_table_c_bg(t), rfg = uui_table_c_fg(t);
        uint32_t tint = t->tint ? t->tint(t->ctx, idx) : 0;
        if (idx == t->selected) { rbg = uui_table_c_sel_bg(t); rfg = uui_table_c_sel_fg(t); }
        else if (idx == t->hovered) { rbg = uui_state_bg(uui_table_c_bg(t), UUI_STATE_HOVER); }
        else if (tint) { rbg = tint; }

        // HALFWAY TO THE BACKGROUND, which is what a cut looks like on
        // every desktop. Applied after the state colours so a faded row
        // that is also selected fades from the SELECTION's text colour.
        if (t->fade && t->fade(t->ctx, idx)) rfg = ugfx_blend(rfg, rbg, 128);

        if (rbg != uui_table_c_bg(t)) ugfx_fill_rect(s, t->x, ry, t->w - bar, rh, rbg);
        for (int c = 0; c < t->col_count && c < UUI_TABLE_MAX_COLS; c++) {
            if (c == lifted && t->col_mode == UUI_TABLE_COLS_COLUMN) continue;  // it travels
            uint32_t cbg = rbg, cfg = rfg;
            // A header-only drag leaves its cells behind until the drop:
            // faded, so it is plain which column is on the move.
            if (c == lifted) cfg = ugfx_blend(rfg, rbg, 140);
            int heat = (t->heat && idx != t->selected) ? t->heat(t->ctx, idx, c) : 0;
            if (heat > 0) {
                // At most ~40% accent, so black text stays readable on
                // the hottest cell.
                if (heat > 255) heat = 255;
                cbg = ugfx_blend(rbg, UTHEME_ACCENT, (uint8_t)(heat * 100 / 255));
                ugfx_fill_rect(s, t->bx[c], ry, t->bw[c], rh, cbg);
            }
            draw_cell(s, t, c, idx, ry, cfg, cbg);
            int pm = t->meter ? t->meter(t->ctx, idx, c) : -1;
            if (pm >= 0 && t->bw[c] > 12) {
                int mx = t->bx[c], mw = t->bw[c];
                mx += 6; mw -= 12;
                if (pm > 1000) pm = 1000;
                int fill = mw * pm / 1000;
                if (pm > 0 && fill < 2) fill = 2;   // a sliver still says "something"
                ugfx_fill_rect(s, mx, ry + rh - 4, mw, 2, ugfx_blend(cbg, rfg, 40));
                ugfx_fill_rect(s, mx, ry + rh - 4, fill, 2, UTHEME_ACCENT);
            }
        }
    }
    ugfx_clip_restore(s, &saved);
    if (lifted >= 0) draw_lift(s, t, hh, first, vis + 1 + extra, disp);

    if (bar) {
        // Beside the ROWS, not the header -- a bar that started at the
        // top would let the thumb sit next to column titles it cannot
        // scroll. IN PIXELS, so the thumb glides with the rows: the
        // ratios are the row ones scaled by rh, which lands the thumb
        // on the same pixels the row-unit hit test computes.
        struct uui_sbar sb = t->sb;
        uui_sbar_place(&sb, t->x + t->w - bar, t->y + hh, t->h - hh - hb);
        uui_sbar_set(&sb, vcount(t) * rh, vis * rh, t->top * rh - disp);
        uui_sbar_draw(&sb, s, uui_table_c_bg(t), uui_table_c_fg(t));
    }
    if (hb) uui_sbar_draw(&t->hsb, s, uui_table_c_bg(t), uui_table_c_fg(t));

    // On the selected ROW, because that is what the arrows move. Round
    // the whole widget instead when the selection is scrolled out of
    // view or there is none -- focus is still here, and an indicator
    // that disappears is the thing this exists to prevent. `selected`
    // is an app row and the loop above walks VIEW positions, so it has
    // to be converted, the same rule uui_table_key()'s arrows follow.
    if (t->focused) {
        int view = uui_table_view_row(t, t->selected);
        int i = view - t->top;
        if (view >= 0 && i >= 0 && i < vis)
            uui_focus_ring(s, t->x, t->y + hh + i * rh + disp, t->w - bar, rh);
        else
            uui_focus_ring(s, t->x, t->y, t->w, t->h);
    }
}

void uui_table_natural_size(const struct uui_table *t, int *out_w, int *out_h) {
    if (out_w) {
        int total = 0;
        for (int i = 0; i < t->col_count; i++) {
            if (!uui_table_column_shown(t, i)) continue;   // a hidden column asks for nothing
            int chars = t->cols[i].width_chars;
            if (chars <= 0) {
                // A stretch column has no width of its own; ask for
                // enough to show its title, and let the layout give
                // more.
                int tw = ugfx_text_width(t->cols[i].title ? t->cols[i].title : "");
                total += tw + UUI_TABLE_PAD_X * 2;
            } else {
                total += col_fixed_w(chars);
            }
        }
        *out_w = total + uui_sbar_width();
    }
    // Header plus a few rows: a preferred MINIMUM, not the whole table.
    // Asking for every row would make a window with 60 processes in it
    // taller than the screen.
    if (out_h) *out_h = uui_table_header_h(t) + uui_table_row_h(t) * 6;
}

int uui_table_header_hit(const struct uui_table *t, int cx, int cy) {
    if (!uui_hit(t->x, t->y, t->w, uui_table_header_h(t), cx, cy)) return -1;
    for (int c = 0; c < t->col_count; c++) {
        int colx, colw;
        uui_table_column_rect(t, c, &colx, &colw);
        if (cx >= colx && cx < colx + colw) return c;
    }
    return -1;
}

int uui_table_double_click(struct uui_table *t, int cx, int cy) {
    int row = uui_table_hit(t, cx, cy);
    unsigned long long now = sys_monotonic_ns();
    if (row >= 0 && row == t->dbl_row && now - t->dbl_ns <= udclick_ns()) {
        t->dbl_row = -1;
        return row;
    }
    t->dbl_row = row;
    t->dbl_ns = now;
    return -1;
}

int uui_table_hit(const struct uui_table *t, int cx, int cy) {
    int bar = uui_table_scrollbar_visible(t) ? uui_sbar_width() : 0;
    int hh = uui_table_header_h(t);
    int hb = uui_table_hbar_visible(t) ? uui_sbar_width() : 0;
    if (!uui_hit(t->x, t->y + hh, t->w - bar, t->h - hh - hb, cx, cy)) return -1;
    int rh = uui_table_row_h(t);
    // Mid-glide the rows sit `disp` px from their resting place, and a
    // hit must land on what is drawn. Floor division, since a row above
    // `top` shows through a positive displacement.
    int rel = cy - t->y - hh - t->anim.disp;
    int view = t->top + (rel >= 0 ? rel / rh : -((-rel + rh - 1) / rh));
    if (view < 0 || view >= vcount(t)) return -1;
    // The APP's row, not the screen position -- every public row index
    // on this widget means the same thing (uui_table.h).
    return uui_table_source_row(t, view);
}

int uui_table_hover(struct uui_table *t, int cx, int cy) {
    int idx = uui_table_hit(t, cx, cy);
    if (idx == t->hovered) return 0;
    t->hovered = idx;
    return 1;
}

int uui_table_click(struct uui_table *t, int cx, int cy) {
    // A header click sorts, and only when the app opted in by supplying
    // a comparator -- otherwise the header stays inert rather than
    // becoming a control that highlights and does nothing.
    if (t->compare) {
        int col = uui_table_header_hit(t, cx, cy);
        if (col >= 0) {
            // Clicking the SAME column reverses; a different one starts
            // ascending. That is the rule on every desktop, and the
            // reason it is here rather than in each app.
            if (col == t->sort_col) uui_table_set_sort(t, col, -t->sort_dir);
            else                    uui_table_set_sort(t, col, 1);
            return 1;
        }
    }

    int idx = uui_table_hit(t, cx, cy);
    if (idx >= 0 && t->structured && t->parent && t->kids[idx]) {
        int ex, ew;
        expander_rect(t, idx, &ex, &ew);
        if (cx >= ex && cx < ex + ew) {
            t->toggled = idx;
            t->selected = idx;
            return 1;
        }
    }
    if (idx >= 0) {
        // An action cell acts even on the row already selected.
        for (int c = 0; c < t->col_count && c < UUI_TABLE_MAX_COLS; c++) {
            int x, w;
            if (!(t->cols[c].flags & UUI_TCOL_ACTION) || !uui_table_column_shown(t, c)) continue;
            uui_table_column_rect(t, c, &x, &w);
            if (w > 0 && cx >= x && cx < x + w && has_action(t, idx, c)) {
                t->acted = idx;
                t->acted_col = c;
                t->selected = idx;
                return 1;
            }
        }
    }
    if (idx < 0 || idx == t->selected) return 0;
    t->selected = idx;
    return 1;
}

// --- the scrollbar's own input ---------------------------------------

// The bar in ROWS, below the header, filled in before anything asks it.
struct uui_sbar *uui_table_sbar(struct uui_table *t) {
    int hh = uui_table_header_h(t);
    int hb = uui_table_hbar_visible(t) ? uui_sbar_width() : 0;
    uui_sbar_place(&t->sb, t->x + t->w - uui_sbar_width(), t->y + hh,
                   uui_table_scrollbar_visible(t) ? t->h - hh - hb : 0);
    uui_sbar_set(&t->sb, vcount(t), uui_table_visible_rows(t), t->top);
    return &t->sb;
}

int uui_table_press(struct uui_table *t, int cx, int cy) {
    int r = uui_sbar_press(uui_table_sbar(t), cx, cy);
    if (t->sb.grab >= 0) uui_scrollanim_cancel(&t->anim); // a drag draws where the thumb is, at once
    if (r & UUI_SBAR_MOVED) {
        uui_scrollanim_arm(&t->anim);   // a page glides
        t->top = t->sb.top;
    }
    return (r & UUI_SBAR_TOOK) != 0;
}

int uui_table_drag(struct uui_table *t, int cx, int cy) {
    if (t->sb.grab < 0) return 0;
    int r = uui_sbar_motion(uui_table_sbar(t), cx, cy, 1);
    if (r & UUI_SBAR_MOVED) t->top = t->sb.top;
    return (r & UUI_SBAR_MOVED) != 0;
}

void uui_table_drag_end(struct uui_table *t) {
    uui_sbar_release(&t->sb);
}

// The bar's hover, for a motion with no button: 1 if it needs a redraw,
// and *on set when the pointer is on the strip (so not on a row).
int uui_table_bar_hover(struct uui_table *t, int cx, int cy, int *on) {
    int r = uui_sbar_motion(uui_table_sbar(t), cx, cy, 0);
    *on = (r & UUI_SBAR_TOOK) != 0;
    return (r & UUI_SBAR_REDRAW) != 0;
}

int uui_table_wheel(struct uui_table *t, int notches) {
    int before = t->top;
    uui_scrollanim_arm(&t->anim);
    // Scrolling must NOT change the selection -- a wheel over a table is
    // navigation, not a choice.
    t->top -= notches * 3;
    table_clamp(t);
    return t->top != before;
}

static void table_reveal(struct uui_table *t) {
    int vis = uui_table_visible_rows(t);
    // `top` is a VIEW offset and `selected` is an APP row, so the two
    // are only interchangeable while the table is unsorted. Comparing
    // them directly scrolls to wherever that app row happens to sit in
    // the underlying data, which once sorted is not where it is drawn.
    int view = uui_table_view_row(t, t->selected);
    if (view < 0) return;
    if (view < t->top) t->top = view;
    else if (view >= t->top + vis) t->top = view - vis + 1;
    table_clamp(t);
}

// --- type-ahead --------------------------------------------------------
//
// The search is ui/uui_seek.h's, shared with uui_listbox. What is added
// here is the index space: the callback is handed a VIEW position and
// pulls the cell for the app row behind it, so a sorted table cycles in
// the order on SCREEN. Walking the app's order instead would make the
// same letter jump around a sorted table, which is the bug
// uui_table_key()'s arrow handling already avoids.

static void tb_seek_text(void *ctx, int view_row, char *out, int cap) {
    const struct uui_table *t = (const struct uui_table *)ctx;
    out[0] = '\0';
    if (!t->cell || t->seek_col < 0) return;
    int row = uui_table_source_row(t, view_row);
    if (row < 0) return;
    t->cell(t->ctx, row, t->seek_col, out, cap);
}

// Returns 1 if the selection moved.
static int table_seek(struct uui_table *t, int key) {
    if (t->seek_col < 0 || !t->cell) return 0;
    int view = uui_table_view_row(t, t->selected);
    int idx = uui_seek_key(&t->seek, key, vcount(t), view,
                            tb_seek_text, t);
    if (idx < 0) return 0;
    int row = uui_table_source_row(t, idx);
    if (row < 0 || row == t->selected) return 0;
    t->selected = row;
    table_reveal(t);
    return 1;
}

// Off a caption: `dir` first, the other way at the ends. A caption is a
// heading, never a keyboard stop.
static int skip_caption(const struct uui_table *t, int view, int dir) {
    int n = vcount(t), v = view;
    while (v >= 0 && v < n && uui_table_caption_at(t, v) >= 0) v += dir;
    if (v >= 0 && v < n) return v;
    v = view;
    while (v >= 0 && v < n && uui_table_caption_at(t, v) >= 0) v -= dir;
    return (v >= 0 && v < n) ? v : -1;
}

// Left and Right in a tree, as in every tree view: Right opens a closed
// parent, then steps to its first child; Left closes an open one, then
// steps to the parent. Opening and closing are the APP's (`toggled`).
static int tree_key(struct uui_table *t, int key) {
    int r = t->selected;
    if (r < 0 || r >= t->row_count || r >= t->rows_cap) return 0;
    if (key == KEY_ARROW_RIGHT) {
        if (!t->kids[r]) return 0;
        if (row_collapsed(t, r)) { t->toggled = r; return 1; }
        int child = uui_table_source_row(t, uui_table_view_row(t, r) + 1);
        if (child < 0 || t->up[child] != r) return 0;
        t->selected = child;
    } else {
        if (t->kids[r] && !row_collapsed(t, r)) { t->toggled = r; return 1; }
        if (t->up[r] < 0) return 0;
        t->selected = t->up[r];
    }
    table_reveal(t);
    return 1;
}

int uui_table_key(struct uui_table *t, int key) {
    if (vcount(t) <= 0) return 0;
    if ((key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) && t->structured && t->parent)
        return tree_key(t, key);
    uui_scrollanim_arm(&t->anim); // a key that scrolls the view glides it
    int before = t->selected;
    int vis = uui_table_visible_rows(t);

    // Motion happens in VIEW order and is converted back at the end:
    // Down must move to the row visibly BELOW, which is not
    // selected + 1 once the table is sorted. Doing this in app indices
    // is the bug that makes arrow keys jump around a sorted table.
    int view = uui_table_view_row(t, t->selected);
    if (view < 0) view = 0;
    int dir = 1;   // which way to step off a caption

    if (key == KEY_ARROW_UP)        { if (view > 0) view--; dir = -1; }
    else if (key == KEY_ARROW_DOWN) { if (view < vcount(t) - 1) view++; }
    else if (key == KEY_HOME)       { view = 0; }
    else if (key == KEY_END)        { view = vcount(t) - 1; dir = -1; }
    else if (key == KEY_PAGE_UP)    { view -= vis; if (view < 0) view = 0; dir = -1; }
    else if (key == KEY_PAGE_DOWN)  { view += vis;
                                       if (view >= vcount(t)) view = vcount(t) - 1; }
    // A PRINTABLE KEY IS A SEARCH, not a keystroke to pass on.
    else if ((key == '\n' || key == '\r') && t->selected >= 0 && first_action_col(t) >= 0) {
        if (!has_action(t, t->selected, first_action_col(t))) return 0;
        t->acted = t->selected;
        t->acted_col = first_action_col(t);
        return 1;
    }
    else if (uui_seek_is_key(key)) return table_seek(t, key);
    else return 0;

    if (view < 0) view = 0;
    view = skip_caption(t, view, dir);
    if (view < 0) return 0;   // nothing but captions
    t->selected = uui_table_source_row(t, view);
    if (t->selected < 0) t->selected = 0;
    // A movement key ends a search in progress, so the next letter
    // starts a new one rather than extending an abandoned prefix.
    uui_seek_reset(&t->seek);
    table_reveal(t);
    return t->selected != before;
}

// --- the ops table ----------------------------------------------------
//
// Filled in for pointer input too, so an app declares a table and never
// mentions input again -- see ui/uui_widget.h on why that matters.
static void tb_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_table_natural_size((const struct uui_table *)w, out_w, out_h);
}

static void tb_ops_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_table *t = (struct uui_table *)w;
    t->x = x; t->y = y; t->w = width; t->h = height;
    // The visible row count just changed, so `top` may now point past a
    // valid first row. This is what makes a RESIZE reflow correctly
    // instead of showing a blank band under the last row.
    table_clamp(t);
}

static void tb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_table_draw(s, (const struct uui_table *)w);
}

// `>= 0`, NOT the row index. The router tests this as a BOOLEAN
// (`!it->ops->hit(...)` in ui/uui_route.c), so returning the index makes
// ROW 0 report "not hit" -- it is the one row whose index is falsey, so
// the first row of the table silently cannot be selected while every
// other row works. See uui_listbox.c, which had the identical bug.
static int tb_ops_hit(const void *w, int cx, int cy) {
    // The WHOLE widget, not just its rows. uui_table_hit() deliberately
    // excludes the header and the scrollbar column -- it answers "which
    // ROW", a different question -- so routing on it meant the router
    // never delivered a press to either. The scrollbar had been working
    // only because a press on it also lands inside no row and was
    // reaching uui_table_press() by way of a hit on a NEIGHBOURING row;
    // a header click landed on nothing at all and was silently dropped,
    // which is why clicking a column title appeared to do nothing.
    //
    // Same distinction uui_listbox's lb_ops_bounds() already draws, and
    // for the same reason.
    const struct uui_table *t = (const struct uui_table *)w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

static int tb_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_table_key((struct uui_table *)w, key);
}

static int tb_ops_accepts_focus(const void *w) { (void)w; return 1; }
static void tb_ops_set_focused(void *w, int focused) {
    ((struct uui_table *)w)->focused = focused;
}

// Pixels a header press must travel before it is a drag, not a sort
// click: the taskbar's TB_DRAG_START, Windows' SM_CXDRAG.
#define COL_DRAG_START 4
#define COL_GLIDE_MS 150

// --- resizing a column: its header's right edge -------------------------

#define DIV_GRAB 3   // pixels either side of a divider that take it

// The shown column whose right edge is at `cx` in the header, or -1.
static int divider_at(const struct uui_table *t, int cx, int cy) {
    int hh = uui_table_header_h(t);
    if (hh <= 0 || cy < t->y || cy >= t->y + hh || cx < t->x || cx >= t->x + table_content_w(t)) return -1;
    int best = -1, bd = DIV_GRAB + 1;
    for (int c = 0; c < t->col_count; c++) {
        if (!uui_table_column_shown(t, c)) continue;
        int x, w;
        uui_table_column_rect(t, c, &x, &w);
        int d = cx - (x + w);
        if (d < 0) d = -d;
        if (w > 0 && d < bd) { bd = d; best = c; }
    }
    return best;
}

static int col_min_w(void) { return ugfx_char_w() * 3; }

// Column `c` as wide as its widest cell or its title, whichever is wider.
static void fit_column(struct uui_table *t, int c) {
    char buf[256];
    int w = ugfx_text_width(t->cols[c].title);
    for (int r = 0; r < t->row_count && t->cell; r++) {
        buf[0] = 0;
        t->cell(t->ctx, r, c, buf, (int)sizeof buf);
        int tw = ugfx_text_width(buf);
        if (tw > w) w = tw;
    }
    w += UUI_TABLE_PAD_X * 2 + 2;
    if (c == 0 && (t->icon || t->parent)) w += uui_table_row_h(t) + UUI_TABLE_PAD_X;   // its icon or expander
    t->user_w[c] = (short)(w < col_min_w() ? col_min_w() : w);
    t->layout_changed = 1;
}

static int tb_ops_cursor(const void *w, int cx, int cy) {
    const struct uui_table *t = (const struct uui_table *)w;
    return t->rsz.col >= 0 || divider_at(t, cx, cy) >= 0 ? WIN_CURSOR_RESIZE_H : WIN_CURSOR_DEFAULT;
}

static int tb_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_table *t = (struct uui_table *)w;
    // The scrollbar outranks the rows: uui_table_hit() excludes the bar
    // column, so a press there has to be offered to the bar first or it
    // reaches nothing at all.
    if (uui_table_press(t, cx, cy)) return 1;
    // The sideways bar, when the columns overflow.
    {
        int r = uui_sbar_press(hbar(t), cx, cy);
        if (r & UUI_SBAR_MOVED) t->hscroll = t->hsb.top;
        if (r & UUI_SBAR_TOOK) return 1;
    }
    // A DIVIDER outranks its column's title: held, it resizes the column
    // to its left; pressed twice in a double click's time, it fits it.
    int div = divider_at(t, cx, cy);
    if (div >= 0) {
        unsigned long long now = sys_monotonic_ns();
        if (div == t->rsz.last_col && now - t->rsz.last_ns <= udclick_ns()) {
            fit_column(t, div);
            t->rsz.last_col = -1;
            return 1;
        }
        int x;
        uui_table_column_rect(t, div, &x, &t->rsz.start_w);
        t->rsz.col = div;
        t->rsz.press_x = cx;
        t->rsz.last_col = div;
        t->rsz.last_ns = now;
        return 1;
    }
    // MOVABLE COLUMNS ARM ON THE PRESS: what it becomes -- a drag, or a
    // sort click -- is decided by the motion after it, as a taskbar
    // button's is. Without them the header still sorts on the press.
    int col = t->col_mode ? uui_table_header_hit(t, cx, cy) : -1;
    if (col >= 0) {
        int colx, colw;
        uui_table_column_rect(t, col, &colx, &colw);
        t->cdrag.armed = 1;
        t->cdrag.active = 0;
        t->cdrag.scroll_ns = 0;
        t->cdrag.col = col;
        t->cdrag.press_x = t->cdrag.x = cx;
        t->cdrag.press_y = cy;
        t->cdrag.grab_dx = cx - colx;
        return 1;
    }
    return uui_table_click(t, cx, cy);
}

static int tb_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_table *t = (struct uui_table *)w;
    if ((buttons & 1) && t->rsz.col >= 0) {
        int nw = t->rsz.start_w + cx - t->rsz.press_x;
        if (nw < col_min_w()) nw = col_min_w();
        if (nw == t->user_w[t->rsz.col]) return 0;
        t->user_w[t->rsz.col] = (short)nw;
        return 1;
    }
    if ((buttons & 1) && t->cdrag.armed) {
        int dx = cx - t->cdrag.press_x;
        if (!t->cdrag.active && (dx > COL_DRAG_START || dx < -COL_DRAG_START) &&
            !col_locked(t, t->cdrag.col)) {
            t->cdrag.active = 1;
            // Read once per drag: two registry reads, not one a frame.
            t->col_glide_ms = uui_anim_ms(COL_GLIDE_MS);
        }
        if (!t->cdrag.active || cx == t->cdrag.x) return 0;
        t->cdrag.x = cx;
        return 1;
    }
    if ((buttons & 1) && t->hsb.grab >= 0) {
        int r = uui_sbar_motion(hbar(t), cx, cy, buttons);
        if (r & UUI_SBAR_MOVED) t->hscroll = t->hsb.top;
        return r != 0;
    }
    if (buttons & 1) return uui_table_drag(t, cx, cy);
    int on = 0, redraw = uui_table_bar_hover(t, cx, cy, &on);
    if (on) return uui_table_hover(t, UUI_NOWHERE, UUI_NOWHERE) | redraw;
    return uui_table_hover(t, cx, cy) | redraw;
}

static int tb_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_table *t = (struct uui_table *)w;
    if (t->rsz.col >= 0) {
        if (t->user_w[t->rsz.col] > 0) t->layout_changed = 1;
        t->rsz.col = -1;
        return 1;
    }
    if (t->hsb.grab >= 0) {
        uui_sbar_release(&t->hsb);
        return 1;
    }
    if (t->cdrag.armed) {
        t->cdrag.armed = 0;
        if (!t->cdrag.active)   // never moved: it was a click on the title
            return uui_table_click(t, t->cdrag.press_x, t->cdrag.press_y);
        // THE DROP: the order the drag showed becomes the order, and
        // the columns glide from where they were drawn into it.
        unsigned char live[UUI_TABLE_MAX_COLS];
        live_order(t, live);
        t->cdrag.active = 0;
        t->cdrag.settle = 1;
        if (memcmp(live, t->col_order, (size_t)t->col_count)) {
            memcpy(t->col_order, live, (size_t)t->col_count);
            t->layout_changed = 1;
        }
        return 1;
    }
    uui_table_drag_end(t);
    return 0;
}

// --- the column chooser: the header's own right-click menu ---------------

#define COL_MENU_RESET 1000

static unsigned col_menu_flags(void *w, int code) {
    const struct uui_table *t = (const struct uui_table *)w;
    if (code == COL_MENU_RESET) return 0;
    int c = code - 1;
    if (c < 0 || c >= t->col_count) return UUI_MI_DISABLED;
    return (uui_table_column_shown(t, c) ? UUI_MI_CHECKED : 0) |
           (col_locked(t, c) ? UUI_MI_DISABLED : 0);
}

static int col_menu_run(void *w, int code) {
    struct uui_table *t = (struct uui_table *)w;
    if (code == COL_MENU_RESET) { uui_table_columns_reset(t); return 1; }
    int c = code - 1;
    if (c < 0 || c >= t->col_count || col_locked(t, c)) return 0;
    uui_table_column_show(t, c, !uui_table_column_shown(t, c));
    return 1;
}

static int tb_ops_context_menu(void *w, int cx, int cy, struct uui_ctxmenu_src *out) {
    struct uui_table *t = (struct uui_table *)w;
    if (!t->col_mode || uui_table_header_h(t) <= 0) return 0;
    if (!uui_hit(t->x, t->y, t->w, uui_table_header_h(t), cx, cy)) return 0;
    // EVERY column, in the order the app declared them -- Windows' list
    // and Dolphin's -- ticked when shown; the locked ones greyed.
    int n = 0;
    for (int c = 0; c < t->col_count; c++)
        t->col_menu[n++] = (struct uui_menu_item)UUI_MENU(t->cols[c].title, c + 1, 0);
    t->col_menu[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    t->col_menu[n++] = (struct uui_menu_item)UUI_MENU("Reset columns", COL_MENU_RESET, 0);
    out->w = t;
    out->items = t->col_menu;
    out->count = n;
    out->flags = col_menu_flags;
    out->run = col_menu_run;
    return 1;
}

// Shift+wheel scrolls sideways, as in every desktop's lists.
static int tb_ops_wheel(void *w, int notches) {
    struct uui_table *t = (struct uui_table *)w;
    if ((uapp_keyboard_mods() & KEY_MOD_SHIFT) && hscroll_max(t)) {
        int was = t->hscroll;
        t->hscroll -= notches * ugfx_char_advance('n') * 6;
        hbar(t);
        return t->hscroll != was;
    }
    return uui_table_wheel(t, notches);
}

// The rows' pitch, the header's height, each column's x and width, and
// the selection -- what a test needs to click a header or a row
// (ui/uui_describe.h).
static void tb_ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_table *t = (const struct uui_table *)w;
    uui_describe_int(d, "row_h", uui_table_row_h(t));
    uui_describe_int(d, "header_h", uui_table_header_h(t));
    for (int c = 0; c < t->col_count; c++) {
        int cx, cw;
        uui_table_column_rect(t, c, &cx, &cw);
        uui_describe_rect_i(d, "col", c, cx, t->y, cw, t->h);
    }
    // The titles, in the order drawn: which columns a narrow table kept.
    char heads[256];
    int o = 0;
    heads[0] = '\0';
    for (int c = 0; c < t->col_count && o < (int)sizeof heads; c++) {
        int ac = uui_table_column_at(t, c);
        if (layout_ok(t) && t->col_hidden[ac]) continue;
        o += snprintf(heads + o, sizeof heads - (size_t)o, "%s%s", o ? "|" : "",
                      t->cols[ac].title ? t->cols[ac].title : "");
    }
    uui_describe_str(d, "heads", heads);
    uui_describe_int(d, "selected", t->selected);
    uui_describe_int(d, "shown", vcount(t));
    // The display order and what is hidden, by app column, so a test
    // reads a drop or a menu choice rather than inferring it from pixels.
    char layout[256];
    if (t->col_mode && uui_table_columns_save(t, layout, (int)sizeof layout))
        uui_describe_str(d, "columns", layout);
    uui_describe_int(d, "col_dragging", t->cdrag.active);
}

static void tb_ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_table *s = (const struct uui_table *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_table_ops = {
    .natural_size = tb_ops_natural_size,
    .set_geometry = tb_ops_set_geometry,
    .draw = tb_ops_draw,
    .hit = tb_ops_hit,
    .key = tb_ops_key,
    .accepts_focus = tb_ops_accepts_focus,
    .set_focused   = tb_ops_set_focused,
    .press = tb_ops_press,
    .motion = tb_ops_motion,
    .release = tb_ops_release,
    .context_menu = tb_ops_context_menu,
    .wheel = tb_ops_wheel,
    .describe = tb_ops_describe,
    .bounds = tb_ops_bounds,
    .cursor = tb_ops_cursor,
};
