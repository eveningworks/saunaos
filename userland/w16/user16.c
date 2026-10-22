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
#define HTCLIENT        1
#define HTCAPTION       2
#define HTHSCROLL       6
#define HTVSCROLL       7
#define WM_NCLBUTTONDBLCLK 0x00A3

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
    uint16_t hcursor = (uint16_t)(wc[14] | wc[15] << 8);
    k.cursor = hcursor ? (uint8_t)w16_cursor_shape(hcursor) : W16_CLASS_NO_CURSOR;
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

// WH_CALLWNDPROC's hook, told of a sent message before its window hears it:
// (HC_ACTION, sent by this task, a CWPSTRUCT -- lParam, wParam, message, hwnd).
static void call_wndproc_hook(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint32_t hook = w16_hook(WH_CALLWNDPROC);
    if (!hook) return;
    uint16_t at = w16_sys_push(w, 10);
    if (w->cpu.fault) return;
    uint8_t cwp[10];
    memcpy(cwp, &lp, 4);
    memcpy(cwp + 4, &wp, 2);
    memcpy(cwp + 6, &msg, 2);
    memcpy(cwp + 8, &hwnd, 2);
    memcpy(w->sysmem + at, cwp, sizeof cwp);
    const uint16_t words[] = { 0, 1, w->sysdata, at };
    w16_call_far(w, (uint16_t)(hook >> 16), (uint16_t)hook, words, 4);
    w16_sys_pop(w, at);
}

uint32_t w16_send(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    if (!x || w->cpu.fault) return 0;
    call_wndproc_hook(w, hwnd, msg, wp, lp);
    // Read again: the hook may have subclassed it (MFC does, on its first message).
    if (!(x = w16_wnd_get(w, hwnd)) || w->cpu.fault) return 0;
    if (x->host) return x->host(w, x, msg, wp, lp);
    return w16_call_wndproc(w, x->proc_sel, x->proc_off, hwnd, msg, wp, lp);
}

void w16_notify(struct w16 *w, struct w16_wnd *x, uint16_t code) {
    if (x->kind == W16_K_CHILD)
        w16_send(w, x->parent, WM_COMMAND, x->id, (uint32_t)code << 16 | x->hwnd);
}

uint32_t w16_host_proc_far(struct w16 *w, int cls) {
    return (uint32_t)w16_thunk_sel(w, W16_HOST_MODULE) << 16 | (uint16_t)cls;
}

int w16_host_proc_call(struct w16 *w, unsigned cls, struct w16_args *a) {
    uint16_t hwnd = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    uint32_t r = x && cls < (unsigned)w->ncls && w->cls[cls].host ? w->cls[cls].host(w, x, msg, wp, lp) : 0;
    set_result(&w->cpu, r);
    return w->cpu.fault ? -1 : 10;
}

// --- the main window ------------------------------------------------------------

static void fill_buffer(struct w16 *w, struct w16_wnd *win, uint32_t colour) {
    pthread_mutex_lock(&w->lock);
    if (win->fb)
        for (int i = 0; i < win->fbw * win->fbh; i++) win->fb[i] = colour;
    pthread_mutex_unlock(&w->lock);
}

