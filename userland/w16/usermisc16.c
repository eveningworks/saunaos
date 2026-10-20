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

// SetTimer(hWnd, nIDEvent, uElapse, lpTimerFunc) -> UINT
// Windows: Starts a timer that fires every uElapse milliseconds -- as WM_TIMER to the window, or a
// call to the timer procedure -- coming only when the program waits for messages and nothing else
// is queued.
// w16: The same.
static int api_SetTimer(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), id = w16_arg16(a), ms = w16_arg16(a);
    c->r[R_AX] = set_timer(w, hwnd, id, ms, w16_arg32(a));
    return 10;
}

// KillTimer(hWnd, uIDEvent) -> BOOL
// Windows: Stops a timer.
// w16: The same.
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

// GetTickCount() -> DWORD
// GetCurrentTime() -> DWORD
// Windows: Milliseconds since Windows started.
// w16: Milliseconds since toy-os started.
static int api_GetTickCount(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    uint32_t ms = (uint32_t)w16_now_ms();
    c->r[R_AX] = (uint16_t)ms;
    c->r[R_DX] = (uint16_t)(ms >> 16);
    return 0;
}

// GetCursorPos(lpPoint) -> void
// Windows: Where the mouse pointer is, in screen coordinates.
// w16: Where it is in the program's screen -- its top-level window, whose client area's corner is
// the screen's 0,0 as the program counts it.
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

// SetCursorPos(X, Y) -> void
// Windows: Moves the mouse pointer to a place on the screen.
// w16: Moves it within the program's own window, and only while that window has the focus: a
// program may not take the pointer anywhere else on the desktop. GetCursorPos answers the new place
// at once.
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

// SetCapture(hWnd) -> HWND
// Windows: Sends the mouse to one window, wherever the pointer goes, until ReleaseCapture -- how a
// drag keeps going outside the window; returns the window that had it.
// w16: The same, toywm keeping the pointer's moves coming while a button is held outside the
// window.
static int api_SetCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    pthread_mutex_lock(&w->lock);
    c->r[R_AX] = w->capture;
    w->capture = w16_arg16(a);
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// ReleaseCapture() -> void
// Windows: Ends SetCapture.
// w16: The same.
static int api_ReleaseCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    pthread_mutex_lock(&w->lock);
    w->capture = 0;
    pthread_mutex_unlock(&w->lock);
    return 0;
}

// GetCapture() -> HWND
// Windows: The window that has captured the mouse, or NULL.
// w16: The same.
static int api_GetCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w->capture;
    return 0;
}

// SetFocus(hWnd) -> HWND
// Windows: Sends the keyboard to a window, with WM_KILLFOCUS and WM_SETFOCUS; returns the one that
// had it.
// w16: The same, among the program's windows.
static int api_SetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_set_focus(w, w16_arg16(a));
    return c->fault ? -1 : 2;
}

// GetFocus() -> HWND
// Windows: The window the keyboard goes to.
// w16: The same.
static int api_GetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w->focus;
    return 0;
}

// SetActiveWindow(hWnd) -> HWND
// Windows: Makes a top-level window the active one; returns the one that was.
// w16: Returns the program's main window and changes nothing: toywm decides which window on the
// desktop is active.
static int api_SetActiveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w->win.hwnd;
    return 2;
}

// GetActiveWindow() -> HWND
// Windows: The active top-level window, if it is the program's.
// w16: The program's top-level window the focus is in.
static int api_GetActiveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    struct w16_wnd *f = w16_wnd_get(w, w->focus), *top = f ? w16_wnd_top(w, f) : 0;
    c->r[R_AX] = top ? top->hwnd : w->win.used ? w->win.hwnd : 0;
    return 0;
}

// ClientToScreen(hWnd, lpPoint) -> void
// ScreenToClient(hWnd, lpPoint) -> void
// Windows: Turn a point between a window's client coordinates and the screen's.
// w16: The same, the screen being the program's (GetCursorPos says what that is).
static int api_ClientToScreen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t p = w16_arg32(a);
    int dx, dy;
    w16_screen_offset(w, x, &dx, &dy);
    if (point_offset(w, p, ordinal == 28 ? dx : -dx, ordinal == 28 ? dy : -dy) < 0) return -1;
    return 6;
}

