// user16.c -- USER's exports: window classes and atoms, making windows,
// the message loop, sending and dispatching, and painting. The window
// tree itself is wnd16.c's; w16's own classes are ctl16.c's, edit16.c's
// and scroll16.c's; dialogs are dialog16.c's.
//
// A WINDOW'S PROCEDURE IS THE PROGRAM'S OR w16's. A program's class has a
// far pointer the CPU calls; a built-in class (BUTTON, EDIT...) has a C
// function, called directly -- Wine's builtin classes. SendMessage and
// DispatchMessage reach either through w16_send(), so a program cannot
// tell a C control from its own.
//
// GetMessage's order is Windows': posted messages first, then WM_QUIT,
// then a synthesized WM_PAINT while a window has an invalid region -- a
// parent before its children, so a child paints over what its parent
// erased -- so a program that never validates is asked to paint forever,
// exactly as on Windows, and BeginPaint/EndPaint (or DefWindowProc) is
// what stops it.
//
// ONE TOP-LEVEL WINDOW PER PROGRAM, beside its dialogs: CreateWindow of a
// second ends the program with a fault that says so.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "keyboard.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include "ui/ugfx.h"

#define W16_ATOM_BASE   0xC000
// CW_USEDEFAULT's size on the 640x480 screen GetSystemMetrics reports:
// what 3.11 gave Notepad there -- the width less the frame's inset, the
// height less the row of icons at the foot.
#define W16_DEFAULT_W   632
#define W16_DEFAULT_H   408
#define GWL_WNDPROC     (-4)
#define GWL_STYLE       (-16)
#define GWL_EXSTYLE     (-20)
#define GWW_HINSTANCE   (-6)
#define GWW_HWNDPARENT  (-8)
#define GWW_ID          (-12)
#define WA_INACTIVE     0
#define WA_ACTIVE       1
#define HTHSCROLL       6
#define HTVSCROLL       7

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

// A string the program passes, however long, as an allocation; "" for NULL.
static char *dup_far(struct w16 *w, uint32_t far) {
    if (!(far >> 16)) return strdup("");
    size_t cap = 256;
    for (;;) {
        char *s = malloc(cap);
        if (!s) return 0;
        int n = cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, (unsigned)cap);
        if (n < 0) { free(s); return 0; }
        if ((size_t)n < cap - 1 || cap >= 65536) return s;
        free(s);
        cap *= 4;
    }
}