int w16_create_main(struct w16 *w, int k, const char *title, uint32_t style, int cx, int cy, uint16_t menu,
                    uint16_t inst, uint32_t param, uint32_t far_title, uint32_t far_class) {
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
    if (hm) w16_set_menu_ex(w, hm, 0);
    // Windows' order before WM_CREATE: WM_GETMINMAXINFO, WM_NCCREATE (0
    // refuses the window), WM_NCCALCSIZE. MFC's hook attaches its C++ object
    // on the first of them, so none may be left out.
    uint16_t mm = w16_sys_push(w, 20);
    if (c->fault) return -1;
    int16_t mmi[10] = { 0, 0, 640 + 2 * W16_FRAME, 480 + 2 * W16_FRAME, -W16_FRAME, -W16_FRAME,
                        120, 2 * W16_FRAME + W16_CAPTION + 1, 640 + 2 * W16_FRAME, 480 + 2 * W16_FRAME };
    memcpy(w->sysmem + mm, mmi, sizeof mmi);
    w16_send(w, win->hwnd, 0x0024, 0, (uint32_t)w->sysdata << 16 | mm);      // WM_GETMINMAXINFO
    w16_sys_pop(w, mm);
    uint32_t r = c->fault ? 0 : w16_send(w, win->hwnd, WM_NCCREATE, 0, lp);
    if (r) {
        uint16_t rc = w16_sys_push(w, 8);
        int16_t wr[4] = { 0, 0, (int16_t)win->w, (int16_t)win->h };
        if (!c->fault) {
            memcpy(w->sysmem + rc, wr, sizeof wr);
            w16_send(w, win->hwnd, WM_NCCALCSIZE, 0, (uint32_t)w->sysdata << 16 | rc);
            w16_sys_pop(w, rc);
        }
        r = c->fault ? 0xFFFFFFFFu : w16_send(w, win->hwnd, WM_CREATE, 0, lp);
    } else {
        r = 0xFFFFFFFFu;
    }
    w16_sys_pop(w, at);
    if (c->fault) return -1;
    if (r == 0xFFFFFFFFu) {
        win->used = 0;
        return 0;
    }
    // An OVERLAPPED window hears its first WM_SIZE and WM_MOVE when first
    // shown, not here (Wine's WIN_NEED_SIZE): Solitaire builds its piles
    // between CreateWindow and ShowWindow, and its WM_SIZE walks them.
    if (style & (WS_CHILD | WS_POPUP)) w16_send(w, win->hwnd, WM_SIZE, 0, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    else win->need_size = 1;
    return win->hwnd;
}

// The size messages a window was owed until it was shown.
static void sized_on_show(struct w16 *w, struct w16_wnd *x) {
    if (!x->need_size) return;
    x->need_size = 0;
    w16_send(w, x->hwnd, WM_SIZE, x->maximized ? SIZE_MAXIMIZED : SIZE_RESTORED,
             (uint32_t)(uint16_t)x->ch << 16 | (uint16_t)x->cw);
    w16_send(w, x->hwnd, WM_MOVE, 0, 0);
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
    int h = w16_create_main(w, k, text, style, cx, cy, menu, inst, param, title, cname);
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
    if (erase && !(x->style & WS_CLIPCHILDREN)) {
        // A parent that does not clip its children paints over them, and Windows invalidates what it
        // painted over: each child draws itself, its frame and scroll bars too, once the parent is done.
        int n;
        struct w16_wnd **v = w16_wnd_children(w, x->hwnd, &n);
        for (int i = 0; v && i < n; i++)
            if (w16_wnd_shown(w, v[i])) w16_invalidate(w, v[i], 1);
        free(v);
    }
    uint16_t hdc = w16_gdi_take_dc(w, x->hwnd);
    if (erase) erased = w16_send(w, x->hwnd, WM_ERASEBKGND, hdc, 0) != 0;
    w16_gdi_reset_dc(w, hdc);                    // WM_ERASEBKGND may have changed its state
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
        // A program that never runs out of messages still shows what it
        // drew, a frame at most every 30 ms -- between messages, and never
        // while a window is owed a paint (its children would be missing).
        if (found && w->fb_dirty && !w16_wnd_next_paint(w) && w16_now_ms() - w->committed_ms >= 30) {
            pthread_mutex_unlock(&w->lock);
            w16_gdi_flush(w);
            pthread_mutex_lock(&w->lock);
        }
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
        w->mouse_keys = m->wparam;
    }
    int16_t pt[2];
    w16_cursor_screen(w, &pt[0], &pt[1]);
    memcpy(out + 14, pt, 4);
    return put_bytes(w, lpmsg, out, sizeof out);
}

// A press in a top-level window's client area asks the window where it is (WM_NCHITTEST), as Windows'
// input did: one that answers HTCAPTION -- Clock with No Title, dragged by its face -- gets the
// caption's message instead, which DefWindowProc turns into a move. Asked only for the left button's
// press and double click, the two a caption acts on.
static void caption_press(struct w16 *w, struct w16_msg *m) {
    if (m->message != WM_LBUTTONDOWN && m->message != WM_LBUTTONDBLCLK) return;
    struct w16_wnd *x = w16_wnd_get(w, m->hwnd);
    if (!x || x->kind == W16_K_CHILD) return;
    int dx, dy;
    w16_screen_offset(w, x, &dx, &dy);
    uint32_t at = (uint32_t)(uint16_t)((int16_t)(m->lparam >> 16) + dy) << 16 | (uint16_t)((int16_t)m->lparam + dx);
    if ((uint16_t)w16_send(w, m->hwnd, WM_NCHITTEST, 0, at) != HTCAPTION) return;
    m->message = m->message == WM_LBUTTONDOWN ? WM_NCLBUTTONDOWN : WM_NCLBUTTONDBLCLK;
    m->wparam = HTCAPTION;
    m->lparam = at;
}

static int get_message(struct w16 *w, struct w16_args *a) {
    uint32_t lpmsg = w16_arg32(a);
    struct msg_filter f;
    f.hwnd = w16_arg16(a);
    f.min = w16_arg16(a);
    f.max = w16_arg16(a);
    struct w16_msg m;
    if (!next_message(w, &f, 1, -1, &m)) return -1;    // the program faulted meanwhile
    caption_press(w, &m);
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
    if (got) caption_press(w, &m);
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
        if (wp == HTCAPTION && x->kind != W16_K_CHILD) w16_task_post_arg(w, W16_POST_MOVE, 0);
        return 0;
    case WM_NCHITTEST: {
        // HTCLIENT inside the client area (Clock calls this first, and makes HTCLIENT its caption with
        // No Title); the frame's parts are w16's own and never asked about, so anywhere else is nowhere.
        int dx, dy;
        w16_screen_offset(w, x, &dx, &dy);
        int cx = (int16_t)lp - dx, cy = (int16_t)(lp >> 16) - dy;
        return cx >= 0 && cy >= 0 && cx < x->cw && cy < x->ch ? HTCLIENT : 0;
    }
    case WM_NCLBUTTONDBLCLK:                     // on a caption: maximize or restore, as w16's own caption does
        if (wp == HTCAPTION && x->kind != W16_K_CHILD && (x->style & WS_MAXIMIZEBOX))
            w16_task_post_arg(w, W16_POST_SYSCMD, x->maximized ? SC_RESTORE : SC_MAXIMIZE);
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
        win->need_size = 0;
        w16_invalidate(w, win, 1);
        w16_task_post_arg(w, W16_POST_SIZE, 0);
        w16_send(w, win->hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t)win->ch << 16 | (uint16_t)win->cw);
    }
    return ok;
}

