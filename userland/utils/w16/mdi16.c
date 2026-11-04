// mdi16.c -- USER's Multiple Document Interface: the MDICLIENT class and DefFrameProc,
// DefMDIChildProc, TranslateMDISysAccel and CalcChildScroll, as Windows 3.1 had them (Wine's
// mdi.c is the same shape).
//
// A frame (the program's main window) holds one MDICLIENT child filling its client area, and the
// client holds the document windows -- Program Manager's groups, SysEdit's files. They are ordinary
// captioned children (user16.c moves, sizes, minimizes and maximizes them); what this file adds is
// what makes them a set: one ACTIVE at a time, its caption lit, the others dim; the Window menu
// listing them by number; and ONE MAXIMIZED FOR ALL -- while a child is maximized the others are
// shown so too when activated, its control-menu box and Restore sit in the frame's menu bar, and
// the frame's title reads "Frame - [Child]".
//
// THE CHILD LIST IS ID ORDER, NOT Z ORDER. Child i has the id first_id + i and the Window menu's
// item i; one destroyed, those after it move down a number, as 3.1 renumbered them. Which child
// is on top is wnd16.c's `order`, which the next/previous commands walk.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_MDICREATE      0x0220
#define WM_MDIDESTROY     0x0221
#define WM_MDIACTIVATE    0x0222
#define WM_MDIRESTORE     0x0223
#define WM_MDINEXT        0x0224
#define WM_MDIMAXIMIZE    0x0225
#define WM_MDITILE        0x0226
#define WM_MDICASCADE     0x0227
#define WM_MDIICONARRANGE 0x0228
#define WM_MDIGETACTIVE   0x0229
#define WM_MDISETMENU     0x0230
#define WM_CHILDACTIVATE  0x0022
#define WM_MDICALCSCROLL  0x003F          // USER's own, posted to the client after a child moved
#define WM_SYSKEYDOWN     0x0104
#define WM_MENUCHAR       0x0120
#define MDITILE_HORIZONTAL   0x0001
#define MDITILE_SKIPDISABLED 0x0002
#define MDIS_ALLCHILDSTYLES  0x0001
#define SC_NEXTWINDOW     0xF040
#define SC_PREVWINDOW     0xF050
#define WS_MINIMIZE       0x20000000u
#define WS_MAXIMIZE       0x01000000u
#define SB_LINEUP         0
#define SB_LINEDOWN       1
#define SB_PAGEUP         2
#define SB_PAGEDOWN       3
#define SB_THUMBPOSITION  4
#define SB_TOP            6
#define SB_BOTTOM         7
#define SB_ENDSCROLL      8
#define VK_TAB 0x09
#define VK_F4  0x73
#define VK_F6  0x75

struct mdi {
    uint16_t win_menu, first_id;   // CLIENTCREATESTRUCT's: the Window menu, the first child's id
    uint16_t *kid;                 // the children, in id order
    int n, cap;
    int listed;                    // the items this file appended to the Window menu
    uint16_t active, maxed;        // the active child; the maximized one, 0 none
    char *title;                   // the frame's own title, before " - [child]"
};

static uint32_t mdi_client_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);

static struct mdi *mdi_of(struct w16_wnd *client) {
    return client && client->host == mdi_client_proc ? client->ctl : 0;
}

struct w16_wnd *w16_mdi_client_of(struct w16 *w, const struct w16_wnd *child) {
    if (!child || child->kind != W16_K_CHILD) return 0;
    struct w16_wnd *p = w16_wnd_get(w, child->parent);
    return mdi_of(p) ? p : 0;
}

static int find(const struct mdi *m, uint16_t h) {
    for (int i = 0; i < m->n; i++)
        if (m->kid[i] == h) return i;
    return -1;
}

static void set_text(struct w16 *w, struct w16_wnd *x, const char *t) {
    char *c = strdup(t);
    if (!c) return;
    pthread_mutex_lock(&w->lock);
    free(x->text);
    x->text = c;
    pthread_mutex_unlock(&w->lock);
    if (x->kind != W16_K_CHILD) w16_task_post_arg(w, W16_POST_TITLE, x->hwnd);
}

// The frame's caption: its own title, and the maximized child's in brackets after it.
static void frame_title(struct w16 *w, struct mdi *m, struct w16_wnd *client) {
    struct w16_wnd *f = w16_wnd_get(w, client->parent), *c = m->maxed ? w16_wnd_get(w, m->maxed) : 0;
    if (!f || !m->title) return;
    if (!c) { set_text(w, f, m->title); return; }
    const char *ct = c->text ? c->text : "";
    size_t n = strlen(m->title) + strlen(ct) + 8;
    char *t = malloc(n);
    if (!t) return;
    snprintf(t, n, "%s - [%s]", m->title, ct);
    set_text(w, f, t);
    free(t);
}

// The Window menu's list: a separator, then "&1 Title" for each child, the active one checked; past
// nine, "&More Windows...". Rebuilt whole whenever a child comes, goes or is renamed.
static void window_menu(struct w16 *w, struct mdi *m) {
    if (!m->win_menu) return;
    int n = w16_menu_count(w, m->win_menu);
    if (n < 0) { m->win_menu = 0; m->listed = 0; return; }
    if (m->listed) w16_menu_cut_at(w, m->win_menu, n - m->listed, n);
    m->listed = 0;
    if (!m->n) return;
    if (w16_menu_put(w, m->win_menu, -1, "", 0, W16_MI_SEPARATOR, 0, 0)) m->listed++;
    for (int i = 0; i < m->n && i < 9; i++) {
        struct w16_wnd *c = w16_wnd_get(w, m->kid[i]);
        const char *t = c && c->text ? c->text : "";
        size_t len = strlen(t) + 8;
        char *s = malloc(len);
        if (!s) break;
        snprintf(s, len, "&%d %s", i + 1, t);
        if (w16_menu_put(w, m->win_menu, -1, s, (uint16_t)(m->first_id + i), m->kid[i] == m->active ? W16_MI_CHECKED : 0, 0, 0))
            m->listed++;
        free(s);
    }
    if (m->n > 9 && w16_menu_put(w, m->win_menu, -1, "&More Windows...", (uint16_t)(m->first_id + 9), 0, 0, 0))
        m->listed++;
}

