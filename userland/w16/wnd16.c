// wnd16.c -- the windows beside the program's main one: a dialog and each
// of its controls, by HWND. The main window keeps W16_HWND (user16.c);
// table entry i is W16_HWND + 2 * (i + 1), so an HWND is never even and
// never 0, as on Windows. The table grows with what is open and a freed
// entry is reused.
//
// THE ENTRIES ARE ALLOCATED ONE BY ONE, so a pointer to one stays good
// while the table itself is reallocated; the table is read and grown
// under `lock`, because the main thread finds a dialog by HWND to draw it.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>

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
    pthread_mutex_unlock(&w->lock);
    return x;
}

void w16_wnd_free(struct w16 *w, struct w16_wnd *x) {
    if (!x || x == &w->win) return;
    pthread_mutex_lock(&w->lock);
    free(x->text);
    free(x->fb);
    memset(x, 0, sizeof *x);
    pthread_mutex_unlock(&w->lock);
}

struct w16_wnd *w16_dlg_item(struct w16 *w, uint16_t dlg, uint16_t id) {
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *x = w->wnds[i];
        if (x && x->used && x->parent == dlg && x->kind != W16_K_DIALOG && x->id == id) return x;
    }
    return 0;
}

struct w16_wnd *w16_dlg_item_at(struct w16 *w, uint16_t dlg, int x, int y) {
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (c && c->used && c->parent == dlg && c->kind != W16_K_DIALOG && c->kind != W16_K_STATIC &&
            x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h)
            return c;
    }
    return 0;
}