// IsIconic(hWnd) -> BOOL
// Windows: Says whether a window is minimized -- shown as an icon at the foot of the screen in 3.x,
// hence the name.
// w16: Always says no, even while the window is minimized.
// Why: w16 minimizes a program's window through toywm, which keeps the state; w16 does not yet keep
// it on the window itself for this call to read.
// Notice: A program that paints its own icon while minimized, or stops its timer then, does
// neither. None run so far depends on it.
// Whole: w16 keeping the minimized state it already sets. On the roadmap: "w16: IsIconic answers
// from the window's state".
static int api_IsIconic(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// GetWindowRect(hWnd, lpRect) -> void
// Windows: A window's rectangle, frame included, in screen coordinates.
// w16: The same, in the program's screen.
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

// BringWindowToTop(hWnd) -> void
// Windows: Puts a window above the others.
// w16: Nothing to do: a program's windows stay in their creation order, and toywm stacks the
// desktop's.
static int api_BringWindowToTop(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 1;
    return 2;
}

// IsWindow(hWnd) -> BOOL
// IsWindowVisible(hWnd) -> BOOL
// Windows: Whether a handle is a window that exists; whether it is shown.
// w16: The same.
static int api_IsWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    c->r[R_AX] = x && !x->destroyed && (ordinal == 47 || w16_wnd_shown(w, x));
    return 2;
}

// FindWindow(lpClassName, lpWindowName) -> HWND
// Windows: Finds a top-level window by its class or its title, in any program. Programs use it to
// find a copy of themselves already running.
// w16: Finds none.
// Why: Each program runs in its own w16, so there are no other programs' windows to find. Its own
// windows are not searched either: a program asks this for someone else's.
// Notice: A program that allows only one copy of itself does not notice the first one; a second
// copy starts beside it.
// Whole: w16 programs that see each other. On the roadmap: "w16: one program starting or finding
// another".
static int api_FindWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg32(a); w16_arg32(a);
    c->r[R_AX] = 0;
    return 8;
}

// The pointer takes it now, until it next moves over a window, whose class cursor it then takes
// (WM_SETCURSOR's default).
// SetCursor(hCursor) -> HCURSOR
// Windows: Changes the mouse pointer's shape now -- an hourglass while busy; returns the old one.
// w16: The same, in toy-os's shape for it (LoadCursor says which).
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

// ShowCursor(bShow) -> int
// Windows: Counts the pointer shown or hidden: it shows while the count is 0 or more; returns the
// new count.
// w16: Answers as if the count were kept, and the pointer stays shown: toywm owns it.
static int api_ShowCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_arg16(a) ? 1 : 0;
    return 2;
}

// SetRect(lpRect, xLeft, yTop, xRight, yBottom) -> void
// Windows: Fills a RECT.
// w16: The same.
static int api_SetRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    uint32_t p;
    (void)ordinal;
    p = w16_arg32(a);
    for (int i = 0; i < 4; i++) r[i] = (int16_t)w16_arg16(a);
    return put_rect(w, p, r) < 0 ? -1 : 12;
}

// SetRectEmpty(lpRect) -> void
// Windows: Fills a RECT with zeros.
// w16: The same.
static int api_SetRectEmpty(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    memset(r, 0, sizeof r);
    return put_rect(w, w16_arg32(a), r) < 0 ? -1 : 4;
}

// CopyRect(lprcDst, lprcSrc) -> BOOL
// Windows: Copies a RECT.
// w16: The same.
static int api_CopyRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int16_t r[4];
    (void)ordinal;
    uint32_t dst = w16_arg32(a);
    if (get_rect(w, w16_arg32(a), r) < 0 || put_rect(w, dst, r) < 0) return -1;
    c->r[R_AX] = 1;
    return 8;
}

// PtInRect(lprc, pt) -> BOOL
// Windows: Whether a point is inside a rectangle -- its right and bottom edges outside.
// w16: The same.
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