// A maximized child's control menu, as 3.11 shows it in the frame's bar: Restore the one item lit.
static uint16_t child_sysmenu(struct w16 *w) {
    uint16_t h = w16_menu_create(w);
    if (!h) return 0;
    static const struct { const char *t; uint16_t id; unsigned f; } it[] = {
        { "&Restore", SC_RESTORE, 0 }, { "&Move", SC_MOVE, W16_MI_GRAYED }, { "&Size", SC_SIZE, W16_MI_GRAYED },
        { "Mi&nimize", SC_MINIMIZE, 0 }, { "Ma&ximize", SC_MAXIMIZE, W16_MI_GRAYED }, { "", 0, W16_MI_SEPARATOR },
        { "&Close\tCtrl+F4", SC_CLOSE, 0 }, { "", 0, W16_MI_SEPARATOR }, { "Nex&t\tCtrl+F6", SC_NEXTWINDOW, 0 },
    };
    for (unsigned i = 0; i < sizeof it / sizeof it[0]; i++) w16_menu_put(w, h, -1, it[i].t, it[i].id, it[i].f, 0, 0);
    return h;
}

// The frame's bar while a child is maximized: its control-menu box first, Restore at the right end.
static void bar_items(struct w16 *w, struct w16_wnd *client, int on) {
    struct w16_wnd *f = w16_wnd_get(w, client->parent);
    uint16_t bar = f ? f->hmenu : 0;
    int n = bar ? w16_menu_count(w, bar) : -1;
    if (n < 0) return;
    if (n > 0 && w16_menu_bmp_at(w, bar, n - 1) == W16_MBMP_RESTORE) w16_menu_cut_at(w, bar, n - 1, n);
    if (w16_menu_bmp_at(w, bar, 0) == W16_MBMP_CHILDBOX) w16_menu_cut_at(w, bar, 0, 1);
    if (!on) return;
    uint16_t sys = child_sysmenu(w);
    if (sys && !w16_menu_put(w, bar, 0, "", 0, W16_MI_POPUP, sys, W16_MBMP_CHILDBOX)) w16_menu_destroy(w, sys);
    w16_menu_put(w, bar, -1, "", SC_RESTORE, W16_MI_HELP, 0, W16_MBMP_RESTORE);
}

// The children of the client, top first, that are shown (and enabled, `enabled_only`).
static struct w16_wnd **z_list(struct w16 *w, struct w16_wnd *client, int *n, int enabled_only) {
    int k, out = 0;
    struct w16_wnd **v = w16_wnd_children(w, client->hwnd, &k);   // bottom first
    struct w16_wnd **top = v ? malloc(sizeof *top * (size_t)(k + 1)) : 0;
    for (int i = k - 1; top && i >= 0; i--)
        if (v[i]->visible && !v[i]->destroyed && !(enabled_only && (v[i]->style & WS_DISABLED))) top[out++] = v[i];
    free(v);
    *n = top ? out : 0;
    return top;
}

static void post_calc(struct w16 *w, struct w16_wnd *client) {
    if (client->style & (WS_HSCROLL | WS_VSCROLL)) w16_post_msg(w, client->hwnd, WM_MDICALCSCROLL, 0, 0);
}

// CHILD MADE THE ACTIVE ONE, as 3.1's MDI did: the one before hears it is no longer active
// (WM_NCACTIVATE, WM_MDIACTIVATE), the new one comes to the top, lit, with the focus, and hears it
// is. While one is maximized, the new one is shown maximized in its place.
static void activate(struct w16 *w, struct w16_wnd *client, uint16_t child) {
    struct mdi *m = mdi_of(client);
    if (!m) return;
    uint16_t prev = m->active, ch = client->hwnd;
    struct w16_wnd *c = child ? w16_wnd_get(w, child) : 0;
    if (child && !c) return;
    if (prev == child) {
        if (c && w->focus != child && !w16_wnd_is_child(w, child, w->focus)) w16_set_focus(w, child);
        return;
    }
    m->active = child;
    if (prev && w16_wnd_get(w, prev)) {
        w16_send(w, prev, WM_NCACTIVATE, 0, 0);
        w16_send(w, prev, WM_MDIACTIVATE, 0, (uint32_t)prev << 16 | child);
    }
    if (!(client = w16_wnd_get(w, ch)) || !(m = mdi_of(client))) return;
    if (m->win_menu) {
        if (prev) w16_menu_check(w, m->win_menu, (uint16_t)(m->first_id + find(m, prev)), 0);
        if (child) w16_menu_check(w, m->win_menu, (uint16_t)(m->first_id + find(m, child)), 1);
    }
    if (!child || !(c = w16_wnd_get(w, child))) return;
    pthread_mutex_lock(&w->lock);
    w16_wnd_to_top(c);
    pthread_mutex_unlock(&w->lock);
    w16_invalidate(w, c, 1);
    if (m->maxed && m->maxed != child && !c->maximized) w16_child_state(w, c, SC_MAXIMIZE);
    if (!w16_wnd_get(w, child)) return;
    w16_send(w, child, WM_NCACTIVATE, 1, 0);
    if (w16_wnd_get(w, child)) w16_set_focus(w, child);
    if (w16_wnd_get(w, child)) w16_send(w, child, WM_MDIACTIVATE, 1, (uint32_t)prev << 16 | child);
}