static void set_result(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

// --- atoms and classes ----------------------------------------------------------

static int same_name(const char *a, const char *b) {
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
    return !*a && !*b;
}

uint16_t w16_atom_add(struct w16 *w, const char *name) {
    for (int i = 0; i < w->natoms; i++)
        if (same_name(w->atoms[i], name)) return (uint16_t)(W16_ATOM_BASE + i);
    if (w->natoms >= 0x3FFF) return 0;
    char **g = realloc(w->atoms, sizeof *g * (size_t)(w->natoms + 1));
    if (!g) return 0;
    w->atoms = g;
    if (!(w->atoms[w->natoms] = strdup(name))) return 0;
    return (uint16_t)(W16_ATOM_BASE + w->natoms++);
}

// A class name: a string, or an atom in the offset with a null selector.
static char *class_name(struct w16 *w, uint32_t far) {
    if ((far >> 16) == 0) {
        unsigned atom = far & 0xFFFF;
        if (atom >= W16_ATOM_BASE && atom - W16_ATOM_BASE < (unsigned)w->natoms)
            return strdup(w->atoms[atom - W16_ATOM_BASE]);
        char s[16];
        snprintf(s, sizeof s, "#%u", atom);
        return strdup(s);
    }
    return dup_far(w, far);
}

int w16_class_find(struct w16 *w, const char *name) {
    for (int i = 0; i < w->ncls; i++)
        if (same_name(w->cls[i].name, name)) return i;
    return -1;
}

static int class_add(struct w16 *w, const struct w16_class *k) {
    if (w->ncls == w->cls_cap) {
        int cap = w->cls_cap ? w->cls_cap * 2 : 16;
        pthread_mutex_lock(&w->lock);        // the main thread reads a class's style
        struct w16_class *g = realloc(w->cls, sizeof *g * (size_t)cap);
        if (g) { w->cls = g; w->cls_cap = cap; }
        pthread_mutex_unlock(&w->lock);
        if (!g) return -1;
    }
    w->cls[w->ncls] = *k;
    return w->ncls++;
}

int w16_class_add_host(struct w16 *w, const char *name, w16_wndproc host, uint16_t style,
                       uint16_t background) {
    struct w16_class k = { .name = strdup(name), .host = host, .style = style, .background = background };
    if (!k.name || !(k.atom = w16_atom_add(w, name))) { free(k.name); return -1; }
    int i = class_add(w, &k);
    if (i < 0) free(k.name);
    return i;
}

static int register_class(struct w16 *w, struct w16_args *a) {
    uint32_t far = w16_arg32(a);
    uint8_t wc[26];
    if (get_bytes(w, far, wc, sizeof wc) < 0) return -1;
    struct w16_class k = { 0 };
    k.style = (uint16_t)(wc[0] | wc[1] << 8);
    k.proc_off = (uint16_t)(wc[2] | wc[3] << 8);
    k.proc_sel = (uint16_t)(wc[4] | wc[5] << 8);
    k.wnd_extra = (uint16_t)(wc[8] | wc[9] << 8);
    k.cursor = (uint8_t)w16_cursor_shape((uint16_t)(wc[14] | wc[15] << 8));
    k.background = (uint16_t)(wc[16] | wc[17] << 8);
    uint32_t cname = (uint32_t)(wc[22] | wc[23] << 8) | (uint32_t)(wc[24] | wc[25] << 8) << 16;
    uint32_t mname = (uint32_t)(wc[18] | wc[19] << 8) | (uint32_t)(wc[20] | wc[21] << 8) << 16;
    if (mname >> 16) k.menu = dup_far(w, mname);
    else k.menu_id = (uint16_t)mname;
    k.name = class_name(w, cname);
    w->cpu.r[R_AX] = 0;
    if (!k.name) return -1;
    if (w16_class_find(w, k.name) >= 0 || !(k.atom = w16_atom_add(w, k.name)) || class_add(w, &k) < 0) {
        free(k.name);
        free(k.menu);
        return 4;
    }
    w->cpu.r[R_AX] = k.atom;
    return 4;
}

// --- sending ----------------------------------------------------------------------

uint32_t w16_send(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    if (!x || w->cpu.fault) return 0;
    if (x->host) return x->host(w, x, msg, wp, lp);
    return w16_call_wndproc(w, x->proc_sel, x->proc_off, hwnd, msg, wp, lp);
}

void w16_notify(struct w16 *w, struct w16_wnd *x, uint16_t code) {
    if (x->kind == W16_K_CHILD)
        w16_send(w, x->parent, WM_COMMAND, x->id, (uint32_t)code << 16 | x->hwnd);
}

// --- the main window ------------------------------------------------------------

static void fill_buffer(struct w16 *w, struct w16_wnd *win, uint32_t colour) {
    pthread_mutex_lock(&w->lock);
    if (win->fb)
        for (int i = 0; i < win->fbw * win->fbh; i++) win->fb[i] = colour;
    pthread_mutex_unlock(&w->lock);
}

static int create_main(struct w16 *w, int k, const char *title, uint32_t style, int cx, int cy,
                       uint16_t menu, uint16_t inst, uint32_t param, uint32_t far_title,
                       uint32_t far_class) {
    struct cpu16 *c = &w->cpu;
    struct w16_wnd *win = &w->win;
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->hwnd = W16_HWND;
    win->kind = W16_K_MAIN;
    win->cls = k;
    win->style = style;
    win->inst = inst;
    win->proc_sel = w->cls[k].proc_sel;
    win->proc_off = w->cls[k].proc_off;
    win->host = w->cls[k].host;
    win->sb[SB_HORZ] = win->sb[SB_VERT] = (struct w16_sbar){ 0, 100, 0 };
    win->text = strdup(title);
    win->extra = w->cls[k].wnd_extra ? calloc(1, w->cls[k].wnd_extra) : 0;
    win->w = cx == W16_CW_USEDEFAULT || cx <= 0 ? W16_DEFAULT_W : cx;
    win->h = cx == W16_CW_USEDEFAULT || cy <= 0 ? W16_DEFAULT_H : cy;
    w16_frame_fit(win, &win->w, &win->h);
    if (w16_top_fit(win) < 0 || !win->text || (w->cls[k].wnd_extra && !win->extra)) {
        cpu16_set_fault(c, FAULT_API, "out of memory for a window");
        return -1;
    }
    fill_buffer(w, win, w16_brush_px(w, w->cls[k].background));

    pthread_mutex_lock(&w->lock);
    w->window_ready = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);

    // WM_CREATE, with a CREATESTRUCT the program may read.
    uint16_t at = w16_sys_push(w, 34);
    uint8_t cs[34] = { 0 };
    uint16_t v16[] = { inst, menu, 0, (uint16_t)win->h, (uint16_t)win->w, 0, 0 };
    memcpy(cs, &param, 4);
    memcpy(cs + 4, v16, sizeof v16);
    memcpy(cs + 18, &style, 4);
    memcpy(cs + 22, &far_title, 4);
    memcpy(cs + 26, &far_class, 4);
    put_bytes(w, (uint32_t)w->sysdata << 16 | at, cs, sizeof cs);
    uint32_t lp = (uint32_t)w->sysdata << 16 | at;
    // Its menu bar: the one named, else its class's, as Windows gave one
    // (Notepad names its own in its class).
    uint16_t hm = menu ? menu : w->cls[k].menu || w->cls[k].menu_id
                                ? w16_load_menu(w, w->cls[k].menu, w->cls[k].menu_id) : 0;
    if (hm) w16_set_menu(w, hm);
    uint32_t r = w16_send(w, win->hwnd, WM_CREATE, 0, lp);
    w16_sys_pop(w, at);
    if (r == 0xFFFFFFFFu) {
        win->used = 0;
        return 0;
    }
    w16_send(w, win->hwnd, WM_SIZE, 0, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    return win->hwnd;
}

static int create_window(struct w16 *w, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t cname = w16_arg32(a), title = w16_arg32(a), style = w16_arg32(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cx = (int16_t)w16_arg16(a), cy = (int16_t)w16_arg16(a);
    uint16_t parent = w16_arg16(a), menu = w16_arg16(a), inst = w16_arg16(a);
    uint32_t param = w16_arg32(a);
    char *name = class_name(w, cname), *text = (title >> 16) ? dup_far(w, title) : strdup("");
    int k = name ? w16_class_find(w, name) : -1;
    free(name);
    c->r[R_AX] = 0;
    if (!text) return -1;
    if (k < 0) { free(text); return 30; }
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  CreateWindow %s style %08x at %d,%d size %d,%d parent %04x id %04x\n",
                w->cls[k].name, style, x, y, cx, cy, parent, menu);
        fflush(g_w16_trace);
    }
    if ((style & WS_CHILD) && parent) {
        struct w16_wnd *ch = w16_wnd_create(w, k, text, style, x, y, cx, cy, parent, menu, inst, param);
        free(text);
        c->r[R_AX] = ch ? ch->hwnd : 0;
        return c->fault ? -1 : 30;
    }
    if (w->win.used) {
        free(text);
        cpu16_set_fault(c, FAULT_API, (style & WS_CHILD) ? "a child window with no parent is not supported"
                                                         : "a second top-level window is not supported yet");
        return -1;
    }
    int h = create_main(w, k, text, style, cx, cy, menu, inst, param, title, cname);
    free(text);
    if (h < 0) return -1;
    c->r[R_AX] = (uint16_t)h;
    return 30;
}

