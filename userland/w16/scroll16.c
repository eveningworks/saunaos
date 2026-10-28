// scroll16.c -- a window's standard scroll bars (WS_VSCROLL, WS_HSCROLL):
// drawn with its non-client area in the 3.1 look -- arrow buttons at the
// ends, a grey trough, a square thumb -- and tracked the way
// DefWindowProc tracked them, in a loop of its own on the task thread: a
// press on an arrow or the trough sends WM_VSCROLL at once and again
// while it is held, a drag of the thumb SB_THUMBTRACK as it moves and
// SB_THUMBPOSITION where it lets go. The window -- an EDIT, or the
// program's own -- does the scrolling; the bar only reports.
//
// A BAR'S PLACE: inside a child's border, against its right or bottom
// edge, its outline the border's; in a top-level window, beyond its
// client area at the right and bottom of its buffer. A SCROLLBAR CONTROL
// (the class, SB_CTL) is a bar that is its whole window: the same drawing
// and tracking, reporting to its parent with its own HWND in lParam's
// high word.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

#define SB_LINEUP        0
#define SB_LINEDOWN      1
#define SB_PAGEUP        2
#define SB_PAGEDOWN      3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK    5
#define SB_ENDSCROLL     8

#define SB_CTL           2
#define SBS_VERT         0x0001

enum { PART_NONE = -1, PART_UP, PART_PAGEUP, PART_THUMB, PART_PAGEDOWN, PART_DOWN };

static int is_ctl(const struct w16_wnd *x) {
    return x->cls >= 0 && x->cls < g_w16.ncls && g_w16.cls[x->cls].host == w16_scrollbar_proc;
}

static int ctl_bar(const struct w16_wnd *x) { return x->style & SBS_VERT ? SB_VERT : SB_HORZ; }

// The one bar being tracked, and the part held down: how it is drawn. A dragged thumb is drawn where
// the drag has it (`pos`), as 3.1's followed the pointer -- a program that scrolls only on
// SB_THUMBPOSITION (Windows Help) leaves its own position until the release.
static struct { uint16_t hwnd; int bar, part, down, pos; } g_track = { 0, 0, PART_NONE, 0, 0 };

// A top-level window's bars reach one pixel past its buffer, onto the
// frame's inner black line, which is their outer edge -- 3.11 drew no second
// line beside it. The buffer clips that pixel; the frame draws it.
static void window_size(const struct w16_wnd *x, int *ww, int *wh) {
    *ww = x->kind == W16_K_CHILD ? x->w : x->fbw + 1;
    *wh = x->kind == W16_K_CHILD ? x->h : x->fbh + 1;
}

int w16_sbar_shown(const struct w16_wnd *x, int bar) {
    uint32_t want = bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL;
    return (x->style & want) && (x->keep_sb || x->sb[bar].max > x->sb[bar].min);
}

// Bar `bar`'s rectangle in window coordinates; 0 when x has none.
static int bar_rect(const struct w16_wnd *x, int bar, int *x0, int *y0, int *x1, int *y1) {
    if (is_ctl(x)) {
        if (bar != ctl_bar(x)) return 0;
        *x0 = *y0 = 0;
        *x1 = x->w;
        *y1 = x->h;
        return 1;
    }
    int ww, wh, vs = w16_sbar_shown(x, SB_VERT), hs = w16_sbar_shown(x, SB_HORZ);
    window_size(x, &ww, &wh);
    int near = x->kind == W16_K_CHILD ? 0 : -1;   // and onto the line above or left of the buffer
    if (bar == SB_VERT && vs) {
        *x0 = ww - W16_SB; *x1 = ww;
        *y0 = near; *y1 = wh - (hs ? W16_SB - 1 : 0);
        return 1;
    }
    if (bar == SB_HORZ && hs) {
        *x0 = near; *x1 = ww - (vs ? W16_SB - 1 : 0);
        *y0 = wh - W16_SB; *y1 = wh;
        return 1;
    }
    return 0;
}

// Along the bar: its length, and the thumb's start (-1 for none).
static int thumb_at(const struct w16_wnd *x, int bar, int len) {
    const struct w16_sbar *b = &x->sb[bar];
    int travel = len - 3 * W16_SB + 2;
    if (b->max <= b->min || travel < 0) return -1;
    int p = g_track.hwnd == x->hwnd && g_track.bar == bar && g_track.part == PART_THUMB ? g_track.pos : b->pos;
    int pos = p < b->min ? b->min : p > b->max ? b->max : p;
    return W16_SB - 1 + (int)((long)(pos - b->min) * travel / (b->max - b->min));
}