// MoveWindow's work, SetWindowPos's too: 1 when x took the place.
static int move_to(struct w16 *w, struct w16_wnd *x, int nx, int ny, int nw, int nh) {
    uint16_t h = x ? x->hwnd : 0;
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
    return ok;
}

// MoveWindow(hwnd, x, y, cx, cy, bRepaint)
static int move_window(struct w16 *w, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    int16_t nx = (int16_t)w16_arg16(a), ny = (int16_t)w16_arg16(a);
    int16_t nw = (int16_t)w16_arg16(a), nh = (int16_t)w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)move_to(w, w16_wnd_get(w, h), nx, ny, nw, nh);
    return 12;
}

#define SWP_NOSIZE       0x0001
#define SWP_NOMOVE       0x0002
#define SWP_NOZORDER     0x0004
#define HWND_TOPMOST     0xFFFF     // (HWND)-1
#define HWND_NOTOPMOST   0xFFFE     // (HWND)-2
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW   0x0040
#define SWP_HIDEWINDOW   0x0080

// The frame a changed style or menu draws, applied: the window keeps its size, the client area
// grows or shrinks inside it and the program hears WM_SIZE -- Clock's No Title and its way back.
static void frame_changed(struct w16 *w, struct w16_wnd *x) {
    int cw = x->cw, ch = x->ch;
    if (x == &w->win) {                          // the main window's buffer is its client area: refitted
        pthread_mutex_lock(&w->lock);
        if (x->used) w16_top_fit(x);
        pthread_mutex_unlock(&w->lock);
    } else {
        w16_wnd_set_geometry(w, x, x->x, x->y, x->w, x->h);
    }
    // SENT, inside SetWindowPos, as Windows sent it: the program sizes what it draws before the call returns.
    if (x->cw != cw || x->ch != ch)
        w16_send(w, x->hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t)(uint16_t)x->ch << 16 | (uint16_t)x->cw);
    w16_invalidate(w, x, 1);
    if (x == &w->win) w16_task_post(w, W16_POST_PAINT);
}

// The z-order is the creation order (wnd16.c) and stays.
// SetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags) -> BOOL
// Windows: Moves, sizes, shows or hides a window, and changes its place in the stacking order; each
// SWP_NO* flag keeps one of them as it is. HWND_TOPMOST keeps it above every window that is not.
// w16: Moves and sizes as MoveWindow does, keeping what an SWP_NO* flag says to; SWP_FRAMECHANGED
// redraws the frame a style change made and resizes the client inside it. The stacking order of a
// program's windows stays their creation order; toywm stacks its top-level windows, and
// HWND_TOPMOST and HWND_NOTOPMOST on the main window are its Always on Top.
static int api_SetWindowPos(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t after = w16_arg16(a);
    int16_t nx = (int16_t)w16_arg16(a), ny = (int16_t)w16_arg16(a);
    int16_t nw = (int16_t)w16_arg16(a), nh = (int16_t)w16_arg16(a);
    uint16_t flags = w16_arg16(a);
    w->cpu.r[R_AX] = x != 0;
    if (!x) return 14;
    if (x == &w->win && !(flags & SWP_NOZORDER) && (after == HWND_TOPMOST || after == HWND_NOTOPMOST))
        w16_task_post_arg(w, W16_POST_ABOVE, after == HWND_TOPMOST);
    if (flags & SWP_NOMOVE) { nx = (int16_t)x->x; ny = (int16_t)x->y; }
    if (flags & SWP_NOSIZE) { nw = (int16_t)x->w; nh = (int16_t)x->h; }
    if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE) &&
        (x == &w->win ? (nw != x->w || nh != x->h) : (nx != x->x || ny != x->y || nw != x->w || nh != x->h)))
        move_to(w, x, nx, ny, nw, nh);
    if (flags & SWP_FRAMECHANGED) frame_changed(w, x);
    if (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) {
        int show = (flags & SWP_SHOWWINDOW) != 0;
        pthread_mutex_lock(&w->lock);
        int was = x->visible;
        x->visible = show;
        if (show) x->style |= WS_VISIBLE;
        else x->style &= ~WS_VISIBLE;
        pthread_mutex_unlock(&w->lock);
        if (show) sized_on_show(w, x);
        if (show && !was) w16_invalidate(w, x, 1);
        else if (!show && was && x->kind == W16_K_CHILD) w16_invalidate(w, w16_wnd_get(w, x->parent), 1);
        if (x == &w->win) w16_task_post(w, W16_POST_PAINT);
    }
    return 14;
}