// The next child down from `from` in z order (wrapping to the top), or the bottom one (`prev`).
static uint16_t next_child(struct w16 *w, struct w16_wnd *client, uint16_t from, int prev) {
    int n;
    struct w16_wnd **v = z_list(w, client, &n, 1);
    uint16_t to = 0;
    if (v && n > 1) {
        int at = 0;
        while (at < n && v[at]->hwnd != from) at++;
        to = prev ? v[n - 1]->hwnd : v[at + 1 < n ? at + 1 : 0]->hwnd;
        if (to == from) to = 0;
    }
    free(v);
    return to;
}

// WM_MDIDESTROY: the next child takes over as active (and maximized, if this one was), the rest
// renumbered, and the window destroyed.
static void destroy_child(struct w16 *w, struct w16_wnd *client, uint16_t child) {
    struct mdi *m = mdi_of(client);
    int at = m ? find(m, child) : -1;
    if (at < 0) return;
    uint16_t ch = client->hwnd;
    if (m->active == child) {
        uint16_t next = next_child(w, client, child, 0);
        if (m->maxed == child) {                 // the next one is maximized in its place, this one not restored
            m->maxed = 0;
            if (!next) {
                bar_items(w, client, 0);
                frame_title(w, m, client);
            } else {
                struct w16_wnd *nx = w16_wnd_get(w, next);
                if (nx && !nx->maximized) w16_child_state(w, nx, SC_MAXIMIZE);
            }
        }
        if (!(client = w16_wnd_get(w, ch)) || !(m = mdi_of(client))) return;
        if (next) activate(w, client, next);
        else {
            m->active = 0;
            if (w->focus == child || w16_wnd_is_child(w, child, w->focus)) w16_set_focus(w, ch);
        }
        if (!(client = w16_wnd_get(w, ch)) || !(m = mdi_of(client))) return;
    }
    if (m->maxed == child) {
        m->maxed = 0;
        bar_items(w, client, 0);
        frame_title(w, m, client);
    }
    if ((at = find(m, child)) < 0) return;
    memmove(&m->kid[at], &m->kid[at + 1], sizeof m->kid[0] * (size_t)(m->n - at - 1));
    m->n--;
    for (int i = at; i < m->n; i++) {
        struct w16_wnd *c = w16_wnd_get(w, m->kid[i]);
        if (c) c->id = (uint16_t)(m->first_id + i);
    }
    window_menu(w, m);
    struct w16_wnd *c = w16_wnd_get(w, child);
    if (c) w16_wnd_destroy(w, c);
    if ((client = w16_wnd_get(w, ch))) post_calc(w, client);
}

// A new child's place when the program leaves it to Windows (CW_USEDEFAULT), and its size: 3.1's
// cascade -- each a caption's height and a frame's width further down and right, starting again at
// the corner once a third of the client is used, the size what is left. `total` is how many are
// already placed; `icons` leaves the row of icons free.
static void cascade_pos(const struct w16_wnd *client, int total, int icons, int *x, int *y, int *cw, int *ch) {
    int spacing = W16_CAPTION + W16_FRAME - 1, h = client->ch;
    if (icons && h - W16_ICON_SPACING >= spacing) h -= W16_ICON_SPACING;
    int stagger = h / (3 * spacing);
    if (stagger < 1) stagger = 1;
    *cw = client->cw - stagger * spacing;
    *ch = h - stagger * spacing;
    *x = *y = spacing * (total % (stagger + 1));
}

static int has_icons(struct w16 *w, struct w16_wnd *client) {
    int n, r = 0;
    struct w16_wnd **v = z_list(w, client, &n, 0);
    for (int i = 0; v && i < n; i++) r |= v[i]->iconic;
    free(v);
    return r;
}

// THE ICONS' PLACES, as 3.1 arranged them: a row along the foot of the parent's client area, left to
// right, W16_ICON_SPACING apart, the next row above when one is full. Slot `i`'s minimized window.
void w16_icon_slot(const struct w16_wnd *parent, int i, int *x, int *y) {
    int per = parent->cw / W16_ICON_SPACING;
    if (per < 1) per = 1;
    *x = W16_ICON_X0 + (i % per) * W16_ICON_SPACING;
    *y = parent->ch - W16_ICON_Y0 - (i / per) * W16_ICON_SPACING;
}

// The first slot no other minimized child of x's parent is in.
int w16_icon_free_slot(struct w16 *w, const struct w16_wnd *x) {
    struct w16_wnd *p = w16_wnd_get(w, x->parent);
    if (!p) return 0;
    int n;
    struct w16_wnd **v = w16_wnd_children(w, p->hwnd, &n);
    int slot = 0;
    for (int again = 1; again;) {
        again = 0;
        int sx, sy;
        w16_icon_slot(p, slot, &sx, &sy);
        for (int i = 0; v && i < n; i++)
            if (v[i] != x && v[i]->iconic && v[i]->visible && v[i]->x == sx && v[i]->y == sy) { slot++; again = 1; break; }
    }
    free(v);
    return slot;
}

