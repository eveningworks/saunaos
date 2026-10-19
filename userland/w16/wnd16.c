// wnd16.c -- the window tree: every HWND a program has -- its main window,
// a dialog, and the children of either, a program's own or a control --
// with its parent, its place, and how it is painted and hit.
//
// HANDLES: the main window keeps W16_HWND; table entry i is W16_HWND +
// 2 * (i + 1), so an HWND is never even and never 0, as on Windows. The
// table grows with what is open and a freed entry is reused, which is why
// a window also carries `order`, its creation number: siblings are in
// that order, which is their z-order (the last made on top) and the
// dialog's tab order.
//
// THE ENTRIES ARE ALLOCATED ONE BY ONE, so a pointer to one stays good
// while the table itself is reallocated. Only the task thread changes the
// tree, so it reads it without the lock; the main thread reads it -- to
// hit-test a press, to route a key -- under `lock`, and the task takes
// `lock` for every change.
//
// A CHILD HAS NO PIXELS OF ITS OWN. Its client area is a rectangle of its
// top-level window's buffer (`fb`), at the sum of its ancestors' places,
// and clipped to each ancestor's client area -- so painting it is drawing
// into that buffer, wherever it lies.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

static unsigned g_order;          // the next window's creation number

struct w16_wnd *w16_wnd_get(struct w16 *w, uint16_t hwnd) {
    if (hwnd == w->win.hwnd && w->win.used) return &w->win;
    if (hwnd < W16_HWND + 2 || !(hwnd & 1)) return 0;
    unsigned i = (unsigned)(hwnd - W16_HWND) / 2 - 1;
    if (i >= (unsigned)w->nwnds || !w->wnds[i] || !w->wnds[i]->used) return 0;
    return w->wnds[i];
}

struct w16_wnd *w16_wnd_new(struct w16 *w, int kind) {
    pthread_mutex_lock(&w->lock);
    int i = 0;
    while (i < w->nwnds && w->wnds[i] && w->wnds[i]->used) i++;
    if (i == w->nwnds) {
        struct w16_wnd **g = realloc(w->wnds, sizeof *g * (size_t)(w->nwnds + 1));
        if (!g) { pthread_mutex_unlock(&w->lock); return 0; }
        w->wnds = g;
        w->wnds[w->nwnds++] = 0;
    }
    if (!w->wnds[i] && !(w->wnds[i] = calloc(1, sizeof **w->wnds))) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    struct w16_wnd *x = w->wnds[i];
    memset(x, 0, sizeof *x);
    x->used = 1;
    x->kind = kind;
    x->hwnd = (uint16_t)(W16_HWND + 2 * (i + 1));
    x->order = ++g_order;
    pthread_mutex_unlock(&w->lock);
    return x;
}

void w16_wnd_free(struct w16 *w, struct w16_wnd *x) {
    if (!x || x == &w->win) return;
    pthread_mutex_lock(&w->lock);
    free(x->text);
    free(x->fb);
    free(x->extra);
    free(x->ctl);
    memset(x, 0, sizeof *x);
    pthread_mutex_unlock(&w->lock);
}

// --- where a window is ----------------------------------------------------------

static struct w16_wnd *parent_of(struct w16 *w, const struct w16_wnd *x) {
    return x->kind == W16_K_CHILD ? w16_wnd_get(w, x->parent) : 0;
}

struct w16_wnd *w16_wnd_top(struct w16 *w, struct w16_wnd *x) {
    for (int guard = 0; x && x->kind == W16_K_CHILD && guard < 64; guard++) x = parent_of(w, x);
    return x && x->kind != W16_K_CHILD ? x : 0;
}

void w16_wnd_insets(const struct w16_wnd *x, int *l, int *t, int *r, int *b) {
    int border = x->kind == W16_K_CHILD && (x->style & (WS_BORDER | WS_DLGFRAME)) ? 1 : 0;
    *l = *t = *r = *b = border;
    if (w16_sbar_shown(x, SB_VERT)) *r += W16_SB - border;   // the bar's own edge is the border's
    if (w16_sbar_shown(x, SB_HORZ)) *b += W16_SB - border;
}

