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
#include "ui/ugfx.h"
#include "lib/uhlp.h"
#include "w16/w16prefs.h"
#include <strings.h>
#include "rt/sys.h"
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


uint64_t w16_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static char *far_dup(struct w16 *w, uint32_t far);
static unsigned caret_blink(const struct w16 *w);

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
// One 16-bit argument of a wsprintf: off the stack (`argp` 0) or out of the far array wvsprintf is given.
static unsigned fmt_arg(struct cpu16 *c, uint32_t argp, unsigned at) {
    if (!argp) return cpu16_arg(c, at);
    const uint8_t *p = cpu16_ptr(c, (uint16_t)(argp >> 16), (uint16_t)(argp + at), 2, 0);
    return p ? (unsigned)(p[0] | p[1] << 8) : 0;
}

// The common formatter: `out` and `fmtp` are far pointers; the arguments start `at` bytes on in
// whichever place `argp` says.
static int format16(struct w16 *w, uint32_t out, uint32_t fmtp, uint32_t argp, unsigned at) {
    struct cpu16 *c = &w->cpu;
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
            uint32_t v = fmt_arg(c, argp, at);
            if (lng) v |= (uint32_t)fmt_arg(c, argp, at + 2) << 16;
            at += lng ? 4 : 2;
            if (*f == 'd' || *f == 'i')
                snprintf(tmp, sizeof tmp, "%ld", lng ? (long)(int32_t)v : (long)(int16_t)v);
            else
                snprintf(tmp, sizeof tmp, *f == 'u' ? "%lu" : *f == 'x' ? "%lx" : "%lX", (unsigned long)v);
            break;
        }
        case 'c':
            tmp[0] = (char)fmt_arg(c, argp, at);
            tmp[1] = 0;
            at += 2;
            break;
        case 's': {
            uint32_t sp = fmt_arg(c, argp, at) | (uint32_t)fmt_arg(c, argp, at + 2) << 16;
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

// The C-convention wsprintf: out, format, then the arguments on the stack.
static int wsprintf16(struct w16 *w) {
    struct cpu16 *c = &w->cpu;
    uint32_t out = cpu16_arg(c, 0) | (uint32_t)cpu16_arg(c, 2) << 16;
    uint32_t fmtp = cpu16_arg(c, 4) | (uint32_t)cpu16_arg(c, 6) << 16;
    return format16(w, out, fmtp, 0, 8);
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

// WindowFromPoint(Point) -> HWND
// Windows: The window under a screen point -- the deepest visible child there, a label passing it
// to what is under it -- or NULL over no window.
// w16: The same over the program's own windows, a dialog before its main window; a point on a
// frame or caption, or on another program, answers NULL.
static int api_WindowFromPoint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t pt = w16_arg32(a);
    int x = (int16_t)pt, y = (int16_t)(pt >> 16);
    uint16_t r = 0;
    for (int i = w->nwnds; i >= 0 && !r; i--) {   // the dialogs, newest first; then the main window
        struct w16_wnd *t = i == 0 ? &w->win : w->wnds[i - 1];
        if (!t || !t->used || t->destroyed || !t->visible || (i && t->kind != W16_K_DIALOG)) continue;
        int dx, dy, cx, cy, nc;
        w16_screen_offset(w, t, &dx, &dy);
        if (x < dx || y < dy || x >= dx + t->cw || y >= dy + t->ch) continue;
        r = w16_wnd_at(w, t, x - dx, y - dy, &cx, &cy, &nc)->hwnd;
    }
    w->cpu.r[R_AX] = r;
    return 4;
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
// w16: The same: Windows' own cursors in toy-os's shape for them, a program's own as drawn (LoadCursor
// says which); NULL takes the pointer away until a window's class cursor brings it back.
static int api_SetCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    c->r[R_AX] = w->cursor_now;
    w->cursor_now = h;
    pthread_mutex_unlock(&w->lock);
    w16_cursor_refresh(w, h);            // its bits may be new: Paintbrush draws its brush into one
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
    w->cpu.r[R_AX] = h ? w16_gdi_take_dc(w, h) : w16_gdi_screen_dc(w);
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
// w16: Remembers it and gives back the one before, so a DIB_PAL_COLORS table can name its colours;
// nothing else changes: w16 draws in true colour, so every colour a program names is shown.
static int api_SelectPalette(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a), hpal = w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = w16_meta_select_palette(w, hdc, hpal) ? hpal : w16_gdi_select_palette(w, hdc, hpal);
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

// SetSysColors(cDspElements, lpnDspElements, lpdwRgbValues) -> void
// Windows: Changes system colours (COLOR_*) for every program and tells them all with
// WM_SYSCOLORCHANGE -- Control Panel's Color applying a scheme, which it writes to WIN.INI's [colors]
// itself.
// w16: The colours GetSysColor answers change for the program's run, and its windows hear
// WM_SYSCOLORCHANGE and are repainted; WIN.INI's [colors] is read at the next start.
// Why: w16 draws its own 3.1 frames, captions, menus and buttons in 3.1's default colours, not
// through the system colours.
// Notice: What the program paints itself in GetSysColor's colours follows a scheme; the frames,
// captions, menus and buttons w16 draws stay in Windows Default.
// Whole: w16's frames and controls drawn in the system colours, so a scheme recolours all of them.
static int api_SetSysColors(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int n = (int16_t)w16_arg16(a);
    uint32_t idx = w16_arg32(a), rgb = w16_arg32(a);
    const uint8_t *pi = n > 0 ? cpu16_ptr(&w->cpu, (uint16_t)(idx >> 16), (uint16_t)idx, (unsigned)n * 2, 0) : 0;
    const uint8_t *pc = n > 0 ? cpu16_ptr(&w->cpu, (uint16_t)(rgb >> 16), (uint16_t)rgb, (unsigned)n * 4, 0) : 0;
    if (n > 0 && (!pi || !pc)) return -1;
    for (int i = 0; i < n; i++) {
        unsigned k = pi[2 * i] | pi[2 * i + 1] << 8;
        uint32_t cr = (uint32_t)pc[4 * i] << 16 | (uint32_t)pc[4 * i + 1] << 8 | pc[4 * i + 2];   // 00BBGGRR
        w16_syscolor_set(k, cr);
    }
    if (n > 0 && w->win.used) {                  // the main window and its dialogs hear it, then repaint
        w16_send(w, w->win.hwnd, WM_SYSCOLORCHANGE, 0, 0);
        w16_invalidate(w, &w->win, 1);
        for (int i = 0; i < w->nwnds; i++)
            if (w->wnds[i] && w->wnds[i]->used && w->wnds[i]->kind == W16_K_DIALOG) {
                w16_send(w, w->wnds[i]->hwnd, WM_SYSCOLORCHANGE, 0, 0);
                if (w->wnds[i]) w16_invalidate(w, w->wnds[i], 1);
            }
    }
    return 10;
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

// wvsprintf(lpOutput, lpFormat, lpArglist) -> int
// Windows: wsprintf with its arguments in an array the caller points to -- a va_list -- not on the
// stack; the length of the result.
// w16: The same specifiers and flags as wsprintf.
static int api_wvsprintf(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t out = w16_arg32(a), fmtp = w16_arg32(a), args = w16_arg32(a);
    int r = format16(w, out, fmtp, args, 0);
    return r < 0 ? -1 : 12;
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

// There is no help viewer, so a request for help fails -- and HELP_QUIT, which a program sends as
// it closes (Minesweeper does, on WM_DESTROY), succeeds.
// One word of a line to hlpbroker, whose fields are tab-separated: no tab or newline in it.
static void help_field(char *out, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) out[n++] = *s == '\t' || *s == '\n' ? ' ' : *s;
    out[n] = 0;
}

// WINHELP.EXE, if drive C: has it: in WINDOWS, at the root, or beside the program. 1 and its host path.
static int find_winhelp(char *host, size_t cap) {
    static const char *const WHERE[] = { "C:\\WINDOWS\\WINHELP.EXE", "C:\\WINHELP.EXE", "WINHELP.EXE" };
    for (unsigned i = 0; i < sizeof WHERE / sizeof WHERE[0]; i++)
        if (w16_dos_path(WHERE[i], host, cap, 0, 0, 0) == 0) return 1;
    return 0;
}

// The context ID WINHELP.EXE is to be given to reach what the call names -- a HELP_CONTEXT number or a
// HELP_KEY keyword -- worked out from the file here, since WINHELP's command line takes an ID string and
// the file keeps only its hash. 1 and the ID, or 0 for "open it at its contents".
static int winhelp_id(const char *host, unsigned cmd, uint32_t data, const char *key, char *id, size_t cap) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 && n < (64L << 20) ? malloc((size_t)n) : 0;
    int ok = 0;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        struct uhlp *h = uhlp_open(buf, (size_t)n, 0);
        if (h) {
            int topic = -1;
            if (cmd == 1 || cmd == 8) topic = uhlp_topic_by_map(h, data);
            else if (key[0]) {
                for (int i = 0; i < uhlp_nkeywords(h) && topic < 0; i++) {
                    const char *k = uhlp_keyword(h, i);
                    size_t kl = strlen(key);
                    if (!strncasecmp(k, key, kl) && (cmd == 0x105 || !k[kl])) {
                        int cnt = 0;
                        const int *t = uhlp_keyword_topics(h, i, &cnt);
                        if (cnt > 0) topic = t[0];
                    }
                }
            }
            uint32_t hash = topic >= 0 ? uhlp_topic_hash(h, topic) : 0;
            ok = hash && uhlp_id_for_hash(hash, id, cap);
            uhlp_close(h);
        }
    }
    free(buf);
    fclose(f);
    return ok;
}

// WinHelp(hWnd, lpszHelp, uCommand, dwData) -> BOOL
// Windows: Opens the program's Help file (.HLP) in Windows Help, at its contents, a topic or a
// keyword; HELP_QUIT closes it.
// w16: Opens it in Help Viewer (/bin/wm/apps/hlpview), found as Windows finds a help file -- the
// program's folder, then drive C:'s WINDOWS -- at the contents, the topic a HELP_CONTEXT number
// names, a HELP_KEY keyword, or after a HELP_COMMAND macro. With the Help setting at WINHELP.EXE, and
// that program on drive C:, it runs Windows' own Help under w16 instead, at the contents or the topic
// a number or keyword names (a HELP_COMMAND macro is not passed). A second call closes the first
// window and opens the next. HELP_QUIT closes the program's Help window. w16 may not start a program
// itself, so the request goes to hlpbroker, which it started before its sandbox closed.
static int api_WinHelp(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t owner = w16_arg16(a);
    uint32_t file = w16_arg32(a);
    uint16_t cmd = w16_arg16(a);
    uint32_t data = w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    if (cmd == 2) {                                   // HELP_QUIT
        if (g_w16_help_fd >= 0 && write(g_w16_help_fd, "QUIT\n", 5) < 0) g_w16_help_fd = -1;
        return 12;
    }
    static char line[1400], tmp[300], word[400];
    char name[160] = "", host[256] = "";
    if (file >> 16) cpu16_str(&w->cpu, (uint16_t)(file >> 16), (uint16_t)file, name, sizeof name);
    if (!name[0] || w16_dos_path(name, host, sizeof host, 0, 0, 0) != 0) {
        char alt[200];
        const char *base = strrchr(name, '\\');
        snprintf(alt, sizeof alt, "C:\\WINDOWS\\%s", base ? base + 1 : name);
        if (!name[0] || w16_dos_path(alt, host, sizeof host, 0, 0, 0) != 0) {
            char text[220];
            snprintf(text, sizeof text, "Cannot find the help file %s.", name[0] ? name : "(none)");
            w16_message_box(w, owner, text, "Help", 0x30);   // MB_ICONEXCLAMATION
            w->cpu.r[R_AX] = 0;
            return 12;
        }
    }
    tmp[0] = 0;
    if ((cmd == 0x101 || cmd == 0x105 || cmd == 0x201 || cmd == 0x102) && (data >> 16))
        cpu16_str(&w->cpu, (uint16_t)(data >> 16), (uint16_t)data, tmp, sizeof tmp);
    int n = 0;
    char exe[256];
    const char *me = strrchr(w->path, '/');
    int is_winhelp = me && !strcasecmp(me + 1, "winhelp.exe");
    if (g_w16_prefs.help == W16_HELP_WINHELP && !is_winhelp && find_winhelp(exe, sizeof exe)) {
        char id[16];
        help_field(word, sizeof word, exe);
        n = snprintf(line, sizeof line, "WINHELP\t%s", word);
        if (cmd != 0x102 && winhelp_id(host, cmd, data, tmp, id, sizeof id))
            n += snprintf(line + n, sizeof line - (size_t)n, "\t-i%s", id);
    } else {
        n = snprintf(line, sizeof line, "OPEN");
        switch (cmd) {
        case 1: case 8:                               // HELP_CONTEXT, HELP_CONTEXTPOPUP
            n += snprintf(line + n, sizeof line - (size_t)n, "\t-c\t%lu", (unsigned long)data);
            break;
        case 0x101: case 0x105: case 0x201:           // HELP_KEY, HELP_PARTIALKEY, HELP_MULTIKEY
        case 0x102:                                   // HELP_COMMAND
            if (tmp[0]) {
                help_field(word, sizeof word, tmp);
                n += snprintf(line + n, sizeof line - (size_t)n, "\t%s\t%s", cmd == 0x102 ? "-m" : "-k", word);
            }
            break;
        default: break;                               // contents, index, help on help
        }
    }
    help_field(word, sizeof word, host);
    n += snprintf(line + n, sizeof line - (size_t)n, "\t%s\n", word);
    if (g_w16_help_fd < 0 || n >= (int)sizeof line || write(g_w16_help_fd, line, (size_t)n) < 0) {
        g_w16_help_fd = -1;
        w16_message_box(w, owner, "Help Viewer could not be started.", "Help", 0x30);
        w->cpu.r[R_AX] = 0;
    }
    return 12;
}

// IsRectEmpty(lprc) -> BOOL
// Windows: Whether a rectangle has no area: its right not past its left, or its bottom not below
// its top.
// w16: The same.
static int api_IsRectEmpty(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t r[4];
    if (get_rect(w, w16_arg32(a), r) < 0) return -1;
    w->cpu.r[R_AX] = r[2] <= r[0] || r[3] <= r[1];
    return 4;
}

// UnionRect(lprcDst, lprcSrc1, lprcSrc2) -> BOOL
// Windows: The smallest rectangle holding both -- an empty one left out; FALSE when both are empty.
// w16: The same.
static int api_UnionRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t r[4], s[4], o[4] = { 0, 0, 0, 0 };
    uint32_t dst = w16_arg32(a), pa = w16_arg32(a), pb = w16_arg32(a);
    if (get_rect(w, pa, r) < 0 || get_rect(w, pb, s) < 0) return -1;
    int re = r[2] <= r[0] || r[3] <= r[1], se = s[2] <= s[0] || s[3] <= s[1];
    if (re && !se) memcpy(o, s, sizeof o);
    else if (se && !re) memcpy(o, r, sizeof o);
    else if (!re) {
        o[0] = r[0] < s[0] ? r[0] : s[0]; o[1] = r[1] < s[1] ? r[1] : s[1];
        o[2] = r[2] > s[2] ? r[2] : s[2]; o[3] = r[3] > s[3] ? r[3] : s[3];
    }
    if (put_rect(w, dst, o) < 0) return -1;
    w->cpu.r[R_AX] = !(re && se);
    return 12;
}

