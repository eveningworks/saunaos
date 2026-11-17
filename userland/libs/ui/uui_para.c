#include "ui/uui_para.h"
#include "ui/ugfx.h"
#include "ui/utextflow.h"
#include "ui/utheme.h"
#include "ui/uui_route.h"     // UUI_NOWHERE, for edit_target
#include "base/udclick.h"
#include "media/uclip.h"
#include "keyboard.h"         // KEY_INSERT, KEY_MOD_*
#include "win_proto.h"        // WIN_CURSOR_TEXT
#include <stdlib.h>
#include <string.h>

// --- the edit core's view of the text: read, never written --------------

static int p_len(void *t) { return ((struct uui_para *)t)->len; }
static char p_at(void *t, int i) {
    const struct uui_para *p = t;
    return i >= 0 && i < p->len ? p->text[i] : '\0';
}
static int p_insert(void *t, int i, char c) { (void)t; (void)i; (void)c; return 0; }
static void p_erase(void *t, int a, int b) { (void)t; (void)a; (void)b; }

static const struct uui_edit_ops PARA_EDIT_OPS = {
    .len = p_len, .at = p_at, .insert = p_insert, .erase = p_erase,
};

static char EMPTY[1];   // the text when the copy failed; never freed

int uui_para_init(struct uui_para *p, const char *text) {
    memset(p, 0, sizeof *p);
    uui_edit_init(&p->ed);
    p->ed.flags = UUI_EDIT_READONLY;
    p->bg = UUI_COLOR_UNSET;
    p->text = strdup(text ? text : "");
    if (!p->text) {
        p->text = EMPTY;
        return 0;
    }
    p->len = (int)strlen(p->text);
    return 1;
}

void uui_para_free(struct uui_para *p) {
    if (p->text != EMPTY) free(p->text);
    p->text = EMPTY;
    p->len = 0;
    uui_edit_init(&p->ed);
    p->ed.flags = UUI_EDIT_READONLY;
}

static int wrap_w(const struct uui_para *p) { return p->w > 0 ? p->w : 1; }

// --- geometry --------------------------------------------------------------

static void para_natural(const void *w, int *ow, int *oh) {
    const struct uui_para *p = w;
    int cap = ugfx_char_advance('n') * (p->wrap_chars > 0 ? p->wrap_chars : 60);
    int widest = 0;
    int lines = utextflow_measure(p->text, p->len, cap, &widest);
    *ow = widest + 1;   // the last glyph's edge, and room for a caret-width selection
    *oh = (lines > 0 ? lines : 1) * ugfx_char_h();
}

static void para_geometry(void *w, int x, int y, int ww, int hh) {
    struct uui_para *p = w;
    p->x = x; p->y = y; p->w = ww; p->h = hh;
}

static void para_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_para *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}

static int para_hit(const void *w, int cx, int cy) {
    const struct uui_para *p = w;
    return cx >= p->x && cx < p->x + p->w && cy >= p->y && cy < p->y + p->h;
}

// --- drawing ---------------------------------------------------------------

// s[from, to) at x, on `bg`. Copied into `tmp` (the text's length + 1) for
// a NUL: the drawing calls take a C string.
static void run(struct ugfx_surface *s, const struct uui_para *p, char *tmp,
                int x, int y, int max_w, int from, int to, uint32_t bg) {
    if (to <= from || max_w <= 0) return;
    memcpy(tmp, p->text + from, (size_t)(to - from));
    tmp[to - from] = '\0';
    ugfx_draw_string_clipped(s, x, y, max_w, tmp, UTHEME_TEXT, bg);
}