static int part_at(const struct w16_wnd *x, int bar, int along, int len) {
    if (x->sb[bar].max <= x->sb[bar].min) return PART_NONE;   // greyed: nothing answers
    if (along < W16_SB) return PART_UP;
    if (along >= len - W16_SB) return PART_DOWN;
    int t = thumb_at(x, bar, len);
    if (t < 0) return PART_NONE;
    if (along < t) return PART_PAGEUP;
    if (along < t + W16_SB) return PART_THUMB;
    return PART_PAGEDOWN;
}

int w16_sbar_hit(const struct w16_wnd *x, int wx, int wy) {
    int x0, y0, x1, y1;
    for (int bar = SB_HORZ; bar <= SB_VERT; bar++)
        if (bar_rect(x, bar, &x0, &y0, &x1, &y1) && wx >= x0 && wy >= y0 && wx < x1 && wy < y1) return bar;
    return -1;
}

// --- drawing (task thread, under the paint lock) ----------------------------------

// 3.1's arrows have a stem: a head 7 wide over a shaft 3 wide, 7 long in
// all, centred in the button -- measured off real Windows 3.11 at VGA.
// A disabled one is drawn grey with a white shadow, as 3.1 greyed it.
static void arrow_button(struct ugfx_surface *s, int x, int y, int dir, int pressed, int disabled) {
    uint32_t black = w16_rgb(0x000000);
    ugfx_draw_rect(s, x, y, W16_SB, W16_SB, black);
    w16_draw_bevel(s, x + 1, y + 1, W16_SB - 2, W16_SB - 2, pressed);
    int cx = x + W16_SB / 2 + pressed, cy = y + W16_SB / 2 + pressed;
    for (int pass = disabled ? 0 : 1; pass < 2; pass++) {
        uint32_t ink = pass ? (disabled ? w16_rgb(0x808080) : black) : w16_rgb(0xFFFFFF);
        int ax = cx + !pass, ay = cy + !pass;    // the white shadow sits one pixel down-right
        for (int i = 0; i < 4; i++) {            // the head
            switch (dir) {
            case 0: ugfx_fill_rect(s, ax - i, ay - 3 + i, 2 * i + 1, 1, ink); break;      // up
            case 1: ugfx_fill_rect(s, ax - 3 + i, ay + i, 7 - 2 * i, 1, ink); break;      // down
            case 2: ugfx_fill_rect(s, ax - 3 + i, ay - i, 1, 2 * i + 1, ink); break;      // left
            default: ugfx_fill_rect(s, ax + i, ay - 3 + i, 1, 7 - 2 * i, ink); break;     // right
            }
        }
        switch (dir) {                           // the stem
        case 0: ugfx_fill_rect(s, ax - 1, ay + 1, 3, 3, ink); break;
        case 1: ugfx_fill_rect(s, ax - 1, ay - 3, 3, 3, ink); break;
        case 2: ugfx_fill_rect(s, ax + 1, ay - 1, 3, 3, ink); break;
        default: ugfx_fill_rect(s, ax - 3, ay - 1, 3, 3, ink); break;
        }
    }
}

// The bar in (x0, y0)-(x1, y1), W16_SB thick.
static void draw_strip(struct w16_wnd *x, int bar, struct ugfx_surface *s, int x0, int y0, int x1, int y1) {
    uint32_t black = w16_rgb(0x000000), trough = w16_rgb(0xC0C0C0);
    int vert = bar == SB_VERT, len = vert ? y1 - y0 : x1 - x0;
    int held = g_track.hwnd == x->hwnd && g_track.bar == bar && g_track.down ? g_track.part : PART_NONE;
    // Nothing to scroll, kept on screen (LBS_DISABLENOSCROLL): 3.1 greys
    // the arrows over a white trough and shows no thumb.
    int off = x->sb[bar].max <= x->sb[bar].min;
    if (off) trough = w16_rgb(0xFFFFFF);
    ugfx_fill_rect(s, x0, y0, x1 - x0, y1 - y0, trough);
    ugfx_draw_rect(s, x0, y0, x1 - x0, y1 - y0, black);
    if (vert) {
        arrow_button(s, x0, y0, 0, held == PART_UP, off);
        arrow_button(s, x0, y1 - W16_SB, 1, held == PART_DOWN, off);
    } else {
        arrow_button(s, x0, y0, 2, held == PART_UP, off);
        arrow_button(s, x1 - W16_SB, y0, 3, held == PART_DOWN, off);
    }
    int t = thumb_at(x, bar, len);
    if (t >= 0) {
        int tx = vert ? x0 : x0 + t, ty = vert ? y0 + t : y0;
        ugfx_draw_rect(s, tx, ty, W16_SB, W16_SB, black);
        w16_draw_bevel(s, tx + 1, ty + 1, W16_SB - 2, W16_SB - 2, 0);
    }
}