// --- painting ---------------------------------------------------------------------

void w16_paint_begin(struct w16 *w, struct w16_wnd *x) {
    struct ugfx_surface s;
    int ox, oy;
    if ((x->kind == W16_K_CHILD || (x->style & (WS_VSCROLL | WS_HSCROLL))) && w16_wnd_paint_nc(w, x, &s, &ox, &oy)) {
        if (x->kind == W16_K_CHILD && (x->style & (WS_BORDER | WS_DLGFRAME)))
            ugfx_draw_rect(&s, ox, oy, x->w, x->h, w16_rgb(0x000000));
        w16_sbar_draw(w, x, &s, ox, oy);
        w16_wnd_paint_done(w);
    }
    pthread_mutex_lock(&w->lock);
    x->inval = x->erase = 0;
    pthread_mutex_unlock(&w->lock);
}

static void begin_paint(struct w16 *w, struct w16_wnd *x, uint32_t lpps) {
    int erase = x->erase, erased = 0;
    w16_paint_begin(w, x);
    uint16_t hdc = w16_gdi_window_dc(w, x->hwnd);
    if (erase) erased = w16_send(w, x->hwnd, WM_ERASEBKGND, hdc, 0) != 0;
    w16_gdi_window_dc(w, x->hwnd);               // WM_ERASEBKGND may have changed its state
    uint8_t ps[32] = { 0 };
    uint16_t v[6] = { hdc, (uint16_t)(erase && !erased), 0, 0, (uint16_t)x->cw, (uint16_t)x->ch };
    memcpy(ps, v, sizeof v);
    put_bytes(w, lpps, ps, sizeof ps);
}

// Every WM_PAINT owed, sent now, and the result shown: what a loop that
// reads only some messages (a scroll bar being tracked) calls so the
// window keeps up with it.
void w16_paint_now(struct w16 *w) {
    uint16_t last = 0;
    for (int guard = 0; guard < 1024 && !w->cpu.fault; guard++) {
        pthread_mutex_lock(&w->lock);
        struct w16_wnd *p = w16_wnd_next_paint(w);
        uint16_t h = p ? p->hwnd : 0;
        pthread_mutex_unlock(&w->lock);
        if (!h || h == last) break;              // one that never validates is asked once
        w16_send(w, h, WM_PAINT, 0, 0);
        last = h;
    }
    w16_gdi_flush(w);
}

// --- the queue ----------------------------------------------------------------------

// A Get/PeekMessage filter: a window (0 any; a window's children count as
// it) and a message range (0, 0 all). A program's modal loop --
// Minesweeper tracking a press on its face -- asks for the mouse messages
// alone, and the rest must wait for it.
struct msg_filter {
    uint16_t hwnd, min, max;
};

