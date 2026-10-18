// frame16.c -- the Windows 3.1 non-client area, drawn by the MAIN thread
// into the toywm window (an undecorated one, WIN_HINT_NO_DECOR): the
// sizing border with its corner notches, the caption, the control-menu
// box and the minimize/maximize arrows. The look is the "Win16 windows in
// toywm" mockup's L3 (docs/w16-design.md). It holds no program state,
// which is why the main thread can draw it while the task runs.
#include "w16_internal.h"

#include "win_proto.h"
#include "ui/ugfx.h"

#define BTN 18                                    // a caption button's width

static uint32_t rgb(uint32_t v) { return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v); }

static int frame_of(const struct w16_wnd *win) { return win->style & WS_THICKFRAME ? W16_FRAME : 1; }
static int has_caption(const struct w16_wnd *win) { return (win->style & WS_CAPTION) == WS_CAPTION; }

void w16_client_rect(const struct w16_wnd *win, int *x, int *y, int *cw, int *ch) {
    int f = frame_of(win), cap = has_caption(win) ? W16_CAPTION : 0;
    *x = f;
    *y = f + cap;
    *cw = win->w - 2 * f;
    *ch = win->h - 2 * f - cap;
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

// A bevelled button face: white top-left, two grey lines bottom-right
// (reversed while pressed), the way 3.1 drew every push button.
static void bevel(struct ugfx_surface *s, int x, int y, int w, int h, int pressed) {
    uint32_t face = rgb(0xC0C0C0), hi = rgb(0xFFFFFF), lo = rgb(0x808080);
    ugfx_fill_rect(s, x, y, w, h, face);
    if (pressed) {
        ugfx_fill_rect(s, x, y, w, 1, lo);
        ugfx_fill_rect(s, x, y, 1, h, lo);
        return;
    }
    ugfx_fill_rect(s, x, y, w - 1, 1, hi);
    ugfx_fill_rect(s, x, y, 1, h - 1, hi);
    ugfx_fill_rect(s, x, y + h - 1, w, 1, lo);
    ugfx_fill_rect(s, x + w - 1, y, 1, h, lo);
    ugfx_fill_rect(s, x + 1, y + h - 2, w - 2, 1, lo);
    ugfx_fill_rect(s, x + w - 2, y + 1, 1, h - 2, lo);
}

// A solid triangle, apex up or down, centred in the box at x,y.
static void arrow(struct ugfx_surface *s, int x, int y, int w, int h, int up, int shift) {
    int rows = 4, cx = x + w / 2 - 1 + shift, cy = y + (h - rows) / 2 + shift;
    for (int r = 0; r < rows; r++) {
        int half = up ? r : rows - 1 - r;
        ugfx_fill_rect(s, cx - half, cy + r, 2 * half + 1, 1, rgb(0x000000));
    }
}

void w16_frame_draw(struct ugfx_surface *s, const struct w16_wnd *win, int active, int pressed) {
    int W = win->w, H = win->h, f = frame_of(win);
    uint32_t black = rgb(0x000000);
    ugfx_draw_rect(s, 0, 0, W, H, black);
    if (f > 1) {
        uint32_t border = rgb(0xC0C0C0);
        ugfx_fill_rect(s, 1, 1, W - 2, f - 2, border);
        ugfx_fill_rect(s, 1, H - f + 1, W - 2, f - 2, border);
        ugfx_fill_rect(s, 1, 1, f - 2, H - 2, border);
        ugfx_fill_rect(s, W - f + 1, 1, f - 2, H - 2, border);
        ugfx_draw_rect(s, f - 1, f - 1, W - 2 * f + 2, H - 2 * f + 2, black);
        // The corner notches: where a drag stops sizing one edge and
        // starts sizing two.
        int n = W16_CAPTION + f - 1;
        ugfx_fill_rect(s, n, 0, 1, f, black);
        ugfx_fill_rect(s, W - n - 1, 0, 1, f, black);
        ugfx_fill_rect(s, n, H - f, 1, f, black);
        ugfx_fill_rect(s, W - n - 1, H - f, 1, f, black);
        ugfx_fill_rect(s, 0, n, f, 1, black);
        ugfx_fill_rect(s, 0, H - n - 1, f, 1, black);
        ugfx_fill_rect(s, W - f, n, f, 1, black);
        ugfx_fill_rect(s, W - f, H - n - 1, f, 1, black);
    }
    if (!has_caption(win)) return;

    int cx = f, cy = f, cw = W - 2 * f, ch = W16_CAPTION - 1;
    ugfx_fill_rect(s, cx, cy, cw, ch, active ? rgb(0x000080) : rgb(0xFFFFFF));
    ugfx_fill_rect(s, cx, cy + ch, cw, 1, black);
    int left = cx, right = cx + cw;
    if (win->style & WS_SYSMENU) {                 // the control-menu box
        ugfx_fill_rect(s, cx, cy, BTN, ch, rgb(0xC0C0C0));
        ugfx_fill_rect(s, cx + BTN, cy, 1, ch, black);
        int bx = cx + 3, by = cy + ch / 2 - 1;
        ugfx_fill_rect(s, bx + 1, by + 1, BTN - 6, 3, rgb(0x808080));
        ugfx_fill_rect(s, bx, by, BTN - 6, 3, rgb(0xFFFFFF));
        ugfx_draw_rect(s, bx, by, BTN - 6, 3, black);
        left = cx + BTN + 1;
    }
    if (win->style & WS_MAXIMIZEBOX) {
        right -= BTN;
        ugfx_fill_rect(s, right - 1, cy, 1, ch, black);
        bevel(s, right, cy, BTN, ch, pressed == HIT_MAX);
        arrow(s, right, cy, BTN, ch, 1, pressed == HIT_MAX);
        right -= 1;
    }
    if (win->style & WS_MINIMIZEBOX) {
        right -= BTN;
        ugfx_fill_rect(s, right - 1, cy, 1, ch, black);
        bevel(s, right, cy, BTN, ch, pressed == HIT_MIN);
        arrow(s, right, cy, BTN, ch, 0, pressed == HIT_MIN);
        right -= 1;
    }
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int tw = ugfx_text_width(win->title), room = right - left - 4;
    int tx = left + (room > tw ? (room - tw) / 2 : 2) + 2;
    int ty = cy + (ch - ugfx_char_h()) / 2;
    ugfx_draw_string_clipped(s, tx, ty, room > 0 ? room : 0, win->title,
                             active ? rgb(0xFFFFFF) : black, UGFX_TRANSPARENT);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

enum w16_hit w16_frame_hit(const struct w16_wnd *win, int x, int y, unsigned *edges) {
    int W = win->w, H = win->h, f = frame_of(win);
    *edges = 0;
    if (x < 0 || y < 0 || x >= W || y >= H) return HIT_NONE;
    if (win->style & WS_THICKFRAME) {
        int n = W16_CAPTION + f;                   // a corner reaches this far along an edge
        if (x < f) *edges |= WIN_EDGE_LEFT;
        if (x >= W - f) *edges |= WIN_EDGE_RIGHT;
        if (y < f) *edges |= WIN_EDGE_TOP;
        if (y >= H - f) *edges |= WIN_EDGE_BOTTOM;
        if (*edges & (WIN_EDGE_LEFT | WIN_EDGE_RIGHT)) {
            if (y < n) *edges |= WIN_EDGE_TOP;
            if (y >= H - n) *edges |= WIN_EDGE_BOTTOM;
        }
        if (*edges & (WIN_EDGE_TOP | WIN_EDGE_BOTTOM)) {
            if (x < n) *edges |= WIN_EDGE_LEFT;
            if (x >= W - n) *edges |= WIN_EDGE_RIGHT;
        }
        if (*edges) return HIT_EDGE;
    } else if (x < f || y < f || x >= W - f || y >= H - f) {
        return HIT_NONE;
    }
    if (has_caption(win) && y < f + W16_CAPTION) {
        int right = W - f;
        if ((win->style & WS_SYSMENU) && x < f + BTN) return HIT_SYSMENU;
        if (win->style & WS_MAXIMIZEBOX) {
            if (x >= right - BTN) return HIT_MAX;
            right -= BTN + 1;
        }
        if ((win->style & WS_MINIMIZEBOX) && x >= right - BTN && x < right) return HIT_MIN;
        return HIT_CAPTION;
    }
    return HIT_CLIENT;
}
