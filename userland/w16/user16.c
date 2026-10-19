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
#include <time.h>
#include "ui/ugfx.h"

#define W16_HWND        0x2001
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

static void clear_timer_pending(struct w16 *w, uint16_t id) {
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id) w->timer[i].pending = 0;
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
    w16_frame_fit(win, &win->w, &win->h);
    int cxp, cyp;
    w16_client_rect(win, &cxp, &cyp, &win->cw, &win->ch);
    win->fb = calloc((size_t)win->cw * win->ch + 1, 4);
    if (!win->fb) { cpu16_set_fault(c, FAULT_API, "out of memory for a window"); return -1; }
    fill_client(w, w16_brush_px(w, w->cls[k].background));

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
    uint16_t hdc = w16_gdi_window_dc(w);
    if (win->erase) erased = send(w, WM_ERASEBKGND, hdc, 0) != 0;
    w16_gdi_window_dc(w);                        // WM_ERASEBKGND may have changed its state
    uint8_t ps[32] = { 0 };
    uint16_t v[6] = { hdc, (uint16_t)(win->erase && !erased), 0, 0, (uint16_t)win->cw, (uint16_t)win->ch };
    memcpy(ps, v, sizeof v);
    put_bytes(w, lpps, ps, sizeof ps);
    pthread_mutex_lock(&w->lock);
    win->inval = win->erase = 0;
    pthread_mutex_unlock(&w->lock);
}

// A Get/PeekMessage filter: a window (0 any) and a message range (0, 0
// all). A program's modal loop -- Minesweeper tracking a press on its
// face -- asks for the mouse messages alone, and the rest must wait for it.
struct msg_filter {
    uint16_t hwnd, min, max;
};

static int wanted(const struct msg_filter *f, uint16_t hwnd, uint16_t msg) {
    if (f->hwnd && hwnd && hwnd != f->hwnd) return 0;
    return (!f->min && !f->max) || (msg >= f->min && msg <= f->max);
}

// The next message the filter takes, Windows' order: posted ones, then
// WM_QUIT, then a WM_PAINT while the window is invalid. `remove` 0 leaves
// it queued (PM_NOREMOVE); `wait` 0 returns 0 when there is none. Before
// waiting, what the program drew is shown.
static int next_message(struct w16 *w, const struct msg_filter *f, int remove, int wait,
                        struct w16_msg *m) {
    pthread_mutex_lock(&w->lock);
    for (;;) {
        int found = 0;
        for (unsigned i = w->qhead; i != w->qtail; i = (i + 1) % W16_QUEUE) {
            if (!wanted(f, w->q[i].hwnd, w->q[i].message)) continue;
            *m = w->q[i];
            if (remove) {
                // Out of the middle of the ring: the ones after it close up.
                for (unsigned j = i; (j + 1) % W16_QUEUE != w->qtail; j = (j + 1) % W16_QUEUE)
                    w->q[j] = w->q[(j + 1) % W16_QUEUE];
                w->qtail = (w->qtail + W16_QUEUE - 1) % W16_QUEUE;
                if (m->message == WM_TIMER) clear_timer_pending(w, m->wparam);
            }
            found = 1;
            break;
        }
        if (found) break;
        if (w->quit_posted && wanted(f, 0, WM_QUIT)) {
            *m = (struct w16_msg){ 0, WM_QUIT, w->quit_code, 0 };
            break;
        }
        if (w->win.used && w->win.visible && w->win.inval && wanted(f, w->win.hwnd, WM_PAINT)) {
            *m = (struct w16_msg){ w->win.hwnd, WM_PAINT, 0, 0 };
            break;
        }
        if (w->fb_dirty) {
            pthread_mutex_unlock(&w->lock);
            w16_gdi_flush(w);
            pthread_mutex_lock(&w->lock);
            continue;
        }
        if (!wait) {
            pthread_mutex_unlock(&w->lock);
            return 0;
        }
        pthread_cond_wait(&w->cond, &w->lock);
    }
    pthread_mutex_unlock(&w->lock);
    return 1;
}