static int wanted(struct w16 *w, const struct msg_filter *f, uint16_t hwnd, uint16_t msg) {
    if (f->hwnd && hwnd && hwnd != f->hwnd && !w16_wnd_is_child(w, f->hwnd, hwnd)) return 0;
    return (!f->min && !f->max) || (msg >= f->min && msg <= f->max);
}

static void take(struct w16 *w, unsigned i) {
    // Out of the middle of the ring: the ones after it close up.
    for (unsigned j = i; (j + 1) % W16_QUEUE != w->qtail; j = (j + 1) % W16_QUEUE)
        w->q[j] = w->q[(j + 1) % W16_QUEUE];
    w->qtail = (w->qtail + W16_QUEUE - 1) % W16_QUEUE;
}

static void clear_timer_pending(struct w16 *w, uint16_t id) {
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].id == id) w->timer[i].pending = 0;
}

// w16's own requests, never a program's: handled between messages.
static void internal(struct w16 *w, const struct w16_msg *m) {
    if (m->message == W16_WM_MENUOPEN) {
        w16_menu_opening(w, m->wparam, (int)m->lparam);
        return;
    }
    if (m->message == W16_WM_ACTIVATE && w16_wnd_get(w, m->hwnd))
        w16_send(w, m->hwnd, WM_ACTIVATE, m->wparam ? WA_ACTIVE : WA_INACTIVE, 0);
}

// A short sleep with the lock let go, the caller looking again after it:
// toy-os's condition variable has no timed wait, and a tracking loop's
// repeat needs only a few milliseconds' accuracy.
static void timed_wait(struct w16 *w) {
    pthread_mutex_unlock(&w->lock);
    struct timespec ts = { 0, 2000000 };
    nanosleep(&ts, 0);
    pthread_mutex_lock(&w->lock);
}

// The next message the filter takes, Windows' order: posted ones, then
// WM_QUIT, then a WM_PAINT while a window is invalid. `remove` 0 leaves
// it queued (PM_NOREMOVE); `wait_ms` as w16_get_msg(). Before waiting,
// what the program drew is shown.
static int next_message(struct w16 *w, const struct msg_filter *f, int remove, int wait_ms,
                        struct w16_msg *m) {
    uint64_t deadline = wait_ms > 0 ? w16_now_ms() + (uint64_t)wait_ms : 0;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        int found = 0, again = 0;
        for (unsigned i = w->qhead; i != w->qtail; i = (i + 1) % W16_QUEUE) {
            if (w->q[i].message == W16_WM_MENUOPEN || w->q[i].message == W16_WM_ACTIVATE) {
                struct w16_msg req = w->q[i];
                take(w, i);
                pthread_mutex_unlock(&w->lock);
                internal(w, &req);
                pthread_mutex_lock(&w->lock);
                again = 1;
                break;
            }
            if (!wanted(w, f, w->q[i].hwnd, w->q[i].message)) continue;
            *m = w->q[i];
            if (remove) {
                take(w, i);
                if (m->message == WM_TIMER) clear_timer_pending(w, m->wparam);
            }
            found = 1;
            break;
        }
        if (again) continue;
        if (found) break;
        if (w->quit_posted && wanted(w, f, 0, WM_QUIT)) {
            *m = (struct w16_msg){ 0, WM_QUIT, w->quit_code, 0, 0 };
            break;
        }
        struct w16_wnd *p = w16_wnd_next_paint(w);
        if (p && wanted(w, f, p->hwnd, WM_PAINT)) {
            *m = (struct w16_msg){ p->hwnd, WM_PAINT, 0, 0, 0 };
            break;
        }
        if (w->fb_dirty) {
            pthread_mutex_unlock(&w->lock);
            w16_gdi_flush(w);
            pthread_mutex_lock(&w->lock);
            continue;
        }
        if (wait_ms == 0 || w->cpu.fault) {
            pthread_mutex_unlock(&w->lock);
            return 0;
        }
        if (wait_ms < 0) pthread_cond_wait(&w->cond, &w->lock);
        else if (w16_now_ms() >= deadline) { pthread_mutex_unlock(&w->lock); return 0; }
        else timed_wait(w);
    }
    if (m->message >= WM_KEYDOWN && m->message <= 0x0108) w->key_mods = m->mods;
    pthread_mutex_unlock(&w->lock);
    return 1;
}

int w16_get_msg(struct w16 *w, struct w16_msg *m, int wait_ms) {
    struct msg_filter f = { 0, 0, 0 };
    return next_message(w, &f, 1, wait_ms, m);
}