// What CreateWindow and MoveWindow take off again (w16_frame_fit under the desktop's frame), so the
// client is the one asked for.
// AdjustWindowRect(lpRect, dwStyle, bMenu) -> void
// Windows: Grows a client rectangle by the frame, caption and menu bar a window of that style has:
// the window rectangle that gives that client area. Scroll bars are not counted.
// w16: The same, by Windows 3.1's measures; under toywm's frame (the Window frame setting) the
// client asked for is still the one the program gets.
static int api_AdjustWindowRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t p = w16_arg32(a), style = w16_arg32(a);
    uint16_t menu = w16_arg16(a);
    int16_t r[4];
    if (get_bytes(w, p, r, 8) < 0) return -1;
    int f = style & WS_THICKFRAME ? W16_FRAME : 1;
    int top = f + ((style & WS_CAPTION) == WS_CAPTION ? W16_CAPTION : 0) + (menu ? W16_MENU_H : 0);
    r[0] = (int16_t)(r[0] - f);
    r[1] = (int16_t)(r[1] - top);
    r[2] = (int16_t)(r[2] + f);
    r[3] = (int16_t)(r[3] + f);
    if (put_bytes(w, p, r, 8) < 0) return -1;
    w->cpu.r[R_AX] = 1;
    return 10;
}

// --- the exports ------------------------------------------------------------------------

// InitApp(hInstance) -> BOOL
// Windows: Part of a program's start-up: sets up its message queue.
// w16: Answers TRUE: the queue is there from the start.
static int api_InitApp(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 1;
    return 2;
}

// PostQuitMessage(nExitCode) -> void
// Windows: Asks the program's message loop to end: GetMessage then returns FALSE with the code.
// w16: The same; the code becomes w16's exit status.
static int api_PostQuitMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    pthread_mutex_lock(&w->lock);
    w->quit_posted = 1;
    w->quit_code = w16_arg16(a);
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// GetClientRect(hWnd, lpRect) -> void
// Windows: A window's client area -- inside its frame, caption, menu bar and scroll bars -- as a
// rectangle from 0,0.
// w16: The same.
static int api_GetClientRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t rc = w16_arg32(a);
    uint16_t r[4] = { 0, 0, x ? (uint16_t)x->cw : 0, x ? (uint16_t)x->ch : 0 };
    if (put_bytes(w, rc, r, sizeof r) < 0) return -1;
    return 6;
}

// BeginPaint(hWnd, lpPaint) -> HDC
// Windows: Starts painting a window in answer to WM_PAINT: a device context clipped to the part
// that needs it, the background erased with the class brush first unless the program does it.
// w16: The same; the window's background is erased (WM_ERASEBKGND) when it was invalidated asking
// for that.
static int api_BeginPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t lpps = w16_arg32(a);
    c->r[R_AX] = 0;
    if (x) {
        begin_paint(w, x, lpps);
        w->painting++;
        c->r[R_AX] = x->dc;
    }
    return 6;
}

// EndPaint(hWnd, lpPaint) -> void
// Windows: Ends the painting BeginPaint began.
// w16: The same. What was drawn shows on screen when the program next waits for a message, as one
// finished frame, so a window never shows half a repaint.
static int api_EndPaint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint32_t lpps = w16_arg32(a);
    const uint8_t *ps = cpu16_ptr(c, (uint16_t)(lpps >> 16), (uint16_t)lpps, 2, 0);
    if (!ps) return -1;
    w16_gdi_give_dc(w, (uint16_t)(ps[0] | ps[1] << 8));   // the DC BeginPaint handed out
    if (w->painting > 0) w->painting--;
    // w16 paints a window whole, not its update region: without WS_CLIPCHILDREN
    // that covered its children (Write's ruler), so they paint again on top.
    if (x && !(x->style & WS_CLIPCHILDREN)) {
        int n;
        struct w16_wnd **kids = w16_wnd_children(w, x->hwnd, &n);
        for (int i = 0; kids && i < n; i++)
            if (!kids[i]->inval && w16_wnd_shown(w, kids[i])) w16_invalidate(w, kids[i], 1);
        free(kids);
    }
    c->r[R_AX] = 1;
    return 6;
}

// MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint) -> BOOL
// Windows: Moves and sizes a window -- a top-level one on the screen, a child in its parent --
// sending WM_MOVE and WM_SIZE.
// w16: The same. A top-level window's size is its toywm window's, frame included as the program
// counted it.
static int api_MoveWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return move_window(w, a);
}

