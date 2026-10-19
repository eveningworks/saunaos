// frame16.c -- the Windows 3.1 non-client area, drawn by the MAIN thread
// into the toywm window (an undecorated one, WIN_HINT_NO_DECOR): the
// sizing border with its corner notches, the caption, the control-menu
// box and the minimize/maximize arrows. The look is the "Win16 windows in
// toywm" mockup's L3 (docs/w16-design.md). It holds no program state,
// which is why the main thread can draw it while the task runs.
#include "w16_internal.h"

#include <stdlib.h>
#include "win_proto.h"
#include "ui/ugfx.h"

#define BTN 18                                    // a caption button's width

static uint32_t rgb(uint32_t v) { return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v); }

int g_w16_desktop_frame;          // the desktop's frame instead of 3.1's

// MAXIMIZED, 3.1 put the border off the screen's edges; here there is
// none, and the caption is the window's top. Under the desktop's frame,
// toywm draws it all and the window is the client.
static int frame_of(const struct w16_wnd *win) {
    if (g_w16_desktop_frame) return 0;
    return win->maximized ? 0 : win->style & WS_THICKFRAME ? W16_FRAME : 1;
}
static int has_caption(const struct w16_wnd *win) {
    return !g_w16_desktop_frame && (win->style & WS_CAPTION) == WS_CAPTION;
}

// A program sizes its window for 3.1's frame (GetSystemMetrics says so);
// under the desktop's, only the client is w16's, so the 3.1 frame comes
// off and the program's client area keeps the size it asked for.
void w16_frame_fit(const struct w16_wnd *win, int *w, int *h) {
    if (!g_w16_desktop_frame) return;
    int f = win->style & WS_THICKFRAME ? W16_FRAME : 1;
    int cap = (win->style & WS_CAPTION) == WS_CAPTION ? W16_CAPTION : 0;
    *w -= 2 * f;
    *h -= 2 * f + cap;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

// The bar is under the caption and over the client, in the 3.1 frame
// and the desktop's alike: it is the program's, so w16 draws it.
void w16_frame_menubar(const struct w16_wnd *win, int *x, int *y, int *w, int *h) {
    int f = frame_of(win);
    *x = f;
    *y = f + (has_caption(win) ? W16_CAPTION : 0);
    *w = win->w - 2 * f;
    *h = win->menubar ? W16_MENU_H : 0;
}

void w16_client_rect(const struct w16_wnd *win, int *x, int *y, int *cw, int *ch) {
    int f = frame_of(win), cap = has_caption(win) ? W16_CAPTION : 0, bar = win->menubar ? W16_MENU_H : 0;
    *x = f;
    *y = f + cap + bar;
    *cw = win->w - 2 * f;
    *ch = win->h - 2 * f - cap - bar;
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

// Restore: a smaller arrow up over one down, as 3.1 drew it.
static void restore_arrows(struct ugfx_surface *s, int x, int y, int w, int h, int shift) {
    int cx = x + w / 2 - 1 + shift, top = y + (h - 8) / 2 + shift;
    for (int r = 0; r < 3; r++) {
        ugfx_fill_rect(s, cx - r, top + r, 2 * r + 1, 1, rgb(0x000000));
        ugfx_fill_rect(s, cx - (2 - r), top + 5 + r, 2 * (2 - r) + 1, 1, rgb(0x000000));
    }
}

void w16_frame_sysbox(const struct w16_wnd *win, int *x, int *y, int *w, int *h) {
    int f = frame_of(win);
    *x = f;
    *y = f;
    *w = BTN;
    *h = W16_CAPTION - 1;
}

void w16_draw_sysbox(struct ugfx_surface *s, int x, int y, int w, int h, int open) {
    ugfx_fill_rect(s, x, y, w, h, open ? rgb(0x000080) : rgb(0xC0C0C0));
    int bx = x + 3, by = y + h / 2 - 1;
    ugfx_fill_rect(s, bx + 1, by + 1, w - 6, 3, rgb(0x808080));
    ugfx_fill_rect(s, bx, by, w - 6, 3, rgb(0xFFFFFF));
    ugfx_draw_rect(s, bx, by, w - 6, 3, rgb(0x000000));
}

void w16_frame_draw(struct ugfx_surface *s, const struct w16_wnd *win, int active, int pressed) {
    if (g_w16_desktop_frame) return;
    int W = win->w, H = win->h, f = frame_of(win);
    uint32_t black = rgb(0x000000);
    if (f) ugfx_draw_rect(s, 0, 0, W, H, black);
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
        // Its menu open, the box is drawn in the highlight, as 3.1 did.
        w16_draw_sysbox(s, cx, cy, BTN, ch, pressed == HIT_SYSMENU);
        ugfx_fill_rect(s, cx + BTN, cy, 1, ch, black);
        left = cx + BTN + 1;
    }
    if (win->style & WS_MAXIMIZEBOX) {
        right -= BTN;
        ugfx_fill_rect(s, right - 1, cy, 1, ch, black);
        bevel(s, right, cy, BTN, ch, pressed == HIT_MAX);
        if (win->maximized) restore_arrows(s, right, cy, BTN, ch, pressed == HIT_MAX);
        else arrow(s, right, cy, BTN, ch, 1, pressed == HIT_MAX);
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
    const char *title = win->text ? win->text : "";
    int tw = ugfx_text_width(title), room = right - left - 4;
    int tx = left + (room > tw ? (room - tw) / 2 : 2) + 2;
    int ty = cy + (ch - ugfx_char_h()) / 2;
    ugfx_draw_string_clipped(s, tx, ty, room > 0 ? room : 0, title,
                             active ? rgb(0xFFFFFF) : black, UGFX_TRANSPARENT);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

enum w16_hit w16_frame_hit(const struct w16_wnd *win, int x, int y, unsigned *edges) {
    int W = win->w, H = win->h, f = frame_of(win);
    *edges = 0;
    if (x < 0 || y < 0 || x >= W || y >= H) return HIT_NONE;
    int bx, by, bw, bh;
    w16_frame_menubar(win, &bx, &by, &bw, &bh);
    if (bh && y >= by && y < by + bh && x >= bx && x < bx + bw) return HIT_MENU;
    if (g_w16_desktop_frame) return HIT_CLIENT;
    if ((win->style & WS_THICKFRAME) && !win->maximized) {
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
    } else if (f && (x < f || y < f || x >= W - f || y >= H - f)) {
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