int w16_get_mouse_msg(struct w16 *w, uint16_t hwnd, struct w16_msg *m, int wait_ms) {
    struct msg_filter f = { hwnd, WM_MOUSEMOVE, WM_RBUTTONUP };
    return next_message(w, &f, 1, wait_ms, m);
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
        w->mouse_hwnd = m->hwnd;
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
    if (!next_message(w, &f, 1, -1, &m)) return -1;    // the program faulted meanwhile
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

// --- DefWindowProc ------------------------------------------------------------------

static void post_title(struct w16 *w, struct w16_wnd *x) {
    if (x->kind != W16_K_CHILD) w16_task_post_arg(w, W16_POST_TITLE, x->hwnd);
}

uint32_t w16_def_window_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    switch (msg) {
    case WM_NCCREATE:
        return 1;
    case WM_SYSCOMMAND:
        // What the frame and the control menu asked, carried out only
        // when the program lets it through, as Windows' DefWindowProc does.
        if (x != &w->win) return 0;
        switch (wp & 0xFFF0) {
        case SC_CLOSE:
            w16_send(w, x->hwnd, WM_CLOSE, 0, 0);
            break;
        case SC_MINIMIZE: case SC_MAXIMIZE: case SC_RESTORE: case SC_KEYMENU:
            w16_task_post_arg(w, W16_POST_SYSCMD, wp & 0xFFF0);
            break;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        w16_wnd_destroy(w, x);
        return 0;
    case WM_PAINT: {
        uint16_t at = w16_sys_push(w, 32);
        begin_paint(w, x, (uint32_t)w->sysdata << 16 | at);
        w16_sys_pop(w, at);
        return 0;
    }
    case WM_ERASEBKGND: {
        uint16_t bg = x->cls >= 0 && x->cls < w->ncls ? w->cls[x->cls].background : 0;
        w16_gdi_fill(w, wp, 0, 0, x->cw, x->ch, bg);
        return 1;
    }
    case WM_ACTIVATE:
        // Activated, the window takes the focus unless a child of it has it.
        if (wp != WA_INACTIVE && w->focus != x->hwnd && !w16_wnd_is_child(w, x->hwnd, w->focus))
            w16_set_focus(w, x->hwnd);
        return 0;
    case WM_SETFONT:                             // the built-in controls draw in it
        x->font = wp;
        if (lp) w16_invalidate(w, x, 1);
        return 0;
    case WM_GETFONT:
        return x->font;
    case WM_SETTEXT: {
        char *t = dup_far(w, lp);
        if (!t) return 0;
        pthread_mutex_lock(&w->lock);
        free(x->text);
        x->text = t;
        pthread_mutex_unlock(&w->lock);
        post_title(w, x);
        if (x->kind == W16_K_CHILD) w16_invalidate(w, x, 1);
        return 1;
    }
    case WM_GETTEXT: {
        const char *t = x->text ? x->text : "";
        unsigned n = (unsigned)strlen(t);
        if (!wp) return 0;
        if (n > wp - 1u) n = wp - 1u;
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, n + 1, 1);
        if (!p) return 0;
        memcpy(p, t, n);
        p[n] = 0;
        return n;
    }
    case WM_GETTEXTLENGTH:
        return x->text ? (uint32_t)strlen(x->text) : 0;
    case WM_NCLBUTTONDOWN:
        if (wp == HTVSCROLL || wp == HTHSCROLL)
            w16_sbar_track(w, x, wp == HTVSCROLL ? SB_VERT : SB_HORZ, (int16_t)lp, (int16_t)(lp >> 16));
        return 0;
    default:
        return 0;
    }
}

// --- moving and sizing ---------------------------------------------------------------

// The main window takes the size now, on this thread, so the program's
// next GetClientRect sees it; the main thread then gives the toywm
// surface the same size. The position is the compositor's.
static int move_main(struct w16 *w, int nw, int nh) {
    struct w16_wnd *win = &w->win;
    int fw = nw, fh = nh;
    w16_frame_fit(win, &fw, &fh);
    nw = fw;
    nh = fh;
    if (nw < 120 && !g_w16_desktop_frame) nw = 120;
    if (nh < 2 * W16_FRAME + W16_CAPTION + 1 && !g_w16_desktop_frame) nh = 2 * W16_FRAME + W16_CAPTION + 1;
    pthread_mutex_lock(&w->lock);
    int ok = !win->maximized;
    if (ok) {
        int ow = win->w, oh = win->h;
        win->w = nw;
        win->h = nh;
        if (w16_top_fit(win) < 0) { win->w = ow; win->h = oh; ok = 0; }
    }
    pthread_mutex_unlock(&w->lock);
    if (ok) {
        w16_invalidate(w, win, 1);
        w16_task_post_arg(w, W16_POST_SIZE, 0);
        w16_send(w, win->hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    }
    return ok;
}

// MoveWindow(hwnd, x, y, cx, cy, bRepaint)
static int move_window(struct w16 *w, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    int16_t nx = (int16_t)w16_arg16(a), ny = (int16_t)w16_arg16(a);
    int16_t nw = (int16_t)w16_arg16(a), nh = (int16_t)w16_arg16(a);
    w16_arg16(a);
    struct w16_wnd *x = w16_wnd_get(w, h);
    int ok = 0;
    if (x == &w->win) {
        ok = move_main(w, nw, nh);
    } else if (x && x->kind == W16_K_CHILD) {
        int moved = x->x != nx || x->y != ny, sized = x->w != nw || x->h != nh;
        w16_invalidate(w, w16_wnd_get(w, x->parent), 1);   // what it covered
        w16_wnd_set_geometry(w, x, nx, ny, nw, nh);
        w16_invalidate(w, x, 1);
        if (sized) w16_send(w, h, WM_SIZE, SIZE_RESTORED, (uint32_t)(uint16_t)x->ch << 16 | (uint16_t)x->cw);
        if (moved) w16_send(w, h, WM_MOVE, 0, (uint32_t)(uint16_t)x->y << 16 | (uint16_t)x->x);
        ok = 1;
    }
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 12;
}

// --- the exports ------------------------------------------------------------------------

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
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t rc = w16_arg32(a);
    uint16_t r[4] = { 0, 0, x ? (uint16_t)x->cw : 0, x ? (uint16_t)x->ch : 0 };
    if (put_bytes(w, rc, r, sizeof r) < 0) return -1;
    return 6;
}