// ArrangeIconicWindows' and WM_MDIICONARRANGE's work: every minimized child into the slots in
// turn, in the order they are in now (left to right, the bottom row first).
int w16_arrange_icons(struct w16 *w, struct w16_wnd *parent) {
    int n, k = 0;
    struct w16_wnd **v = w16_wnd_children(w, parent->hwnd, &n);
    for (int i = 0; v && i < n; i++)
        if (v[i]->iconic && v[i]->visible) v[k++] = v[i];
    for (int i = 1; i < k; i++)                  // by row from the bottom, then left to right
        for (int j = i; j > 0 && (v[j]->y > v[j - 1]->y || (v[j]->y == v[j - 1]->y && v[j]->x < v[j - 1]->x)); j--) {
            struct w16_wnd *t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    uint16_t *h = k ? malloc(sizeof *h * (size_t)k) : 0;
    for (int i = 0; h && i < k; i++) h[i] = v[i]->hwnd;
    free(v);
    for (int i = 0; h && i < k; i++) {
        struct w16_wnd *c = w16_wnd_get(w, h[i]);
        int x, y;
        w16_icon_slot(parent, i, &x, &y);
        if (c) w16_move_to(w, c, x, y, c->w, c->h);
    }
    free(h);
    return k ? W16_ICON_SPACING : 0;
}

static void cascade(struct w16 *w, struct w16_wnd *client, unsigned how) {
    struct mdi *m = mdi_of(client);
    if (m && m->maxed) {
        struct w16_wnd *c = w16_wnd_get(w, m->maxed);
        if (c) w16_child_state(w, c, SC_RESTORE);
    }
    int n, k = 0;
    struct w16_wnd **v = z_list(w, client, &n, (how & MDITILE_SKIPDISABLED) != 0);
    for (int i = 0; v && i < n; i++)
        if (!v[i]->iconic) v[k++] = v[i];
    int icons = has_icons(w, client);
    for (int i = k - 1, at = 0; i >= 0; i--, at++) {   // the bottom one at the corner, the top one last
        int x, y, cw, ch;
        cascade_pos(client, at, icons, &x, &y, &cw, &ch);
        w16_move_to(w, v[i], x, y, cw, ch);
    }
    free(v);
    if (icons) w16_arrange_icons(w, client);
}

// 3.1's tile: as many rows as the square root of the count, the columns made up of what is left,
// the last column taking the rest; MDITILE_HORIZONTAL swaps rows and columns.
static void tile(struct w16 *w, struct w16_wnd *client, unsigned how) {
    struct mdi *m = mdi_of(client);
    if (m && m->maxed) {
        struct w16_wnd *c = w16_wnd_get(w, m->maxed);
        if (c) w16_child_state(w, c, SC_RESTORE);
    }
    int n, k = 0;
    struct w16_wnd **v = z_list(w, client, &n, (how & MDITILE_SKIPDISABLED) != 0);
    for (int i = 0; v && i < n; i++)
        if (!v[i]->iconic) v[k++] = v[i];
    if (k) {
        int icons = has_icons(w, client), H = client->ch - (icons ? W16_ICON_SPACING : 0), W = client->cw;
        int rows = 1;
        while ((rows + 1) * (rows + 1) <= k) rows++;
        int cols = k / rows;
        if (how & MDITILE_HORIZONTAL) { int t = rows; rows = cols; cols = t; }
        int ys = H / rows, xs = W / cols, i = 0, x = 0;
        for (int c = 1; c <= cols && i < k; c++) {
            if (c == cols) { rows = k - i; ys = H / rows; }
            int y = 0;
            for (int r = 1; r <= rows && i < k; r++, i++) {
                w16_move_to(w, v[i], x, y, xs, ys);
                y += ys;
            }
            x += xs;
        }
    }
    free(v);
    if (has_icons(w, client)) w16_arrange_icons(w, client);
}

// CalcChildScroll's work: the client's scroll bars range over its children wherever they are, so
// one moved out of sight can be scrolled back to; none while a child is maximized.
static void calc_scroll(struct w16 *w, struct w16_wnd *client, int which) {
    struct mdi *m = mdi_of(client);
    int n, l = 0, t = 0, r = client->cw, b = client->ch;
    struct w16_wnd **v = z_list(w, client, &n, 0);
    int maxed = m && m->maxed;
    for (int i = 0; v && i < n && !maxed; i++) {
        if (v[i]->maximized) maxed = 1;
        if (v[i]->x < l) l = v[i]->x;
        if (v[i]->y < t) t = v[i]->y;
        if (v[i]->x + v[i]->w > r) r = v[i]->x + v[i]->w;
        if (v[i]->y + v[i]->h > b) b = v[i]->y + v[i]->h;
    }
    free(v);
    if (maxed) l = t = 0, r = client->cw, b = client->ch;
    // In the children's own coordinates: the range from the leftmost child to where the view would
    // show the rightmost, and the view, which is always at 0 -- scrolling moves the children.
    if ((which == SB_HORZ || which == 3) && (client->style & WS_HSCROLL))
        w16_sbar_set(w, client, SB_HORZ, l, r - client->cw, 0, 1);
    if ((which == SB_VERT || which == 3) && (client->style & WS_VSCROLL))
        w16_sbar_set(w, client, SB_VERT, t, b - client->ch, 0, 1);
}

// ScrollChildren's work: the client scrolled, which moves every child the other way.
static void scroll_children(struct w16 *w, struct w16_wnd *client, int bar, unsigned code, int thumb) {
    struct w16_sbar *s = &client->sb[bar];
    int pos = s->pos, len = (bar == SB_VERT ? client->ch : client->cw) / 2, shift = W16_SB, to = pos;
    switch (code) {
    case SB_LINEUP: to = pos - shift; break;
    case SB_LINEDOWN: to = pos + shift; break;
    case SB_PAGEUP: to = pos - len; break;
    case SB_PAGEDOWN: to = pos + len; break;
    case SB_THUMBPOSITION: to = thumb; break;
    case SB_TOP: to = s->min; break;
    case SB_BOTTOM: to = s->max; break;
    case SB_ENDSCROLL: calc_scroll(w, client, bar); return;
    default: return;
    }
    if (to < s->min) to = s->min;
    if (to > s->max) to = s->max;
    if (to == pos) return;
    w16_sbar_set(w, client, bar, s->min, s->max, to, 1);
    int n, d = pos - to;
    struct w16_wnd **v = z_list(w, client, &n, 0);
    uint16_t *h = n ? malloc(sizeof *h * (size_t)n) : 0;
    for (int i = 0; h && i < n; i++) h[i] = v[i]->hwnd;
    free(v);
    for (int i = 0; h && i < n; i++) {
        struct w16_wnd *c = w16_wnd_get(w, h[i]);
        if (c) w16_move_to(w, c, c->x + (bar == SB_HORZ ? d : 0), c->y + (bar == SB_VERT ? d : 0), c->w, c->h);
    }
    free(h);
}

// WM_MDICREATE: an MDICREATESTRUCT -- szClass, szTitle, hOwner, x, y, cx, cy, style, lParam -- made a
// child, which hears WM_CREATE with lpCreateParams pointing at that same structure, as in 3.1.
static uint16_t create_child(struct w16 *w, struct w16_wnd *client, uint32_t lp) {
    struct mdi *m = mdi_of(client);
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 26, 0);
    if (!m || !p) return 0;
    uint32_t cname, title, style;
    int16_t x, y, cx, cy;
    uint16_t inst;
    memcpy(&cname, p, 4);
    memcpy(&title, p + 4, 4);
    memcpy(&inst, p + 8, 2);
    memcpy(&x, p + 10, 2);
    memcpy(&y, p + 12, 2);
    memcpy(&cx, p + 14, 2);
    memcpy(&cy, p + 16, 2);
    memcpy(&style, p + 18, 4);
    char *name = w16_class_name(w, cname), *text = (title >> 16) ? w16_far_strdup(w, title) : strdup("");
    int k = name ? w16_class_find(w, name) : -1;
    free(name);
    if (k < 0 || !text) { free(text); return 0; }
    uint32_t keep = style & (WS_MINIMIZE | WS_MAXIMIZE | WS_HSCROLL | WS_VSCROLL);
    if (client->style & MDIS_ALLCHILDSTYLES) keep = style;
    style = keep | WS_CHILD | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_VISIBLE;
    if (m->maxed && !(style & WS_MINIMIZE)) style |= WS_MAXIMIZE;   // shown maximized as the one before was
    int dx, dy, dw, dh;
    cascade_pos(client, m->n, has_icons(w, client), &dx, &dy, &dw, &dh);
    if (x == W16_CW_USEDEFAULT) { x = (int16_t)dx; y = (int16_t)dy; }
    if (cx == W16_CW_USEDEFAULT || !cx) { cx = (int16_t)dw; cy = (int16_t)dh; }
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        uint16_t *g = realloc(m->kid, sizeof *g * (size_t)cap);
        if (!g) { free(text); return 0; }
        m->kid = g;
        m->cap = cap;
    }
    uint16_t ch = client->hwnd, id = (uint16_t)(m->first_id + m->n);
    m->kid[m->n++] = 0;                          // its place, while WM_CREATE runs
    struct w16_wnd *c = w16_wnd_create(w, k, text, style & ~(WS_MINIMIZE | WS_MAXIMIZE), x, y, cx, cy, ch, id, inst, lp);
    free(text);
    if (!(client = w16_wnd_get(w, ch)) || !(m = mdi_of(client))) return 0;
    int at = find(m, 0);
    if (!c) {
        if (at >= 0) {
            memmove(&m->kid[at], &m->kid[at + 1], sizeof m->kid[0] * (size_t)(m->n - at - 1));
            m->n--;
        }
        return 0;
    }
    uint16_t h = c->hwnd;
    if (at >= 0) m->kid[at] = h;
    pthread_mutex_lock(&w->lock);
    c->style |= style & (WS_MINIMIZE | WS_MAXIMIZE);
    pthread_mutex_unlock(&w->lock);
    window_menu(w, m);
    if ((style & WS_MAXIMIZE) && (c = w16_wnd_get(w, h))) w16_child_state(w, c, SC_MAXIMIZE);
    if ((style & WS_MINIMIZE) && (c = w16_wnd_get(w, h))) w16_child_state(w, c, SC_MINIMIZE);
    if ((client = w16_wnd_get(w, ch))) activate(w, client, h);
    if ((client = w16_wnd_get(w, ch))) post_calc(w, client);
    return w16_wnd_get(w, h) ? h : 0;
}