int w16_top_fit(struct w16_wnd *win) {
    int cx, cy, fbw, fbh, l, t, r, b;
    w16_client_rect(win, &cx, &cy, &fbw, &fbh);
    w16_wnd_insets(win, &l, &t, &r, &b);
    int cw = fbw - r > 1 ? fbw - r : 1, ch = fbh - b > 1 ? fbh - b : 1;
    if (win->fb && fbw == win->fbw && fbh == win->fbh && cw == win->cw && ch == win->ch) return 0;
    if (!win->fb || fbw != win->fbw || fbh != win->fbh) {
        uint32_t *fb = malloc(((size_t)fbw * fbh + 1) * 4);
        if (!fb) return -1;
        // Until the program repaints -- which the emulated CPU may take a
        // few frames over, and a drag resizes many times a second -- the
        // frame shows the old client area where it still fits and the
        // class background beyond it, as Windows' own erase would, never
        // a black buffer.
        int k = win->cls;
        uint32_t bg = k >= 0 && k < g_w16.ncls ? w16_brush_color(g_w16.cls[k].background) : 0xFFFFFF;
        for (size_t i = 0; i < (size_t)fbw * fbh + 1; i++) fb[i] = bg;
        int kw = win->fb && win->cw < fbw ? win->cw : fbw, kh = win->fb && win->ch < fbh ? win->ch : fbh;
        for (int y = 0; win->fb && y < kh; y++)
            memcpy(fb + (size_t)y * fbw, win->fb + (size_t)y * win->fbw, (size_t)kw * 4);
        free(win->fb);
        win->fb = fb;
        win->fbw = fbw;
        win->fbh = fbh;
    }
    win->cw = cw;
    win->ch = ch;
    return 1;
}

void w16_wnd_set_geometry(struct w16 *w, struct w16_wnd *x, int nx, int ny, int nw, int nh) {
    int l, t, r, b;
    pthread_mutex_lock(&w->lock);
    x->x = nx;
    x->y = ny;
    x->w = nw > 0 ? nw : 0;
    x->h = nh > 0 ? nh : 0;
    w16_wnd_insets(x, &l, &t, &r, &b);
    x->cw = x->w - l - r > 0 ? x->w - l - r : 0;
    x->ch = x->h - t - b > 0 ? x->h - t - b : 0;
    pthread_mutex_unlock(&w->lock);
}

// x's window rectangle and client origin in its top-level's buffer.
static void place(struct w16 *w, struct w16_wnd *x, int *wx, int *wy, int *ox, int *oy) {
    int l, t, r, b;
    w16_wnd_insets(x, &l, &t, &r, &b);
    if (x->kind != W16_K_CHILD) {
        *wx = *wy = 0;                           // a top-level's buffer is its interior
        *ox = *oy = 0;
        return;
    }
    struct w16_wnd *p = parent_of(w, x);
    int px = 0, py = 0, pwx, pwy;
    if (p) place(w, p, &pwx, &pwy, &px, &py);
    *wx = px + x->x;
    *wy = py + x->y;
    *ox = *wx + l;
    *oy = *wy + t;
}

void w16_wnd_origin(struct w16 *w, struct w16_wnd *x, int *ox, int *oy) {
    int wx, wy;
    place(w, x, &wx, &wy, ox, oy);
}

static int meet(int *x0, int *y0, int *x1, int *y1, int a0, int b0, int a1, int b1) {
    if (a0 > *x0) *x0 = a0;
    if (b0 > *y0) *y0 = b0;
    if (a1 < *x1) *x1 = a1;
    if (b1 < *y1) *y1 = b1;
    return *x0 < *x1 && *y0 < *y1;
}

// What of rectangle (x0..x1, y0..y1) of the buffer x's ancestors let show.
static int under_ancestors(struct w16 *w, struct w16_wnd *x, int *x0, int *y0, int *x1, int *y1) {
    struct w16_wnd *top = w16_wnd_top(w, x);
    if (!top) return 0;
    if (!meet(x0, y0, x1, y1, 0, 0, top->fbw, top->fbh)) return 0;
    for (struct w16_wnd *p = parent_of(w, x); p; p = parent_of(w, p)) {
        int ox, oy;
        w16_wnd_origin(w, p, &ox, &oy);
        if (!meet(x0, y0, x1, y1, ox, oy, ox + p->cw, oy + p->ch)) return 0;
    }
    return 1;
}