// OffsetRect(lprc, dx, dy) -> BOOL
// InflateRect(lprc, dx, dy) -> BOOL
// Windows: Move a rectangle by dx, dy; or grow it by them on every side (shrink, when negative).
// w16: The same.
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

// IntersectRect(lprcDst, lprcSrc1, lprcSrc2) -> BOOL
// Windows: Where two rectangles overlap; FALSE, and an empty rectangle, when they do not.
// w16: The same.
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

// FillRect(hdc, lprc, hbr) -> int
// Windows: Fills a rectangle with a brush -- right and bottom edges left out.
// w16: The same.
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

// InvertRect(hdc, lprc) -> void
// Windows: Inverts a rectangle's colours -- how a selection or a pressed card shows.
// w16: The same.
static int api_InvertRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int16_t r[4];
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    if (get_rect(w, w16_arg32(a), r) < 0) return -1;
    w16_gdi_invert(w, hdc, r[0], r[1], r[2], r[3]);
    return 6;
}

// FrameRect(hdc, lprc, hbr) -> int
// Windows: Draws a one-pixel border round a rectangle in a brush.
// w16: The same.
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

// DrawText(hdc, lpString, nCount, lpRect, uFormat) -> int
// Windows: Draws text in a rectangle: left, centred or right, wrapped at words, on one line, its
// tabs expanded, '&' underlining the next letter, or only measured (DT_CALCRECT); returns its
// height.
// w16: The same formats in the DC's font; nCount -1 draws up to the string's end.
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

// LoadBitmap(hInstance, lpBitmapName) -> HBITMAP
// Windows: A bitmap from a BITMAP resource -- or, with hInstance NULL, one of Windows' own (the
// check mark, scroll arrows, size box...).
// w16: A program's own bitmap, as it is. Windows' own bitmaps are not drawn yet: 0, as for a name
// that is not there.
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

// GetWindowDC(hWnd) -> HDC
// Windows: A DC for the whole window, frame and caption included -- for a program that draws its
// own frame.
// w16: The client area's DC: the frame is w16's own (or toywm's), and a program does not paint on
// it.
static int api_GetWindowDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    w->cpu.r[R_AX] = h ? w16_gdi_window_dc(w, h) : w16_gdi_screen_dc(w);
    return 2;
}

// DrawIcon(hdc, X, Y, hIcon) -> BOOL
// Windows: Draws an icon -- its picture through its mask -- at a place.
// w16: The same for an icon LoadIcon loaded from the program's resources; Windows' own icons
// (IDI_*) are not drawn yet.
static int api_DrawIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint16_t hicon = w16_arg16(a);
    uint32_t *px = malloc(32 * 32 * 4);
    int iw, ih;
    int ok = px && w16_icon_of_handle(hicon, px, &iw, &ih) == 0;
    if (ok) w16_gdi_draw_argb(w, hdc, x, y, px, iw, ih);
    free(px);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 8;
}

// GetMessagePos() -> DWORD
// Windows: Where the pointer was when the last message was made, in screen coordinates.
// w16: The same, in the program's screen.
static int api_GetMessagePos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    int16_t x, y;
    w16_cursor_screen(w, &x, &y);
    w->cpu.r[R_AX] = (uint16_t)x;
    w->cpu.r[R_DX] = (uint16_t)y;
    return 0;
}

// GetMessageTime() -> LONG
// Windows: When the last message was made, as GetTickCount counts.
// w16: The same.
static int api_GetMessageTime(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    uint32_t ms = (uint32_t)w16_now_ms();
    w->cpu.r[R_AX] = (uint16_t)ms;
    w->cpu.r[R_DX] = (uint16_t)(ms >> 16);
    return 0;
}

// HOOKS: one per WH_* type, the newest. WH_CALLWNDPROC is CALLED -- before
// every sent message reaches its window procedure (w16_send), which is how
// MFC attaches its C++ window to the HWND CreateWindow is making (Hearts).
// The rest are kept and never called: SetWindowsHook answers the one set
// before it, DefHookProc and CallNextHookEx pass nothing on.
static uint32_t g_hooks[16];                     // per WH_* (-1..14)

uint32_t w16_hook(int id) { return id >= -1 && id <= 14 ? g_hooks[id + 1] : 0; }