// GetDC(hWnd) -> HDC
// Windows: A device context to draw in a window's client area outside WM_PAINT; GetDC(NULL) one for
// the whole screen.
// w16: The window's device context, its defaults restored -- or a second one while the first is still
// held, as Windows' cache gave; GetDC(NULL) one for the program's screen, which it may read from but
// which shows nothing it draws.
static int api_GetDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    c->r[R_AX] = h ? w16_gdi_take_dc(w, h) : w16_gdi_screen_dc(w);
    return 2;
}

// ReleaseDC(hWnd, hDC) -> int
// Windows: Gives back a device context GetDC lent.
// w16: The same, and 1: the window's own DC is free to hand out again, a second one deleted.
static int api_ReleaseDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    w16_gdi_give_dc(w, w16_arg16(a));
    c->r[R_AX] = 1;
    return 4;
}

// A program polling with it would spin a core, so an empty queue costs the caller a millisecond --
// Windows yielded to other tasks there.
// PeekMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg) -> BOOL
// Windows: Looks for a message without waiting -- for a window, in a range -- taking it off the
// queue with PM_REMOVE; FALSE when there is none. Programs that animate while idle loop on it.
// w16: The same. A program that finds nothing gives up a millisecond each time, where Windows let
// other programs run.
static int api_PeekMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return peek_message(w, a);
}

// PostMessage(hWnd, Msg, wParam, lParam) -> BOOL
// Windows: Puts a message on a window's queue and returns at once.
// w16: The same, on the program's one queue.
static int api_PostMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    w16_post_msg(w, h, msg, wp, lp);
    c->r[R_AX] = 1;
    return 10;
}

// SendMessage(hWnd, Msg, wParam, lParam) -> LRESULT
// Windows: Calls a window's procedure with a message now and returns its answer.
// w16: The same, for the program's windows and w16's own controls inside them; a hook on
// WH_CALLWNDPROC hears it first.
static int api_SendMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t h = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    set_result(c, w16_send(w, h, msg, wp, lp));
    return c->fault ? -1 : 10;
}

// ValidateRect(hWnd, lpRect) -> void
// Windows: Marks part of a window as painted, so no WM_PAINT comes for it.
// w16: Marks the whole client area painted, whatever rectangle is given -- w16 keeps one "needs
// painting" mark per window.
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

// LoadString(hInstance, uID, lpBuffer, nBufferMax) -> int
// Windows: Copies a string from the module's string table resource -- in blocks of 16 -- into a
// buffer; the length copied.
// w16: The same.
static int api_LoadString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return w16_user_loadstring(w, a);
}

// CreateWindowEx(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam) -> HWND
// Windows: CreateWindow with an extended style -- a dialog's frame, a window kept on top, one that
// takes dropped files.
// w16: CreateWindow; no extended style changes what w16 draws yet.
static int api_CreateWindowEx(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    int r = create_window(w, a);
    return r < 0 ? r : 34;
}

// CallWindowProc(lpPrevWndFunc, hWnd, Msg, wParam, lParam) -> LRESULT
// Windows: Calls a window procedure by its address -- how a subclass passes a message on to the
// procedure it replaced.
// w16: The same; when the procedure is one of w16's own controls', the call lands in w16's C code,
// so a program can subclass an EDIT or a BUTTON.
static int api_CallWindowProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t fn = w16_arg32(a);
    uint16_t hwnd = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    set_result(&w->cpu, w16_call_wndproc(w, (uint16_t)(fn >> 16), (uint16_t)fn, hwnd, msg, wp, lp));
    return w->cpu.fault ? -1 : 14;
}

// GetClassInfo(hInstance, lpClassName, lpWndClass) -> BOOL
// Windows: Fills a WNDCLASS with what a registered class was registered with.
// w16: The same; for w16's own classes (BUTTON, EDIT, LISTBOX...) the procedure is one a program
// can call, so it can superclass them -- register its own class on top, passing messages on.
static int api_GetClassInfo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t inst = w16_arg16(a);
    uint32_t cname = w16_arg32(a), out = w16_arg32(a);
    char *name = class_name(w, cname);
    int k = name ? w16_class_find(w, name) : -1;
    free(name);
    w->cpu.r[R_AX] = 0;
    if (k < 0) return w->cpu.fault ? -1 : 10;
    const struct w16_class *cl = &w->cls[k];
    uint32_t proc = cl->host ? w16_host_proc_far(w, k) : (uint32_t)cl->proc_sel << 16 | cl->proc_off;
    uint8_t wc[26] = { 0 };
    uint16_t v16;
    v16 = cl->style;       memcpy(wc, &v16, 2);
    memcpy(wc + 2, &proc, 4);
    v16 = cl->wnd_extra;   memcpy(wc + 8, &v16, 2);
    v16 = cl->host ? 0 : inst; memcpy(wc + 10, &v16, 2);
    v16 = cl->background;  memcpy(wc + 16, &v16, 2);
    memcpy(wc + 22, &cname, 4);                  // lpszClassName: the caller's own string
    if (put_bytes(w, out, wc, sizeof wc) < 0) return -1;
    w->cpu.r[R_AX] = 1;
    return 10;
}

