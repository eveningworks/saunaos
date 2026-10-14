// Paint's text tool: a box placed by a click, typed into, and put into
// the picture when it is let go -- a click elsewhere, Esc, another tool.
// One of Paint's units -- see paint.h.
//
// THE TEXT IS DRAWN INTO THE PICTURE AS IT IS TYPED, from the copy the
// undo step took when the box was placed: each keystroke puts that copy
// back over the box and draws the text again. So what shows is exactly
// what will be kept, at every zoom, and committing is the undo step
// closing over the box's rect (lib/upixundo.h).
//
// The face is whatever ui/uui_fontpick.h names -- Liberation, DejaVu --
// rasterized here at the size, in the first colour; Fill puts Colour 2
// behind it. Latin-1, as every string this system draws.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"

#define TEXT_MAX 2048

static int g_active;
static int g_x, g_y;                 // the box's top-left, picture pixels
static char g_buf[TEXT_MAX];
static int g_len;
static struct upix_rect g_drawn;     // what the last render covered
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

// The undo copy back over what the last render covered.
static void restore(void) {
    const struct uimg *snap = &g_undo.snap;
    if (!snap->px || snap->w != g_img.w || snap->h != g_img.h) return;
    for (int j = 0; j < g_drawn.h; j++)
        memcpy(g_img.px + (size_t)(g_drawn.y + j) * g_img.w + g_drawn.x,
               snap->px + (size_t)(g_drawn.y + j) * snap->w + g_drawn.x, (size_t)g_drawn.w * 4);
}

// The box's size in picture pixels: the widest line by the line count.
static void measure(int *w, int *h, int *lh) {
    const struct ugfx_font *was = ugfx_set_font(&g_font);
    *lh = ugfx_char_h();
    int lines = 1, widest = 0, cur = 0;
    char line[TEXT_MAX];
    for (int i = 0; i <= g_len; i++) {
        if (i == g_len || g_buf[i] == '\n') {
            line[cur] = '\0';
            int tw = ugfx_text_width(line);
            if (tw > widest) widest = tw;
            cur = 0;
            if (i < g_len) lines++;
        } else if (cur < TEXT_MAX - 1) {
            line[cur++] = g_buf[i];
        }
    }
    ugfx_set_font(was);
    *w = widest + ugfx_char_w();   // room for the caret after the last letter
    *h = lines * *lh;
}

static void render(void) {
    if (!g_active || !g_img.px) return;
    restore();
    struct upix_rect was = g_drawn;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    if (!font_ready()) { paint_changed(&was); return; }
    int w, h, lh;
    measure(&w, &h, &lh);
    upix_rect_add(&g_drawn, &g_img, g_x, g_y, w, h);
    if (g_drawn.w <= 0) { paint_changed(&was); return; }
    struct ugfx_surface s = ugfx_surface_for_pixels(g_img.px, g_img.w, g_img.h);
    if (g_text_fill)
        ugfx_fill_rect(&s, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h, g_colour[1] & 0xFFFFFF);
    const struct ugfx_font *old = ugfx_set_font(&g_font);
    int y = g_y, start = 0;
    char line[TEXT_MAX];
    for (int i = 0; i <= g_len; i++) {
        if (i == g_len || g_buf[i] == '\n') {
            int n = i - start;
            memcpy(line, g_buf + start, (size_t)n);
            line[n] = '\0';
            ugfx_draw_string(&s, g_x, y, line, g_colour[0] & 0xFFFFFF, UGFX_TRANSPARENT);
            y += lh;
            start = i + 1;
        }
    }
    ugfx_set_font(old);
    // A SURFACE HAS NO ALPHA, the picture does: a pixel the text changed
    // is opaque, one it did not is the undo copy's exactly -- a
    // transparent picture stays transparent around the letters.
    const struct uimg *snap = &g_undo.snap;
    for (int j = 0; j < g_drawn.h; j++) {
        uint32_t *row = g_img.px + (size_t)(g_drawn.y + j) * g_img.w + g_drawn.x;
        const uint32_t *orig = snap->px + (size_t)(g_drawn.y + j) * snap->w + g_drawn.x;
        for (int i = 0; i < g_drawn.w; i++)
            row[i] = ((row[i] ^ orig[i]) & 0xFFFFFF) ? (row[i] | 0xFF000000u) : orig[i];
    }
    struct upix_rect both = was;
    upix_rect_add(&both, &g_img, g_drawn.x, g_drawn.y, g_drawn.w, g_drawn.h);
    paint_changed(&both);
}