int w16_wnd_visible_rect(struct w16 *w, struct w16_wnd *x, int *x0, int *y0, int *x1, int *y1) {
    int ox, oy;
    w16_wnd_origin(w, x, &ox, &oy);
    *x0 = ox;
    *y0 = oy;
    *x1 = ox + x->cw;
    *y1 = oy + x->ch;
    return under_ancestors(w, x, x0, y0, x1, y1);
}

int w16_wnd_shown(struct w16 *w, struct w16_wnd *x) {
    for (int guard = 0; x && guard < 64; guard++) {
        if (!x->visible || x->destroyed) return 0;
        if (x->kind != W16_K_CHILD) return 1;
        x = parent_of(w, x);
    }
    return 0;
}

int w16_wnd_is_child(struct w16 *w, uint16_t parent, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    for (int guard = 0; x && x->kind == W16_K_CHILD && guard < 64; guard++) {
        if (x->parent == parent) return 1;
        x = parent_of(w, x);
    }
    return 0;
}

static int by_order(const void *a, const void *b) {
    const struct w16_wnd *x = *(const struct w16_wnd *const *)a, *y = *(const struct w16_wnd *const *)b;
    return x->order < y->order ? -1 : x->order > y->order;
}

struct w16_wnd **w16_wnd_children(struct w16 *w, uint16_t parent, int *n) {
    struct w16_wnd **v = malloc(sizeof *v * (size_t)(w->nwnds + 1));
    *n = 0;
    for (int i = 0; v && i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (c && c->used && !c->destroyed && c->kind == W16_K_CHILD && c->parent == parent) v[(*n)++] = c;
    }
    if (v) qsort(v, (size_t)*n, sizeof *v, by_order);
    return v;
}

// --- painting --------------------------------------------------------------------

static void invalidate_locked(struct w16 *w, struct w16_wnd *x, int erase, int depth) {
    x->inval = 1;
    x->erase |= erase;
    if (depth > 32) return;
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (c && c->used && c->kind == W16_K_CHILD && c->parent == x->hwnd) invalidate_locked(w, c, 1, depth + 1);
    }
}

void w16_invalidate(struct w16 *w, struct w16_wnd *x, int erase) {
    if (!x) return;
    pthread_mutex_lock(&w->lock);
    invalidate_locked(w, x, erase, 0);
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

static int depth_of(struct w16 *w, struct w16_wnd *x) {
    int d = 0;
    while (x && x->kind == W16_K_CHILD && d < 64) { x = parent_of(w, x); d++; }
    return d;
}

struct w16_wnd *w16_wnd_next_paint(struct w16 *w) {
    struct w16_wnd *best = 0;
    int best_depth = 0;
    if (w->win.used && w->win.inval && w16_wnd_shown(w, &w->win)) return &w->win;
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *x = w->wnds[i];
        if (!x || !x->used || !x->inval || !w16_wnd_shown(w, x)) continue;
        int d = depth_of(w, x);
        if (!best || d < best_depth || (d == best_depth && x->order < best->order)) { best = x; best_depth = d; }
    }
    return best;
}

int w16_wnd_paint(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int *ox, int *oy) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *top = w16_wnd_top(w, x);
    int x0, y0, x1, y1;
    if (!top || !top->fb || !w16_wnd_visible_rect(w, x, &x0, &y0, &x1, &y1)) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    *s = ugfx_surface_for_pixels(top->fb, top->fbw, top->fbh);
    ugfx_set_clip_rect(s, x0, y0, x1 - x0, y1 - y0);
    w16_wnd_origin(w, x, ox, oy);
    return 1;
}

int w16_wnd_paint_nc(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int *ox, int *oy) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *top = w16_wnd_top(w, x);
    int wx, wy, cx, cy;
    place(w, x, &wx, &wy, &cx, &cy);
    int x0 = wx, y0 = wy, x1, y1;
    if (x->kind == W16_K_CHILD) { x1 = wx + x->w; y1 = wy + x->h; }
    else if (top) { x1 = top->fbw; y1 = top->fbh; }
    else { x1 = x0; y1 = y0; }
    if (!top || !top->fb || !under_ancestors(w, x, &x0, &y0, &x1, &y1)) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    *s = ugfx_surface_for_pixels(top->fb, top->fbw, top->fbh);
    ugfx_set_clip_rect(s, x0, y0, x1 - x0, y1 - y0);
    *ox = wx;
    *oy = wy;
    return 1;
}