// The windows EnumWindows and EnumChildWindows report, collected first:
// the program's function may create or destroy windows as it goes.
static int enum_call(struct w16 *w, const uint16_t *v, int n, uint32_t fn, uint32_t data) {
    int r = 1;
    for (int i = 0; i < n && r && !w->cpu.fault; i++) {
        if (!w16_wnd_get(w, v[i])) continue;
        const uint16_t words[] = { v[i], (uint16_t)(data >> 16), (uint16_t)data };
        r = (uint16_t)w16_call_far(w, (uint16_t)(fn >> 16), (uint16_t)fn, words, 3) != 0;
    }
    return r;
}

static void descendants(struct w16 *w, uint16_t parent, uint16_t **v, int *n, int *cap) {
    int k;
    struct w16_wnd **kids = w16_wnd_children(w, parent, &k);
    for (int i = 0; kids && i < k; i++) {
        if (*n == *cap) {
            int c = *cap ? *cap * 2 : 16;
            uint16_t *g = realloc(*v, sizeof *g * (size_t)c);
            if (!g) break;
            *v = g;
            *cap = c;
        }
        (*v)[(*n)++] = kids[i]->hwnd;
        descendants(w, kids[i]->hwnd, v, n, cap);
    }
    free(kids);
}

// EnumWindows(lpEnumFunc, lParam) -> BOOL
// EnumChildWindows(hWndParent, lpEnumFunc, lParam) -> BOOL
// Windows: Calls a function of the program's for each top-level window on the screen -- every
// program's -- or for each child (and grandchild) of a window, until it returns FALSE.
// w16: The same over this program's windows: its main window and its dialogs, or the children of
// one. Other programs' windows are not this program's to see (each runs in its own w16).
static int api_EnumWindows(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t parent = ordinal == 55 ? w16_arg16(a) : 0;
    uint32_t fn = w16_arg32(a), data = w16_arg32(a);
    uint16_t *v = 0;
    int n = 0, cap = 0;
    if (ordinal == 55) {
        descendants(w, parent, &v, &n, &cap);
    } else {
        cap = w->nwnds + 1;
        v = malloc(sizeof *v * (size_t)cap);
        if (v && w->win.used) v[n++] = w->win.hwnd;
        for (int i = 0; v && i < w->nwnds; i++)
            if (w->wnds[i] && w->wnds[i]->used && w->wnds[i]->kind == W16_K_DIALOG) v[n++] = w->wnds[i]->hwnd;
    }
    w->cpu.r[R_AX] = (uint16_t)enum_call(w, v, n, fn, data);
    free(v);
    return w->cpu.fault ? -1 : ordinal == 55 ? 10 : 8;
}

