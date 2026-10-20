// usermisc16.c -- USER's smaller exports: rectangles, timers, the mouse
// capture, window queries, wsprintf, and the menu and accelerator calls
// that stand in until menus are built (stage 3: a menu handle names its
// resource, and nothing draws it yet). user16.c keeps classes, the window,
// the message loop and painting.
//
// TIMERS are the MAIN thread's (w16.c's on_tick): it posts WM_TIMER into
// the queue when one is due, at most one pending per timer, as Windows
// coalesces them; GetMessage clears `pending` when it hands one out.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lib/uclip.h"


uint64_t w16_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static char *far_dup(struct w16 *w, uint32_t far);

static int get_rect(struct w16 *w, uint32_t far, int16_t r[4]) {
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 8, 0);
    if (!p) return -1;
    memcpy(r, p, 8);
    return 0;
}

static int put_rect(struct w16 *w, uint32_t far, const int16_t r[4]) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 8, 1);
    if (!p) return -1;
    memcpy(p, r, 8);
    return 0;
}

static int point_offset(struct w16 *w, uint32_t far, int dx, int dy) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 4, 1);
    if (!p) return -1;
    int16_t pt[2];
    memcpy(pt, p, 4);
    pt[0] = (int16_t)(pt[0] + dx);
    pt[1] = (int16_t)(pt[1] + dy);
    memcpy(p, pt, 4);
    return 0;
}

// SCREEN COORDINATES are the top-level window's own: w16 knows the frame,
// not where the compositor put the window. A client point of `x` becomes
// one by adding x's place in its top-level window's client buffer and
// that buffer's place in the window (the main window's frame).
void w16_screen_offset(struct w16 *w, struct w16_wnd *x, int *dx, int *dy) {
    *dx = *dy = 0;
    if (!x) return;
    struct w16_wnd *top = w16_wnd_top(w, x);
    w16_wnd_origin(w, x, dx, dy);
    if (top == &w->win) {
        int cx, cy, cw, ch;
        w16_client_rect(top, &cx, &cy, &cw, &ch);
        *dx += cx;
        *dy += cy;
    } else if (top) {
        *dx += w16_dlg_frame(top);
        *dy += w16_dlg_frame_top(top) + w16_dlg_caption(top);
    }
}

// The last mouse point in screen coordinates.
void w16_cursor_screen(struct w16 *w, int16_t *x, int16_t *y) {
    int dx, dy;
    w16_screen_offset(w, w16_wnd_get(w, w->mouse_hwnd ? w->mouse_hwnd : w->win.hwnd), &dx, &dy);
    *x = (int16_t)(w->mouse_x + dx);
    *y = (int16_t)(w->mouse_y + dy);
}

// --- wsprintf ----------------------------------------------------------------