// BeginPaint(hwnd, lpPaint)
static int api_BeginPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t lpps = w16_arg32(a);
    c->r[R_AX] = 0;
    if (x) {
        begin_paint(w, x, lpps);
        c->r[R_AX] = x->dc;
    }
    return 6;
}

// EndPaint(hwnd, lpPaint): what was drawn shows when the program next waits
static int api_EndPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    w16_arg32(a);
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
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = w16_gdi_window_dc(w, h ? h : w->win.hwnd);
    return 2;
}

// ReleaseDC(hwnd, hdc)
static int api_ReleaseDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a); w16_arg16(a);
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
    set_result(c, w16_send(w, h, msg, wp, lp));
    return c->fault ? -1 : 10;
}

// ValidateRect(hwnd, lpRect): the whole window
static int api_ValidateRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w16_arg32(a);
    if (x) {
        pthread_mutex_lock(&w->lock);
        x->inval = x->erase = 0;
        pthread_mutex_unlock(&w->lock);
    }
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

// ShowWindow(hwnd, nCmdShow) -> whether it was visible
static int api_ShowWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t cmd = w16_arg16(a);
    int show = cmd != 0;
    c->r[R_AX] = 0;
    if (!x) return 4;
    if (x == &w->win) {
        // SW_SHOWMINIMIZED, SW_MINIMIZE and SW_SHOWMINNOACTIVE; SW_MAXIMIZE;
        // SW_RESTORE -- toywm's to do, as the frame's buttons are.
        if (cmd == 2 || cmd == 6 || cmd == 7) w16_task_post_arg(w, W16_POST_SYSCMD, SC_MINIMIZE);
        else if (cmd == 3) w16_task_post_arg(w, W16_POST_SYSCMD, SC_MAXIMIZE);
        else if (cmd == 9) w16_task_post_arg(w, W16_POST_SYSCMD, SC_RESTORE);
    }
    pthread_mutex_lock(&w->lock);
    c->r[R_AX] = (uint16_t)x->visible;
    int was = x->visible;
    x->visible = show;
    if (show) x->style |= WS_VISIBLE;
    else x->style &= ~WS_VISIBLE;
    pthread_mutex_unlock(&w->lock);
    if (show) w16_invalidate(w, x, 1);
    else if (was && x->kind == W16_K_CHILD) w16_invalidate(w, w16_wnd_get(w, x->parent), 1);
    if (x == &w->win) w16_task_post(w, W16_POST_PAINT);
    return 4;
}

// DestroyWindow(hwnd)
static int api_DestroyWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    if (x) w16_wnd_destroy(w, x);
    c->r[R_AX] = x != 0;
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
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    set_result(c, x ? w16_def_window_proc(w, x, msg, wp, lp) : 0);
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

uint32_t w16_dispatch(struct w16 *w, const struct w16_msg *m) {
    if (m->message == WM_TIMER && m->lparam) {   // a TIMERPROC: called, not the window procedure
        uint32_t t = (uint32_t)w16_now_ms();
        w16_call_wndproc(w, (uint16_t)(m->lparam >> 16), (uint16_t)m->lparam, m->hwnd, m->message,
                         m->wparam, t);
        return 0;
    }
    return w16_send(w, m->hwnd, m->message, m->wparam, m->lparam);
}

// DispatchMessage(lpMsg)
static int api_DispatchMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint8_t m[10];
    if (get_bytes(w, w16_arg32(a), m, sizeof m) < 0) return -1;
    struct w16_msg msg = { (uint16_t)(m[0] | m[1] << 8), (uint16_t)(m[2] | m[3] << 8),
                           (uint16_t)(m[4] | m[5] << 8),
                           (uint32_t)(m[6] | m[7] << 8 | m[8] << 16 | (uint32_t)m[9] << 24), 0 };
    set_result(c, w16_dispatch(w, &msg));
    return 4;
}

