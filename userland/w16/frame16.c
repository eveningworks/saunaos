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

static uint32_t rgb(uint32_t v) { return w16_rgb(v); }

int g_w16_desktop_frame;          // the desktop's frame instead of 3.1's

// MAXIMIZED, 3.1 put the border off the screen's edges; here there is
// none, and the caption is the window's top. Under the desktop's frame,
// toywm draws it all and the window is the client.
// A WS_POPUP naming no border has none: its whole window is client (the Windows Tutorial's full screen).
int w16_border_of(uint32_t style) {
    if (style & WS_THICKFRAME) return W16_FRAME;
    return (style & WS_POPUP) && !(style & (WS_BORDER | WS_DLGFRAME)) ? 0 : 1;
}
// A CHILD WITH A CAPTION (w16_child_framed()) is framed by w16 whatever the setting -- the desktop's frame
// is the main window's alone -- and keeps its frame maximized, outside its parent's client area, as 3.1
// put it there.
static int frame_of(const struct w16_wnd *win) {
    int child = win->kind == W16_K_CHILD;
    if (g_w16_desktop_frame && !child) return 0;
    if (child && win->iconic) return 0;
    return win->maximized && win->kind == W16_K_MAIN ? 0 : w16_border_of(win->style);   // toywm's maximized main window
}
static int has_caption(const struct w16_wnd *win) {
    int child = win->kind == W16_K_CHILD;
    return (!g_w16_desktop_frame || child) && !(child && win->iconic) && (win->style & WS_CAPTION) == WS_CAPTION;
}

int w16_child_framed(const struct w16_wnd *x) {
    return x->kind == W16_K_CHILD && (x->style & WS_CAPTION) == WS_CAPTION;
}

// A frame hit as Windows' WM_NCHITTEST code: HTCAPTION 2, HTSYSMENU 3, HTREDUCE 8, HTZOOM 9, the edges
// HTLEFT 10 to HTBOTTOMRIGHT 17; HTNOWHERE 0, HTCLIENT 1.
int w16_hit_code(enum w16_hit hit, unsigned edges) {
    switch (hit) {
    case HIT_CLIENT: return 1;
    case HIT_CAPTION: return 2;
    case HIT_SYSMENU: return 3;
    case HIT_MIN: return 8;
    case HIT_MAX: return 9;
    case HIT_EDGE: {
        int l = edges & WIN_EDGE_LEFT, r = edges & WIN_EDGE_RIGHT, t = edges & WIN_EDGE_TOP, b = edges & WIN_EDGE_BOTTOM;
        if (t && l) return 13;
        if (t && r) return 14;
        if (b && l) return 16;
        if (b && r) return 17;
        if (l) return 10;
        if (r) return 11;
        if (t) return 12;
        return 15;
    }
    default: return 0;
    }
}