// The C-convention wsprintf: arguments on the stack in order, the caller
// pops them. %d %i %u %x %X %c %s %%, with '-', '0', a width and 'l'.
static int wsprintf16(struct w16 *w) {
    struct cpu16 *c = &w->cpu;
    unsigned at = 0;
    uint32_t out = cpu16_arg(c, at) | (uint32_t)cpu16_arg(c, at + 2) << 16;
    at += 4;
    uint32_t fmtp = cpu16_arg(c, at) | (uint32_t)cpu16_arg(c, at + 2) << 16;
    at += 4;
    char fmt[512], buf[1024];
    if (cpu16_str(c, (uint16_t)(fmtp >> 16), (uint16_t)fmtp, fmt, sizeof fmt) < 0) {
        cpu16_set_fault(c, FAULT_API, "wsprintf was passed a bad format");
        return -1;
    }
    unsigned n = 0;
    for (const char *f = fmt; *f && n < sizeof buf - 1; f++) {
        if (*f != '%') { buf[n++] = *f; continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0;
        for (; *f == '-' || *f == '0'; f++) { if (*f == '-') left = 1; else zero = 1; }
        for (; *f >= '0' && *f <= '9'; f++) width = width * 10 + (*f - '0');
        if (*f == 'l' || *f == 'L') { lng = 1; f++; }
        char tmp[300];
        switch (*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            uint32_t v = cpu16_arg(c, at);
            if (lng) v |= (uint32_t)cpu16_arg(c, at + 2) << 16;
            at += lng ? 4 : 2;
            if (*f == 'd' || *f == 'i')
                snprintf(tmp, sizeof tmp, "%ld", lng ? (long)(int32_t)v : (long)(int16_t)v);
            else
                snprintf(tmp, sizeof tmp, *f == 'u' ? "%lu" : *f == 'x' ? "%lx" : "%lX", (unsigned long)v);
            break;
        }
        case 'c':
            tmp[0] = (char)cpu16_arg(c, at);
            tmp[1] = 0;
            at += 2;
            break;
        case 's': {
            uint32_t sp = cpu16_arg(c, at) | (uint32_t)cpu16_arg(c, at + 2) << 16;
            at += 4;
            if (cpu16_str(c, (uint16_t)(sp >> 16), (uint16_t)sp, tmp, sizeof tmp) < 0) tmp[0] = 0;
            break;
        }
        case '%': tmp[0] = '%'; tmp[1] = 0; break;
        default: tmp[0] = 0; if (!*f) f--; break;
        }
        int len = (int)strlen(tmp), pad = width > len ? width - len : 0;
        if (!left) while (pad-- > 0 && n < sizeof buf - 1) buf[n++] = zero ? '0' : ' ';
        for (int i = 0; i < len && n < sizeof buf - 1; i++) buf[n++] = tmp[i];
        if (left) while (pad-- > 0 && n < sizeof buf - 1) buf[n++] = ' ';
    }
    buf[n] = 0;
    uint8_t *p = cpu16_ptr(c, (uint16_t)(out >> 16), (uint16_t)out, n + 1, 1);
    if (!p) return -1;
    memcpy(p, buf, n + 1);
    c->r[R_AX] = (uint16_t)n;
    return 0;                                    // the caller pops
}

// --- timers ------------------------------------------------------------------

static uint16_t set_timer(struct w16 *w, uint16_t hwnd, uint16_t id, uint32_t ms, uint32_t proc) {
    pthread_mutex_lock(&w->lock);
    struct w16_timer *t = 0;
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id && w->timer[i].hwnd == hwnd) t = &w->timer[i];
    for (int i = 0; !t && i < W16_TIMERS; i++)
        if (!w->timer[i].used) t = &w->timer[i];
    if (t) {
        *t = (struct w16_timer){ .hwnd = hwnd, .id = id, .proc = proc, .interval_ms = ms ? ms : 1,
                                 .due_ms = w16_now_ms() + (ms ? ms : 1), .used = 1 };
    }
    pthread_mutex_unlock(&w->lock);
    return t ? (id ? id : 1) : 0;
}

void w16_set_timer(struct w16 *w, uint16_t hwnd, uint16_t id, uint32_t ms) { set_timer(w, hwnd, id, ms, 0); }

void w16_kill_timer(struct w16 *w, uint16_t hwnd, uint16_t id) {
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id && w->timer[i].hwnd == hwnd) w->timer[i].used = 0;
    pthread_mutex_unlock(&w->lock);
}

void w16_timers_fire(struct w16 *w) {
    uint64_t now = w16_now_ms();
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < W16_TIMERS; i++) {
        struct w16_timer *t = &w->timer[i];
        if (!t->used || t->pending || now < t->due_ms) continue;
        unsigned next = (w->qtail + 1) % W16_QUEUE;
        if (next == w->qhead) break;
        w->q[w->qtail] = (struct w16_msg){ t->hwnd ? t->hwnd : w->win.hwnd, WM_TIMER, t->id, t->proc, 0 };
        w->qtail = next;
        t->pending = 1;
        t->due_ms = now + t->interval_ms;
        pthread_cond_signal(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
}

// --- the dispatcher -------------------------------------------------------------

// SetTimer(hwnd, id, ms, lpTimerFunc)
static int api_SetTimer(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), id = w16_arg16(a), ms = w16_arg16(a);
    c->r[R_AX] = set_timer(w, hwnd, id, ms, w16_arg32(a));
    return 10;
}

// KillTimer(hwnd, id)
static int api_KillTimer(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), id = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    int found = 0;
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id && w->timer[i].hwnd == hwnd) {
            w->timer[i].used = 0;
            found = 1;
        }
    pthread_mutex_unlock(&w->lock);
    c->r[R_AX] = (uint16_t)found;
    return 4;
}