// A MSG, its `pt` the cursor in SCREEN coordinates -- where a program
// tracking a press compares it with a rectangle it ClientToScreen()ed.
static int put_msg(struct w16 *w, uint32_t lpmsg, const struct w16_msg *m) {
    uint8_t out[18] = { 0 };
    memcpy(out, &m->hwnd, 2);
    memcpy(out + 2, &m->message, 2);
    memcpy(out + 4, &m->wparam, 2);
    memcpy(out + 6, &m->lparam, 4);
    uint32_t t = (uint32_t)w16_now_ms();
    memcpy(out + 10, &t, 4);
    if (m->message >= WM_MOUSEMOVE && m->message <= 0x0209) {
        w->mouse_x = (int16_t)m->lparam;
        w->mouse_y = (int16_t)(m->lparam >> 16);
    }
    int16_t pt[2];
    w16_cursor_screen(w, &pt[0], &pt[1]);
    memcpy(out + 14, pt, 4);
    return put_bytes(w, lpmsg, out, sizeof out);
}

static int get_message(struct w16 *w, struct w16_args *a) {
    uint32_t lpmsg = w16_arg32(a);
    struct msg_filter f;
    f.hwnd = w16_arg16(a);
    f.min = w16_arg16(a);
    f.max = w16_arg16(a);
    struct w16_msg m;
    next_message(w, &f, 1, 1, &m);
    if (put_msg(w, lpmsg, &m) < 0) return -1;
    w->cpu.r[R_AX] = m.message != WM_QUIT;
    return 10;
}

// PeekMessage(lpMsg, hwnd, min, max, flags). A program polling with it
// would spin a core, so an empty queue costs the caller a millisecond --
// Windows yielded to other tasks there.
static int peek_message(struct w16 *w, struct w16_args *a) {
    uint32_t lpmsg = w16_arg32(a);
    struct msg_filter f;
    f.hwnd = w16_arg16(a);
    f.min = w16_arg16(a);
    f.max = w16_arg16(a);
    uint16_t flags = w16_arg16(a);
    struct w16_msg m;
    int got = next_message(w, &f, flags & 1, 0, &m);
    if (got && put_msg(w, lpmsg, &m) < 0) return -1;
    if (!got) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, 0);
    }
    w->cpu.r[R_AX] = (uint16_t)got;
    return 12;
}

static uint32_t def_window_proc(struct w16 *w, uint16_t msg, uint16_t wparam) {
    switch (msg) {
    case WM_SYSCOMMAND:
        // What the frame and the control menu asked, carried out only
        // when the program lets it through, as Windows' DefWindowProc does.
        switch (wparam & 0xFFF0) {
        case SC_CLOSE:
            send(w, WM_CLOSE, 0, 0);
            break;
        case SC_MINIMIZE: case SC_MAXIMIZE: case SC_RESTORE: case SC_KEYMENU:
            w16_task_post_arg(w, W16_POST_SYSCMD, wparam & 0xFFF0);
            break;
        default:
            break;
        }
        return 0;
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
        w16_gdi_flush(w);
        return 0;
    case WM_ERASEBKGND:
        w16_gdi_fill(w, wparam, 0, 0, w->win.cw, w->win.ch, w->cls[w->win.cls].background);
        return 1;
    default:
        return 0;
    }
}