// GetClassName(hWnd, lpClassName, nMaxCount) -> int
// Windows: The name of a window's class, cut to the buffer; the characters copied.
// w16: The same: a program's class as it registered it, w16's own as Windows named them (Button,
// Edit, #32770 for a dialog...).
static int api_GetClassName(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    int16_t cap = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!x || x->cls < 0 || x->cls >= w->ncls || cap <= 0) return 8;
    const char *name = w->cls[x->cls].name;
    size_t n = strlen(name);
    if (n > (size_t)cap - 1) n = (size_t)cap - 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (uint32_t)n + 1, 1);
    if (!p) return -1;
    memcpy(p, name, n);
    p[n] = 0;
    w->cpu.r[R_AX] = (uint16_t)n;
    return 8;
}

// FlashWindow(hWnd, bInvert) -> BOOL
// Windows: Flips a window's caption between active and inactive once -- called on a timer, it
// flashes to ask for attention; returns whether it looked active before.
// w16: Answers as Windows would and draws no flash: toywm, not the program, says a window wants
// attention.
static int api_FlashWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)(w->active != 0);
    return 4;
}

// GetCaretBlinkTime() -> UINT
// Windows: How long the caret stays on, and off, in milliseconds -- the Control Panel's Cursor
// Blink Rate, 530 by default.
// w16: 530, the rate w16's own EDIT blinks at.
static int api_GetCaretBlinkTime(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    pthread_mutex_lock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)caret_blink(w);
    pthread_mutex_unlock(&w->lock);
    return 0;
}

// GetUpdateRect(hWnd, lpRect, bErase) -> BOOL
// Windows: The smallest rectangle round the part of a window waiting to be painted, and whether
// there is any; bErase erases its background first.
// w16: The same: w16 invalidates a window whole, so a window owing a paint answers its whole client
// area.
static int api_GetUpdateRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    w16_arg16(a);
    int owed = x && x->inval;
    int16_t r[4] = { 0, 0, (int16_t)(owed ? x->cw : 0), (int16_t)(owed ? x->ch : 0) };
    if ((far >> 16) && put_rect(w, far, r) < 0) return -1;
    w->cpu.r[R_AX] = (uint16_t)owed;
    return 8;
}

// InSendMessage() -> BOOL
// Windows: Whether the message being handled was sent by another task, which waits for the reply.
// w16: FALSE: no other program can send this one a message.
static int api_InSendMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = 0;
    return 0;
}