// GetTickCount, GetCurrentTime: ms since boot
static int api_GetTickCount(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    uint32_t ms = (uint32_t)w16_now_ms();
    c->r[R_AX] = (uint16_t)ms;
    c->r[R_DX] = (uint16_t)(ms >> 16);
    return 0;
}

// GetCursorPos(lpPoint)
static int api_GetCursorPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t p;
    (void)ordinal;
    int16_t pt[2];
    w16_cursor_screen(w, &pt[0], &pt[1]);
    p = w16_arg32(a);
    uint8_t *q = cpu16_ptr(c, (uint16_t)(p >> 16), (uint16_t)p, 4, 1);
    if (!q) return -1;
    memcpy(q, pt, 4);
    return 4;
}

// SetCursorPos(x, y): screen coordinates, which are the main window's
// (w16_screen_offset). toywm moves the pointer only while that window has
// the focus and only within it; GetCursorPos answers the new place at once.
static int api_SetCursorPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int dx, dy;
    w16_screen_offset(w, &w->win, &dx, &dy);
    w->mouse_hwnd = w->win.hwnd;
    w->mouse_x = (int16_t)(x - dx);
    w->mouse_y = (int16_t)(y - dy);
    w16_task_post_arg(w, W16_POST_POINTER, (int)((uint32_t)(uint16_t)x << 16 | (uint16_t)y));
    return 4;
}

// SetCapture(hwnd) -> the previous
static int api_SetCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    pthread_mutex_lock(&w->lock);
    c->r[R_AX] = w->capture;
    w->capture = w16_arg16(a);
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// ReleaseCapture
static int api_ReleaseCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    pthread_mutex_lock(&w->lock);
    w->capture = 0;
    pthread_mutex_unlock(&w->lock);
    return 0;
}

// GetCapture
static int api_GetCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w->capture;
    return 0;
}

// SetFocus(hwnd) -> the previous
static int api_SetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_set_focus(w, w16_arg16(a));
    return c->fault ? -1 : 2;
}

// GetFocus
static int api_GetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w->focus;
    return 0;
}

// SetActiveWindow(hwnd) -> the previous: w16's one top-level window, or its dialog
static int api_SetActiveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w->win.hwnd;
    return 2;
}

// GetActiveWindow: the top-level window the focus is in
static int api_GetActiveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    struct w16_wnd *f = w16_wnd_get(w, w->focus), *top = f ? w16_wnd_top(w, f) : 0;
    c->r[R_AX] = top ? top->hwnd : w->win.used ? w->win.hwnd : 0;
    return 0;
}

// ClientToScreen, ScreenToClient(hwnd, lpPoint)
static int api_ClientToScreen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t p = w16_arg32(a);
    int dx, dy;
    w16_screen_offset(w, x, &dx, &dy);
    if (point_offset(w, p, ordinal == 28 ? dx : -dx, ordinal == 28 ? dy : -dy) < 0) return -1;
    return 6;
}

// IsIconic(hwnd)
static int api_IsIconic(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// GetWindowRect(hwnd, lpRect): in screen coordinates
static int api_GetWindowRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int16_t wr[4] = { 0, 0, 0, 0 };
    if (x && x->kind == W16_K_CHILD) {
        int dx, dy, l, t, r, b;
        w16_screen_offset(w, x, &dx, &dy);
        w16_wnd_insets(x, &l, &t, &r, &b);
        wr[0] = (int16_t)(dx - l);
        wr[1] = (int16_t)(dy - t);
    }
    if (x) { wr[2] = (int16_t)(wr[0] + x->w); wr[3] = (int16_t)(wr[1] + x->h); }
    if (put_rect(w, w16_arg32(a), wr) < 0) return -1;
    return 6;
}

// BringWindowToTop(hwnd)
static int api_BringWindowToTop(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 1;
    return 2;
}

// IsWindow, IsWindowVisible(hwnd)
static int api_IsWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    c->r[R_AX] = x && !x->destroyed && (ordinal == 47 || w16_wnd_shown(w, x));
    return 2;
}

// FindWindow(class, title): no other program
static int api_FindWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg32(a); w16_arg32(a);
    c->r[R_AX] = 0;
    return 8;
}

// SetCursor(h) -> the previous: the pointer takes it now, until it next
// moves over a window, whose class cursor it then takes (WM_SETCURSOR's
// default)
static int api_SetCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    static uint16_t current;
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = current;
    current = h;
    w16_task_post_arg(w, W16_POST_CURSOR, w16_cursor_shape(h));
    return 2;
}

