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
#include <string.h>
#include <time.h>

#define HMENU_BASE 0x7800
#define HACCEL     0x7F01
#define DESKTOP_HWND 0x2000

uint64_t w16_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

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

// The last mouse point in screen coordinates: the client origin added, as
// ClientToScreen adds it (w16 knows the frame, not the window's place).
void w16_cursor_screen(struct w16 *w, int16_t *x, int16_t *y) {
    int cx, cy, cw, ch;
    w16_client_rect(&w->win, &cx, &cy, &cw, &ch);
    *x = (int16_t)(w->mouse_x + cx);
    *y = (int16_t)(w->mouse_y + cy);
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

static uint16_t set_timer(struct w16 *w, uint16_t id, uint32_t ms, uint32_t proc) {
    pthread_mutex_lock(&w->lock);
    struct w16_timer *t = 0;
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id) t = &w->timer[i];
    for (int i = 0; !t && i < W16_TIMERS; i++)
        if (!w->timer[i].used) t = &w->timer[i];
    if (t) {
        *t = (struct w16_timer){ .id = id, .proc = proc, .interval_ms = ms ? ms : 1,
                                 .due_ms = w16_now_ms() + (ms ? ms : 1), .used = 1 };
    }
    pthread_mutex_unlock(&w->lock);
    return t ? (id ? id : 1) : 0;
}

void w16_timers_fire(struct w16 *w) {
    uint64_t now = w16_now_ms();
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < W16_TIMERS; i++) {
        struct w16_timer *t = &w->timer[i];
        if (!t->used || t->pending || now < t->due_ms) continue;
        unsigned next = (w->qtail + 1) % W16_QUEUE;
        if (next == w->qhead) break;
        w->q[w->qtail] = (struct w16_msg){ w->win.hwnd, WM_TIMER, t->id, t->proc };
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
    w16_arg16(a);
    uint16_t id = w16_arg16(a), ms = w16_arg16(a);
    c->r[R_AX] = set_timer(w, id, ms, w16_arg32(a));
    return 10;
}

// KillTimer(hwnd, id)
static int api_KillTimer(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint16_t id = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    int found = 0;
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id) { w->timer[i].used = 0; found = 1; }
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

// SetCapture(hwnd) -> the previous
static int api_SetCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = w->capture;
    w->capture = w16_arg16(a);
    return 2;
}

// ReleaseCapture
static int api_ReleaseCapture(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->capture = 0;
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

// SetFocus, SetActiveWindow(hwnd) -> the previous
static int api_SetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w->win.hwnd;
    return 2;
}

// GetFocus, GetActiveWindow
static int api_GetFocus(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w->win.used ? w->win.hwnd : 0;
    return 0;
}

// ClientToScreen, ScreenToClient(hwnd, lpPoint)
static int api_ClientToScreen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint32_t p;
    // The window's place on the screen is the compositor's; w16 knows
    // only the frame, so screen coordinates are the window's own.
    w16_arg16(a);
    p = w16_arg32(a);
    int cx, cy, cw, ch;
    w16_client_rect(&w->win, &cx, &cy, &cw, &ch);
    if (point_offset(w, p, ordinal == 28 ? cx : -cx, ordinal == 28 ? cy : -cy) < 0) return -1;
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

// GetWindowRect(hwnd, lpRect)
static int api_GetWindowRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    int16_t wr[4] = { 0, 0, (int16_t)w->win.w, (int16_t)w->win.h };
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
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = w->win.used && h == w->win.hwnd && (ordinal == 47 || w->win.visible);
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

// SetCursor(h) -> the previous
static int api_SetCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
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

// MessageBeep(type): no sound yet
static int api_MessageBeep(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    w16_arg16(a);
    return 2;
}

// LoadMenu(hInst, lpName): a handle naming the resource
static int api_LoadMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t p;
    (void)ordinal;
    w16_arg16(a);
    p = w16_arg32(a);
    char name[32] = "";
    uint16_t id = (p >> 16) ? 0 : (uint16_t)p;
    if ((p >> 16) && cpu16_str(c, (uint16_t)(p >> 16), (uint16_t)p, name, sizeof name) < 0) return -1;
    struct ne_resource res;
    int i = w16_res_find(w, NE_RT_MENU, 0, id, name[0] ? name : 0, &res);
    c->r[R_AX] = i < 0 ? 0 : (uint16_t)(HMENU_BASE + i);
    return 6;
}

// CheckMenuItem, EnableMenuItem -> the previous state
static int api_CheckMenuItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg16(a);
    c->r[R_AX] = 0;
    return 6;
}

// GetSystemMenu(hwnd, revert)
static int api_GetSystemMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg16(a);
    c->r[R_AX] = HMENU_BASE + 0x7F;
    return 4;
}

// GetMenu(hwnd)
static int api_GetMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w->menu;
    return 2;
}

// SetMenu(hwnd, hmenu)
static int api_SetMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    w->menu = w16_arg16(a);
    c->r[R_AX] = 1;
    return 4;
}

// GetSubMenu(hmenu, pos)
static int api_GetSubMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = (uint16_t)(HMENU_BASE + 0x40 + w16_arg16(a));
    return 4;
}

// DrawMenuBar(hwnd)
static int api_DrawMenuBar(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    w16_arg16(a);
    return 2;
}

// LoadAccelerators(hInst, lpName)
static int api_LoadAccelerators(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg32(a);
    c->r[R_AX] = HACCEL;
    return 6;
}

// TranslateAccelerator(hwnd, hAccel, lpMsg): none yet
static int api_TranslateAccelerator(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg32(a);
    c->r[R_AX] = 0;
    return 8;
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
    c->r[R_AX] = DESKTOP_HWND;
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

static const struct w16_api g_user_misc_api[] = {
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
    { 59, api_SetFocus },
    { 60, api_GetFocus },
    { 69, api_SetCursor },
    { 71, api_ShowCursor },
    { 72, api_SetRect },
    { 73, api_SetRectEmpty },
    { 74, api_CopyRect },
    { 76, api_PtInRect },
    { 77, api_OffsetRect },
    { 78, api_OffsetRect },
    { 79, api_IntersectRect },
    { 81, api_FillRect },
    { 104, api_MessageBeep },
    { 150, api_LoadMenu },
    { 154, api_CheckMenuItem },
    { 155, api_CheckMenuItem },
    { 156, api_GetSystemMenu },
    { 157, api_GetMenu },
    { 158, api_SetMenu },
    { 159, api_GetSubMenu },
    { 160, api_DrawMenuBar },
    { 177, api_LoadAccelerators },
    { 178, api_TranslateAccelerator },
    { 180, api_GetSysColor },
    { 236, api_GetCapture },
    { 286, api_GetDesktopWindow },
    { 287, api_GetLastActivePopup },
    { 420, api_wsprintf },
};

int w16_user_misc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_user_misc_api, sizeof g_user_misc_api / sizeof g_user_misc_api[0], w, ordinal, a);
}