// ScrollDC(hDC, dx, dy, lprcScroll, lprcClip, hrgnUpdate, lprcUpdate) -> BOOL
// Windows: Moves the pixels of a rectangle of a DC by dx, dy, inside a clip rectangle, and says what
// the move uncovered -- as a region, a rectangle or both -- for the program to paint.
// w16: The same.
static int api_ScrollDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t dx = (int16_t)w16_arg16(a), dy = (int16_t)w16_arg16(a);
    uint32_t ps = w16_arg32(a), pc = w16_arg32(a);
    uint16_t rgn = w16_arg16(a);
    uint32_t pu = w16_arg32(a);
    int16_t sr[4], cr[4], up[4];
    if ((ps >> 16) && get_rect(w, ps, sr) < 0) return -1;
    if ((pc >> 16) && get_rect(w, pc, cr) < 0) return -1;
    w->cpu.r[R_AX] = (uint16_t)w16_gdi_scroll(w, hdc, dx, dy, ps >> 16 ? sr : 0, pc >> 16 ? cr : 0, up);
    if (rgn) w16_gdi_region_set(w, rgn, up);
    if ((pu >> 16) && put_rect(w, pu, up) < 0) return -1;
    return 20;
}

// EqualRect(lprc1, lprc2) -> BOOL
// Windows: Whether two rectangles have the same four sides.
// w16: The same.
static int api_EqualRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t r[4], s[4];
    uint32_t p = w16_arg32(a), q = w16_arg32(a);
    if (get_rect(w, p, r) < 0 || get_rect(w, q, s) < 0) return -1;
    w->cpu.r[R_AX] = memcmp(r, s, sizeof r) == 0;
    return 8;
}

// IsZoomed(hWnd) -> BOOL
// Windows: Whether a window is maximized.
// w16: The program's main window is, when the compositor has it maximized; no other window is.
static int api_IsZoomed(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    pthread_mutex_lock(&w->lock);
    w->cpu.r[R_AX] = x && x->kind != W16_K_CHILD && x->maximized;
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// The windows beside `x`: its parent's children, or for a top-level window the program's own
// (the main window, then its dialogs). Allocated; the count in *n.
static uint16_t *siblings_of(struct w16 *w, const struct w16_wnd *x, int *n) {
    uint16_t *v = 0;
    int cap = 0;
    *n = 0;
    if (x->kind == W16_K_CHILD) {
        int k;
        struct w16_wnd **kids = w16_wnd_children(w, x->parent, &k);
        v = malloc(sizeof *v * (size_t)(k ? k : 1));
        for (int i = 0; v && kids && i < k; i++) v[(*n)++] = kids[i]->hwnd;
        return v;
    }
    cap = w->nwnds + 1;
    v = malloc(sizeof *v * (size_t)cap);
    if (v && w->win.used) v[(*n)++] = w->win.hwnd;
    for (int i = 0; v && i < w->nwnds; i++)
        if (w->wnds[i] && w->wnds[i]->used && w->wnds[i]->kind == W16_K_DIALOG) v[(*n)++] = w->wnds[i]->hwnd;
    return v;
}

// GetWindow(hWnd, uCmd) -> HWND
// GetNextWindow(hWnd, uFlag) -> HWND
// Windows: The window that stands in a relation to another: GW_HWNDFIRST, GW_HWNDLAST, GW_HWNDNEXT
// or GW_HWNDPREV among its siblings, GW_OWNER, GW_CHILD (its first child); NULL when there is none.
// GetNextWindow is the sibling pair of it.
// w16: The same over the program's own windows, in the order they were made.
static int api_GetWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), cmd = w16_arg16(a);
    struct w16_wnd *x = w16_wnd_get(w, h);
    w->cpu.r[R_AX] = 0;
    if (!x) return 4;
    if (cmd == 5) {                              // GW_CHILD
        int k;
        struct w16_wnd **kids = w16_wnd_children(w, h, &k);
        w->cpu.r[R_AX] = kids && k ? kids[0]->hwnd : 0;
        return 4;
    }
    if (cmd == 4) {                              // GW_OWNER
        w->cpu.r[R_AX] = x->kind == W16_K_DIALOG ? x->parent : 0;
        return 4;
    }
    int n;
    uint16_t *v = siblings_of(w, x, &n);
    int at = -1;
    for (int i = 0; v && i < n; i++) if (v[i] == h) at = i;
    if (v && at >= 0) {
        switch (cmd) {
        case 0: w->cpu.r[R_AX] = v[0]; break;
        case 1: w->cpu.r[R_AX] = v[n - 1]; break;
        case 2: w->cpu.r[R_AX] = at + 1 < n ? v[at + 1] : 0; break;
        case 3: w->cpu.r[R_AX] = at > 0 ? v[at - 1] : 0; break;
        default: break;
        }
    }
    free(v);
    return 4;
}

// SetMessageQueue(cMsg) -> BOOL
// Windows: Makes the program's message queue hold cMsg messages -- by default 8; a program that
// posts a burst asks for more.
// w16: Answers TRUE: the queue is the size it is.
static int api_SetMessageQueue(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w->cpu.r[R_AX] = 1;
    return 2;
}

// CallMsgFilter(lpMsg, nCode) -> BOOL
// Windows: Offers a message to the program's message-filter hook, which a modal loop of the
// program's own does; TRUE when the hook handled it.
// w16: FALSE: no WH_MSGFILTER hook is called (SetWindowsHook says which are).
static int api_CallMsgFilter(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 6;
}

// ScrollWindow(hWnd, XAmount, YAmount, lpRect, lpClipRect) -> void
// Windows: Moves the contents of a window's client area, or of a rectangle in it, and its child
// windows, by an amount; what is uncovered is invalid and repainted.
// w16: The window is repainted whole -- w16 paints a window whole, not its update region -- so a
// program draws what it would have drawn for the uncovered strip, and everything with it; the
// child windows are not moved.
// Why: w16 paints a window whole from its WM_PAINT, so there is no pixel move to make.
// Notice: A program that scrolls child windows this way finds them where they were.
// Whole: Invalid regions as rectangles, and the children moved with the pixels. On the roadmap: "w16:
// invalid regions as rectangles".
static int api_ScrollWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    w16_arg16(a);
    w16_arg16(a);
    w16_arg32(a);
    w16_arg32(a);
    struct w16_wnd *x = w16_wnd_get(w, h);
    if (x) w16_invalidate(w, x, 1);
    return 14;
}

// ShowScrollBar(hWnd, wBar, bShow) -> void
// Windows: Shows or hides a window's scroll bar, or both.
// w16: Nothing changes: a window's bars are the ones it was made with (WS_HSCROLL, WS_VSCROLL),
// and its client area is sized for them.
// Why: A window's client area is sized for its bars when it is made.
// Notice: A bar the program hides stays, and one it shows is missing if the window was made without it.
// Whole: The frame and client area recomputed when a bar comes or goes.
static int api_ShowScrollBar(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)w;
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    w16_arg16(a);
    return 6;
}