// ShowCursor(show) -> the display count
static int api_ShowCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_arg16(a) ? 1 : 0;
    return 2;
}

// SetRect(lpRect, l, t, r, b)
static int api_SetRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    uint32_t p;
    (void)ordinal;
    p = w16_arg32(a);
    for (int i = 0; i < 4; i++) r[i] = (int16_t)w16_arg16(a);
    return put_rect(w, p, r) < 0 ? -1 : 12;
}

// SetRectEmpty(lpRect)
static int api_SetRectEmpty(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    memset(r, 0, sizeof r);
    return put_rect(w, w16_arg32(a), r) < 0 ? -1 : 4;
}

// CopyRect(lpDst, lpSrc)
static int api_CopyRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int16_t r[4];
    (void)ordinal;
    uint32_t dst = w16_arg32(a);
    if (get_rect(w, w16_arg32(a), r) < 0 || put_rect(w, dst, r) < 0) return -1;
    c->r[R_AX] = 1;
    return 8;
}

// PtInRect(lpRect, POINT): the right and bottom edges are out
static int api_PtInRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int16_t r[4];
    uint32_t p;
    (void)ordinal;
    p = w16_arg32(a);
    uint32_t pt = w16_arg32(a);
    int16_t x = (int16_t)pt, y = (int16_t)(pt >> 16);
    if (get_rect(w, p, r) < 0) return -1;
    c->r[R_AX] = x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
    return 8;
}

// OffsetRect, InflateRect(lpRect, dx, dy)
static int api_OffsetRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    uint32_t p;
    p = w16_arg32(a);
    int16_t dx = (int16_t)w16_arg16(a), dy = (int16_t)w16_arg16(a);
    if (get_rect(w, p, r) < 0) return -1;
    if (ordinal == 77) { r[0] += dx; r[2] += dx; r[1] += dy; r[3] += dy; }
    else { r[0] -= dx; r[2] += dx; r[1] -= dy; r[3] += dy; }
    return put_rect(w, p, r) < 0 ? -1 : 8;
}

// IntersectRect(lpDst, lpA, lpB): nonzero if they meet
static int api_IntersectRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int16_t r[4], s[4];
    (void)ordinal;
    uint32_t dst = w16_arg32(a), pa = w16_arg32(a), pb = w16_arg32(a);
    if (get_rect(w, pa, r) < 0 || get_rect(w, pb, s) < 0) return -1;
    int16_t o[4] = { r[0] > s[0] ? r[0] : s[0], r[1] > s[1] ? r[1] : s[1],
                     r[2] < s[2] ? r[2] : s[2], r[3] < s[3] ? r[3] : s[3] };
    int meet = o[0] < o[2] && o[1] < o[3];
    if (!meet) memset(o, 0, sizeof o);
    if (put_rect(w, dst, o) < 0) return -1;
    c->r[R_AX] = (uint16_t)meet;
    return 12;
}

// FillRect(hdc, lpRect, hbr)
static int api_FillRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int16_t r[4];
    uint32_t p;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    p = w16_arg32(a);
    uint16_t br = w16_arg16(a);
    if (get_rect(w, p, r) < 0) return -1;
    w16_gdi_fill(w, hdc, r[0], r[1], r[2], r[3], br);
    c->r[R_AX] = 1;
    return 8;
}

// InvertRect(hdc, lpRect)
static int api_InvertRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    if (get_rect(w, w16_arg32(a), r) < 0) return -1;
    w16_gdi_invert(w, hdc, r[0], r[1], r[2], r[3]);
    return 6;
}

// FrameRect(hdc, lpRect, hbr): a one-pixel border in the brush
static int api_FrameRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    uint32_t p = w16_arg32(a);
    uint16_t br = w16_arg16(a);
    if (get_rect(w, p, r) < 0) return -1;
    w->cpu.r[R_AX] = 1;
    if (r[2] <= r[0] || r[3] <= r[1]) return 8;
    w16_gdi_fill(w, hdc, r[0], r[1], r[2], r[1] + 1, br);
    w16_gdi_fill(w, hdc, r[0], r[3] - 1, r[2], r[3], br);
    w16_gdi_fill(w, hdc, r[0], r[1], r[0] + 1, r[3], br);
    w16_gdi_fill(w, hdc, r[2] - 1, r[1], r[2], r[3], br);
    return 8;
}