// A SCROLLBAR control thicker or thinner than W16_SB (Mouse's, 20 tall) is the W16_SB bar stretched
// across it, as 3.1 StretchBlt its arrow bitmaps to the control: thickness row j shows the strip's
// row (2j + 1) * W16_SB / (2 * thick) -- a 20-tall bar doubles its arrows' middle row, as 3.11's.
static void draw_bar(struct w16_wnd *x, int bar, struct ugfx_surface *s, int ox, int oy) {
    int x0, y0, x1, y1;
    if (!bar_rect(x, bar, &x0, &y0, &x1, &y1)) return;
    x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    int vert = bar == SB_VERT, len = vert ? y1 - y0 : x1 - x0, thick = vert ? x1 - x0 : y1 - y0;
    uint32_t *px = thick != W16_SB && thick > 0 && len > 0 ? malloc((size_t)len * W16_SB * 4) : 0;
    if (!px) {
        draw_strip(x, bar, s, x0, y0, vert ? x0 + W16_SB : x1, vert ? y1 : y0 + W16_SB);
        return;
    }
    struct ugfx_surface strip = ugfx_surface_for_pixels(px, vert ? W16_SB : len, vert ? len : W16_SB);
    draw_strip(x, bar, &strip, 0, 0, strip.w, strip.h);
    for (int j = 0; j < thick; j++) {
        int from = (2 * j + 1) * W16_SB / (2 * thick);
        if (vert) ugfx_blit(s, x0 + j, y0, 1, len, px + from, W16_SB);
        else ugfx_blit(s, x0, y0 + j, len, 1, px + (size_t)from * len, len);
    }
    free(px);
}

void w16_sbar_draw(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    (void)w;
    draw_bar(x, SB_VERT, s, ox, oy);
    draw_bar(x, SB_HORZ, s, ox, oy);
    if (w16_sbar_shown(x, SB_VERT) && w16_sbar_shown(x, SB_HORZ)) {   // the corner square
        int ww, wh;
        window_size(x, &ww, &wh);
        ugfx_fill_rect(s, ox + ww - W16_SB + 1, oy + wh - W16_SB + 1, W16_SB - 1, W16_SB - 1, w16_rgb(0xC0C0C0));
    }
}

static void redraw(struct w16 *w, struct w16_wnd *x) {
    if (is_ctl(x)) {
        w16_invalidate(w, x, 0);
        w16_paint_now(w);
        return;
    }
    struct ugfx_surface s;
    int ox, oy;
    if (!(x->style & (WS_VSCROLL | WS_HSCROLL)) || !w16_wnd_paint_nc(w, x, &s, &ox, &oy)) return;
    w16_sbar_draw(w, x, &s, ox, oy);
    w16_wnd_paint_done(w);
}

// A bar shown or hidden: the client area grows or shrinks, and the window
// hears WM_SIZE, as on Windows.
static void relayout(struct w16 *w, struct w16_wnd *x) {
    int changed;
    if (x->kind == W16_K_CHILD) {
        int cw = x->cw, ch = x->ch;
        w16_wnd_set_geometry(w, x, x->x, x->y, x->w, x->h);
        changed = cw != x->cw || ch != x->ch;
    } else {
        pthread_mutex_lock(&w->lock);
        changed = w16_top_fit(x) > 0;
        pthread_mutex_unlock(&w->lock);
    }
    if (!changed) return;
    w16_invalidate(w, x, 1);
    w16_send(w, x->hwnd, WM_SIZE, 0, (uint32_t)(uint16_t)x->ch << 16 | (uint16_t)x->cw);
}