// GetClassWord(hWnd, nIndex) -> WORD
// SetClassWord(hWnd, nIndex, wNewWord) -> WORD
// Windows: Reads or sets a word of a window's CLASS -- its cursor, icon, background brush, style, or
// a word of the class's own extra bytes (cbClsExtra) -- for every window of the class; Set returns
// the old word.
// w16: The same. A cursor set this way is what the pointer takes over the class's windows from its
// next move -- Paintbrush's tools switch theirs so. The sizes and the atom are read only; a
// GCW_HMODULE, or an index past the extra bytes, answers 0.
static int api_ClassWord(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    int16_t index = (int16_t)w16_arg16(a);
    int set = ordinal == 130;
    uint16_t v = set ? w16_arg16(a) : 0, old = 0;
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *x = w16_wnd_get(w, h);
    struct w16_class *k = x && x->cls >= 0 && x->cls < w->ncls ? &w->cls[x->cls] : 0;
    uint16_t *word = 0;
    if (k) {
        switch (index) {
        case -10: word = &k->background; break;    // GCW_HBRBACKGROUND
        case -12: word = &k->cursor; break;        // GCW_HCURSOR
        case -14: word = &k->icon; break;          // GCW_HICON
        case -26: word = &k->style; break;         // GCW_STYLE
        case -18: old = k->wnd_extra; break;       // GCW_CBWNDEXTRA
        case -20: old = k->cls_extra; break;       // GCW_CBCLSEXTRA
        case -32: old = k->atom; break;            // GCW_ATOM
        default:
            if (index >= 0 && index + 2 <= k->cls_extra) {
                old = (uint16_t)(k->extra[index] | k->extra[index + 1] << 8);
                if (set) { k->extra[index] = (uint8_t)v; k->extra[index + 1] = (uint8_t)(v >> 8); }
            }
        }
        if (word) {
            old = *word;
            if (set) *word = v;
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (set && index == -12) w16_cursor_refresh(w, v);
    w->cpu.r[R_AX] = old;
    return set ? 6 : 4;
}

// The text of a dialog's control set from C, as SetDlgItemText would.
static void set_item_text(struct w16 *w, uint16_t dlg, uint16_t id, const char *t) {
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    if (!c) return;
    size_t n = strlen(t);
    uint16_t at = w16_sys_push(w, (unsigned)n + 1);
    if (w->cpu.fault) return;
    memcpy(w->sysmem + at, t, n + 1);
    w16_send(w, c->hwnd, WM_SETTEXT, 0, (uint32_t)w->sysdata << 16 | at);
    w16_sys_pop(w, at);
}

#define LB_RESETCONTENT (WM_USER + 5)
#define LB_GETCURSEL    (WM_USER + 9)

// DlgDirList(hDlg, lpPathSpec, nIDListBox, nIDStaticPath, wFileType) -> int
// Windows: Fills a dialog's list box with a folder's files that match a pattern -- with
// DDL_DIRECTORY its folders as [name], with DDL_DRIVES the drives as [-c-] -- after making the
// spec's folder the current one; the pattern alone is written back to lpPathSpec, and the static
// control gets the current folder. FALSE when the folder is not there.
// w16: The same on the drives the program sees, names in lower case as 3.1 listed them.
static int api_DlgDirList(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t list = w16_arg16(a), stat_id = w16_arg16(a), attr = w16_arg16(a);
    char spec[260] = "*.*";
    if ((far >> 16) && cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, spec, sizeof spec) < 0) return -1;
    // The folder part becomes the current folder: all of a spec naming one, else what is before
    // its pattern.
    const char *pattern = spec;
    int ok = 1;
    if (!strchr(spec, '*') && !strchr(spec, '?') && spec[0] && w16_dos_chdir(spec) == 0) {
        pattern = "*.*";
    } else {
        char dir[260];
        pattern = w16_dos_split(spec, dir, sizeof dir);
        ok = w16_dos_chdir(dir) == 0;
    }
    if (ok && (far >> 16)) {                     // the pattern back, as Windows left it
        size_t n = strlen(pattern) + 1;
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n, 1);
        if (!p) return -1;
        memmove(p, pattern, n);
    }
    struct w16_wnd *lb = ok && list ? w16_dlg_item(w, dlg, list) : 0;
    if (lb) {
        w16_send(w, lb->hwnd, LB_RESETCONTENT, 0, 0);
        char path[300];
        snprintf(path, sizeof path, "%s", pattern[0] ? pattern : "*.*");
        if (w16_listbox_dir(w, lb, attr, path) == -1 && w->cpu.fault) return -1;
    }
    if (ok && stat_id) {
        char low[160];
        size_t n = 0;
        for (const char *c = w16_dos_cwd(); *c && n < sizeof low - 1; c++) low[n++] = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
        low[n] = 0;
        set_item_text(w, dlg, stat_id, low);
    }
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 12;
}

// DlgDirSelect(hDlg, lpString, nIDListBox) -> BOOL
// Windows: The entry chosen in a DlgDirList list box, into lpString: a folder's name with a
// backslash after it and a drive as "c:" -- TRUE for either -- or a file's name, FALSE.
// w16: The same.
static int api_DlgDirSelect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t list = w16_arg16(a);
    struct w16_wnd *lb = w16_dlg_item(w, dlg, list);
    int sel = lb ? (int16_t)w16_send(w, lb->hwnd, LB_GETCURSEL, 0, 0) : -1;
    const char *t = sel >= 0 ? w16_listbox_text(lb, sel) : 0;
    char out[160] = "";
    int folder = 0;
    size_t n = t ? strlen(t) : 0;
    if (n == 5 && t[0] == '[' && t[1] == '-' && t[3] == '-') {
        snprintf(out, sizeof out, "%c:", t[2]);
        folder = 1;
    } else if (n >= 2 && t[0] == '[' && t[n - 1] == ']') {
        snprintf(out, sizeof out, "%.*s\\", (int)(n - 2), t + 1);
        folder = 1;
    } else if (t) {
        snprintf(out, sizeof out, "%s", t);
    }
    size_t len = strlen(out) + 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)len, 1);
    if (!p) return -1;
    memcpy(p, out, len);
    w->cpu.r[R_AX] = (uint16_t)folder;
    return 8;
}

// OpenDriver(lpDriverName, lpSectionName, lParam) -> HDRVR
// Windows: Loads an installable driver -- a DLL SYSTEM.INI's [drivers] names, the MIDI Mapper or an
// MCI device's -- and sends it DRV_LOAD, DRV_ENABLE and DRV_OPEN; its handle, or NULL.
// w16: NULL, as for a driver that would not load, so a program says so in its own words.
// Why: A driver is Windows' own code for hardware toy-os drives itself, and MMSYSTEM's sound is
// toy-os's.
// Notice: Control Panel's Drivers lists SYSTEM.INI's drivers and says "Cannot load the MIDI Mapper
// driver"; a program asking MCI for a CD or a MIDI device finds none.
// Whole: The MIDI Mapper and MCI's drivers loaded as w16 modules, playing through toy-os's sound.
static int api_OpenDriver(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = 0;
    return 12;
}

// CloseDriver(hDriver, lParam1, lParam2) -> LONG
// SendDriverMessage(hDriver, wMessage, lParam1, lParam2) -> LONG
// DefDriverProc(dwDriverIdentifier, hDriver, wMessage, lParam1, lParam2) -> LONG
// GetDriverModuleHandle(hDriver) -> HINSTANCE
// GetNextDriver(hDriver, dwFlags) -> HDRVR
// Windows: An open installable driver closed, sent a message, answered for by default, its module,
// and the next one open.
// w16: 0: OpenDriver opens none, so there is nothing to close, send to or step through.
static int api_Driver(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)a;
    static const struct { unsigned ordinal; int argbytes; } ARGS[] = {
        { 251, 12 }, { 253, 10 }, { 254, 2 }, { 255, 16 }, { 257, 6 },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof ARGS / sizeof ARGS[0]; i++)
        if (ARGS[i].ordinal == ordinal) n = ARGS[i].argbytes;
    w->cpu.r[R_AX] = 0;
    w->cpu.r[R_DX] = 0;
    return n;
}

