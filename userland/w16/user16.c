// user16.c -- USER's exports: window classes, the window, the message
// loop and painting. Stage 2 holds ONE top-level window per program; a
// second, or a child window, ends the program with a fault that says so.
//
// GetMessage's order is Windows': posted messages first, then WM_QUIT,
// then a synthesized WM_PAINT while the window has an invalid region --
// so a program that never validates is asked to paint forever, exactly
// as on Windows, and BeginPaint/EndPaint (or DefWindowProc) is what stops
// it.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

#define W16_HWND        0x2001
#define W16_HDC         0x3001
#define W16_ATOM_BASE   0xC000
#define W16_DEFAULT_W   480
#define W16_DEFAULT_H   320

static const uint32_t g_syscolors[] = {
    0xC0C0C0, 0x008080, 0x000080, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000,
    0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0xFFFFFF, 0x000080, 0xFFFFFF, 0xC0C0C0,
    0x808080, 0x808080, 0x000000, 0x000000, 0xFFFFFF,
};

// COLOR_* (0..20) -> a ugfx colour; Windows' defaults.
uint32_t w16_syscolor(unsigned index) {
    uint32_t rgb = index < sizeof g_syscolors / sizeof g_syscolors[0] ? g_syscolors[index] : 0xFFFFFF;
    return ugfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static int put_bytes(struct w16 *w, uint32_t far, const void *src, unsigned len) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, len, 1);
    if (!p) return -1;
    memcpy(p, src, len);
    return 0;
}

static int get_bytes(struct w16 *w, uint32_t far, void *dst, unsigned len) {
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, len, 0);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}

// A class name: a string, or an atom in the offset with a null selector.
static int name_of(struct w16 *w, uint32_t far, char *out, unsigned cap) {
    if ((far >> 16) == 0) {
        snprintf(out, cap, "#%u", (unsigned)(far & 0xFFFF));
        return 0;
    }
    return cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, cap) < 0 ? -1 : 0;
}

