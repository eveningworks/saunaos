// Paint's text tool: a box placed by a click, edited, moved, and kept as
// a TEXT LAYER when it is let go -- a click elsewhere, Esc, another tool
// -- that the tool re-opens with a click on it later (Krita's, GIMP's).
// One of Paint's units -- see paint.h.
//
// THE BOX DRAWS INTO A LAYER OF ITS OWN as it is typed (paint_layers.c's
// live layer): each change clears what the last render covered and draws
// the text again. So what shows is exactly what will be kept, at every
// zoom, and letting it go is ONE step -- the layer and its words. The
// caret, the selection and the frame are drawn OVER the picture, never
// into it.
//
// EDITING IS ui/uui_edit.h's -- caret, selection, Ctrl+A/C/X/V, its own
// typing history -- over this file's flat buffer; the lines are
// ui/utextflow.h's, in the box's face. Until it is let go the box can be
// moved by its frame and given a wrap width by a side handle (Windows 11
// Paint's). The face is whatever ui/uui_fontpick.h names, rasterized
// here at the size, in the first colour; Fill puts Colour 2 behind it.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uapp.h"
#include "ui/uui_edit.h"
#include "ui/uui_undo.h"
#include "ui/utextflow.h"
#include "keyboard.h"

#define TEXT_MAX 2048
#define HANDLE_PX 7                  // a wrap handle, screen pixels

enum drag { DRAG_NONE, DRAG_SELECT, DRAG_MOVE, DRAG_WRAP_L, DRAG_WRAP_R };

static int g_active;
static int g_x, g_y;                 // the box's top-left, picture pixels
static int g_wrap;                   // its wrap width; 0 = it grows with the text
static char g_buf[TEXT_MAX];
static int g_len;
static struct uui_edit g_ed;
static struct uui_undo g_hist;
static unsigned char g_hist_buf[8192];
static enum drag g_drag;
static int g_grab_x, g_grab_y;       // a move: where in the box it was taken
static struct upix_rect g_drawn;     // what the last render covered
static int g_reopened = -1;          // the text layer being edited again, or -1
static struct ugfx_font g_font;
static void *g_arena;
static unsigned long g_arena_size;
static char g_font_key[160];         // path + size + embolden of g_font
static int g_have_font;

int text_active(void) { return g_active; }

// The face for the picker's choice, loaded once per change.
static int font_ready(void) {
    char path[128];
    int emb = 0;
    if (!uui_fontpick_path(&g_fontpick, path, sizeof path, &emb)) return 0;
    int px = uui_fontpick_px(&g_fontpick);
    char key[sizeof g_font_key];
    snprintf(key, sizeof key, "%s %d %d", path, px, emb);
    if (g_have_font && !strcmp(key, g_font_key)) return 1;
    unsigned long need = ugfx_font_arena_size(px);
    if (need > g_arena_size) {
        void *a = realloc(g_arena, need);
        if (!a) return 0;
        g_arena = a;
        g_arena_size = need;
    }
    g_have_font = ugfx_font_load(path, px, emb, &g_font, g_arena, g_arena_size);
    if (g_have_font) snprintf(g_font_key, sizeof g_font_key, "%s", key);
    else ulogf("paint: could not load %s at %d\n", path, px);
    return g_have_font;
}

// --- the buffer, as the edit core reaches it --------------------------
//
// THE LINE OPS MEASURE, so every call into uui_edit runs with the box's
// face selected.

static int ed_len(void *t) { (void)t; return g_len; }
static char ed_at(void *t, int i) { (void)t; return i >= 0 && i < g_len ? g_buf[i] : 0; }

static int ed_insert_text(void *t, int i, const char *s, int n) {
    (void)t;
    if (n > TEXT_MAX - 1 - g_len) n = TEXT_MAX - 1 - g_len;
    if (n <= 0 || i < 0 || i > g_len) return 0;
    memmove(g_buf + i + n, g_buf + i, (size_t)(g_len - i));
    memcpy(g_buf + i, s, (size_t)n);
    g_len += n;
    g_buf[g_len] = '\0';
    return n;
}

static int ed_insert(void *t, int i, char c) { return ed_insert_text(t, i, &c, 1); }

static void ed_erase(void *t, int s, int e) {
    (void)t;
    if (s < 0) s = 0;
    if (e > g_len) e = g_len;
    if (s >= e) return;
    memmove(g_buf + s, g_buf + e, (size_t)(g_len - e));
    g_len -= e - s;
    g_buf[g_len] = '\0';
}