// EnumTaskWindows(hTask, lpEnumFunc, lParam) -> BOOL
// Windows: Calls a function for each top-level window a task owns, until it returns FALSE.
// w16: The same for this program's windows -- all of them are its own task's.
static int api_EnumTaskWindows(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t fn = w16_arg32(a), data = w16_arg32(a);
    int cap = w->nwnds + 1, n = 0;
    uint16_t *v = malloc(sizeof *v * (size_t)cap);
    if (v && w->win.used) v[n++] = w->win.hwnd;
    for (int i = 0; v && i < w->nwnds; i++)
        if (w->wnds[i] && w->wnds[i]->used && w->wnds[i]->kind == W16_K_DIALOG) v[n++] = w->wnds[i]->hwnd;
    w->cpu.r[R_AX] = (uint16_t)enum_call(w, v, n, fn, data);
    free(v);
    return w->cpu.fault ? -1 : 10;
}

// TrackPopupMenu(hMenu, wFlags, x, y, nReserved, hWnd, lpRect) -> BOOL
// Windows: Shows a popup menu at a point of the screen and lets the user choose; the choice is
// sent to the window as WM_COMMAND.
// w16: Shows nothing and answers FALSE: a popup that is not hung on a menu bar is not drawn yet.
// Why: Only a menu bar's popups are drawn; a popup at a point needs its own grab, keys and commit.
// Notice: A program's context menu never appears.
// Whole: A popup menu at a point, chosen by pointer or key, its choice sent as WM_COMMAND. On the
// roadmap: "w16: popup menus at a point".
static int api_TrackPopupMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    for (int i = 0; i < 5; i++) w16_arg16(a);
    w16_arg16(a);
    w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 16;
}

// AnsiUpperBuff(lpString, nLength) -> UINT
// Windows: Capitals for the first nLength characters of a buffer, in place (0 meaning 65536);
// returns how many it did.
// w16: The same, accented letters too.
static int api_AnsiUpperBuff(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    unsigned n = w16_arg16(a);
    if (!n) n = 65536;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, 1);
    if (!p) return -1;
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)fold(p[i]);
    w->cpu.r[R_AX] = (uint16_t)n;
    return 6;
}

// LoadIconHandler(hResData, bNew) -> HICON
// Windows: An undocumented call -- an icon from resource data already loaded.
// w16: Answers 0, no icon: a program that asks goes on without one.
// Why: It is undocumented and nothing seen so far has needed the icon.
// Notice: The program has no icon from it.
// Whole: An icon made from resource data already loaded.
static int api_LoadIconHandler(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 4;
}

// DrawFocusRect(hdc, lprc) -> void
// Windows: Draws, or erases, the dotted rectangle that marks the control with the keyboard; it is
// an XOR, so a second call with the same rectangle removes it.
// w16: The same: every second pixel of the border inverted.
static int api_DrawFocusRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int16_t r[4];
    if (get_rect(w, far, r) < 0) return -1;
    w16_gdi_focus_rect(w, hdc, r[0], r[1], r[2], r[3]);
    return 6;
}

// RealizePalette(hdc) -> UINT
// Windows: Maps the DC's logical palette into the device's hardware palette; returns how many
// entries changed.
// w16: Changes nothing and answers 0: colours are always the static VGA ones.
static int api_RealizePalette(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_meta_realize_palette(w16_arg16(a));
    w->cpu.r[R_AX] = 0;
    return 2;
}

// ClipCursor(lpRect) -> void
// Windows: Keeps the pointer inside a rectangle of the screen, or lets it go again with NULL.
// w16: Takes the rectangle and does nothing: the pointer is never confined.
// Why: toywm owns the pointer, and one program may not hold the desktop's.
// Notice: A drag that Windows would stop at the rectangle's edge goes on past it.
// Whole: Holding the pointer inside the program's own window while it has the capture.
static int api_ClipCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)w;
    (void)ordinal;
    w16_arg32(a);
    return 4;
}

// --- the caret -------------------------------------------------------------------------
//
// One per program, as Windows had one per system: drawn by the main thread over the window's last
// frame (w16_caret_draw), inverted as DSTINVERT inverts -- a palette index's complement -- so a black
// bar shows on white and white on black; a bitmap caret is XORed (SRCINVERT). Each change asks for the
// window to be drawn again.

static void caret_changed(struct w16 *w) {
    w->caret.since = w16_now_ms();
    w16_task_post(w, W16_POST_PAINT);
}

// CreateCaret(hwnd, hBitmap, nWidth, nHeight) -> void
// Windows: A new caret for a window -- the old one, whoever's, destroyed -- hidden until ShowCaret:
// a solid block (hBitmap NULL), a grey one (1) or a bitmap XORed over the window, nWidth by nHeight
// pixels (0: the window border's width or height; a bitmap: its own size).
// w16: The same.
static int api_CreateCaret(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), bm = w16_arg16(a);
    int cw = (int16_t)w16_arg16(a), ch = (int16_t)w16_arg16(a);
    uint32_t *bits = bm > 1 ? w16_gdi_bitmap_copy(w, bm, &cw, &ch) : 0;
    pthread_mutex_lock(&w->lock);
    free(w->caret.bits);
    w->caret.bits = bits;
    w->caret.hwnd = w16_wnd_get(w, hwnd) ? hwnd : 0;
    w->caret.w = cw > 0 ? cw : 1;                // SM_CXBORDER, SM_CYBORDER
    w->caret.h = ch > 0 ? ch : 1;
    w->caret.grey = bm == 1;
    w->caret.hidden = 1;
    pthread_mutex_unlock(&w->lock);
    caret_changed(w);
    return 8;
}

// DestroyCaret() -> void
// SetCaretPos(X, Y) -> void
// HideCaret(hwnd) -> void
// ShowCaret(hwnd) -> void
// Windows: The caret removed, moved (client pixels of its window), hidden and shown -- a count, each
// Hide wanting a Show, and only for the caret's own window or NULL -- blinking at GetCaretBlinkTime's
// rate while shown.
// w16: The same.
static int api_Caret(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t hwnd = 0;
    int x = 0, y = 0, ret = 0;
    if (ordinal == 165) { x = (int16_t)w16_arg16(a); y = (int16_t)w16_arg16(a); ret = 4; }
    else if (ordinal != 164) { hwnd = w16_arg16(a); ret = 2; }
    pthread_mutex_lock(&w->lock);
    int had = w->caret.hwnd != 0, mine = had && (!hwnd || hwnd == w->caret.hwnd);
    switch (ordinal) {
    case 164: w->caret.hwnd = 0; free(w->caret.bits); w->caret.bits = 0; break;  // DestroyCaret
    case 165: w->caret.x = x; w->caret.y = y; break;                             // SetCaretPos
    case 166: if (mine) w->caret.hidden++; break;                                // HideCaret
    default:  if (mine && w->caret.hidden > 0) w->caret.hidden--; break;         // 167 ShowCaret
    }
    pthread_mutex_unlock(&w->lock);
    if (had) caret_changed(w);
    return ret;
}

