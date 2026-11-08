// screen16.c -- what a program draws on the SCREEN (GetDC(0), CreateDC("DISPLAY")) where one of its
// own popups lies. In 3.1 there was one framebuffer: a screen DC drew over every window, and what it
// drew stayed until the window under it repainted. Here each popup is a toywm window of its own over
// the main window's buffer, so what lands on a popup goes into an OVERLAY of that popup's -- the shape of
// X's Composite overlay window -- laid over everything the popup paints until it repaints there.
//
// A call is drawn on the main window's buffer as the screen shows it: begin lays each popup it can reach
// on, as drawn so far, overlay and all, so a ROP or a read sees the screen; end takes every pixel the call
// changed on a popup into the topmost popup's overlay -- one drawn back to the popup's own pixel clears
// it, which is how a program's save-under or a second XOR takes its drawing away -- and puts the main
// window's own pixels back under the popups. Off a popup the call stays drawn on the main window.
//
// An overlay goes when its popup repaints (BeginPaint clears its update region), moves, is sized or
// hides; 3.1 would have copied a moved window's screen bits along, but the program that drew them
// draws again.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>

// One popup a call can reach: its corner in the main window's buffer, the part of it the call may draw
// on there, the main window's pixels under that part, the popup as it looks there, and its own paint.
struct reach {
    struct w16_wnd *d;
    int px0, py0;                 // the popup's corner, main buffer coordinates
    int l, t, r, b;               // what of it lies in the call's box
    uint32_t *under, *before, *own;
};

static struct reach *g_reach;
static int g_nreach, g_reach_cap;
static const struct gdi_target *g_t;

void w16_over_drop(struct w16_wnd *d) {
    if (!d->over) return;
    free(d->over->px);
    free(d->over);
    d->over = 0;
}

// The overlay as the popup now is, NULL when it has none or has moved, been sized or hidden since.
static struct w16_over *over_of(struct w16_wnd *d) {
    struct w16_over *o = d->over;
    if (o && (!d->visible || o->x != d->x || o->y != d->y || o->w != d->w || o->h != d->h)) {
        w16_over_drop(d);
        o = 0;
    }
    return o;
}

static struct w16_over *over_make(struct w16_wnd *d) {
    struct w16_over *o = over_of(d);
    if (o) return o;
    size_t n = (size_t)d->w * d->h;
    if (!(o = calloc(1, sizeof *o)) || !(o->px = calloc(n, 10))) {   // px, spx, on, son in one block
        free(o);
        return 0;
    }
    o->spx = o->px + n;
    o->on = (uint8_t *)(o->spx + n);
    o->son = o->on + n;
    o->x = d->x;
    o->y = d->y;
    o->w = d->w;
    o->h = d->h;
    d->over = o;
    return o;
}

void w16_over_apply(struct w16 *w, struct w16_wnd *d, uint32_t *px, int latest) {
    (void)w;
    struct w16_over *o = over_of(d);
    if (!o) return;
    const uint32_t *v = latest ? o->px : o->spx;
    const uint8_t *on = latest ? o->on : o->son;
    for (size_t i = 0, n = (size_t)o->w * o->h; i < n; i++)
        if (on[i]) px[i] = v[i];
}

void w16_over_commit(struct w16_wnd *d) {
    struct w16_over *o = over_of(d);
    if (!o) return;
    size_t n = (size_t)o->w * o->h;
    memcpy(o->spx, o->px, n * 4);
    memcpy(o->son, o->on, n);
}

void w16_over_clear(struct w16 *w, struct w16_wnd *x, const struct urgn *rgn) {
    struct w16_wnd *top = w16_wnd_top(w, x);
    struct w16_over *o = top && top != &w->win ? over_of(top) : 0;
    if (!o) return;
    int dx, dy;                                  // x's client (0, 0) in the popup's window
    w16_screen_offset(w, x, &dx, &dy);
    dx -= top->x;
    dy -= top->y;
    struct urect all = { 0, 0, x->cw, x->ch };
    int n = rgn ? rgn->n : 1;
    for (int i = 0; i < n; i++) {
        const struct urect *r = rgn ? &rgn->r[i] : &all;
        int l = r->x + dx, t = r->y + dy, rr = l + r->w, b = t + r->h;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (rr > o->w) rr = o->w;
        if (b > o->h) b = o->h;
        for (int y = t; y < b; y++)
            if (l < rr) memset(o->on + (size_t)y * o->w + l, 0, (size_t)(rr - l));
    }
}

static void reach_free(void) {
    for (int i = 0; i < g_nreach; i++) {
        free(g_reach[i].under);
        free(g_reach[i].before);
        free(g_reach[i].own);
    }
    g_nreach = 0;
    g_t = 0;
}