// WM_MDISETMENU: the frame's menu bar and the Window menu replaced (3.1's 16-bit form: lParam the
// two handles, wParam TRUE only to refresh the list); the bar it had, returned.
static uint32_t set_menu(struct w16 *w, struct w16_wnd *client, uint16_t refresh, uint32_t lp) {
    struct mdi *m = mdi_of(client);
    struct w16_wnd *f = w16_wnd_get(w, client->parent);
    if (!m || !f) return 0;
    uint16_t old = f->hmenu, bar = (uint16_t)lp, win = (uint16_t)(lp >> 16);
    if (refresh) { window_menu(w, m); return old; }
    if (win && win != m->win_menu) {
        int n = m->win_menu ? w16_menu_count(w, m->win_menu) : -1;
        if (n >= 0 && m->listed) w16_menu_cut_at(w, m->win_menu, n - m->listed, n);
        m->listed = 0;
        m->win_menu = win;
    }
    if (bar && bar != old) {
        if (m->maxed) bar_items(w, client, 0);
        w16_set_menu(w, f, bar);
        if (m->maxed) bar_items(w, client, 1);
    }
    window_menu(w, m);
    return old;
}

static uint32_t mdi_client_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct mdi *m = mdi_of(x);
    switch (msg) {
    case WM_CREATE: {
        // CREATESTRUCT's lpCreateParams: a CLIENTCREATESTRUCT -- the Window menu, the first child's id.
        const uint8_t *cs = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 4, 0);
        uint32_t ccs = 0;
        if (cs) memcpy(&ccs, cs, 4);
        const uint8_t *cc = ccs >> 16 ? cpu16_ptr(&w->cpu, (uint16_t)(ccs >> 16), (uint16_t)ccs, 4, 0) : 0;
        struct mdi *n = calloc(1, sizeof *n);
        if (!n) return 0xFFFFFFFFu;
        if (cc) {
            memcpy(&n->win_menu, cc, 2);
            memcpy(&n->first_id, cc + 2, 2);
        }
        struct w16_wnd *f = w16_wnd_get(w, x->parent);
        n->title = strdup(f && f->text ? f->text : "");
        pthread_mutex_lock(&w->lock);
        x->ctl = n;
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    case WM_NCDESTROY:
        if (m) {
            free(m->kid);
            free(m->title);
            m->kid = 0;
            m->title = 0;
            m->n = 0;
        }
        return 0;
    case WM_MDICREATE:
        return create_child(w, x, lp);
    case WM_MDIDESTROY:
        destroy_child(w, x, wp);
        return 0;
    case WM_MDIACTIVATE:
        if (m && find(m, wp) >= 0) activate(w, x, wp);
        return 0;
    case WM_MDIRESTORE:
    case WM_MDIMAXIMIZE:
        if (m && find(m, wp) >= 0) w16_send(w, wp, WM_SYSCOMMAND, msg == WM_MDIRESTORE ? SC_RESTORE : SC_MAXIMIZE, 0);
        return 0;
    case WM_MDINEXT: {
        if (!m) return 0;
        uint16_t from = wp && find(m, wp) >= 0 ? wp : m->active, to = next_child(w, x, from, lp != 0);
        uint16_t ch = x->hwnd;
        if (!to) return 0;
        activate(w, x, to);
        struct w16_wnd *old = w16_wnd_get(w, from);
        if (!lp && old && (x = w16_wnd_get(w, ch))) {   // the one left goes to the bottom, as 3.1 put it
            w16_wnd_to_bottom(w, old);
            w16_invalidate(w, x, 1);
        }
        return 0;
    }
    case WM_MDITILE:
        tile(w, x, wp);
        return 0;
    case WM_MDICASCADE:
        cascade(w, x, wp);
        return 0;
    case WM_MDIICONARRANGE:
        w16_arrange_icons(w, x);
        return 0;
    case WM_MDIGETACTIVE:
        return m ? (uint32_t)(m->maxed && m->maxed == m->active) << 16 | m->active : 0;
    case WM_MDISETMENU:
        return set_menu(w, x, wp, lp);
    case WM_MDICALCSCROLL:
        calc_scroll(w, x, 3);
        return 0;
    case WM_SETFOCUS:                            // the focus goes on to the active child
        if (m && m->active && w16_wnd_get(w, m->active)) w16_set_focus(w, m->active);
        return 0;
    case WM_NCACTIVATE:                          // the frame's caption lit or dimmed: the active child's with it
        if (m && m->active && w16_wnd_get(w, m->active)) w16_send(w, m->active, WM_NCACTIVATE, wp, lp);
        return 1;
    case WM_SIZE:
        if (m && m->maxed) {                     // the maximized child follows the client's size
            struct w16_wnd *c = w16_wnd_get(w, m->maxed);
            int f = c ? w16_border_of(c->style) : 0;
            if (c) w16_move_to(w, c, -f, -f - W16_CAPTION, x->cw + 2 * f, x->ch + 2 * f + W16_CAPTION);
        }
        if ((x = w16_wnd_get(w, x->hwnd))) post_calc(w, x);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL:
        scroll_children(w, x, msg == WM_VSCROLL ? SB_VERT : SB_HORZ, wp, (int16_t)lp);
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}