// GetCaretPos(lpPoint) -> void
// Windows: Where the caret is, in its window's client pixels.
// w16: The same.
static int api_GetCaretPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t p = w16_arg32(a);
    int16_t pt[2];
    pthread_mutex_lock(&w->lock);
    pt[0] = (int16_t)w->caret.x;
    pt[1] = (int16_t)w->caret.y;
    pthread_mutex_unlock(&w->lock);
    uint8_t *q = cpu16_ptr(&w->cpu, (uint16_t)(p >> 16), (uint16_t)p, 4, 1);
    if (!q) return -1;
    memcpy(q, pt, 4);
    return 4;
}

// SetCaretBlinkTime(uMSeconds) -> void
// Windows: How long the caret stays on, and off -- for every program, as the Control Panel sets it.
// w16: The same, for this program's caret alone.
static int api_SetCaretBlinkTime(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    unsigned ms = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    if (ms) w->caret.blink_ms = ms;
    pthread_mutex_unlock(&w->lock);
    return 2;
}

static unsigned caret_blink(const struct w16 *w) { return w->caret.blink_ms ? w->caret.blink_ms : 530; }

int w16_caret_phase(struct w16 *w, struct w16_wnd *top) {
    struct w16_wnd *x = w->caret.hwnd && !w->caret.hidden ? w16_wnd_get(w, w->caret.hwnd) : 0;
    if (!x || !x->visible || w16_wnd_top(w, x) != top) return -1;
    return (int)((w16_now_ms() - w->caret.since) / caret_blink(w) % 2 == 0);
}

void w16_caret_draw(struct w16 *w, struct ugfx_surface *s, struct w16_wnd *top, int cx, int cy) {
    if (w16_caret_phase(w, top) != 1) return;
    struct w16_wnd *x = w16_wnd_get(w, w->caret.hwnd);
    int ox = 0, oy = 0;
    if (x != top) w16_wnd_origin(w, x, &ox, &oy);
    // Cut to the window's client area, and to the top window's.
    int l = ox + w->caret.x, t = oy + w->caret.y, r = l + w->caret.w, b = t + w->caret.h;
    if (l < ox) l = ox;
    if (t < oy) t = oy;
    if (r > ox + x->cw) r = ox + x->cw;
    if (b > oy + x->ch) b = oy + x->ch;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > top->fbw) r = top->fbw;
    if (b > top->fbh) b = top->fbh;
    for (int yy = t; yy < b; yy++) {
        int py = cy + yy;
        if (py < 0 || py >= s->h) continue;
        uint32_t *row = s->pixels + (size_t)py * s->w;
        for (int xx = l; xx < r; xx++) {
            int px = cx + xx;
            if (px < 0 || px >= s->w || (w->caret.grey && (xx + yy) % 2)) continue;
            uint32_t d = row[px], *b = w->caret.bits;
            uint32_t v = b ? gdi_rop3(0x66, 0, b[(yy - oy - w->caret.y) * w->caret.w + xx - ox - w->caret.x], d)   // SRCINVERT
                           : gdi_rop3(0x55, 0, 0, d);                                                     // DSTINVERT
            row[px] = v | (d & 0xFF000000u);
        }
    }
}

// GlobalAddAtom(lpszString) -> ATOM
// GlobalDeleteAtom(nAtom) -> ATOM
// GlobalGetAtomName(nAtom, lpszBuffer, nSize) -> UINT
// Windows: AddAtom's family over the system-wide table, shared by every program.
// w16: AddAtom's own table: a program is the only one there is.
static int api_GlobalAtom(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_atom_call(w, ordinal == 268 ? 70 : ordinal == 269 ? 71 : 72, a);
}

// GetMessageExtraInfo() -> LONG
// Windows: The extra value the last message's source attached: a driver's own word with it.
// w16: 0: nothing attaches one.
static int api_GetMessageExtraInfo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = w->cpu.r[R_DX] = 0;
    return 0;
}