void text_commit(void) {
    if (!g_active) return;
    g_active = 0;
    // Nothing typed: nothing to keep, and the undo copy is let go.
    if (!g_len) { restore(); upixundo_commit(&g_undo, &g_img, &(struct upix_rect){ 0, 0, 0, 0 }); }
    else upixundo_commit(&g_undo, &g_img, &g_drawn);
    ulogf("paint: text committed, %d chars at %d,%d\n", g_len, g_x, g_y);
    g_len = 0;
    g_drawn = (struct upix_rect){ 0, 0, 0, 0 };
    paint_text_changed();
}

void text_pointer_down(int x, int y) {
    // INSIDE THE BOX a click keeps typing; anywhere else it lets the box
    // go and starts the next one there -- Windows Paint's.
    if (g_active && x >= g_drawn.x && y >= g_drawn.y && x < g_drawn.x + g_drawn.w &&
        y < g_drawn.y + g_drawn.h)
        return;
    int was = g_active;
    text_commit();
    if (was) return;   // that click only let the old box go
    if (x < 0 || y < 0 || x >= g_img.w || y >= g_img.h) return;
    if (upixundo_begin(&g_undo, &g_img) != 0) { paint_note("Not enough memory for a text box"); return; }
    g_active = 1;
    g_x = x;
    g_y = y;
    g_len = 0;
    g_buf[0] = '\0';
    render();
    paint_colour_used(0);
    paint_text_changed();
}

int text_key(int key) {
    if (!g_active) return 0;
    if (key == 0x1B) { text_commit(); return 1; }
    if (key == '\b' || key == 0x7F) { if (g_len) g_buf[--g_len] = '\0'; }
    else if (key == '\r' || key == '\n') { if (g_len < TEXT_MAX - 1) { g_buf[g_len++] = '\n'; g_buf[g_len] = '\0'; } }
    else if (key >= 0x20 && key <= 0xFF && key != 0x7F) {
        if (g_len < TEXT_MAX - 1) { g_buf[g_len++] = (char)key; g_buf[g_len] = '\0'; }
    } else {
        return 0;
    }
    render();
    return 1;
}

void text_restyle(void) { render(); }

int text_box(int *x, int *y, int *w, int *h) {
    if (!g_active) return 0;
    *x = g_x; *y = g_y;
    *w = g_drawn.w > 0 ? g_drawn.w : 8;
    *h = g_drawn.h > 0 ? g_drawn.h : uui_fontpick_px(&g_fontpick);
    return 1;
}

// The box's frame and the caret after the last letter, in screen space.
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
    // The caret: after the last line's last letter.
    if (!g_have_font) return;
    int last = g_len;
    while (last > 0 && g_buf[last - 1] != '\n') last--;
    int lines = 0;
    for (int i = 0; i < g_len; i++) if (g_buf[i] == '\n') lines++;
    const struct ugfx_font *was = ugfx_set_font(&g_font);
    int lh = ugfx_char_h(), tw = ugfx_text_width(g_buf + last);
    ugfx_set_font(was);
    int cx, cy, cy1;
    uui_paintcanvas_to_screen(c, bx + tw + 1, by + lines * lh, &cx, &cy);
    uui_paintcanvas_to_screen(c, bx + tw + 1, by + (lines + 1) * lh, &cx, &cy1);
    ugfx_fill_rect(s, cx, cy, 2, cy1 - cy, UTHEME_TEXT);
}