// The popups the call's box meets, lowest first -- each pass the next in the order they were shown.
static void reach_find(struct w16 *w, const struct gdi_target *t) {
    int mx, my, mw, mh;
    w16_client_rect(&w->win, &mx, &my, &mw, &mh);   // screen_copy()'s mapping: the popups' x, y less this
    unsigned after = 0;
    for (;;) {
        struct w16_wnd *d = 0;
        for (int i = 0; i < w->nwnds; i++) {
            struct w16_wnd *c = w->wnds[i];
            if (!c || !c->used || c->kind != W16_K_DIALOG || !c->visible || c->fullscreen) continue;
            if (c->order > after && (!d || c->order < d->order)) d = c;
        }
        if (!d) return;
        after = d->order;
        struct reach k = { d, d->x - mx, d->y - my, 0, 0, 0, 0, 0, 0, 0 };
        k.l = k.px0 > t->x0 ? k.px0 : t->x0;
        k.t = k.py0 > t->y0 ? k.py0 : t->y0;
        k.r = k.px0 + d->w < t->x1 ? k.px0 + d->w : t->x1;
        k.b = k.py0 + d->h < t->y1 ? k.py0 + d->h : t->y1;
        if (k.l >= k.r || k.t >= k.b) continue;
        size_t n = (size_t)(k.r - k.l) * (k.b - k.t);
        k.under = malloc(n * 4);
        k.before = malloc(n * 4);
        k.own = malloc((size_t)d->w * d->h * 4);
        if (g_nreach == g_reach_cap) {
            int cap = g_reach_cap ? 2 * g_reach_cap : 4;
            struct reach *g = realloc(g_reach, (size_t)cap * sizeof *g);
            if (g) { g_reach = g; g_reach_cap = cap; }
        }
        if (!k.under || !k.before || !k.own || g_nreach == g_reach_cap) {   // out of memory: drawn as before
            free(k.under); free(k.before); free(k.own);
            continue;
        }
        g_reach[g_nreach++] = k;
    }
}

// Rows [k->t, k->b) of the part k covers, between the buffer and `rows` (to: buffer -> rows).
static void rows_copy(const struct reach *k, uint32_t *rows, int to) {
    int rw = k->r - k->l;
    for (int y = k->t; y < k->b; y++) {
        uint32_t *fb = g_t->px + (size_t)y * g_t->w + k->l, *v = rows + (size_t)(y - k->t) * rw;
        memcpy(to ? v : fb, to ? fb : v, (size_t)rw * 4);
    }
}

void w16_screen_begin(struct w16 *w, const struct gdi_target *t) {
    reach_free();
    if (!t->px || t->px != w->win.fb) return;
    g_t = t;
    reach_find(w, t);
    for (int i = 0; i < g_nreach; i++) rows_copy(&g_reach[i], g_reach[i].under, 1);
    for (int i = 0; i < g_nreach; i++) {         // laid on lowest first, so the higher ones lie over it
        struct reach *k = &g_reach[i];
        struct w16_wnd *d = k->d;
        w16_dlgwin_paint(d, k->own, 1);
        uint32_t *seen = k->before;              // the popup with its overlay, then into the buffer
        int rw = k->r - k->l;
        struct w16_over *o = over_of(d);
        for (int y = k->t; y < k->b; y++)
            for (int x = k->l; x < k->r; x++) {
                size_t j = (size_t)(y - k->py0) * d->w + (x - k->px0);
                seen[(size_t)(y - k->t) * rw + (x - k->l)] = o && o->on[j] ? o->px[j] : k->own[j];
            }
        rows_copy(k, seen, 0);
    }
    for (int i = 0; i < g_nreach; i++) rows_copy(&g_reach[i], g_reach[i].before, 1);   // as the call finds it
}

static int covered_above(int i, int x, int y) {
    for (int j = i + 1; j < g_nreach; j++)
        if (x >= g_reach[j].l && x < g_reach[j].r && y >= g_reach[j].t && y < g_reach[j].b) return 1;
    return 0;
}

void w16_screen_end(struct w16 *w) {
    (void)w;
    if (!g_t) return;
    for (int i = g_nreach - 1; i >= 0; i--) {     // the topmost first: a pixel is its popup's
        struct reach *k = &g_reach[i];
        struct w16_wnd *d = k->d;
        int rw = k->r - k->l;
        struct w16_over *o = 0;
        for (int y = k->t; y < k->b; y++)
            for (int x = k->l; x < k->r; x++) {
                uint32_t now = g_t->px[(size_t)y * g_t->w + x];
                if (now == k->before[(size_t)(y - k->t) * rw + (x - k->l)] || covered_above(i, x, y)) continue;
                if (!o && !(o = over_make(d))) break;
                size_t j = (size_t)(y - k->py0) * d->w + (x - k->px0);
                o->on[j] = now != k->own[j];
                o->px[j] = now;
            }
    }
    for (int i = 0; i < g_nreach; i++) rows_copy(&g_reach[i], g_reach[i].under, 0);
    reach_free();
}