const struct w16_api g_user_misc_api[] = {
    { 10, api_SetTimer },
    { 12, api_KillTimer },
    { 13, api_GetTickCount },
    { 15, api_GetTickCount },
    { 16, api_ClipCursor },
    { 17, api_GetCursorPos },
    { 18, api_SetCapture },
    { 19, api_ReleaseCapture },
    { 22, api_SetFocus },
    { 23, api_GetFocus },
    { 28, api_ClientToScreen },
    { 29, api_ClientToScreen },
    { 30, api_WindowFromPoint },
    { 31, api_IsIconic },
    { 32, api_GetWindowRect },
    { 45, api_BringWindowToTop },
    { 47, api_IsWindow },
    { 49, api_IsWindow },
    { 50, api_FindWindow },
    { 54, api_EnumWindows },
    { 55, api_EnumWindows },
    { 58, api_GetClassName },
    { 59, api_SetActiveWindow },
    { 60, api_GetActiveWindow },
    { 61, api_ScrollWindow },
    { 67, api_GetWindowDC },
    { 69, api_SetCursor },
    { 70, api_SetCursorPos },
    { 71, api_ShowCursor },
    { 72, api_SetRect },
    { 73, api_SetRectEmpty },
    { 74, api_CopyRect },
    { 75, api_IsRectEmpty },
    { 76, api_PtInRect },
    { 77, api_OffsetRect },
    { 78, api_OffsetRect },
    { 79, api_IntersectRect },
    { 80, api_UnionRect },
    { 81, api_FillRect },
    { 82, api_InvertRect },
    { 83, api_FrameRect },
    { 84, api_DrawIcon },
    { 85, api_DrawText },
    { 99, api_DlgDirSelect },
    { 100, api_DlgDirList },
    { 104, api_MessageBeep },
    { 105, api_FlashWindow },
    { 119, api_GetMessagePos },
    { 120, api_GetMessageTime },
    { 121, api_SetWindowsHook },
    { 123, api_CallMsgFilter },
    { 129, api_ClassWord },
    { 130, api_ClassWord },
    { 163, api_CreateCaret },
    { 164, api_Caret },
    { 165, api_Caret },
    { 166, api_Caret },
    { 167, api_Caret },
    { 168, api_SetCaretBlinkTime },
    { 169, api_GetCaretBlinkTime },
    { 171, api_WinHelp },
    { 175, api_LoadBitmap },
    { 180, api_GetSysColor },
    { 181, api_SetSysColors },
    { 183, api_GetCaretPos },
    { 190, api_GetUpdateRect },
    { 192, api_InSendMessage },
    { 196, api_TabbedTextOut },
    { 221, api_ScrollDC },
    { 225, api_EnumTaskWindows },
    { 230, api_GetWindow },
    { 234, api_UnhookWindowsHook },
    { 235, api_DefHookProc },
    { 236, api_GetCapture },
    { 244, api_EqualRect },
    { 251, api_Driver },
    { 252, api_OpenDriver },
    { 253, api_Driver },
    { 254, api_Driver },
    { 255, api_Driver },
    { 257, api_Driver },
    { 262, api_GetWindow },
    { 266, api_SetMessageQueue },
    { 267, api_ShowScrollBar },
    { 268, api_GlobalAtom },
    { 269, api_GlobalAtom },
    { 271, api_GlobalAtom },
    { 272, api_IsZoomed },
    { 282, api_SelectPalette },
    { 283, api_RealizePalette },
    { 286, api_GetDesktopWindow },
    { 287, api_GetLastActivePopup },
    { 288, api_GetMessageExtraInfo },
    { 291, api_SetWindowsHookEx },
    { 292, api_UnhookWindowsHookEx },
    { 293, api_CallNextHookEx },
    { 416, api_TrackPopupMenu },
    { 420, api_wsprintf },
    { 421, api_wvsprintf },
    { 430, api_lstrcmp },
    { 431, api_AnsiUpper },
    { 432, api_AnsiUpper },
    { 437, api_AnsiUpperBuff },
    { 456, api_LoadIconHandler },
    { 466, api_DrawFocusRect },
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

// VkKeyScan(cChar) -> UINT
// Windows: Which key types a character, by the keyboard layout in use: the virtual-key code in the
// low byte, the shift state in the high (1 Shift, 2 Ctrl, 4 Alt); -1 when no key types it.
// w16: The same, by the US layout -- the one 3.1 shipped as its default. A program asks to show or
// match a key (Write's optional hyphen, Ctrl+Shift+-); the characters it is sent come from the
// layout toy-os uses.
static int api_VkKeyScan(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    unsigned ch = w16_arg16(a) & 0xFF;
    static const char punct[] = ";=,-./`[\\]'";
    static const char shifted[] = ":+<_>?~{|}\"";
    static const uint8_t vk[] = { 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE };
    static const char digits_shifted[] = ")!@#$%^&*(";
    unsigned r = 0xFFFF;
    const char *p;
    if (ch >= 'a' && ch <= 'z') r = ch - 32;
    else if (ch >= 'A' && ch <= 'Z') r = 0x100 | ch;
    else if (ch >= '0' && ch <= '9') r = ch;
    else if (ch == ' ' || ch == 8 || ch == 9 || ch == 13 || ch == 27) r = ch;
    else if (ch >= 1 && ch <= 26) r = 0x200 | (ch + 64);   // Ctrl+letter
    else if (ch && (p = strchr(digits_shifted, (int)ch))) r = 0x100 | (unsigned)('0' + (p - digits_shifted));
    else if (ch && (p = strchr(punct, (int)ch))) r = vk[p - punct];
    else if (ch && (p = strchr(shifted, (int)ch))) r = 0x100 | vk[p - shifted];
    w->cpu.r[R_AX] = (uint16_t)r;
    return 2;
}

// The US keyboard's set-1 scan code of each virtual key, VK_BACK (8) to VK_OEM_7 (0xDE); 0 for none.
static uint8_t vk_scan(unsigned vk) {
    static const char letters[] = "\x1e\x30\x2e\x20\x12\x21\x22\x23\x17\x24\x25\x26\x32\x31\x18\x19"
                                  "\x10\x13\x1f\x14\x16\x2f\x11\x2d\x15\x2c";            // A to Z
    static const struct { uint8_t vk, scan; } other[] = {
        { 0x08, 0x0E }, { 0x09, 0x0F }, { 0x0D, 0x1C }, { 0x10, 0x2A }, { 0x11, 0x1D }, { 0x12, 0x38 },
        { 0x14, 0x3A }, { 0x1B, 0x01 }, { 0x20, 0x39 }, { 0x21, 0x49 }, { 0x22, 0x51 }, { 0x23, 0x4F },
        { 0x24, 0x47 }, { 0x25, 0x4B }, { 0x26, 0x48 }, { 0x27, 0x4D }, { 0x28, 0x50 }, { 0x2D, 0x52 },
        { 0x2E, 0x53 }, { 0x60, 0x52 }, { 0x61, 0x4F }, { 0x62, 0x50 }, { 0x63, 0x51 }, { 0x64, 0x4B },
        { 0x65, 0x4C }, { 0x66, 0x4D }, { 0x67, 0x47 }, { 0x68, 0x48 }, { 0x69, 0x49 }, { 0x6A, 0x37 },
        { 0x6B, 0x4E }, { 0x6D, 0x4A }, { 0x6E, 0x53 }, { 0x6F, 0x35 }, { 0x7A, 0x57 }, { 0x7B, 0x58 },
        { 0xBA, 0x27 }, { 0xBB, 0x0D }, { 0xBC, 0x33 }, { 0xBD, 0x0C }, { 0xBE, 0x34 }, { 0xBF, 0x35 },
        { 0xC0, 0x29 }, { 0xDB, 0x1A }, { 0xDC, 0x2B }, { 0xDD, 0x1B }, { 0xDE, 0x28 },
    };
    if (vk >= 'A' && vk <= 'Z') return (uint8_t)letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return (uint8_t)(vk - '1' + 2);
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return (uint8_t)(vk - 0x70 + 0x3B);   // F1 to F10
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++)
        if (other[i].vk == vk) return other[i].scan;
    return 0;
}

// MapVirtualKey(wCode, wMapType) -> UINT
// Windows: Translates by the keyboard layout: type 0 a virtual key to its scan code, 1 a scan code to
// its virtual key, 2 a virtual key to the character it types unshifted (a letter in capitals); 0
// when there is none.
// w16: The same, by the US layout VkKeyScan uses. A scan code shared by two keys (the numeric pad's
// and the arrows) maps back to the one 3.1's table lists first.
static int api_MapVirtualKey(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    unsigned code = w16_arg16(a), type = w16_arg16(a), r = 0;
    static const char punct[] = ";=,-./`[\\]'";
    static const uint8_t pvk[] = { 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE };
    if (type == 0) r = vk_scan(code);
    else if (type == 1) {
        for (unsigned vk = 8; vk <= 0xDE && !r; vk++) if (vk_scan(vk) == code && code) r = vk;
    } else if (type == 2) {
        if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ' || code == 8 ||
            code == 9 || code == 13 || code == 27) r = code;
        else if (code >= 0x60 && code <= 0x69) r = '0' + code - 0x60;
        for (unsigned i = 0; !r && i < sizeof pvk; i++) if (pvk[i] == code) r = (uint8_t)punct[i];
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    return 4;
}

const struct w16_api g_keyboard_api[] = {
    { 5, api_AnsiToOem },
    { 6, api_AnsiToOem },
    { 129, api_VkKeyScan },
    { 131, api_MapVirtualKey },
};
const unsigned g_keyboard_api_n = sizeof g_keyboard_api / sizeof g_keyboard_api[0];
const unsigned g_user_misc_api_n = sizeof g_user_misc_api / sizeof g_user_misc_api[0];

int w16_user_misc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_user_misc_api, sizeof g_user_misc_api / sizeof g_user_misc_api[0], w, ordinal, a);
}