void w16_mdi_register(struct w16 *w) {
    w16_class_add_host(w, "MDICLIENT", mdi_client_proc, 0, 12 + 1);   // COLOR_APPWORKSPACE
}

// A press in a child of an MDI client, or in one of its own children, activates that child first
// -- 3.1's WM_MOUSEACTIVATE and WM_CHILDACTIVATE, between taking the press and the program seeing it.
void w16_mdi_press(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    for (int depth = 0; x && x->kind == W16_K_CHILD && depth < 32; depth++) {
        struct w16_wnd *client = w16_mdi_client_of(w, x);
        if (client) {
            struct mdi *m = mdi_of(client);
            if (m && m->active != x->hwnd) w16_send(w, x->hwnd, WM_CHILDACTIVATE, 0, 0);
            return;
        }
        x = w16_wnd_get(w, x->parent);
    }
}

// --- the calls ------------------------------------------------------------------------

// DefFrameProc(hWnd, hWndMDIClient, wMsg, wParam, lParam) -> LONG
// Windows: An MDI frame's default: a Window menu item activates its child, a control-menu command
// from the bar (a maximized child's) goes to the active child, the frame's size goes to the client,
// its focus to the active child, and its title keeps the maximized child's after it.
// w16: The same.
static int api_DefFrameProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), hc = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a), r = 0;
    struct w16_wnd *x = w16_wnd_get(w, hwnd), *client = w16_wnd_get(w, hc);
    struct mdi *m = mdi_of(client);
    if (!x) { w->cpu.r[R_AX] = w->cpu.r[R_DX] = 0; return 12; }
    if (!m) {
        r = w16_def_window_proc(w, x, msg, wp, lp);
    } else switch (msg) {
    case WM_COMMAND:
        if (wp >= m->first_id && wp < m->first_id + 9 && wp - m->first_id < m->n) {
            uint16_t c = m->kid[wp - m->first_id];
            struct w16_wnd *cw = w16_wnd_get(w, c);
            if (cw && cw->iconic) w16_send(w, c, WM_SYSCOMMAND, SC_RESTORE, 0);
            if ((client = w16_wnd_get(w, hc))) activate(w, client, c);
        } else if (wp == m->first_id + 9 && m->n > 9) {
            uint16_t c = w16_mdi_more_windows(w, client, m->kid, m->n, m->active);
            if (c && (client = w16_wnd_get(w, hc))) activate(w, client, c);
        } else if (wp >= 0xF000 && m->active) {  // the maximized child's control menu, in the bar
            w16_send(w, m->active, WM_SYSCOMMAND, wp, lp);
        } else {
            r = w16_def_window_proc(w, x, msg, wp, lp);
        }
        break;
    case WM_NCACTIVATE:
        w16_send(w, hc, WM_NCACTIVATE, wp, lp);
        if ((x = w16_wnd_get(w, hwnd))) r = w16_def_window_proc(w, x, msg, wp, lp);
        break;
    case WM_SETTEXT: {
        char *t = w16_far_strdup(w, lp);
        if (t) {
            free(m->title);
            m->title = t;
            frame_title(w, m, client);
        }
        r = 1;
        break;
    }
    case WM_SETFOCUS:
        w16_set_focus(w, hc);
        break;
    case WM_SIZE:
        w16_move_to(w, client, 0, 0, (int16_t)lp, (int16_t)(lp >> 16));
        break;
    case WM_MENUCHAR:                            // Alt+- opens the active child's control menu
        if (wp == '-' && m->active) {
            w16_send(w, m->active, WM_SYSCOMMAND, SC_KEYMENU, '-');
            r = 0x00010000u;
        }
        break;
    default:
        r = w16_def_window_proc(w, x, msg, wp, lp);
        break;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(r >> 16);
    return 12;
}