// DrawText(hdc, lpString, nCount, lpRect, uFormat) -> the text's height;
// nCount -1: up to the NUL
static int api_DrawText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int16_t n = (int16_t)w16_arg16(a);
    uint32_t pr = w16_arg32(a);
    uint16_t fmt = w16_arg16(a);
    char *text;
    if (n < 0) {
        text = far_dup(w, far);
    } else {
        const uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n, 0) : 0;
        if (n && !p) return -1;
        text = p ? strndup((const char *)p, (size_t)n) : strdup("");
    }
    if (!text) return -1;
    if (get_rect(w, pr, r) < 0) { free(text); return -1; }
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  DrawText \"%s\" in %d,%d,%d,%d format %04x\n", text, r[0], r[1], r[2], r[3], fmt);
        fflush(g_w16_trace);
    }
    w->cpu.r[R_AX] = (uint16_t)w16_gdi_draw_text(w, hdc, text, r, fmt);
    free(text);
    if ((fmt & 0x0400) && put_rect(w, pr, r) < 0) return -1;   // DT_CALCRECT
    return 14;
}

// LoadBitmap(hInst, lpName): a BITMAP resource as a bitmap. 3.1's own
// (OBM_*, hInst NULL) are not drawn yet: 0, as for a missing name.
static int api_LoadBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t inst = w16_arg16(a);
    uint32_t name = w16_arg32(a);
    char sname[64] = "";
    w->cpu.r[R_AX] = 0;
    if ((name >> 16) && cpu16_str(&w->cpu, (uint16_t)(name >> 16), (uint16_t)name, sname, sizeof sname) < 0)
        return -1;
    const struct ne_module *m = w16_module_of(w, inst);
    struct ne_resource r;
    if (m && ne_resource_find(m, 2, 0, (name >> 16) ? 0 : (uint16_t)name, sname[0] ? sname : 0, &r) >= 0)
        w->cpu.r[R_AX] = w16_gdi_bitmap_of_dib(w, m->img + r.off, r.len);
    return 6;
}

// MessageBeep(type): no sound yet
static int api_MessageBeep(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    w16_arg16(a);
    return 2;
}

// GetSysColor(index) -> a COLORREF
static int api_GetSysColor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint32_t px = w16_syscolor(w16_arg16(a));
    uint32_t cr = (px & 0xFF) << 16 | (px & 0xFF00) | (px >> 16 & 0xFF);
    c->r[R_AX] = (uint16_t)cr;
    c->r[R_DX] = (uint16_t)(cr >> 16);
    return 2;
}

// GetDesktopWindow
static int api_GetDesktopWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = W16_DESKTOP;
    return 0;
}

// GetLastActivePopup(hwnd): no popups yet
static int api_GetLastActivePopup(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_arg16(a);
    return 2;
}

// wsprintf
static int api_wsprintf(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    return wsprintf16(w);
}

// --- strings -----------------------------------------------------------------

// A far string, allocated; NULL after a fault.
static char *far_dup(struct w16 *w, uint32_t far) {
    size_t cap = 256;
    for (;;) {
        char *s = malloc(cap);
        if (!s) { cpu16_set_fault(&w->cpu, FAULT_API, "out of memory for a string"); return 0; }
        int n = cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, (unsigned)cap);
        if (n < 0) { free(s); return 0; }
        if ((size_t)n < cap - 1 || cap >= 65536) return s;
        free(s);
        cap *= 4;
    }
}