// SetWindowsHook(nFilterType, pfnFilterProc) -> HHOOK
// Windows: Puts a function in a chain that Windows calls at a set point: WH_CALLWNDPROC before each
// message a window is sent, WH_GETMESSAGE as a message leaves the queue, WH_KEYBOARD for each key,
// WH_MSGFILTER inside a dialog's or a menu's own loop, and others.
// w16: Keeps every hook a program sets and answers with the one it replaced, as Windows did. Only
// WH_CALLWNDPROC is called -- before each message w16 sends. The others are kept and never called.
// Why: An MFC program cannot make a window without WH_CALLWNDPROC: MFC ties its C++ object to the
// window inside that hook, and Hearts is an MFC program. The other kinds need w16's message, dialog
// and menu loops to call out at the points 3.1 did, and no program run so far sets them.
// Notice: Hearts sets only WH_CALLWNDPROC and notices nothing. A program that watches keys or
// messages through WH_KEYBOARD or WH_GETMESSAGE would not see them.
// Whole: The other kinds called from the message, dialog and menu loops. On the roadmap: "w16: the
// hooks besides WH_CALLWNDPROC called".
static int api_SetWindowsHook(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t id = (int16_t)w16_arg16(a);
    uint32_t fn = w16_arg32(a), old = 0;
    if (id >= -1 && id <= 14) { old = g_hooks[id + 1]; g_hooks[id + 1] = fn; }
    w->cpu.r[R_AX] = (uint16_t)old;
    w->cpu.r[R_DX] = (uint16_t)(old >> 16);
    return 6;
}

// UnhookWindowsHook(nCode, pfnFilterProc) -> BOOL
// Windows: Takes a hook SetWindowsHook set out of its chain.
// w16: The same.
static int api_UnhookWindowsHook(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t id = (int16_t)w16_arg16(a);
    uint32_t fn = w16_arg32(a);
    int ok = id >= -1 && id <= 14 && g_hooks[id + 1] == fn;
    if (ok) g_hooks[id + 1] = 0;
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 6;
}

// DefHookProc(nCode, wParam, lParam, lplpfnNextHook) -> DWORD
// Windows: Passes a hook's event on to the next hook in the chain.
// w16: Answers 0: w16 keeps one hook of each kind, so there is no next one to call.
static int api_DefHookProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = w->cpu.r[R_DX] = 0;
    return 12;
}

// SetWindowsHookEx(idHook, lpfn, hInstance, hTask) -> HHOOK
// Windows: SetWindowsHook's newer form (3.1): the same hooks, with a handle the program gives back
// to take its hook out.
// w16: As SetWindowsHook: WH_CALLWNDPROC is called, the other kinds are kept and never called.
// Why: As SetWindowsHook. Hearts sets its WH_CALLWNDPROC hook this way, once for each window MFC
// makes.
// Notice: As SetWindowsHook.
// Whole: As SetWindowsHook: on the roadmap, "w16: the hooks besides WH_CALLWNDPROC called".
static int api_SetWindowsHookEx(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t id = (int16_t)w16_arg16(a);
    uint32_t fn = w16_arg32(a);
    w16_arg16(a); w16_arg16(a);
    uint32_t h = 0;
    if (id >= -1 && id <= 14) { g_hooks[id + 1] = fn; h = 0x48000000u | (uint32_t)(id + 1); }
    w->cpu.r[R_AX] = (uint16_t)h;
    w->cpu.r[R_DX] = (uint16_t)(h >> 16);
    return 10;
}

// UnhookWindowsHookEx(hhk) -> BOOL
// Windows: Takes out a hook SetWindowsHookEx set.
// w16: The same.
static int api_UnhookWindowsHookEx(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t h = w16_arg32(a);
    int ok = (h >> 16) == 0x4800 && (h & 0xFFFF) < 16;
    if (ok) g_hooks[h & 0xFFFF] = 0;
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 4;
}

// CallNextHookEx(hhk, nCode, wParam, lParam) -> LRESULT
// Windows: Passes a hook's event on to the next hook in the chain.
// w16: Answers 0: there is no next one (DefHookProc says why).
static int api_CallNextHookEx(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg16(a); w16_arg16(a); w16_arg32(a);
    w->cpu.r[R_AX] = w->cpu.r[R_DX] = 0;
    return 12;
}