void w16_wnd_paint_done(struct w16 *w) {
    w->fb_dirty = 1;
    pthread_mutex_unlock(&w->lock);
}

// --- input --------------------------------------------------------------------

// A window a press passes through to the one under it: a label (a STATIC,
// as its WM_NCHITTEST says HTTRANSPARENT), or a disabled one.
static int hit_through(const struct w16_wnd *c) {
    return c->transparent || (c->style & WS_DISABLED) != 0;
}

struct w16_wnd *w16_wnd_at(struct w16 *w, struct w16_wnd *top, int x, int y, int *cx, int *cy, int *nc) {
    struct w16_wnd *at = top;
    int l, t, r, b;
    *cx = x;
    *cy = y;
    *nc = 0;
    w16_wnd_insets(top, &l, &t, &r, &b);
    if (x >= top->cw || y >= top->ch) {          // a top-level's own scroll bars
        *nc = 1;
        return top;
    }
    for (int depth = 0; depth < 32; depth++) {
        struct w16_wnd *hit = 0;
        for (int i = 0; i < w->nwnds; i++) {
            struct w16_wnd *c = w->wnds[i];
            if (!c || !c->used || c->destroyed || c->kind != W16_K_CHILD || c->parent != at->hwnd ||
                !c->visible || hit_through(c))
                continue;
            if (*cx < c->x || *cy < c->y || *cx >= c->x + c->w || *cy >= c->y + c->h) continue;
            if (!hit || c->order > hit->order) hit = c;
        }
        if (!hit) return at;
        int wx = *cx - hit->x, wy = *cy - hit->y;
        w16_wnd_insets(hit, &l, &t, &r, &b);
        at = hit;
        if (wx < l || wy < t || wx >= hit->w - r || wy >= hit->h - b) {
            *cx = wx;
            *cy = wy;
            *nc = 1;
            return at;
        }
        *cx = wx - l;
        *cy = wy - t;
    }
    return at;
}

uint16_t w16_set_focus(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *to = hwnd ? w16_wnd_get(w, hwnd) : 0;
    pthread_mutex_lock(&w->lock);
    uint16_t old = w->focus;
    if (hwnd && !to) { pthread_mutex_unlock(&w->lock); return old; }
    w->focus = hwnd;
    pthread_mutex_unlock(&w->lock);
    if (old == hwnd) return old;
    if (old && w16_wnd_get(w, old)) w16_send(w, old, WM_KILLFOCUS, hwnd, 0);
    if (to && w->focus == hwnd) {
        // A dialog remembers its control that last had the focus, to give
        // it back when the dialog is active again.
        struct w16_wnd *top = w16_wnd_top(w, to);
        if (top && top->kind == W16_K_DIALOG && top != to) top->focus = hwnd;
        w16_send(w, hwnd, WM_SETFOCUS, old, 0);
    }
    return old;
}

// --- making and ending windows ----------------------------------------------------

static void drop_queued(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    unsigned out = w->qhead;
    for (unsigned i = w->qhead; i != w->qtail; i = (i + 1) % W16_QUEUE)
        if (w->q[i].hwnd != hwnd) w->q[(out++) % W16_QUEUE] = w->q[i];
    w->qtail = out % W16_QUEUE;
    for (int i = 0; i < W16_TIMERS; i++)
        if (w->timer[i].used && w->timer[i].hwnd == hwnd) w->timer[i].used = 0;
    if (w->capture == hwnd) w->capture = 0;
    pthread_mutex_unlock(&w->lock);
}