static int fold(int c) { return c >= 'a' && c <= 'z' ? c - 32 : (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c; }

// lstrcmp, lstrcmpi(lpString1, lpString2): -1, 0 or 1, in the ANSI order
static int api_lstrcmp(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint32_t p1 = w16_arg32(a), p2 = w16_arg32(a);
    char *s = far_dup(w, p1), *t = s ? far_dup(w, p2) : 0;
    if (!s || !t) { free(s); return -1; }
    int r = 0;
    for (const unsigned char *x = (unsigned char *)s, *y = (unsigned char *)t; !r; x++, y++) {
        int cx = ordinal == 471 ? fold(*x) : *x, cy = ordinal == 471 ? fold(*y) : *y;
        r = cx < cy ? -1 : cx > cy;
        if (!*x || !*y) break;
    }
    free(s);
    free(t);
    w->cpu.r[R_AX] = (uint16_t)(int16_t)r;
    return 8;
}

// AnsiUpper, AnsiLower(lpString): in place -- or, with a null selector, the
// one character in the offset's low byte
static int api_AnsiUpper(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint32_t far = w16_arg32(a);
    int up = ordinal == 431;
    if (!(far >> 16)) {
        int ch = far & 0xFF;
        ch = up ? fold(ch) : (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 32 : ch;
        w->cpu.r[R_AX] = (uint16_t)ch;
        w->cpu.r[R_DX] = 0;
        return 4;
    }
    char *s = far_dup(w, far);
    if (!s) return -1;
    size_t n = strlen(s);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n + 1, 1);
    for (size_t i = 0; p && i < n; i++) {
        int ch = p[i];
        p[i] = (uint8_t)(up ? fold(ch) : (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 32 : ch);
    }
    free(s);
    if (!p) return -1;
    w->cpu.r[R_AX] = (uint16_t)far;
    w->cpu.r[R_DX] = (uint16_t)(far >> 16);
    return 4;
}

// AnsiNext(lpCurrent): the next character, staying on the terminator
static int api_AnsiNext(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 1, 0);
    if (!p) return -1;
    uint32_t r = *p ? far + 1 : far;
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(far >> 16);
    return 4;
}

// AnsiPrev(lpStart, lpCurrent): the character before, never before lpStart
static int api_AnsiPrev(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t start = w16_arg32(a), cur = w16_arg32(a);
    uint16_t off = (uint16_t)cur > (uint16_t)start ? (uint16_t)((uint16_t)cur - 1) : (uint16_t)start;
    w->cpu.r[R_AX] = off;
    w->cpu.r[R_DX] = (uint16_t)(cur >> 16);
    return 8;
}

// TabbedTextOut(hdc, x, y, lpString, nCount, nTabPositions, lpnTabStopPositions,
// nTabOrigin) -> MAKELONG(width, height): tabs every 8 average characters,
// or every first stop's distance when one is given
static int api_TabbedTextOut(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int16_t n = (int16_t)w16_arg16(a), ntabs = (int16_t)w16_arg16(a);
    uint32_t tabs = w16_arg32(a);
    w16_arg16(a);
    char *text = n > 0 ? malloc((size_t)n + 1) : strdup("");
    if (!text) return -1;
    if (n > 0) {
        const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n, 0);
        if (!p) { free(text); return -1; }
        memcpy(text, p, (size_t)n);
        text[n] = 0;
    }
    int ave, cy, tab;
    w16_text_size("x", &ave, &cy);
    tab = ave * 8;
    if (ntabs > 0 && (tabs >> 16)) {
        const uint8_t *t = cpu16_ptr(&w->cpu, (uint16_t)(tabs >> 16), (uint16_t)tabs, 2, 0);
        if (!t) { free(text); return -1; }
        int16_t first = (int16_t)(t[0] | t[1] << 8);
        if (first > 0) tab = first;
    }
    int width = w16_gdi_text(w, hdc, x, y, text, tab);
    free(text);
    w->cpu.r[R_AX] = (uint16_t)(width > 0 ? width : 0);
    w->cpu.r[R_DX] = (uint16_t)cy;
    return 22;
}

// --- the clipboard: the EDIT control moves its text itself (edit16.c) ------------

#define CF_TEXT 1

// OpenClipboard(hwnd), CloseClipboard: one clipboard, always open to us
static int api_OpenClipboard(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    if (ordinal == 137) w16_arg16(a);
    w->cpu.r[R_AX] = 1;
    return ordinal == 137 ? 2 : 0;
}

// EnumClipboardFormats(fmt): CF_TEXT when there is text, then the end
static int api_EnumClipboardFormats(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t fmt = w16_arg16(a);
    w->cpu.r[R_AX] = fmt == 0 && uclip_peek_op() != UCLIP_NONE ? CF_TEXT : 0;
    return 2;
}

// IsClipboardFormatAvailable(fmt)
static int api_IsClipboardFormatAvailable(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    static struct uclip c;
    int n;
    uint16_t fmt = w16_arg16(a);
    w->cpu.r[R_AX] = fmt == CF_TEXT && uclip_load(&c) && uclip_text(&c, &n) != 0;
    return 2;
}