// DefMDIChildProc(hWnd, wMsg, wParam, lParam) -> LONG
// Windows: An MDI child's default: closing asks its client to destroy it, a press or the focus
// activates it, Next and Previous go round the children, and while maximized its title and control
// menu are the frame's.
// w16: The same.
static int api_DefMDIChildProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a), r = 0;
    struct w16_wnd *x = w16_wnd_get(w, hwnd), *client = x ? w16_mdi_client_of(w, x) : 0;
    struct mdi *m = mdi_of(client);
    uint16_t hc = client ? client->hwnd : 0;
    if (!x) { w->cpu.r[R_AX] = w->cpu.r[R_DX] = 0; return 10; }
    if (!m) {
        r = w16_def_window_proc(w, x, msg, wp, lp);
    } else switch (msg) {
    case WM_SETTEXT:
        r = w16_def_window_proc(w, x, msg, wp, lp);
        if ((client = w16_wnd_get(w, hc)) && (m = mdi_of(client))) {
            window_menu(w, m);
            if (m->maxed == hwnd) frame_title(w, m, client);
        }
        break;
    case WM_CLOSE:
        w16_send(w, hc, WM_MDIDESTROY, hwnd, 0);
        break;
    case WM_SETFOCUS:
        if (m->active != hwnd) activate(w, client, hwnd);
        break;
    case WM_CHILDACTIVATE:
        activate(w, client, hwnd);
        break;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_MOVE: case SC_SIZE:
            if (!x->maximized) r = w16_def_window_proc(w, x, msg, wp, lp);
            break;
        case SC_NEXTWINDOW:
            w16_send(w, hc, WM_MDINEXT, 0, 0);
            break;
        case SC_PREVWINDOW:
            w16_send(w, hc, WM_MDINEXT, 0, 1);
            break;
        case SC_KEYMENU:
            w16_task_post_arg(w, W16_POST_CHILD_MENU, hwnd);
            break;
        default:
            r = w16_def_window_proc(w, x, msg, wp, lp);
            break;
        }
        break;
    case WM_SIZE:
        // Maximized: the frame's bar and title take it on, and one maximized before is restored.
        // No longer maximized: they let it go.
        if (x->maximized && m->maxed != hwnd) {
            uint16_t old = m->maxed;
            struct w16_wnd *o = old ? w16_wnd_get(w, old) : 0;
            if (o && o->maximized) w16_child_state(w, o, SC_RESTORE);
            if ((client = w16_wnd_get(w, hc)) && (m = mdi_of(client))) {
                int had = m->maxed != 0;
                m->maxed = hwnd;
                if (!had) bar_items(w, client, 1);
                frame_title(w, m, client);
            }
        } else if (!x->maximized && m->maxed == hwnd) {
            m->maxed = 0;
            bar_items(w, client, 0);
            frame_title(w, m, client);
        }
        if ((client = w16_wnd_get(w, hc))) post_calc(w, client);
        if ((x = w16_wnd_get(w, hwnd))) r = w16_def_window_proc(w, x, msg, wp, lp);
        break;
    case WM_MOVE:
        post_calc(w, client);
        r = w16_def_window_proc(w, x, msg, wp, lp);
        break;
    case WM_MENUCHAR:
        if (wp == '-') {
            w16_task_post_arg(w, W16_POST_CHILD_MENU, hwnd);
            r = 0x00010000u;
        }
        break;
    default:
        r = w16_def_window_proc(w, x, msg, wp, lp);
        break;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(r >> 16);
    return 10;
}

