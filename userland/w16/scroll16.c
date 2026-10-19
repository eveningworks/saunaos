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
// client area at the right and bottom of its buffer.
#include "w16_internal.h"

#include <string.h>
#include "ui/ugfx.h"

#define SB_LINEUP        0
#define SB_LINEDOWN      1
#define SB_PAGEUP        2
#define SB_PAGEDOWN      3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK    5
#define SB_ENDSCROLL     8

enum { PART_NONE = -1, PART_UP, PART_PAGEUP, PART_THUMB, PART_PAGEDOWN, PART_DOWN };

// The one bar being tracked, and the part held down: how it is drawn.
static struct { uint16_t hwnd; int bar, part, down; } g_track = { 0, 0, PART_NONE, 0 };

static void window_size(const struct w16_wnd *x, int *ww, int *wh) {
    *ww = x->kind == W16_K_CHILD ? x->w : x->fbw;
    *wh = x->kind == W16_K_CHILD ? x->h : x->fbh;
}

int w16_sbar_shown(const struct w16_wnd *x, int bar) {
    uint32_t want = bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL;
    return (x->style & want) && (x->keep_sb || x->sb[bar].max > x->sb[bar].min);
}

// Bar `bar`'s rectangle in window coordinates; 0 when x has none.
static int bar_rect(const struct w16_wnd *x, int bar, int *x0, int *y0, int *x1, int *y1) {
    int ww, wh, vs = w16_sbar_shown(x, SB_VERT), hs = w16_sbar_shown(x, SB_HORZ);
    window_size(x, &ww, &wh);
    if (bar == SB_VERT && vs) {
        *x0 = ww - W16_SB; *x1 = ww;
        *y0 = 0; *y1 = wh - (hs ? W16_SB - 1 : 0);
        return 1;
    }
    if (bar == SB_HORZ && hs) {
        *x0 = 0; *x1 = ww - (vs ? W16_SB - 1 : 0);
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
    int pos = b->pos < b->min ? b->min : b->pos > b->max ? b->max : b->pos;
    return W16_SB - 1 + (int)((long)(pos - b->min) * travel / (b->max - b->min));
}

static int part_at(const struct w16_wnd *x, int bar, int along, int len) {
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

static void arrow_button(struct ugfx_surface *s, int x, int y, int dir, int pressed) {
    uint32_t black = w16_rgb(0x000000);
    ugfx_draw_rect(s, x, y, W16_SB, W16_SB, black);
    w16_draw_bevel(s, x + 1, y + 1, W16_SB - 2, W16_SB - 2, pressed);
    int cx = x + W16_SB / 2 + pressed, cy = y + W16_SB / 2 + pressed;
    for (int i = 0; i < 4; i++) {                // a solid triangle, 7 wide at its base
        int half = i;
        switch (dir) {
        case 0: ugfx_fill_rect(s, cx - half, cy - 2 + i, 2 * half + 1, 1, black); break;   // up
        case 1: ugfx_fill_rect(s, cx - half, cy + 1 - i, 2 * half + 1, 1, black); break;   // down
        case 2: ugfx_fill_rect(s, cx - 2 + i, cy - half, 1, 2 * half + 1, black); break;   // left
        default: ugfx_fill_rect(s, cx + 1 - i, cy - half, 1, 2 * half + 1, black); break;  // right
        }
    }
}

static void draw_bar(struct w16_wnd *x, int bar, struct ugfx_surface *s, int ox, int oy) {
    int x0, y0, x1, y1;
    if (!bar_rect(x, bar, &x0, &y0, &x1, &y1)) return;
    x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    uint32_t black = w16_rgb(0x000000), trough = w16_rgb(0xC0C0C0);
    int vert = bar == SB_VERT, len = vert ? y1 - y0 : x1 - x0;
    int held = g_track.hwnd == x->hwnd && g_track.bar == bar && g_track.down ? g_track.part : PART_NONE;
    ugfx_fill_rect(s, x0, y0, x1 - x0, y1 - y0, trough);
    ugfx_draw_rect(s, x0, y0, x1 - x0, y1 - y0, black);
    if (vert) {
        arrow_button(s, x0, y0, 0, held == PART_UP);
        arrow_button(s, x0, y1 - W16_SB, 1, held == PART_DOWN);
    } else {
        arrow_button(s, x0, y0, 2, held == PART_UP);
        arrow_button(s, x1 - W16_SB, y0, 3, held == PART_DOWN);
    }
    int t = thumb_at(x, bar, len);
    if (t >= 0) {
        int tx = vert ? x0 : x0 + t, ty = vert ? y0 + t : y0;
        ugfx_draw_rect(s, tx, ty, W16_SB, W16_SB, black);
        w16_draw_bevel(s, tx + 1, ty + 1, W16_SB - 2, W16_SB - 2, 0);
    }
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
    int was = w16_sbar_shown(x, bar);
    int changed = b->min != min || b->max != max || b->pos != pos;
    pthread_mutex_lock(&w->lock);
    b->min = (int16_t)min;
    b->max = (int16_t)max;
    b->pos = (int16_t)pos;
    pthread_mutex_unlock(&w->lock);
    if (was != w16_sbar_shown(x, bar)) relayout(w, x);
    else if (changed && redraw_it && w16_wnd_shown(w, x)) redraw(w, x);
}

// --- tracking ---------------------------------------------------------------------

static void send_scroll(struct w16 *w, struct w16_wnd *x, int bar, int code, int pos) {
    w16_send(w, x->hwnd, bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, (uint16_t)code, (uint16_t)pos);
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
    pthread_mutex_lock(&w->lock);
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
                if (np != pos) { pos = np; send_scroll(w, x, bar, SB_THUMBTRACK, pos); }
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
    if (w->capture == hwnd) w->capture = 0;
    pthread_mutex_unlock(&w->lock);
    x = w16_wnd_get(w, hwnd);
    if (x) {
        send_scroll(w, x, bar, SB_ENDSCROLL, 0);
        redraw(w, x);
    }
}

// --- the exports ------------------------------------------------------------------

// SetScrollPos(hwnd, nBar, nPos, bRedraw) -> the old position
static int api_SetScrollPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t bar = w16_arg16(a);
    int16_t pos = (int16_t)w16_arg16(a);
    uint16_t redraw_it = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (x && bar <= SB_VERT) {
        w->cpu.r[R_AX] = (uint16_t)x->sb[bar].pos;
        w16_sbar_set(w, x, bar, x->sb[bar].min, x->sb[bar].max, pos, redraw_it);
    }
    return 8;
}

// GetScrollPos(hwnd, nBar)
static int api_GetScrollPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t bar = w16_arg16(a);
    w->cpu.r[R_AX] = x && bar <= SB_VERT ? (uint16_t)x->sb[bar].pos : 0;
    return 4;
}

// SetScrollRange(hwnd, nBar, nMin, nMax, bRedraw)
static int api_SetScrollRange(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t bar = w16_arg16(a);
    int16_t min = (int16_t)w16_arg16(a), max = (int16_t)w16_arg16(a);
    uint16_t redraw_it = w16_arg16(a);
    if (x && bar <= SB_VERT) w16_sbar_set(w, x, bar, min, max, x->sb[bar].pos, redraw_it);
    return 10;
}

// GetScrollRange(hwnd, nBar, lpMin, lpMax)
static int api_GetScrollRange(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t bar = w16_arg16(a);
    uint32_t pmin = w16_arg32(a), pmax = w16_arg32(a);
    int16_t v[2] = { 0, 0 };
    if (x && bar <= SB_VERT) { v[0] = x->sb[bar].min; v[1] = x->sb[bar].max; }
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