// WinHelp(hwnd, lpHelpFile, wCommand, dwData): there is no help viewer,
// so a request for help fails -- and HELP_QUIT, which a program sends as
// it closes (Minesweeper does, on WM_DESTROY), succeeds.
static int api_WinHelp(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t owner = w16_arg16(a);
    uint32_t file = w16_arg32(a);
    uint16_t cmd = w16_arg16(a);
    w16_arg32(a);
    // Asked for help, not just closing it (HELP_QUIT): say what is missing
    // rather than nothing, and report it shown -- the program has no
    // better thing to do with a failure.
    if (cmd != 2) {
        char name[80] = "", text[200];
        if (file >> 16) cpu16_str(&w->cpu, (uint16_t)(file >> 16), (uint16_t)file, name, sizeof name);
        const char *base = strrchr(name, '\\');
        snprintf(text, sizeof text, "%s is a Windows Help file, and w16 cannot show Help files yet.",
                 base ? base + 1 : name[0] ? name : "This program's help");
        w16_message_box(w, owner, text, "Help", 0x40);   // MB_ICONASTERISK
    }
    w->cpu.r[R_AX] = 1;
    return 12;
}

const struct w16_api g_user_misc_api[] = {
    { 10, api_SetTimer },
    { 12, api_KillTimer },
    { 13, api_GetTickCount },
    { 15, api_GetTickCount },
    { 17, api_GetCursorPos },
    { 18, api_SetCapture },
    { 19, api_ReleaseCapture },
    { 22, api_SetFocus },
    { 23, api_GetFocus },
    { 28, api_ClientToScreen },
    { 29, api_ClientToScreen },
    { 31, api_IsIconic },
    { 32, api_GetWindowRect },
    { 45, api_BringWindowToTop },
    { 47, api_IsWindow },
    { 49, api_IsWindow },
    { 50, api_FindWindow },
    { 59, api_SetActiveWindow },
    { 60, api_GetActiveWindow },
    { 69, api_SetCursor },
    { 70, api_SetCursorPos },
    { 71, api_ShowCursor },
    { 72, api_SetRect },
    { 73, api_SetRectEmpty },
    { 74, api_CopyRect },
    { 76, api_PtInRect },
    { 77, api_OffsetRect },
    { 78, api_OffsetRect },
    { 79, api_IntersectRect },
    { 81, api_FillRect },
    { 82, api_InvertRect },
    { 83, api_FrameRect },
    { 85, api_DrawText },
    { 104, api_MessageBeep },
    { 137, api_OpenClipboard },
    { 138, api_OpenClipboard },
    { 144, api_EnumClipboardFormats },
    { 171, api_WinHelp },
    { 175, api_LoadBitmap },
    { 180, api_GetSysColor },
    { 193, api_IsClipboardFormatAvailable },
    { 196, api_TabbedTextOut },
    { 236, api_GetCapture },
    { 286, api_GetDesktopWindow },
    { 287, api_GetLastActivePopup },
    { 420, api_wsprintf },
    { 430, api_lstrcmp },
    { 431, api_AnsiUpper },
    { 432, api_AnsiUpper },
    { 471, api_lstrcmp },
    { 472, api_AnsiNext },
    { 473, api_AnsiPrev },
};

// KEYBOARD's AnsiToOem, OemToAnsi(src, dst): w16's DOS has no code page of
// its own -- its file names are the program's ANSI ones -- so the two
// sets are one and a string is copied as it is.
static int api_AnsiToOem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t src = w16_arg32(a), dst = w16_arg32(a);
    char *s = far_dup(w, src);
    if (!s) return -1;
    size_t n = strlen(s);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(dst >> 16), (uint16_t)dst, (unsigned)n + 1, 1);
    if (p) memmove(p, s, n + 1);
    free(s);
    if (!p) return -1;
    w->cpu.r[R_AX] = 1;
    return 8;
}

const struct w16_api g_keyboard_api[] = {
    { 5, api_AnsiToOem },
    { 6, api_AnsiToOem },
};
const unsigned g_keyboard_api_n = sizeof g_keyboard_api / sizeof g_keyboard_api[0];
const unsigned g_user_misc_api_n = sizeof g_user_misc_api / sizeof g_user_misc_api[0];

int w16_user_misc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_user_misc_api, sizeof g_user_misc_api / sizeof g_user_misc_api[0], w, ordinal, a);
}