// MoveWindow: the window takes the size now, on this thread, so the
// program's next GetClientRect sees it; the main thread then gives the
// toywm surface the same size. The position is the compositor's.
static int move_window(struct w16 *w, struct w16_args *a) {
    w16_arg16(a);
    w16_arg16(a); w16_arg16(a);
    int16_t nw = (int16_t)w16_arg16(a), nh = (int16_t)w16_arg16(a);
    w16_arg16(a);
    struct w16_wnd *win = &w->win;
    int fw = nw, fh = nh;
    w16_frame_fit(win, &fw, &fh);
    nw = (int16_t)fw;
    nh = (int16_t)fh;
    if (nw < 120 && !g_w16_desktop_frame) nw = 120;
    if (nh < 2 * W16_FRAME + W16_CAPTION + 1 && !g_w16_desktop_frame) nh = 2 * W16_FRAME + W16_CAPTION + 1;
    pthread_mutex_lock(&w->lock);
    int ok = !win->maximized;
    if (ok) {
        int cx, cy, cw, ch;
        struct w16_wnd probe = *win;
        probe.w = nw;
        probe.h = nh;
        w16_client_rect(&probe, &cx, &cy, &cw, &ch);
        uint32_t *fb = calloc((size_t)cw * ch + 1, 4);
        if (fb) {
            free(win->fb);
            win->fb = fb;
            win->w = nw; win->h = nh; win->cw = cw; win->ch = ch;
            win->inval = win->erase = 1;
        } else {
            ok = 0;
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (ok) {
        w16_task_post_arg(w, W16_POST_SIZE, 0);
        send(w, WM_SIZE, SIZE_RESTORED, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    }
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 12;
}

// InitApp(hInstance)
static int api_InitApp(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 1;
    return 2;
}

// PostQuitMessage(code)
static int api_PostQuitMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    pthread_mutex_lock(&w->lock);
    w->quit_posted = 1;
    w->quit_code = w16_arg16(a);
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// GetClientRect(hwnd, lpRect)
static int api_GetClientRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t rc = w16_arg32(a);
    uint16_t r[4] = { 0, 0, (uint16_t)w->win.cw, (uint16_t)w->win.ch };
    if (put_bytes(w, rc, r, sizeof r) < 0) return -1;
    return 6;
}

// BeginPaint(hwnd, lpPaint)
static int api_BeginPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    begin_paint(w, w16_arg32(a));
    c->r[R_AX] = w->window_dc;
    return 6;
}

// EndPaint(hwnd, lpPaint)
static int api_EndPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    w16_arg32(a);
    w16_gdi_flush(w);
    c->r[R_AX] = 1;
    return 6;
}

static int api_MoveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return move_window(w, a);
}

// GetDC(hwnd): the window's one DC
static int api_GetDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = w16_gdi_window_dc(w);
    return 2;
}

// ReleaseDC(hwnd, hdc)
static int api_ReleaseDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg16(a);
    w16_gdi_flush(w);
    c->r[R_AX] = 1;
    return 4;
}

static int api_PeekMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return peek_message(w, a);
}

// PostMessage(hwnd, msg, wParam, lParam)
static int api_PostMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    w16_post_msg(w, h, msg, wp, lp);
    c->r[R_AX] = 1;
    return 10;
}

// SendMessage(hwnd, msg, wParam, lParam)
static int api_SendMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    set_result(c, w->win.used && h == w->win.hwnd ? send(w, msg, wp, lp) : 0);
    return 10;
}

// ValidateRect(hwnd, lpRect): the whole window
static int api_ValidateRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg32(a);
    pthread_mutex_lock(&w->lock);
    w->win.inval = w->win.erase = 0;
    pthread_mutex_unlock(&w->lock);
    return 6;
}

// LoadString
static int api_LoadString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return w16_user_loadstring(w, a);
}

// CreateWindow
static int api_CreateWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return create_window(w, a);
}

// ShowWindow(hwnd, nCmdShow)
static int api_ShowWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint16_t cmd = w16_arg16(a);
    int show = cmd != 0;
    // SW_SHOWMINIMIZED, SW_MINIMIZE and SW_SHOWMINNOACTIVE; SW_MAXIMIZE;
    // SW_RESTORE -- toywm's to do, as the frame's buttons are.
    if (cmd == 2 || cmd == 6 || cmd == 7) w16_task_post_arg(w, W16_POST_SYSCMD, SC_MINIMIZE);
    else if (cmd == 3) w16_task_post_arg(w, W16_POST_SYSCMD, SC_MAXIMIZE);
    else if (cmd == 9) w16_task_post_arg(w, W16_POST_SYSCMD, SC_RESTORE);
    pthread_mutex_lock(&w->lock);
    c->r[R_AX] = (uint16_t)w->win.visible;
    w->win.visible = show;
    if (show) w->win.inval = w->win.erase = 1;
    pthread_mutex_unlock(&w->lock);
    w16_task_post(w, W16_POST_PAINT);
    return 4;
}