// CreateWindow(lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam) -> HWND
// Windows: Makes a window of a registered class: a top-level window or a child in another, with its
// style, place, size and menu, sending WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE and WM_CREATE
// as it is made.
// w16: The same messages in the same order -- MFC programs depend on it. A top-level window is a
// toywm window with Windows 3.1's frame drawn by w16 (or toywm's own frame, by the Window frame
// setting); a child is drawn into it.
static int api_CreateWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return create_window(w, a);
}

// ShowWindow(hWnd, nCmdShow) -> BOOL
// Windows: Shows, hides, minimizes, maximizes or restores a window; whether it was visible.
// w16: The same. A top-level window is minimized and maximized through toywm; a window that was
// sized while hidden hears its WM_SIZE as it is first shown.
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
    if (show) sized_on_show(w, x);
    if (show) w16_invalidate(w, x, 1);
    else if (was && x->kind == W16_K_CHILD) w16_invalidate(w, w16_wnd_get(w, x->parent), 1);
    if (x == &w->win) w16_task_post(w, W16_POST_PAINT);
    return c->fault ? -1 : 4;
}

// DestroyWindow(hWnd) -> BOOL
// Windows: Destroys a window and its children, sending WM_DESTROY to each.
// w16: The same.
static int api_DestroyWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    if (x) w16_wnd_destroy(w, x);
    c->r[R_AX] = x != 0;
    return 2;
}

// RegisterClass(lpWndClass) -> ATOM
// Windows: Registers a window class: its procedure, background brush, icon, cursor and extra bytes
// per window.
// w16: The same.
static int api_RegisterClass(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return register_class(w, a);
}

// DefWindowProc(hWnd, Msg, wParam, lParam) -> LRESULT
// Windows: What Windows does with a message the program does not handle: erases the background,
// draws the frame, runs the control menu, closes on WM_CLOSE, gives the cursor.
// w16: The same for what w16's windows do: WM_ERASEBKGND with the class brush, WM_SYSCOMMAND
// (minimize, maximize, restore, close), WM_CLOSE destroying the window, WM_SETTEXT and WM_GETTEXT
// for the caption, the class cursor.
static int api_DefWindowProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    set_result(c, x ? w16_def_window_proc(w, x, msg, wp, lp) : 0);
    return 10;
}

// GetMessage(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax) -> BOOL
// Windows: Waits for the program's next message: posted messages and input first, WM_PAINT for a
// window that needs it when nothing else waits, WM_TIMER after that; FALSE for WM_QUIT.
// w16: The same. A mouse move still waiting is replaced by a newer one, so a slow program is not
// handed a backlog of old positions.
static int api_GetMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return get_message(w, a);
}

// TranslateMessage(lpMsg) -> BOOL
// Windows: Turns a key press into WM_CHAR with the character it types, by the keyboard layout.
// w16: Answers FALSE and does nothing: w16 posts WM_CHAR itself as each key arrives, with the
// character toy-os's keyboard layout gave it.
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

// DispatchMessage(lpMsg) -> LONG
// Windows: Calls the window procedure a message is for; a WM_TIMER with a procedure calls that.
// w16: The same.
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

// UpdateWindow(hWnd) -> void
// Windows: Sends WM_PAINT now, rather than when the program next waits, if the window needs
// painting.
// w16: The same, then its children's.
static int api_UpdateWindow(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    update(w, w16_wnd_get(w, w16_arg16(a)), 0);
    return 2;
}

// InvalidateRect(hWnd, lpRect, bErase) -> void
// Windows: Marks part of a window as needing paint -- erased first, if asked -- so WM_PAINT comes
// for it.
// w16: Marks the whole client area, whatever rectangle is given: w16 keeps one mark per window, and
// repainting more than was asked draws the same picture.
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

// LoadCursor(hInstance, lpCursorName) -> HCURSOR
// LoadIcon(hInstance, lpIconName) -> HICON
// Windows: A cursor or an icon, from the module's resources or -- with hInstance NULL -- one of
// Windows' own (the arrow, the hourglass, IDI_APPLICATION...).
// w16: A cursor is one of toy-os's pointer shapes standing for Windows' (arrow, I-beam, hourglass,
// cross, sizes); a program's own cursor resource shows as the arrow. An icon from the program's
// resources is drawn as it is; Windows' own icons are handles that draw nothing yet.
static int api_LoadCursor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint16_t inst = w16_arg16(a);
    uint32_t name = w16_arg32(a);
    uint16_t icon = 0;
    if (ordinal == 174 && inst) {
        char s[32] = "";
        if ((name >> 16) && cpu16_str(c, (uint16_t)(name >> 16), (uint16_t)name, s, sizeof s) < 0) return -1;
        icon = w16_icon_handle(w16_module_of(w, inst), (name >> 16) ? 0 : (uint16_t)name, s[0] ? s : 0);
    }
    if (ordinal == 174) c->r[R_AX] = icon ? icon : (uint16_t)(0x4000 | ordinal);
    else if (!inst && !(name >> 16) && (name & 0xFF00) == 0x7F00) c->r[R_AX] = (uint16_t)(CURSOR_BASE | (name & 0xFF));
    else c->r[R_AX] = CURSOR_OWN;
    return 6;
}