static int ed_line_start(void *t, int i) {
    (void)t;
    int x, line;
    utextflow_point(g_buf, g_len, g_wrap, i, &x, &line);
    return utextflow_line_bounds(g_buf, g_len, g_wrap, line, 0);
}

static int ed_line_end(void *t, int i) {
    (void)t;
    int x, line, end;
    utextflow_point(g_buf, g_len, g_wrap, i, &x, &line);
    utextflow_line_bounds(g_buf, g_len, g_wrap, line, &end);
    return end;
}

static int ed_line_up(void *t, int i) {
    (void)t;
    int x, line;
    utextflow_point(g_buf, g_len, g_wrap, i, &x, &line);
    return line ? utextflow_index(g_buf, g_len, g_wrap, x, line - 1) : 0;
}

static int ed_line_down(void *t, int i) {
    (void)t;
    int x, line;
    utextflow_point(g_buf, g_len, g_wrap, i, &x, &line);
    if (line + 1 >= utextflow_measure(g_buf, g_len, g_wrap, 0)) return g_len;
    return utextflow_index(g_buf, g_len, g_wrap, x, line + 1);
}

static const struct uui_edit_ops ED_OPS = {
    .len = ed_len, .at = ed_at, .insert = ed_insert, .erase = ed_erase,
    .line_start = ed_line_start, .line_end = ed_line_end,
    .line_up = ed_line_up, .line_down = ed_line_down, .insert_text = ed_insert_text,
};

// --- the box ----------------------------------------------------------

// What the last render covered, transparent again: the layer is the box's alone.
static void restore(void) {
    for (int j = 0; j < g_drawn.h; j++)
        memset(g_layer->px + (size_t)(g_drawn.y + j) * g_layer->w + g_drawn.x, 0, (size_t)g_drawn.w * 4);
}

// The box's size in picture pixels, with g_font selected: the wrap width
// (or the widest line and room for the caret) by the lines.
static void measure(int *w, int *h) {
    int widest, lines = utextflow_measure(g_buf, g_len, g_wrap, &widest);
    *w = g_wrap > 0 ? g_wrap : widest + ugfx_char_w();
    *h = lines * ugfx_char_h();
}