// SelectPalette(hdc, hpal, bForceBackground) -> HPALETTE
// Windows: Puts a logical palette into a DC, for a program that needs its own 256 colours on a
// palette display.
// w16: Changes nothing: w16 draws in true colour, so every colour a program names is shown. Returns
// no previous palette.
static int api_SelectPalette(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 6;
}

// WNetGetCaps(nIndex) -> UINT
// Windows: What the network driver can do -- Windows for Workgroups' sharing and connections.
// w16: 0 for every question: to a program, there is no network.
static int api_WNetGetCaps(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 2;
}

// MessageBeep(uType) -> void
// Windows: Plays the sound of an event: the one for the message box icon uType names -- Asterisk,
// Exclamation, Critical Stop, Question -- else the Default Beep; the speaker's beep without a sound
// card.
// w16: The same event (sndPlaySound says which file plays it), through toy-os's sound daemon.
static int api_MessageBeep(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_sound_event(w, w16_beep_alias(w16_arg16(a)));
    return 2;
}

// GetSysColor(nIndex) -> COLORREF
// Windows: A colour of the desktop's scheme: window, window text, caption, menu, button face and
// shadow...
// w16: Windows 3.1's default scheme, as w16 draws its windows and controls in it.
static int api_GetSysColor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint32_t px = w16_syscolor(w16_arg16(a));
    uint32_t cr = (px & 0xFF) << 16 | (px & 0xFF00) | (px >> 16 & 0xFF);
    c->r[R_AX] = (uint16_t)cr;
    c->r[R_DX] = (uint16_t)(cr >> 16);
    return 2;
}

// GetDesktopWindow() -> HWND
// Windows: The desktop window, behind every other.
// w16: A handle standing for it: the program's screen.
static int api_GetDesktopWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = W16_DESKTOP;
    return 0;
}

// GetLastActivePopup(hwndOwner) -> HWND
// Windows: The popup a window owns that was active last -- its dialog, say -- or the window itself.
// w16: The window itself.
static int api_GetLastActivePopup(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w16_arg16(a);
    return 2;
}

// _wsprintf(lpszOut, lpszFormat, ...) -> int
// Windows: Formats a string, C's sprintf in USER: %d %i %u %x %X %c %s, with '-', '0', a width and
// 'l' for a long; up to 1024 characters.
// w16: The same specifiers and flags.
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

// lstrcmp(lpszString1, lpszString2) -> int
// lstrcmpi(lpszString1, lpszString2) -> int
// Windows: Compare two strings in the ANSI character set's order -- lstrcmpi without case; -1, 0 or
// 1.
// w16: The same, accented capitals and small letters folded together by lstrcmpi.
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

// AnsiUpper(lpString) -> LPSTR
// AnsiLower(lpString) -> LPSTR
// Windows: A string in capitals, or small letters, in place, accented letters too; or, given a null
// selector, the one character in the offset's low byte.
// w16: The same, both ways.
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

// AnsiNext(lpchCurrentChar) -> LPSTR
// Windows: The next character of a string, staying on its terminating 0.
// w16: The same; every character is one byte (IsDBCSLeadByte says why).
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

// AnsiPrev(lpchStart, lpchCurrentChar) -> LPSTR
// Windows: The character before, never before the string's start.
// w16: The same.
static int api_AnsiPrev(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t start = w16_arg32(a), cur = w16_arg32(a);
    uint16_t off = (uint16_t)cur > (uint16_t)start ? (uint16_t)((uint16_t)cur - 1) : (uint16_t)start;
    w->cpu.r[R_AX] = off;
    w->cpu.r[R_DX] = (uint16_t)(cur >> 16);
    return 8;
}

// TabbedTextOut(hDC, X, Y, lpString, nCount, nTabPositions, lpnTabStopPositions, nTabOrigin) -> LONG
// Windows: TextOut with its tabs expanded to tab stops: the stops given, every first stop's
// distance, or every 8 average characters; returns the width and height drawn.
// w16: The same.
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