// UpdateWindow(hwnd): its WM_PAINT now, then its children's
static void update(struct w16 *w, struct w16_wnd *x, int depth) {
    if (!x || depth > 32) return;
    uint16_t h = x->hwnd;
    if (x->inval && w16_wnd_shown(w, x)) w16_send(w, h, WM_PAINT, 0, 0);
    int n;
    struct w16_wnd **kids = w16_wnd_children(w, h, &n);
    for (int i = 0; kids && i < n; i++) update(w, kids[i], depth + 1);
    free(kids);
}

static int api_UpdateWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    update(w, w16_wnd_get(w, w16_arg16(a)), 0);
    return 2;
}

// InvalidateRect(hwnd, lpRect, bErase): the whole window
static int api_InvalidateRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w16_arg32(a);
    w16_invalidate(w, x, w16_arg16(a) != 0);
    return 8;
}

// A cursor handle names Windows' cursor: LoadCursor(NULL, IDC_*) is
// CURSOR_BASE plus the IDC's low byte (IDC_ARROW 32512 is 0x7F00); a
// program's own cursor resource draws as the arrow.
#define CURSOR_BASE 0x4000
#define CURSOR_OWN  (CURSOR_BASE | 0xFF)

int w16_cursor_shape(uint16_t h) {
    switch (h) {
    case CURSOR_BASE | 0x01: return WIN_CURSOR_TEXT;        // IDC_IBEAM
    case CURSOR_BASE | 0x02: return WIN_CURSOR_WAIT;        // IDC_WAIT
    case CURSOR_BASE | 0x03: return WIN_CURSOR_CROSSHAIR;   // IDC_CROSS
    case CURSOR_BASE | 0x80: case CURSOR_BASE | 0x86: return WIN_CURSOR_MOVE;   // IDC_SIZE, SIZEALL
    case CURSOR_BASE | 0x84: return WIN_CURSOR_RESIZE_H;    // IDC_SIZEWE
    case CURSOR_BASE | 0x85: return WIN_CURSOR_RESIZE_V;    // IDC_SIZENS
    default: return WIN_CURSOR_DEFAULT;
    }
}

// LoadCursor(hInst, lpName), LoadIcon: a handle to nothing yet
static int api_LoadCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint16_t inst = w16_arg16(a);
    uint32_t name = w16_arg32(a);
    if (ordinal == 174) c->r[R_AX] = (uint16_t)(0x4000 | ordinal);
    else if (!inst && !(name >> 16) && (name & 0xFF00) == 0x7F00) c->r[R_AX] = (uint16_t)(CURSOR_BASE | (name & 0xFF));
    else c->r[R_AX] = CURSOR_OWN;
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

// EnableWindow(hwnd, bEnable) -> whether it was disabled
static int api_EnableWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int enable = w16_arg16(a) != 0;
    w->cpu.r[R_AX] = 0;
    if (!x) return 4;
    pthread_mutex_lock(&w->lock);
    int was = (x->style & WS_DISABLED) != 0;
    if (enable) x->style &= ~WS_DISABLED;
    else x->style |= WS_DISABLED;
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)was;
    if (was == enable) {
        if (!enable && (w->focus == x->hwnd || w16_wnd_is_child(w, x->hwnd, w->focus))) w16_set_focus(w, 0);
        w16_send(w, x->hwnd, WM_ENABLE, (uint16_t)enable, 0);
        if (x->kind == W16_K_CHILD) w16_invalidate(w, x, 1);
    }
    return 4;
}

// IsWindowEnabled(hwnd)
static int api_IsWindowEnabled(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w->cpu.r[R_AX] = x && !(x->style & WS_DISABLED);
    return 2;
}

// GetWindowText(hwnd, lpString, nMaxCount): WM_GETTEXT
static int api_GetWindowText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t max = w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)w16_send(w, h, WM_GETTEXT, max, far);
    return w->cpu.fault ? -1 : 8;
}

// SetWindowText(hwnd, lpString): WM_SETTEXT
static int api_SetWindowText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    w16_send(w, h, WM_SETTEXT, 0, w16_arg32(a));
    return w->cpu.fault ? -1 : 6;
}

// GetWindowTextLength(hwnd)
static int api_GetWindowTextLength(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = (uint16_t)w16_send(w, w16_arg16(a), WM_GETTEXTLENGTH, 0, 0);
    return w->cpu.fault ? -1 : 2;
}