// GetSystemMetrics(nIndex) -> int
// Windows: Sizes of the screen and of Windows' parts: the screen's width and height, scroll bar and
// caption sizes, icon and cursor sizes...
// w16: Windows 3.1's values on a 640 x 480 VGA screen, the world a program is drawn in before any
// scaling; 0 for an index it does not know.
static int api_GetSystemMetrics(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    int16_t i = (int16_t)w16_arg16(a);
    // By index (SM_*): from SM_CXSCREEN to SM_CYFULLSCREEN, then SM_MOUSEPRESENT at 19 -- a
    // program told 0 there runs its keyboard interface (Write parks the pointer and never
    // sets the I-beam) -- the scroll bars again, the caption buttons (SM_CXSIZE),
    // the sizing border (SM_CXFRAME, W16_FRAME) and the double-click box at 36.
    static const int16_t m[] = { 640, 480, 17, 17, 20, 1, 1, 18, 18, 18, 18, 32, 32,
                                 32, 32, 18, 640, 460, 0, 1, 17, 17, 0, 0, 0, 0, 0, 0,
                                 0, 0, 18, 18, W16_FRAME, W16_FRAME, 0, 0, 4, 4 };
    c->r[R_AX] = (uint16_t)(i >= 0 && i < (int)(sizeof m / sizeof m[0]) ? m[i] : 0);
    return 2;
}

// EnableWindow(hWnd, bEnable) -> BOOL
// Windows: Enables or disables a window -- a disabled one takes no mouse or keys, a control greys;
// whether it was disabled.
// w16: The same; a control redraws greyed.
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

// IsWindowEnabled(hWnd) -> BOOL
// Windows: Whether a window takes input.
// w16: The same.
static int api_IsWindowEnabled(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w->cpu.r[R_AX] = x && !(x->style & WS_DISABLED);
    return 2;
}

// GetWindowText(hWnd, lpString, nMaxCount) -> int
// Windows: A window's caption, or a control's text: WM_GETTEXT.
// w16: The same.
static int api_GetWindowText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t max = w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)w16_send(w, h, WM_GETTEXT, max, far);
    return w->cpu.fault ? -1 : 8;
}

// SetWindowText(hWnd, lpString) -> void
// Windows: Sets a window's caption or a control's text: WM_SETTEXT.
// w16: The same; a top-level window's caption shows in toywm's taskbar too.
static int api_SetWindowText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    w16_send(w, h, WM_SETTEXT, 0, w16_arg32(a));
    return w->cpu.fault ? -1 : 6;
}

// GetWindowTextLength(hWnd) -> int
// Windows: The length of a window's text: WM_GETTEXTLENGTH.
// w16: The same.
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
    case GWL_WNDPROC: {
        // w16's own control answers its class's W16HOST far pointer, which
        // CallWindowProc (or a far call) takes back to the C procedure: a
        // program may subclass an EDIT and pass on what it does not handle.
        uint32_t host = x->cls >= 0 && x->cls < w->ncls && w->cls[x->cls].host ? w16_host_proc_far(w, x->cls) : 0;
        old = x->host ? host : (uint32_t)x->proc_sel << 16 | x->proc_off;
        if (set && host && v == host) {
            x->host = w->cls[x->cls].host;
        } else if (set) {
            x->host = 0;
            x->proc_sel = (uint16_t)(v >> 16);
            x->proc_off = (uint16_t)v;
        }
        return old;
    }
    case GWW_HINSTANCE:
        return x->inst;
    case GWW_HWNDPARENT:
        return x->parent;
    case GWW_ID:
        // A top-level window's is its MENU: Clock's No Title sets it to 0 to drop the menu bar, and
        // back to put it again (it never calls SetMenu).
        if (x->kind != W16_K_CHILD && x == &w->win) {
            old = w->menu;
            if (set) w16_set_menu(w, (uint16_t)v);
            return old;
        }
        return x->id;
    default:
        return 0;
    }
}

// GetWindowWord(hWnd, nIndex) -> WORD
// SetWindowWord(hWnd, nIndex, wNewWord) -> WORD
// GetWindowLong(hWnd, nIndex) -> LONG
// SetWindowLong(hWnd, nIndex, dwNewLong) -> LONG
// Windows: Read or change a window's fields: a negative index is one of Windows' own -- its
// procedure (GWL_WNDPROC, how a window is subclassed), style, id, parent, instance -- a positive
// one the window's own extra bytes.
// w16: The same fields, and the window's extra bytes as its class asked for them.
static int api_GetWindowWord(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    int16_t index = (int16_t)w16_arg16(a);
    int set = ordinal == 134 || ordinal == 136, lng = ordinal == 135 || ordinal == 136;
    uint32_t v = set ? (lng ? w16_arg32(a) : w16_arg16(a)) : 0;
    set_result(&w->cpu, x ? window_field(w, x, index, lng ? 4 : 2, set, v) : 0);
    if (!lng) w->cpu.r[R_DX] = 0;
    return 4 + (set ? (lng ? 4 : 2) : 0);
}