// DestroyWindow(hwnd)
static int api_DestroyWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    def_window_proc(w, WM_CLOSE, 0);
    c->r[R_AX] = 1;
    return 2;
}

// RegisterClass
static int api_RegisterClass(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return register_class(w, a);
}

// DefWindowProc(hwnd, msg, wParam, lParam)
static int api_DefWindowProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
    w16_arg32(a);
    set_result(c, def_window_proc(w, msg, wp));
    return 10;
}

static int api_GetMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return get_message(w, a);
}

// TranslateMessage: the main thread posts
static int api_TranslateMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg32(a);                            // WM_CHAR itself (see w16.c's on_key)
    c->r[R_AX] = 0;
    return 4;
}

// DispatchMessage(lpMsg)
static int api_DispatchMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint8_t m[10];
    if (get_bytes(w, w16_arg32(a), m, sizeof m) < 0) return -1;
    uint16_t hwnd = (uint16_t)(m[0] | m[1] << 8), msg = (uint16_t)(m[2] | m[3] << 8);
    uint16_t wp = (uint16_t)(m[4] | m[5] << 8);
    uint32_t lp = (uint32_t)(m[6] | m[7] << 8 | m[8] << 16 | (uint32_t)m[9] << 24);
    if (msg == WM_TIMER && lp) {             // a TIMERPROC: called, not the window procedure
        uint32_t t = (uint32_t)w16_now_ms();
        w16_call_wndproc(w, (uint16_t)(lp >> 16), (uint16_t)lp, hwnd, msg, wp, t);
        set_result(c, 0);
        return 4;
    }
    set_result(c, hwnd == w->win.hwnd && w->win.used ? send(w, msg, wp, lp) : 0);
    return 4;
}

// UpdateWindow(hwnd)
static int api_UpdateWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    if (w->win.used && w->win.visible && w->win.inval) send(w, WM_PAINT, 0, 0);
    return 2;
}

// InvalidateRect(hwnd, lpRect, bErase)
static int api_InvalidateRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
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
}

// LoadCursor, LoadIcon: a handle to nothing yet
static int api_LoadCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    w16_arg16(a);
    w16_arg32(a);
    c->r[R_AX] = (uint16_t)(0x4000 | ordinal);
    return 6;
}

// GetSystemMetrics(index)
static int api_GetSystemMetrics(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    int16_t i = (int16_t)w16_arg16(a);
    static const int16_t m[] = { 640, 480, 17, 17, 20, 1, 1, 18, 18, 18, 18, 32, 32,
                                 32, 32, 18, 640, 460 };
    c->r[R_AX] = (uint16_t)(i >= 0 && i < (int)(sizeof m / sizeof m[0]) ? m[i] : 0);
    return 2;
}

const struct w16_api g_user_api[] = {
    { 5, api_InitApp },
    { 6, api_PostQuitMessage },
    { 33, api_GetClientRect },
    { 39, api_BeginPaint },
    { 40, api_EndPaint },
    { 41, api_CreateWindow },
    { 42, api_ShowWindow },
    { 53, api_DestroyWindow },
    { 56, api_MoveWindow },
    { 57, api_RegisterClass },
    { 66, api_GetDC },
    { 68, api_ReleaseDC },
    { 107, api_DefWindowProc },
    { 108, api_GetMessage },
    { 109, api_PeekMessage },
    { 110, api_PostMessage },
    { 111, api_SendMessage },
    { 113, api_TranslateMessage },
    { 114, api_DispatchMessage },
    { 124, api_UpdateWindow },
    { 125, api_InvalidateRect },
    { 127, api_ValidateRect },
    { 173, api_LoadCursor },
    { 174, api_LoadCursor },
    { 176, api_LoadString },
    { 179, api_GetSystemMetrics },
};
const unsigned g_user_api_n = sizeof g_user_api / sizeof g_user_api[0];

int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_user_misc(w, ordinal, a);
    if (r != -2) return r;
    return w16_api_call(g_user_api, sizeof g_user_api / sizeof g_user_api[0], w, ordinal, a);
}