void w16_wnd_destroy(struct w16 *w, struct w16_wnd *x) {
    if (!x || x->destroyed) return;
    uint16_t hwnd = x->hwnd, parent = x->parent;
    int child = x->kind == W16_K_CHILD, dialog = x->kind == W16_K_DIALOG;
    pthread_mutex_lock(&w->lock);
    if (w->focus == hwnd || w16_wnd_is_child(w, hwnd, w->focus)) w->focus = 0;
    pthread_mutex_unlock(&w->lock);
    w16_send(w, hwnd, WM_DESTROY, 0, 0);
    int n;
    struct w16_wnd **kids = w16_wnd_children(w, hwnd, &n);
    for (int i = 0; kids && i < n; i++) w16_wnd_destroy(w, kids[i]);
    free(kids);
    x->destroyed = 1;
    w16_send(w, hwnd, WM_NCDESTROY, 0, 0);
    drop_queued(w, hwnd);
    w16_gdi_forget_window(w, hwnd);
    pthread_mutex_lock(&w->lock);
    x->visible = 0;
    pthread_mutex_unlock(&w->lock);
    if (dialog) {
        w16_dlg_window_gone(w, x);
        w16_wnd_free(w, x);
    }
    if (child) {
        w16_invalidate(w, w16_wnd_get(w, parent), 1);   // what it covered shows again
        w16_wnd_free(w, x);
    }
}

struct w16_wnd *w16_wnd_create(struct w16 *w, int cls, const char *title, uint32_t style, int x, int y,
                               int cx, int cy, uint16_t parent, uint16_t id, uint16_t inst,
                               uint32_t param) {
    if (cls < 0 || cls >= w->ncls) return 0;
    struct w16_wnd *p = parent ? w16_wnd_get(w, parent) : 0;
    if (!(style & WS_CHILD) || !p) return 0;     // top-level windows are user16.c's
    struct w16_wnd *c = w16_wnd_new(w, W16_K_CHILD);
    if (!c) return 0;
    const struct w16_class *k = &w->cls[cls];
    c->cls = cls;
    c->style = style;
    c->parent = parent;
    c->id = id;
    c->inst = inst;
    c->proc_sel = k->proc_sel;
    c->proc_off = k->proc_off;
    c->host = k->host;
    c->text = strdup(title ? title : "");
    c->extra = k->wnd_extra ? calloc(1, k->wnd_extra) : 0;
    c->sb[SB_HORZ] = c->sb[SB_VERT] = (struct w16_sbar){ 0, 100, 0 };
    if (!c->text || (k->wnd_extra && !c->extra)) { w16_wnd_free(w, c); return 0; }
    if (x == W16_CW_USEDEFAULT) x = y = 0;
    if (cx == W16_CW_USEDEFAULT) cx = cy = 0;
    w16_wnd_set_geometry(w, c, x, y, cx, cy);
    uint16_t hwnd = c->hwnd;
    // WM_CREATE, with a CREATESTRUCT in w16's scratch area: lpCreateParams,
    // hInstance, hMenu, hwndParent, cy, cx, y, x, style, the two names.
    uint16_t at = w16_sys_push(w, 34);
    uint8_t cs[34] = { 0 };
    uint16_t v16[] = { inst, id, parent, (uint16_t)cy, (uint16_t)cx, (uint16_t)y, (uint16_t)x };
    memcpy(cs, &param, 4);
    memcpy(cs + 4, v16, sizeof v16);
    memcpy(cs + 18, &style, 4);
    uint8_t *mem = cpu16_ptr(&w->cpu, w->sysdata, at, sizeof cs, 1);
    if (mem) memcpy(mem, cs, sizeof cs);
    uint32_t lp = (uint32_t)w->sysdata << 16 | at;
    int ok = w16_send(w, hwnd, WM_NCCREATE, 0, lp) != 0 && w16_send(w, hwnd, WM_CREATE, 0, lp) != 0xFFFFFFFFu;
    w16_sys_pop(w, at);
    if (!ok || !w16_wnd_get(w, hwnd)) {
        if (w16_wnd_get(w, hwnd)) w16_wnd_destroy(w, c);
        return 0;
    }
    w16_send(w, hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t)(uint16_t)c->ch << 16 | (uint16_t)c->cw);
    w16_send(w, hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)c->y << 16 | (uint16_t)c->x);
    if (style & WS_VISIBLE) {
        pthread_mutex_lock(&w->lock);
        c->visible = 1;
        pthread_mutex_unlock(&w->lock);
        w16_invalidate(w, c, 1);
    }
    return c;
}