// RegisterWindowMessage(lpString) -> UINT
// Windows: A message number unique to a string, the same in every program that registers it -- how
// programs agree on a message, as the Find dialog's "commdlg_FindReplace".
// w16: The same, from the program's atom table.
static int api_RegisterWindowMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    char *s = dup_far(w, w16_arg32(a));
    if (!s) return -1;
    w->cpu.r[R_AX] = w16_atom_add(w, s);
    free(s);
    return 4;
}

// GetParent(hWnd) -> HWND
// Windows: A child window's parent, or a top-level window's owner.
// w16: The same.
static int api_GetParent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    w->cpu.r[R_AX] = x ? x->parent : 0;
    return 2;
}

// IsChild(hWndParent, hWnd) -> BOOL
// Windows: Whether a window is a child -- at any depth -- of another.
// w16: The same.
static int api_IsChild(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t p = w16_arg16(a), h = w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)w16_wnd_is_child(w, p, h);
    return 4;
}

// GetKeyState(nVirtKey) -> int
// Windows: Whether a key was down when the message being handled was made (the high bit), and a
// toggle key's state (the low bit).
// w16: Shift, Ctrl and Alt as they were for the key message being handled, and the mouse buttons
// as they were for the last mouse message the program took -- how a drag loop learns the button is
// still held while its queue is empty (Write's selection). Any other key reads as up, and no toggle
// is kept.
static int api_GetKeyState(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t vk = w16_arg16(a);
    unsigned m = w->key_mods, b = w->mouse_keys;
    int down = (vk == VK_SHIFT && (m & KEY_MOD_SHIFT)) || (vk == VK_CONTROL && (m & KEY_MOD_CTRL)) ||
               (vk == 0x12 && (m & KEY_MOD_ALT)) ||
               (vk == 0x01 && (b & MK_LBUTTON)) || (vk == 0x02 && (b & MK_RBUTTON)) ||
               (vk == 0x04 && (b & MK_MBUTTON));
    w->cpu.r[R_AX] = down ? 0x8000 : 0;
    return 2;
}

// GetAsyncKeyState(vKey) -> int
// Windows: Whether a key is down at this moment (the high bit), and whether it was pressed since the
// last call (the low bit) -- not as of the message being handled, as GetKeyState answers.
// w16: GetKeyState's answer: Shift, Ctrl and Alt as of the key message handled, the mouse buttons
// as of the last mouse message taken -- which is as late as w16 knows them (Clock asks for the left
// button while its title-less window is dragged). The low bit is never set.
static int api_GetAsyncKeyState(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return api_GetKeyState(w, ordinal, a);
}

// GetDialogBaseUnits() -> DWORD
// Windows: The System font's average character width (low word) and height (high word): what a
// dialog unit is made of, a quarter of the width and an eighth of the height.
// w16: Windows 3.11's at VGA, 8 by 16 -- the units its dialogs are laid out in (dlgwin16.c).
static int api_GetDialogBaseUnits(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    int bx, by;
    w16_dlg_units(&bx, &by);
    w->cpu.r[R_AX] = (uint16_t)bx;
    w->cpu.r[R_DX] = (uint16_t)by;
    return 0;
}

// GetDlgCtrlID(hWnd) -> int
// Windows: A child window's id.
// w16: The same.
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
    { 102, api_AdjustWindowRect },
    { 106, api_GetKeyState },
    { 107, api_DefWindowProc },
    { 108, api_GetMessage },
    { 109, api_PeekMessage },
    { 110, api_PostMessage },
    { 111, api_SendMessage },
    { 113, api_TranslateMessage },
    { 114, api_DispatchMessage },
    { 118, api_RegisterWindowMessage },
    { 122, api_CallWindowProc },
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
    { 232, api_SetWindowPos },
    { 243, api_GetDialogBaseUnits },
    { 249, api_GetAsyncKeyState },
    { 277, api_GetDlgCtrlID },
    { 404, api_GetClassInfo },
    { 452, api_CreateWindowEx },
};
const unsigned g_user_api_n = sizeof g_user_api / sizeof g_user_api[0];

int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_api_call(g_dialog_api, g_dialog_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_api_call(g_menu_api, g_menu_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_api_call(g_scroll_api, g_scroll_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_api_call(g_clip_api, g_clip_api_n, w, ordinal, a);
    if (r != -2) return r;
    r = w16_user_misc(w, ordinal, a);
    if (r != -2) return r;
    return w16_api_call(g_user_api, sizeof g_user_api / sizeof g_user_api[0], w, ordinal, a);
}