// Get/SetWindowWord and Get/SetWindowLong: a negative index is one of
// USER's own fields, a positive one the window's cbWndExtra bytes.
static uint32_t window_field(struct w16 *w, struct w16_wnd *x, int index, int bytes, int set, uint32_t v) {
    uint32_t old = 0;
    if (index >= 0) {
        unsigned extra = x->cls >= 0 && x->cls < w->ncls ? w->cls[x->cls].wnd_extra : 0;
        if (!x->extra || (unsigned)index + (unsigned)bytes > extra) return 0;
        memcpy(&old, x->extra + index, (size_t)bytes);
        if (set) memcpy(x->extra + index, &v, (size_t)bytes);
        return old;
    }
    switch (index) {
    case GWL_STYLE:
        old = x->style;
        if (set) {
            pthread_mutex_lock(&w->lock);
            x->style = v;
            pthread_mutex_unlock(&w->lock);
            if (x->kind == W16_K_CHILD) w16_wnd_set_geometry(w, x, x->x, x->y, x->w, x->h);
        }
        return old;
    case GWL_EXSTYLE:
        return 0;
    case GWL_WNDPROC:
        // A program subclassing w16's own control gets a pointer it can
        // never call back through (no CallWindowProc yet): refuse it.
        if (x->host) return 0;
        old = (uint32_t)x->proc_sel << 16 | x->proc_off;
        if (set) { x->proc_sel = (uint16_t)(v >> 16); x->proc_off = (uint16_t)v; }
        return old;
    case GWW_HINSTANCE:
        return x->inst;
    case GWW_HWNDPARENT:
        return x->parent;
    case GWW_ID:
        return x->id;
    default:
        return 0;
    }
}

static int api_GetWindowWord(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int16_t index = (int16_t)w16_arg16(a);
    int set = ordinal == 134 || ordinal == 136, lng = ordinal == 135 || ordinal == 136;
    uint32_t v = set ? (lng ? w16_arg32(a) : w16_arg16(a)) : 0;
    set_result(&w->cpu, x ? window_field(w, x, index, lng ? 4 : 2, set, v) : 0);
    if (!lng) w->cpu.r[R_DX] = 0;
    return 4 + (set ? (lng ? 4 : 2) : 0);
}

// RegisterWindowMessage(lpString): an atom, unique per string
static int api_RegisterWindowMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    char *s = dup_far(w, w16_arg32(a));
    if (!s) return -1;
    w->cpu.r[R_AX] = w16_atom_add(w, s);
    free(s);
    return 4;
}

// GetParent(hwnd)
static int api_GetParent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w->cpu.r[R_AX] = x ? x->parent : 0;
    return 2;
}

// IsChild(hwndParent, hwnd)
static int api_IsChild(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t p = w16_arg16(a), h = w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)w16_wnd_is_child(w, p, h);
    return 4;
}

// GetKeyState(vk): the Shift, Ctrl and Alt of the key being handled, as
// the high bit of the answer; any other key reads as up
static int api_GetKeyState(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t vk = w16_arg16(a);
    unsigned m = w->key_mods;
    int down = (vk == VK_SHIFT && (m & KEY_MOD_SHIFT)) || (vk == VK_CONTROL && (m & KEY_MOD_CTRL)) ||
               (vk == 0x12 && (m & KEY_MOD_ALT));
    w->cpu.r[R_AX] = down ? 0x8000 : 0;
    return 2;
}

// GetDlgCtrlID(hwnd)
static int api_GetDlgCtrlID(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w->cpu.r[R_AX] = x ? x->id : 0;
    return 2;
}

const struct w16_api g_user_api[] = {
    { 5, api_InitApp },
    { 6, api_PostQuitMessage },
    { 33, api_GetClientRect },
    { 34, api_EnableWindow },
    { 35, api_IsWindowEnabled },
    { 36, api_GetWindowText },
    { 37, api_SetWindowText },
    { 38, api_GetWindowTextLength },
    { 39, api_BeginPaint },
    { 40, api_EndPaint },
    { 41, api_CreateWindow },
    { 42, api_ShowWindow },
    { 46, api_GetParent },
    { 48, api_IsChild },
    { 53, api_DestroyWindow },
    { 56, api_MoveWindow },
    { 57, api_RegisterClass },
    { 66, api_GetDC },
    { 68, api_ReleaseDC },
    { 106, api_GetKeyState },
    { 107, api_DefWindowProc },
    { 108, api_GetMessage },
    { 109, api_PeekMessage },
    { 110, api_PostMessage },
    { 111, api_SendMessage },
    { 113, api_TranslateMessage },
    { 114, api_DispatchMessage },
    { 118, api_RegisterWindowMessage },
    { 124, api_UpdateWindow },
    { 125, api_InvalidateRect },
    { 127, api_ValidateRect },
    { 133, api_GetWindowWord },
    { 134, api_GetWindowWord },
    { 135, api_GetWindowWord },
    { 136, api_GetWindowWord },
    { 173, api_LoadCursor },
    { 174, api_LoadCursor },
    { 176, api_LoadString },
    { 179, api_GetSystemMetrics },
    { 277, api_GetDlgCtrlID },
};
const unsigned g_user_api_n = sizeof g_user_api / sizeof g_user_api[0];

int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_api_call(g_dialog_api, g_dialog_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_api_call(g_menu_api, g_menu_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_api_call(g_scroll_api, g_scroll_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_user_misc(w, ordinal, a);
    if (r != -2) return r;
    return w16_api_call(g_user_api, sizeof g_user_api / sizeof g_user_api[0], w, ordinal, a);
}