// OpenClipboard(hWnd) -> BOOL
// CloseClipboard() -> BOOL
// Windows: Open the clipboard for the program to read or change, and close it.
// w16: Both succeed: the clipboard is toy-os's, shared with every program, and an EDIT's Cut, Copy
// and Paste reach it.
static int api_OpenClipboard(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    if (ordinal == 137) w16_arg16(a);
    w->cpu.r[R_AX] = 1;
    return ordinal == 137 ? 2 : 0;
}

// EnumClipboardFormats(format) -> UINT
// Windows: The clipboard's formats one by one: what follows `format`, 0 at the end.
// w16: CF_TEXT when toy-os's clipboard holds text, then the end.
static int api_EnumClipboardFormats(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t fmt = w16_arg16(a);
    w->cpu.r[R_AX] = fmt == 0 && uclip_peek_op() != UCLIP_NONE ? CF_TEXT : 0;
    return 2;
}

// IsClipboardFormatAvailable(format) -> BOOL
// Windows: Whether the clipboard holds a format -- whether Paste should be enabled.
// w16: TRUE for CF_TEXT when toy-os's clipboard holds text.
static int api_IsClipboardFormatAvailable(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    static struct uclip c;
    int n;
    uint16_t fmt = w16_arg16(a);
    w->cpu.r[R_AX] = fmt == CF_TEXT && uclip_load(&c) && uclip_text(&c, &n) != 0;
    return 2;
}

// There is no help viewer, so a request for help fails -- and HELP_QUIT, which a program sends as
// it closes (Minesweeper does, on WM_DESTROY), succeeds.
// WinHelp(hWnd, lpszHelp, uCommand, dwData) -> BOOL
// Windows: Opens the program's Help file (.HLP) in Windows Help, at its contents, a topic or a
// keyword.
// w16: Shows a box saying the file is a Windows Help file that w16 cannot show yet, and answers
// that Help opened. The call a program makes as it closes, to close its Help, simply succeeds.
// Why: A .HLP file is a compiled format of its own -- compressed topics, its own fonts and
// pictures, hot spots -- and toy-os has no reader for it.
// Notice: Its Help menu and F1 show that box instead of Help.
// Whole: A viewer for .HLP files. On the roadmap: "w16: Windows Help files shown".
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
    { 67, api_GetWindowDC },
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
    { 84, api_DrawIcon },
    { 85, api_DrawText },
    { 104, api_MessageBeep },
    { 119, api_GetMessagePos },
    { 120, api_GetMessageTime },
    { 121, api_SetWindowsHook },
    { 137, api_OpenClipboard },
    { 138, api_OpenClipboard },
    { 144, api_EnumClipboardFormats },
    { 171, api_WinHelp },
    { 175, api_LoadBitmap },
    { 180, api_GetSysColor },
    { 193, api_IsClipboardFormatAvailable },
    { 196, api_TabbedTextOut },
    { 234, api_UnhookWindowsHook },
    { 235, api_DefHookProc },
    { 236, api_GetCapture },
    { 282, api_SelectPalette },
    { 286, api_GetDesktopWindow },
    { 287, api_GetLastActivePopup },
    { 291, api_SetWindowsHookEx },
    { 292, api_UnhookWindowsHookEx },
    { 293, api_CallNextHookEx },
    { 420, api_wsprintf },
    { 430, api_lstrcmp },
    { 431, api_AnsiUpper },
    { 432, api_AnsiUpper },
    { 471, api_lstrcmp },
    { 472, api_AnsiNext },
    { 473, api_AnsiPrev },
    { 513, api_WNetGetCaps },
};

// w16's DOS has no code page of its own -- its file names are the program's ANSI ones -- so the two
// sets are one and a string is copied as it is.
// AnsiToOem(lpszWindowsStr, lpszOemStr) -> int
// OemToAnsi(lpszOemStr, lpszWindowsStr) -> BOOL
// Windows: Translate a string between Windows' character set and DOS's code page -- for a file name
// DOS will see.
// w16: Copies it as it is: w16's DOS keeps the program's own characters in its names, so the two
// sets are one.
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