void w16_sbar_set(struct w16 *w, struct w16_wnd *x, int bar, int min, int max, int pos, int redraw_it) {
    struct w16_sbar *b = &x->sb[bar];
    if (max < min) max = min;
    if (pos < min) pos = min;
    if (pos > max) pos = max;
    int was = is_ctl(x) || w16_sbar_shown(x, bar);
    int changed = b->min != min || b->max != max || b->pos != pos;
    pthread_mutex_lock(&w->lock);
    b->min = (int16_t)min;
    b->max = (int16_t)max;
    b->pos = (int16_t)pos;
    pthread_mutex_unlock(&w->lock);
    if (was != (is_ctl(x) || w16_sbar_shown(x, bar))) relayout(w, x);
    else if (changed && redraw_it && w16_wnd_shown(w, x)) redraw(w, x);
}

// --- tracking ---------------------------------------------------------------------

static void send_scroll(struct w16 *w, struct w16_wnd *x, int bar, int code, int pos) {
    uint16_t to = is_ctl(x) ? x->parent : x->hwnd;
    uint32_t lp = (uint16_t)pos | (is_ctl(x) ? (uint32_t)x->hwnd << 16 : 0);
    w16_send(w, to, bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, (uint16_t)code, lp);
    w16_paint_now(w);                            // the window scrolls under the held button
}

static int part_code(int part) {
    switch (part) {
    case PART_UP:       return SB_LINEUP;
    case PART_DOWN:     return SB_LINEDOWN;
    case PART_PAGEUP:   return SB_PAGEUP;
    case PART_PAGEDOWN: return SB_PAGEDOWN;
    default:            return -1;
    }
}

void w16_sbar_track(struct w16 *w, struct w16_wnd *x, int bar, int wx, int wy) {
    int x0, y0, x1, y1, l, t, r, b;
    if (!bar_rect(x, bar, &x0, &y0, &x1, &y1)) return;
    w16_wnd_insets(x, &l, &t, &r, &b);
    int vert = bar == SB_VERT, len = vert ? y1 - y0 : x1 - x0;
    int along = vert ? wy - y0 : wx - x0, part = part_at(x, bar, along, len);
    if (part == PART_NONE) return;
    uint16_t hwnd = x->hwnd;
    int grab = along - thumb_at(x, bar, len), pos = x->sb[bar].pos;
    g_track.hwnd = hwnd;
    g_track.bar = bar;
    g_track.part = part;
    g_track.down = 1;
    g_track.pos = pos;
    // The capture the bar takes for its drag is handed back after it, not dropped: an open
    // drop-down's list holds it, and without it the list never shuts on a press outside.
    pthread_mutex_lock(&w->lock);
    uint16_t held = w->capture;
    w->capture = hwnd;
    pthread_mutex_unlock(&w->lock);
    redraw(w, x);
    if (part != PART_THUMB) send_scroll(w, x, bar, part_code(part), 0);
    int delay = 350;                             // then repeats, as 3.1's did
    for (;;) {
        struct w16_msg m;
        int got = w16_get_mouse_msg(w, hwnd, &m, part == PART_THUMB ? -1 : delay);
        x = w16_wnd_get(w, hwnd);
        if (!x || w->cpu.fault) break;
        if (!got) {
            // Held: an arrow repeats; the trough pages until the thumb reaches the pointer.
            int now = part_at(x, bar, along, len);
            if (g_track.down && (part != PART_PAGEUP && part != PART_PAGEDOWN ? 1 : now == part))
                send_scroll(w, x, bar, part_code(part), 0);
            delay = 50;
            continue;
        }
        int mx = (int16_t)m.lparam + l, my = (int16_t)(m.lparam >> 16) + t;   // window coordinates
        along = vert ? my - y0 : mx - x0;
        if (m.message == WM_MOUSEMOVE) {
            if (part == PART_THUMB) {
                int travel = len - 3 * W16_SB + 2, at = along - grab - (W16_SB - 1);
                const struct w16_sbar *sb = &x->sb[bar];
                int np = travel > 0 ? sb->min + (int)((long)at * (sb->max - sb->min) / travel) : sb->min;
                if (np < sb->min) np = sb->min;
                if (np > sb->max) np = sb->max;
                if (np != pos) {
                    g_track.pos = pos = np;
                    send_scroll(w, x, bar, SB_THUMBTRACK, pos);
                    if ((x = w16_wnd_get(w, hwnd))) redraw(w, x);
                    else break;
                }
            } else {
                int inside = mx >= x0 && my >= y0 && mx < x1 && my < y1 && part_at(x, bar, along, len) == part;
                if (inside != g_track.down) { g_track.down = inside; redraw(w, x); }
            }
        } else if (m.message == WM_LBUTTONUP) {
            if (part == PART_THUMB) send_scroll(w, x, bar, SB_THUMBPOSITION, pos);
            break;
        }
    }
    g_track.hwnd = 0;
    g_track.part = PART_NONE;
    pthread_mutex_lock(&w->lock);
    if (w->capture == hwnd) w->capture = held;
    pthread_mutex_unlock(&w->lock);
    x = w16_wnd_get(w, hwnd);
    if (x) {
        send_scroll(w, x, bar, SB_ENDSCROLL, 0);
        redraw(w, x);
    }
}