static void render(void) {
    if (!g_active || !g_layer->px) return;
    restore();
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    if (!font_ready()) { paint_changed(&was); return; }
    const struct ugfx_font *old = ugfx_set_font(&g_font);
    int w, h, lh = ugfx_char_h();
    measure(&w, &h);
    upix_rect_add(&g_drawn, g_layer, g_x, g_y, w, h);
    if (g_drawn.w <= 0) { ugfx_set_font(old); paint_changed(&was); return; }
    uint32_t ink = g_colour[0] & 0xFFFFFF;
    if (g_text_fill) {
        for (int j = 0; j < g_drawn.h; j++) {
            uint32_t *row = g_layer->px + (size_t)(g_drawn.y + j) * g_layer->w + g_drawn.x;
            for (int i = 0; i < g_drawn.w; i++) row[i] = g_colour[1] | 0xFF000000u;
        }
    }
    // THE LETTERS ARE DRAWN AS COVERAGE -- white on black in a scratch the
    // box's size -- and laid on the layer as the ink at that alpha. Drawn
    // straight onto the layer, their soft edges would blend with whatever
    // colour a transparent pixel happens to hold, and fringe.
    static struct uimg cov;
    static size_t cov_cap;
    size_t need = (size_t)g_drawn.w * g_drawn.h;
    if (cov_cap < need) {
        uint32_t *px = realloc(cov.px, need * 4);
        if (!px) { ugfx_set_font(old); paint_changed(&was); return; }
        cov.px = px;
        cov_cap = need;
    }
    cov.w = g_drawn.w;
    cov.h = g_drawn.h;
    for (size_t i = 0; i < need; i++) cov.px[i] = 0;
    struct ugfx_surface s = ugfx_surface_for_pixels(cov.px, cov.w, cov.h);
    int ox = g_x - g_drawn.x, oy = g_y - g_drawn.y;
    static char line[TEXT_MAX];
    struct utextflow_line l = { 0 };
    for (int at = 0, y = oy;; at = l.next, y += lh) {
        utextflow_line(g_buf, g_len, at, g_wrap, &l);
        int n = l.end - l.start;
        memcpy(line, g_buf + l.start, (size_t)n);
        line[n] = '\0';
        ugfx_draw_string(&s, ox, y, line, 0xFFFFFF, UGFX_TRANSPARENT);
        // Lines the face does not have: under the baseline, through the
        // middle of the small letters -- a fifteenth of the size.
        if (n && (g_fontpick.underline || g_fontpick.strike)) {
            int th = lh / 15 > 1 ? lh / 15 : 1;
            if (g_fontpick.underline) ugfx_fill_rect(&s, ox, y + lh * 7 / 8, l.width, th, 0xFFFFFF);
            if (g_fontpick.strike) ugfx_fill_rect(&s, ox, y + lh * 11 / 20, l.width, th, 0xFFFFFF);
        }
        if (l.last) break;
    }
    ugfx_set_font(old);
    for (size_t i = 0; i < need; i++) {
        uint32_t p = cov.px[i];
        unsigned a = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (g > a) a = g;
        if (b > a) a = b;
        cov.px[i] = (uint32_t)a << 24 | ink;
    }
    upix_over_at(g_layer, g_drawn.x, g_drawn.y, &cov);
    struct upix_rect both = was;
    upix_rect_add(&both, g_layer, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    paint_changed(&both);
}

void text_commit(void) {
    if (!g_active) return;
    g_drag = DRAG_NONE;
    // Nothing typed: no layer. Otherwise the words as they are set.
    if (!g_len) {
        g_active = 0;
        layers_live_end(0);
    } else {
        static struct paint_text t;
        memset(&t, 0, sizeof t);
        memcpy(t.buf, g_buf, (size_t)g_len);
        t.len = g_len;
        t.x = g_x;
        t.y = g_y;
        t.wrap = g_wrap;
        text_box(&t.x, &t.y, &t.w, &t.h);
        uui_fontpick_get(&g_fontpick, &t.font);
        t.ink = g_colour[0];
        t.fill_colour = g_colour[1];
        t.fill = g_text_fill;
        g_active = 0;
        layers_live_end(&t);
    }
    ulogf("paint: text committed, %d chars at %d,%d wrap %d\n", g_len, g_x, g_y, g_wrap);
    g_reopened = -1;
    g_len = 0;
    g_buf[0] = '\0';
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    paint_text_changed();
}

int text_box(int *x, int *y, int *w, int *h) {
    if (!g_active) return 0;
    *x = g_x; *y = g_y;
    if (g_have_font) {
        const struct ugfx_font *was = ugfx_set_font(&g_font);
        measure(w, h);
        ugfx_set_font(was);
    } else {
        *w = 8;
        *h = uui_fontpick_px(&g_fontpick);
    }
    return 1;
}

// What a point in picture pixels is over: the text, a wrap handle, the
// frame (a band a few SCREEN pixels wide around the box), or nothing.
static enum drag zone_at(int x, int y) {
    int bx, by, bw, bh;
    if (!text_box(&bx, &by, &bw, &bh)) return DRAG_NONE;
    int z = g_canvas.zoom > 0 ? g_canvas.zoom : 100;
    int band = (HANDLE_PX * 100 + z - 1) / z;   // picture pixels, at least one
    int mid = by + bh / 2;
    if (y >= mid - band && y <= mid + band) {
        if (x >= bx - band && x <= bx) return DRAG_WRAP_L;
        if (x >= bx + bw && x <= bx + bw + band) return DRAG_WRAP_R;
    }
    if (x >= bx && y >= by && x < bx + bw && y < by + bh) return DRAG_SELECT;
    if (x >= bx - band && y >= by - band && x < bx + bw + band && y < by + bh + band) return DRAG_MOVE;
    return DRAG_NONE;
}

// The caret position nearest a point in picture pixels.
static int index_at(int x, int y) {
    const struct ugfx_font *was = ugfx_set_font(&g_font);
    int lh = ugfx_char_h();
    int i = utextflow_index(g_buf, g_len, g_wrap, x - g_x, lh > 0 ? (y - g_y) / lh : 0);
    ugfx_set_font(was);
    return i;
}

// A box at (x, y) on a new layer, or text layer `reopen` again -- its
// words, face and colours back in the box and the bar, the caret at the end.
static void open_box(int x, int y, int reopen) {
    const struct paint_text *t = layers_text(reopen);
    if (!layers_live_begin(t ? reopen : -1)) return;
    g_active = 1;
    g_reopened = t ? reopen : -1;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    if (t) {
        memcpy(g_buf, t->buf, (size_t)t->len + 1);
        g_len = t->len;
        g_x = t->x;
        g_y = t->y;
        g_wrap = t->wrap;
        uui_fontpick_set(&g_fontpick, &t->font);
        g_colour[0] = t->ink;
        if (t->fill) g_colour[1] = t->fill_colour;
        g_text_fill = t->fill;
    } else {
        g_x = x;
        g_y = y;
        g_wrap = 0;
        g_len = 0;
        g_buf[0] = '\0';
    }
    uui_edit_init(&g_ed);
    g_ed.cursor = g_len;
    uui_undo_init(&g_hist, g_hist_buf, sizeof g_hist_buf);
    g_ed.undo = &g_hist;
    render();
    paint_colour_used(0);
    paint_text_changed();
}

void text_pointer(enum uui_paint_phase phase, int x, int y, unsigned mods) {
    if (phase == UUI_PAINT_HOVER) {
        static const int CURSOR[] = {
            [DRAG_NONE] = WIN_CURSOR_TEXT, [DRAG_SELECT] = WIN_CURSOR_TEXT, [DRAG_MOVE] = WIN_CURSOR_MOVE,
            [DRAG_WRAP_L] = WIN_CURSOR_RESIZE_H, [DRAG_WRAP_R] = WIN_CURSOR_RESIZE_H,
        };
        g_canvas.cursor = CURSOR[zone_at(x, y)];
        return;
    }
    if (phase == UUI_PAINT_UP) { g_drag = DRAG_NONE; return; }
    if (phase == UUI_PAINT_MOVE) {
        if (!g_active) return;
        if (g_drag == DRAG_SELECT) {
            uui_edit_place(&g_ed, &ED_OPS, 0, index_at(x, y), 1);
            paint_text_changed();
            return;
        }
        if (g_drag == DRAG_MOVE) {
            g_x = x - g_grab_x;
            g_y = y - g_grab_y;
        } else if (g_drag == DRAG_WRAP_R) {
            g_wrap = x - g_x > 1 ? x - g_x : 1;
        } else if (g_drag == DRAG_WRAP_L) {
            int right = g_x + g_wrap;
            if (x > right - 1) x = right - 1;
            g_x = x;
            g_wrap = right - x;
        } else {
            return;
        }
        render();
        paint_text_changed();
        return;
    }
    // DOWN. Over the box: edit, move or wrap it; anywhere else: let it
    // go, and a click with no box open starts one there (Windows Paint's).
    enum drag z = g_active ? zone_at(x, y) : DRAG_NONE;
    if (z == DRAG_SELECT) {
        int shift = (mods & KEY_MOD_SHIFT) != 0;
        uui_edit_place(&g_ed, &ED_OPS, 0, index_at(x, y), shift);
        if (!shift) { g_ed.sel_anchor = g_ed.cursor; g_ed.sel_active = 1; }   // a drag selects from here
        g_drag = DRAG_SELECT;
        paint_text_changed();
        return;
    }
    if (z != DRAG_NONE) {
        if ((z == DRAG_WRAP_L || z == DRAG_WRAP_R) && g_wrap <= 0) {
            // A box that grew with its text starts wrapping at the width it has.
            int bx, by, bw = 0, bh;
            text_box(&bx, &by, &bw, &bh);
            g_wrap = bw;
        }
        g_grab_x = x - g_x;
        g_grab_y = y - g_y;
        g_drag = z;
        return;
    }
    int was = g_active;
    text_commit();
    if (was) return;   // that click only let the old box go
    if (x < 0 || y < 0 || x >= g_layer->w || y >= g_layer->h) return;
    open_box(x, y, layers_text_at(x, y));
}

int text_key(int key, unsigned mods) {
    if (!g_active) return 0;
    if (key == 0x1B) { text_commit(); return 1; }
    const struct ugfx_font *was = ugfx_set_font(&g_font);
    int took;
    if (key == '\r' || key == '\n') {
        uui_undo_begin(&g_hist);
        uui_edit_delete_selection(&g_ed, &ED_OPS, 0);
        if (uui_edit_insert(&g_ed, &ED_OPS, 0, g_ed.cursor, '\n')) g_ed.cursor++;
        uui_undo_end(&g_hist);
        took = 1;
    } else if (key == 0x1A || key == 0x19) {
        // THE BOX'S OWN HISTORY FIRST; with nothing typed to undo, Ctrl+Z
        // is the picture's -- which lets the box go and takes it away.
        took = key == 0x1A ? uui_edit_undo(&g_ed, &ED_OPS, 0) : uui_edit_redo(&g_ed, &ED_OPS, 0);
    } else {
        took = uui_edit_key(&g_ed, &ED_OPS, 0, key, mods);
    }
    ugfx_set_font(was);
    if (took) render();
    return took;
}

void text_restyle(void) { render(); }

// The open box, its caret, selection and text ('\n' as '|'), last on
// the line so a test can read it whole.
void text_log_layout(void) {
    int bx, by, bw, bh, a = 0, b = 0;
    if (!text_box(&bx, &by, &bw, &bh)) { uapp_logf_layout("paint: layout text 0 0 0 0 open 0\n"); return; }
    if (uui_edit_has_selection(&g_ed)) uui_edit_range(&g_ed, &a, &b);
    static char shown[TEXT_MAX];
    for (int i = 0; i <= g_len; i++) shown[i] = g_buf[i] == '\n' ? '|' : g_buf[i];
    uapp_logf_layout("paint: layout text %d %d %d %d open 1 wrap %d cursor %d sel %d %d text %s\n",
                     bx, by, bw, bh, g_wrap, g_ed.cursor, a, b, shown);
}

// The box's frame, its wrap handles, the selection and the caret, in
// screen space.
void text_draw_over(struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    int bx, by, bw, bh;
    if (!text_box(&bx, &by, &bw, &bh)) return;
    int x0, y0, x1, y1;
    uui_paintcanvas_to_screen(c, bx, by, &x0, &y0);
    uui_paintcanvas_to_screen(c, bx + bw, by + bh, &x1, &y1);
    for (int i = x0; i < x1; i += 6) {
        ugfx_fill_rect(s, i, y0 - 1, i + 3 < x1 ? 3 : x1 - i, 1, UTHEME_ACCENT);
        ugfx_fill_rect(s, i, y1, i + 3 < x1 ? 3 : x1 - i, 1, UTHEME_ACCENT);
    }
    for (int i = y0; i < y1; i += 6) {
        ugfx_fill_rect(s, x0 - 1, i, 1, i + 3 < y1 ? 3 : y1 - i, UTHEME_ACCENT);
        ugfx_fill_rect(s, x1, i, 1, i + 3 < y1 ? 3 : y1 - i, UTHEME_ACCENT);
    }
    int hy = (y0 + y1) / 2 - HANDLE_PX / 2;
    for (int k = 0; k < 2; k++) {
        int hx = (k ? x1 : x0) - HANDLE_PX / 2;
        ugfx_fill_rect(s, hx, hy, HANDLE_PX, HANDLE_PX, UTHEME_ACCENT);
        ugfx_fill_rect(s, hx + 1, hy + 1, HANDLE_PX - 2, HANDLE_PX - 2, UTHEME_WHITE);
    }
    if (!g_have_font) return;
    const struct ugfx_font *was = ugfx_set_font(&g_font);
    int lh = ugfx_char_h();
    // THE SELECTION: the accent, part-transparent, over each line's share.
    if (uui_edit_has_selection(&g_ed)) {
        uint32_t tint = UTHEME_ACCENT;
        int a, b;
        uui_edit_range(&g_ed, &a, &b);
        struct utextflow_line l = { 0 };
        for (int at = 0, k = 0;; at = l.next, k++) {
            utextflow_line(g_buf, g_len, at, g_wrap, &l);
            int from = a > l.start ? a : l.start, to = b < l.end ? b : l.end;
            int brk = b > l.end && l.next > l.end && a <= l.end;   // it runs on past this line's break
            if (from < to || brk) {
                int sx0 = 0, sx1;
                for (int i = l.start; i < from; i++) sx0 += ugfx_char_advance(g_buf[i]);
                sx1 = sx0;
                for (int i = from; i < to; i++) sx1 += ugfx_char_advance(g_buf[i]);
                if (brk) sx1 += ugfx_char_w() / 2;
                int rx0, ry0, rx1, ry1;
                uui_paintcanvas_to_screen(c, bx + sx0, by + k * lh, &rx0, &ry0);
                uui_paintcanvas_to_screen(c, bx + sx1, by + (k + 1) * lh, &rx1, &ry1);
                if (rx1 > rx0 && ry1 > ry0)
                    ugfx_blit_scaled_alpha(s, rx0, ry0, rx1 - rx0, ry1 - ry0, &tint, 1, 1, 1, 96);
            }
            if (l.last) break;
        }
    }
    int cx, line, sx, sy, sy1;
    utextflow_point(g_buf, g_len, g_wrap, g_ed.cursor, &cx, &line);
    ugfx_set_font(was);
    uui_paintcanvas_to_screen(c, bx + cx, by + line * lh, &sx, &sy);
    uui_paintcanvas_to_screen(c, bx + cx, by + (line + 1) * lh, &sx, &sy1);
    ugfx_fill_rect(s, sx, sy, 2, sy1 - sy, UTHEME_TEXT);
}