// A program sizes its window for 3.1's frame (GetSystemMetrics says so);
// under the desktop's, only the client is w16's, so the 3.1 frame comes
// off and the program's client area keeps the size it asked for.
void w16_frame_fit(const struct w16_wnd *win, int *w, int *h) {
    if (!g_w16_desktop_frame) return;
    int f = w16_border_of(win->style);
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

// A solid triangle, apex up or down, centred in the box at x,y -- the up one a row higher than
// the down one, as 3.11 drew Maximize beside Minimize.
static void arrow(struct ugfx_surface *s, int x, int y, int w, int h, int up, int shift) {
    int rows = 4, cx = x + w / 2 - 1 + shift, cy = y + (h - rows) / 2 + shift - (up ? 1 : 0);
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

// Its menu open, the box is inverted on the palette's indices, as 3.11 shows it: dark grey, the bar
// black.
void w16_draw_sysbox(struct ugfx_surface *s, int x, int y, int w, int h, int open) {
    ugfx_fill_rect(s, x, y, w, h, rgb(0xC0C0C0));
    int bx = x + 2, by = y + (h - 4) / 2;           // 13 by 3 in an 18-pixel box, as 3.11 has it
    ugfx_fill_rect(s, bx + 1, by + 1, w - 5, 3, rgb(0x808080));
    ugfx_fill_rect(s, bx, by, w - 5, 3, rgb(0xFFFFFF));
    ugfx_draw_rect(s, bx, by, w - 5, 3, rgb(0x000000));
    for (int j = 0; open && j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t d = ugfx_get_pixel(s, x + i, y + j);
            ugfx_fill_rect(s, x + i, y + j, 1, 1, gdi_rop3(0x55, 0, 0, d) | (d & 0xFF000000u));   // DSTINVERT
        }
}

// A child's box (an MDI document's): OBM_CLOSE's second half, a shorter bar, 7 by 3.
void w16_draw_child_sysbox(struct ugfx_surface *s, int x, int y, int w, int h, int open) {
    ugfx_fill_rect(s, x, y, w, h, rgb(0xC0C0C0));
    int bx = x + 6, by = y + (h - 4) / 2;
    ugfx_fill_rect(s, bx + 1, by + 1, 7, 3, rgb(0x808080));
    ugfx_fill_rect(s, bx, by, 7, 3, rgb(0xFFFFFF));
    ugfx_draw_rect(s, bx, by, 7, 3, rgb(0x000000));
    for (int j = 0; open && j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t d = ugfx_get_pixel(s, x + i, y + j);
            ugfx_fill_rect(s, x + i, y + j, 1, 1, gdi_rop3(0x55, 0, 0, d) | (d & 0xFF000000u));   // DSTINVERT
        }
}

// Windows' own bitmaps (LoadBitmap(NULL, OBM_*)) with no display driver on drive C: to take them from,
// drawn by the painters w16 draws its own frames and scroll bars with, at the sizes 3.11's VGA.DRV
// has: the control-menu boxes, Minimize, Maximize and Restore up and pressed, and the scroll arrows
// up, pressed and greyed. The rest (check marks, size boxes, the old 3.0 set) are not drawn: 0.
uint16_t w16_obm_standin(struct w16 *w, unsigned id) {
    enum { CLOSE = 32754, REDUCE = 32749, ZOOM = 32748, RESTORE = 32747, REDUCED = 32746, ZOOMD = 32745,
           RESTORED = 32744, UPARROW = 32753, UPARROWD = 32743, UPARROWI = 32737 };
    int bw, bh;
    if (id == CLOSE) { bw = 36; bh = 18; }
    else if (id >= RESTORED && id <= REDUCE) { bw = BTN + 1; bh = BTN; }
    else if ((id >= UPARROW - 3 && id <= UPARROW) || (id >= UPARROWD - 3 && id <= UPARROWD) ||
             (id >= UPARROWI - 3 && id <= UPARROWI)) { bw = bh = W16_SB; }
    else return 0;
    uint32_t *px;
    uint16_t h = w16_gdi_bitmap_new(w, bw, bh, &px);
    if (!h) return 0;
    struct ugfx_surface s = ugfx_surface_for_pixels(px, bw, bh);
    if (id == CLOSE) {                           // a window's box, then an MDI child's shorter bar
        w16_draw_sysbox(&s, 0, 0, 18, 18, 0);
        ugfx_fill_rect(&s, 18, 0, 18, 18, rgb(0xC0C0C0));
        ugfx_fill_rect(&s, 25, 8, 7, 3, rgb(0x808080));
        ugfx_fill_rect(&s, 24, 7, 7, 3, rgb(0xFFFFFF));
        ugfx_draw_rect(&s, 24, 7, 7, 3, rgb(0x000000));
    } else if (id >= RESTORED && id <= REDUCE) { // the separator, then the caption button
        int pressed = id <= REDUCED, kind = (pressed ? id + 3 : id) - RESTORE;   // 0 restore, 1 zoom, 2 reduce
        ugfx_fill_rect(&s, 0, 0, 1, bh, rgb(0x000000));
        bevel(&s, 1, 0, BTN, BTN, pressed);
        if (kind == 0) restore_arrows(&s, 1, 0, BTN, BTN, pressed);
        else arrow(&s, 1, 0, BTN, BTN, kind == 1, pressed);
    } else {                                     // UP, DN, RG, LF in that order down from UPARROW
        unsigned base = id > UPARROWD ? UPARROW : id > UPARROWI ? UPARROWD : UPARROWI;
        static const int DIR[4] = { 0, 1, 3, 2 };
        w16_sbar_arrow(&s, 0, 0, DIR[base - id], base == UPARROWD, base == UPARROWI);
    }
    return h;
}

void w16_frame_draw(struct ugfx_surface *s, const struct w16_wnd *win, int active, int pressed) {
    if (g_w16_desktop_frame && win->kind != W16_K_CHILD) return;   // a captioned child keeps its own
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
        // starts sizing two: on the line under the caption and on the
        // caption buttons' edges, as 3.11 had them.
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
        if (win->kind == W16_K_CHILD) w16_draw_child_sysbox(s, cx, cy, BTN, ch, pressed == HIT_SYSMENU);
        else w16_draw_sysbox(s, cx, cy, BTN, ch, pressed == HIT_SYSMENU);
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
    if (g_w16_desktop_frame && win->kind != W16_K_CHILD) return HIT_CLIENT;
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