static void para_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_para *p = w;
    if (p->len == 0) return;
    char *tmp = malloc((size_t)p->len + 1);
    if (!tmp) return;
    int lh = ugfx_char_h(), ww = wrap_w(p);
    int a = 0, b = 0;
    if (uui_edit_has_selection(&p->ed)) uui_edit_range(&p->ed, &a, &b);
    uint32_t bg = UUI_COLOR(p->bg, UTHEME_PANEL_BG), sel = UTHEME_SELECTION;
    struct utextflow_line l = { 0 };
    for (int at = 0, k = 0;; at = l.next, k++) {
        utextflow_line(p->text, p->len, at, ww, &l);
        int y = p->y + k * lh;
        if (y >= p->y + p->h) break;
        int from = a > l.start ? a : l.start, to = b < l.end ? b : l.end;
        if (from > to) from = to = l.end;
        if (from < l.start) from = to = l.start;
        int x0 = p->x, xf = x0 + utextflow_width(p->text, l.start, from);
        int xt = xf + utextflow_width(p->text, from, to);
        // A selection running on past this line's break shows as a sliver.
        if (b > l.end && l.next > l.end && a <= l.end)
            ugfx_fill_rect(s, xt, y, ugfx_char_advance(' '), lh, sel);
        if (to > from) ugfx_fill_rect(s, xf, y, xt - xf, lh, sel);
        int right = p->x + p->w;
        run(s, p, tmp, x0, y, right - x0, l.start, from, bg);
        run(s, p, tmp, xf, y, right - xf, from, to, sel);
        run(s, p, tmp, xt, y, right - xt, to, l.end, bg);
        if (l.last) break;
    }
    free(tmp);
}

// --- selecting -------------------------------------------------------------

static int index_at(const struct uui_para *p, int cx, int cy) {
    int line = (cy - p->y) / ugfx_char_h();
    if (cy < p->y) line = 0;
    return utextflow_index(p->text, p->len, wrap_w(p), cx - p->x, line);
}

static int para_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_para *p = w;
    static struct udclick_run clicks;
    int n = udclick_count(&clicks, cx, cy);
    int i = index_at(p, cx, cy);
    if (n == 2) uui_edit_select_word(&p->ed, &PARA_EDIT_OPS, p, i);
    else if (n == 3) uui_edit_select_all(&p->ed, &PARA_EDIT_OPS, p);   // a paragraph is one "line"
    else uui_edit_place(&p->ed, &PARA_EDIT_OPS, p, i, (mods & KEY_MOD_SHIFT) != 0);
    p->dragging = n == 1;
    return 1;
}

static int para_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_para *p = w;
    (void)buttons;
    if (!p->dragging) return 0;
    if (!p->ed.sel_active) {
        p->ed.sel_anchor = p->ed.cursor;
        p->ed.sel_active = 1;
    }
    uui_edit_place(&p->ed, &PARA_EDIT_OPS, p, index_at(p, cx, cy), 1);
    return 1;
}

static int para_release(void *w, int cx, int cy) {
    struct uui_para *p = w;
    (void)cx; (void)cy;
    p->dragging = 0;
    return 1;
}

static int para_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_TEXT;
}

static int para_edit_target(void *w, int cx, int cy, struct uui_edit_target *out) {
    struct uui_para *p = w;
    if (cx == UUI_NOWHERE || !para_hit(p, cx, cy)) return 0;
    out->ed = &p->ed;
    out->ops = &PARA_EDIT_OPS;
    out->text = p;
    int x, line;
    utextflow_point(p->text, p->len, wrap_w(p), p->ed.cursor, &x, &line);
    out->x = p->x + x;
    out->y = p->y + (line + 1) * ugfx_char_h();
    return 1;
}

int uui_para_key(struct uui_para *p, int key, unsigned mods) {
    int copy = key == 0x03 || (key == KEY_INSERT && (mods & KEY_MOD_CTRL));
    if (copy && !uui_edit_has_selection(&p->ed)) {
        uclip_set_text(p->text, p->len);   // nothing selected: all of it, as MessageBox does
        return 1;
    }
    if (copy || key == 0x01) return uui_edit_key(&p->ed, &PARA_EDIT_OPS, p, key, mods);
    return 0;
}

const struct uui_widget_ops uui_para_ops = {
    .natural_size = para_natural,
    .set_geometry = para_geometry,
    .bounds = para_bounds,
    .draw = para_draw,
    .hit = para_hit,
    .edit_target = para_edit_target,
    .press = para_press,
    .motion = para_motion,
    .release = para_release,
    .cursor = para_cursor,
};