// --- the SCROLLBAR class ------------------------------------------------------------

static void paint_ctl(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    (void)w;
    draw_bar(x, ctl_bar(x), s, ox, oy);
}

uint32_t w16_scrollbar_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    switch (msg) {
    case WM_PAINT:
        w16_ctl_paint(w, x, paint_ctl);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        w16_sbar_track(w, x, ctl_bar(x), (int16_t)lp, (int16_t)(lp >> 16));
        return 0;
    case WM_ENABLE:
        w16_invalidate(w, x, 0);
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}

// The bar a Set/GetScroll call means: SB_HORZ or SB_VERT of a window, or
// the control itself for SB_CTL; -1 for none.
static int which_bar(const struct w16_wnd *x, unsigned bar) {
    if (!x) return -1;
    if (bar == SB_CTL) return is_ctl(x) ? ctl_bar(x) : -1;
    return bar <= SB_VERT ? (int)bar : -1;
}

// --- the exports ------------------------------------------------------------------

// SetScrollPos(hwnd, nBar, nPos, bRedraw) -> int
// Windows: Moves the thumb of a window's horizontal or vertical scroll bar to a position in its
// range, redrawing it if asked; returns the old position.
// w16: The same, on the scroll bars w16 draws in Windows 3.1's look -- a window's own, or a
// SCROLLBAR control's (SB_CTL); the position is kept inside the range.
static int api_SetScrollPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int bar = which_bar(x, w16_arg16(a));
    int16_t pos = (int16_t)w16_arg16(a);
    uint16_t redraw_it = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (bar >= 0) {
        w->cpu.r[R_AX] = (uint16_t)x->sb[bar].pos;
        w16_sbar_set(w, x, bar, x->sb[bar].min, x->sb[bar].max, pos, redraw_it);
    }
    return 8;
}

// GetScrollPos(hwnd, nBar) -> int
// Windows: Where a window's scroll bar thumb is.
// w16: The same; 0 for a window or bar that is not there.
static int api_GetScrollPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int bar = which_bar(x, w16_arg16(a));
    w->cpu.r[R_AX] = bar >= 0 ? (uint16_t)x->sb[bar].pos : 0;
    return 4;
}

// SetScrollRange(hwnd, nBar, nMin, nMax, bRedraw) -> void
// Windows: Sets the range a scroll bar's thumb moves over; a range of 0 to 0 hides the bar.
// w16: The same: an empty range takes the bar away and gives the client area its room.
static int api_SetScrollRange(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int bar = which_bar(x, w16_arg16(a));
    int16_t min = (int16_t)w16_arg16(a), max = (int16_t)w16_arg16(a);
    uint16_t redraw_it = w16_arg16(a);
    if (bar >= 0) w16_sbar_set(w, x, bar, min, max, x->sb[bar].pos, redraw_it);
    return 10;
}

// GetScrollRange(hwnd, nBar, lpMinPos, lpMaxPos) -> void
// Windows: Writes a scroll bar's range into the two integers given.
// w16: The same; 0 and 0 for a window or bar that is not there.
static int api_GetScrollRange(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int bar = which_bar(x, w16_arg16(a));
    uint32_t pmin = w16_arg32(a), pmax = w16_arg32(a);
    int16_t v[2] = { 0, 0 };
    if (bar >= 0) { v[0] = x->sb[bar].min; v[1] = x->sb[bar].max; }
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(pmin >> 16), (uint16_t)pmin, 2, 1);
    uint8_t *q = cpu16_ptr(&w->cpu, (uint16_t)(pmax >> 16), (uint16_t)pmax, 2, 1);
    if (!p || !q) return -1;
    memcpy(p, &v[0], 2);
    memcpy(q, &v[1], 2);
    return 12;
}

const struct w16_api g_scroll_api[] = {
    { 62, api_SetScrollPos },
    { 63, api_GetScrollPos },
    { 64, api_SetScrollRange },
    { 65, api_GetScrollRange },
};
const unsigned g_scroll_api_n = sizeof g_scroll_api / sizeof g_scroll_api[0];