// TranslateMDISysAccel(hWndClient, lpMsg) -> BOOL
// Windows: The MDI keys, for the active child: Ctrl+F4 closes it, Ctrl+F6 (and Ctrl+Tab) goes to the
// next one, with Shift to the one before; TRUE when the key was one of them.
// w16: The same.
static int api_TranslateMDISysAccel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hc = w16_arg16(a);
    uint32_t lpmsg = w16_arg32(a);
    struct cpu16 *c = &w->cpu;
    c->r[R_AX] = 0;
    const uint8_t *p = cpu16_ptr(c, (uint16_t)(lpmsg >> 16), (uint16_t)lpmsg, 10, 0);
    if (!p) return -1;
    uint16_t msg = (uint16_t)(p[2] | p[3] << 8), vk = (uint16_t)(p[4] | p[5] << 8);
    struct mdi *m = mdi_of(w16_wnd_get(w, hc));
    unsigned mods = w->key_mods;
    if (!m || !m->active || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) || !(mods & 2)) return 6;
    struct w16_wnd *act = w16_wnd_get(w, m->active);
    if (!act || (act->style & WS_DISABLED)) return 6;
    unsigned sc = 0;
    if (vk == VK_F4 && !(mods & 4)) sc = SC_CLOSE;
    else if (vk == VK_F6 || vk == VK_TAB) sc = mods & 1 ? SC_PREVWINDOW : SC_NEXTWINDOW;
    if (!sc) return 6;
    w16_send(w, m->active, WM_SYSCOMMAND, (uint16_t)sc, vk);
    c->r[R_AX] = 1;
    return 6;
}

// CalcChildScroll(hWnd, wScroll) -> void
// Windows: Sets an MDI client's scroll bars to range over its children (SB_HORZ, SB_VERT, SB_BOTH),
// so a child moved out of sight can be scrolled back to.
// w16: The same.
static int api_CalcChildScroll(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t which = w16_arg16(a);
    if (x) calc_scroll(w, x, which);
    return 4;
}

// ScrollChildren(hWnd, wMsg, wParam, lParam) -> void
// Windows: WM_HSCROLL or WM_VSCROLL done for an MDI client: its children moved by a line, a page or
// to the thumb.
// w16: The same.
static int api_ScrollChildren(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *x = w16_wnd_get(w, w16_arg16(a));
    uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    if (x && (msg == WM_HSCROLL || msg == WM_VSCROLL)) scroll_children(w, x, msg == WM_VSCROLL ? SB_VERT : SB_HORZ, wp, (int16_t)lp);
    return 10;
}

const struct w16_api g_mdi_api[] = {
    { 445, api_DefFrameProc },
    { 447, api_DefMDIChildProc },
    { 451, api_TranslateMDISysAccel },
    { 462, api_CalcChildScroll },
    { 463, api_ScrollChildren },
};
const unsigned g_mdi_api_n = sizeof g_mdi_api / sizeof g_mdi_api[0];

// --- More Windows... ------------------------------------------------------------------

#define IDOK         1
#define IDCANCEL     2
#define ID_LIST      100
#define LB_ADDSTRING (WM_USER + 1)
#define LB_SETCURSEL (WM_USER + 7)
#define LB_GETCURSEL (WM_USER + 9)
#define LBN_DBLCLK   2
#define LBS_NOTIFY   0x0001u
#define BS_DEFPUSH   0x01u

static int select_proc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (msg == WM_INITDIALOG) return 1;
    if (msg != WM_COMMAND) return 0;
    if (wp == ID_LIST && (lp >> 16) != LBN_DBLCLK) return 1;
    int pick = 0;
    if (wp == IDOK || wp == ID_LIST) {
        struct w16_wnd *l = w16_dlg_item(w, d->hwnd, ID_LIST);
        pick = l ? (int16_t)w16_send(w, l->hwnd, LB_GETCURSEL, 0, 0) + 1 : 0;
    } else if (wp != IDCANCEL) {
        return 0;
    }
    d->result = (int16_t)pick;
    d->ended = 1;
    return 1;
}

// 3.1's Select Window box: every child by title, the active one chosen, OK and Cancel beside.
uint16_t w16_mdi_more_windows(struct w16 *w, struct w16_wnd *client, const uint16_t *kid, int n, uint16_t active) {
    struct w16_wnd *f = w16_wnd_get(w, client->parent);
    int bx, by;
    w16_dlg_units(&bx, &by);
    uint16_t *h = malloc(sizeof *h * (size_t)n);
    if (!h) return 0;
    memcpy(h, kid, sizeof *h * (size_t)n);
    struct w16_wnd *d = w16_dlg_new(w, f ? f->hwnd : 0, 0, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
                                    w16_du_w(186, bx), w16_du_h(80, by), "Select Window", 0, select_proc);
    struct w16_wnd *l = d ? w16_dlg_add(w, d, "LISTBOX", ID_LIST, WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                        w16_du_x(0, 6, bx), w16_du_y(0, 6, by), w16_du_w(120, bx), w16_du_h(68, by), "") : 0;
    int ok = l && w16_dlg_add(w, d, "BUTTON", IDOK, WS_VISIBLE | WS_TABSTOP | BS_DEFPUSH, w16_du_x(0, 132, bx),
                              w16_du_y(0, 6, by), w16_du_w(48, bx), w16_du_h(14, by), "OK") &&
             w16_dlg_add(w, d, "BUTTON", IDCANCEL, WS_VISIBLE | WS_TABSTOP, w16_du_x(0, 132, bx), w16_du_y(0, 24, by),
                         w16_du_w(48, bx), w16_du_h(14, by), "Cancel");
    if (!ok) {
        if (d) w16_wnd_destroy(w, d);
        free(h);
        return 0;
    }
    d->defid = IDOK;
    int sel = 0;
    for (int i = 0; i < n; i++) {
        struct w16_wnd *c = w16_wnd_get(w, h[i]);
        w16_listbox_str(w, l, LB_ADDSTRING, 0, c && c->text ? c->text : "");
        if (h[i] == active) sel = i;
    }
    w16_send(w, l->hwnd, LB_SETCURSEL, (uint16_t)sel, 0);
    w16_dlg_show(w, d, 1, 0);
    int pick = w16_dlg_run(w, d);
    uint16_t r = pick > 0 && pick <= n && w16_wnd_get(w, h[pick - 1]) ? h[pick - 1] : 0;
    free(h);
    return r;
}