static int find_class(struct w16 *w, const char *name) {
    for (int i = 0; i < w->ncls; i++) {
        const char *a = w->cls[i].name, *b = name;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    if (name[0] == '#') {
        unsigned atom = 0;
        for (const char *p = name + 1; *p; p++) atom = atom * 10 + (unsigned)(*p - '0');
        if (atom >= W16_ATOM_BASE && atom - W16_ATOM_BASE < (unsigned)w->ncls) return (int)(atom - W16_ATOM_BASE);
    }
    return -1;
}

static void fill_client(struct w16 *w, uint32_t colour) {
    struct w16_wnd *win = &w->win;
    pthread_mutex_lock(&w->lock);
    if (win->fb)
        for (int i = 0; i < win->cw * win->ch; i++) win->fb[i] = colour;
    pthread_mutex_unlock(&w->lock);
}

static uint32_t send(struct w16 *w, uint16_t msg, uint16_t wparam, uint32_t lparam) {
    const struct w16_class *k = &w->cls[w->win.cls];
    return w16_call_wndproc(w, k->proc_sel, k->proc_off, w->win.hwnd, msg, wparam, lparam);
}

static void set_result(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

static int register_class(struct w16 *w, struct w16_args *a) {
    uint32_t far = w16_arg32(a);
    uint8_t wc[26];
    if (get_bytes(w, far, wc, sizeof wc) < 0) return -1;
    if (w->ncls == W16_CLASSES) { w->cpu.r[R_AX] = 0; return 4; }
    struct w16_class *k = &w->cls[w->ncls];
    k->style = (uint16_t)(wc[0] | wc[1] << 8);
    k->proc_off = (uint16_t)(wc[2] | wc[3] << 8);
    k->proc_sel = (uint16_t)(wc[4] | wc[5] << 8);
    k->background = (uint16_t)(wc[16] | wc[17] << 8);
    uint32_t cname = (uint32_t)(wc[22] | wc[23] << 8) | (uint32_t)(wc[24] | wc[25] << 8) << 16;
    if (name_of(w, cname, k->name, sizeof k->name) < 0) return -1;
    if (find_class(w, k->name) >= 0) { w->cpu.r[R_AX] = 0; return 4; }
    w->cpu.r[R_AX] = (uint16_t)(W16_ATOM_BASE + w->ncls++);
    return 4;
}

static int create_window(struct w16 *w, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t cname = w16_arg32(a), title = w16_arg32(a), style = w16_arg32(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cx = (int16_t)w16_arg16(a), cy = (int16_t)w16_arg16(a);
    uint16_t parent = w16_arg16(a), menu = w16_arg16(a), inst = w16_arg16(a);
    uint32_t param = w16_arg32(a);
    (void)x; (void)y;
    char name[64];
    if (name_of(w, cname, name, sizeof name) < 0) return -1;
    int k = find_class(w, name);
    if (k < 0) { c->r[R_AX] = 0; return 30; }
    if (parent || w->win.used) {
        cpu16_set_fault(c, FAULT_API, parent ? "child windows are not supported yet"
                                             : "a second top-level window is not supported yet");
        return -1;
    }
    struct w16_wnd *win = &w->win;
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->hwnd = W16_HWND;
    win->cls = k;
    win->style = style;
    if ((title >> 16) && cpu16_str(c, (uint16_t)(title >> 16), (uint16_t)title, win->title,
                                   sizeof win->title) < 0)
        return -1;
    win->w = cx == W16_CW_USEDEFAULT || cx <= 0 ? W16_DEFAULT_W : cx;
    win->h = cx == W16_CW_USEDEFAULT || cy <= 0 ? W16_DEFAULT_H : cy;
    int cxp, cyp;
    w16_client_rect(win, &cxp, &cyp, &win->cw, &win->ch);
    win->fb = calloc((size_t)win->cw * win->ch + 1, 4);
    if (!win->fb) { cpu16_set_fault(c, FAULT_API, "out of memory for a window"); return -1; }
    fill_client(w, w16_brush_color(w->cls[k].background));

    pthread_mutex_lock(&w->lock);
    w->window_ready = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);

    // WM_CREATE, with a CREATESTRUCT the program may read.
    uint16_t at = w16_sys_alloc(w, 34);
    uint8_t cs[34] = { 0 };
    memcpy(cs, &param, 4);
    memcpy(cs + 4, &inst, 2);
    memcpy(cs + 6, &menu, 2);
    memcpy(cs + 10, &(uint16_t){ (uint16_t)win->h }, 2);
    memcpy(cs + 12, &(uint16_t){ (uint16_t)win->w }, 2);
    memcpy(cs + 18, &style, 4);
    memcpy(cs + 22, &title, 4);
    memcpy(cs + 26, &cname, 4);
    put_bytes(w, (uint32_t)w->sysdata << 16 | at, cs, sizeof cs);
    if (send(w, WM_CREATE, 0, (uint32_t)w->sysdata << 16 | at) == 0xFFFFFFFFu) {
        win->used = 0;
        c->r[R_AX] = 0;
        return 30;
    }
    send(w, WM_SIZE, 0, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    c->r[R_AX] = win->hwnd;
    return 30;
}

static void begin_paint(struct w16 *w, uint32_t lpps) {
    struct w16_wnd *win = &w->win;
    int erased = 0;
    if (win->erase) erased = send(w, WM_ERASEBKGND, W16_HDC, 0) != 0;
    uint8_t ps[32] = { 0 };
    uint16_t v[6] = { W16_HDC, (uint16_t)(win->erase && !erased), 0, 0, (uint16_t)win->cw, (uint16_t)win->ch };
    memcpy(ps, v, sizeof v);
    put_bytes(w, lpps, ps, sizeof ps);
    pthread_mutex_lock(&w->lock);
    win->inval = win->erase = 0;
    pthread_mutex_unlock(&w->lock);
    w16_gdi_reset(w);
}

static int get_message(struct w16 *w, struct w16_args *a) {
    uint32_t lpmsg = w16_arg32(a);
    w16_arg16(a); w16_arg16(a); w16_arg16(a);    // hwnd and the filter: one window
    struct w16_msg m;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        if (w->qhead != w->qtail) {
            m = w->q[w->qhead];
            w->qhead = (w->qhead + 1) % W16_QUEUE;
            break;
        }
        if (w->quit_posted) {
            m = (struct w16_msg){ 0, WM_QUIT, w->quit_code, 0 };
            break;
        }
        if (w->win.used && w->win.visible && w->win.inval) {
            m = (struct w16_msg){ w->win.hwnd, WM_PAINT, 0, 0 };
            break;
        }
        pthread_cond_wait(&w->cond, &w->lock);
    }
    pthread_mutex_unlock(&w->lock);
    uint8_t out[18] = { 0 };
    memcpy(out, &m.hwnd, 2);
    memcpy(out + 2, &m.message, 2);
    memcpy(out + 4, &m.wparam, 2);
    memcpy(out + 6, &m.lparam, 4);
    if (put_bytes(w, lpmsg, out, sizeof out) < 0) return -1;
    w->cpu.r[R_AX] = m.message != WM_QUIT;
    return 10;
}

static uint32_t def_window_proc(struct w16 *w, uint16_t msg, uint16_t wparam) {
    (void)wparam;
    switch (msg) {
    case WM_CLOSE:
        if (w->win.used) {
            send(w, WM_DESTROY, 0, 0);
            pthread_mutex_lock(&w->lock);
            w->win.visible = 0;
            pthread_mutex_unlock(&w->lock);
        }
        return 0;
    case WM_PAINT:
        begin_paint(w, (uint32_t)w->sysdata << 16 | 0x200);
        w16_task_post(w, W16_POST_PAINT);
        return 0;
    case WM_ERASEBKGND:
        fill_client(w, w16_brush_color(w->cls[w->win.cls].background));
        return 1;
    default:
        return 0;
    }
}

int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    switch (ordinal) {
    case 5:                                      // InitApp(hInstance)
        w16_arg16(a);
        c->r[R_AX] = 1;
        return 2;
    case 6:                                      // PostQuitMessage(code)
        pthread_mutex_lock(&w->lock);
        w->quit_posted = 1;
        w->quit_code = w16_arg16(a);
        pthread_cond_signal(&w->cond);
        pthread_mutex_unlock(&w->lock);
        return 2;
    case 33: {                                   // GetClientRect(hwnd, lpRect)
        w16_arg16(a);
        uint32_t rc = w16_arg32(a);
        uint16_t r[4] = { 0, 0, (uint16_t)w->win.cw, (uint16_t)w->win.ch };
        if (put_bytes(w, rc, r, sizeof r) < 0) return -1;
        return 6;
    }
    case 39: {                                   // BeginPaint(hwnd, lpPaint)
        w16_arg16(a);
        begin_paint(w, w16_arg32(a));
        c->r[R_AX] = W16_HDC;
        return 6;
    }
    case 40:                                     // EndPaint(hwnd, lpPaint)
        w16_arg16(a);
        w16_arg32(a);
        w16_task_post(w, W16_POST_PAINT);
        c->r[R_AX] = 1;
        return 6;
    case 41: return create_window(w, a);
    case 42: {                                   // ShowWindow(hwnd, nCmdShow)
        w16_arg16(a);
        int show = w16_arg16(a) != 0;
        pthread_mutex_lock(&w->lock);
        c->r[R_AX] = (uint16_t)w->win.visible;
        w->win.visible = show;
        if (show) w->win.inval = w->win.erase = 1;
        pthread_mutex_unlock(&w->lock);
        w16_task_post(w, W16_POST_PAINT);
        return 4;
    }
    case 53:                                     // DestroyWindow(hwnd)
        w16_arg16(a);
        def_window_proc(w, WM_CLOSE, 0);
        c->r[R_AX] = 1;
        return 2;
    case 57: return register_class(w, a);
    case 107: {                                  // DefWindowProc(hwnd, msg, wParam, lParam)
        w16_arg16(a);
        uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
        w16_arg32(a);
        set_result(c, def_window_proc(w, msg, wp));
        return 10;
    }
    case 108: return get_message(w, a);
    case 113:                                    // TranslateMessage: the main thread posts
        w16_arg32(a);                            // WM_CHAR itself (see w16.c's on_key)
        c->r[R_AX] = 0;
        return 4;
    case 114: {                                  // DispatchMessage(lpMsg)
        uint8_t m[10];
        if (get_bytes(w, w16_arg32(a), m, sizeof m) < 0) return -1;
        uint16_t hwnd = (uint16_t)(m[0] | m[1] << 8), msg = (uint16_t)(m[2] | m[3] << 8);
        uint16_t wp = (uint16_t)(m[4] | m[5] << 8);
        uint32_t lp = (uint32_t)(m[6] | m[7] << 8 | m[8] << 16 | (uint32_t)m[9] << 24);
        set_result(c, hwnd == w->win.hwnd && w->win.used ? send(w, msg, wp, lp) : 0);
        return 4;
    }
    case 124:                                    // UpdateWindow(hwnd)
        w16_arg16(a);
        if (w->win.used && w->win.visible && w->win.inval) send(w, WM_PAINT, 0, 0);
        return 2;
    case 125:                                    // InvalidateRect(hwnd, lpRect, bErase)
        w16_arg16(a);
        w16_arg32(a);
        {
            int erase = w16_arg16(a) != 0;
            pthread_mutex_lock(&w->lock);
            w->win.inval = 1;
            w->win.erase |= erase;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&w->lock);
        }
        return 8;
    case 173: case 174:                          // LoadCursor, LoadIcon: a handle to nothing yet
        w16_arg16(a);
        w16_arg32(a);
        c->r[R_AX] = (uint16_t)(0x4000 | ordinal);
        return 6;
    case 179: {                                  // GetSystemMetrics(index)
        int16_t i = (int16_t)w16_arg16(a);
        static const int16_t m[] = { 640, 480, 17, 17, 20, 1, 1, 18, 18, 18, 18, 32, 32,
                                     32, 32, 18, 640, 460 };
        c->r[R_AX] = (uint16_t)(i >= 0 && i < (int)(sizeof m / sizeof m[0]) ? m[i] : 0);
        return 2;
    }
    default:
        return -2;
    }
}
