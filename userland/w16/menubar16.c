// menubar16.c -- USER's menus and accelerators: the HMENUs a program
// loads from a MENU resource and changes (CheckMenuItem, EnableMenuItem),
// the menu bar SetMenu puts under the caption, and TranslateAccelerator's
// keyboard shortcuts from an ACCELERATORS resource.
//
// TWO THREADS, AS EVERYWHERE IN w16. The task builds and changes the
// menus; the main thread draws the bar from them and opens a title's
// popup (menu16.c) -- both under `lock`. Opening is a round trip, because
// Windows sends WM_INITMENU and WM_INITMENUPOPUP BEFORE a popup shows so
// a program can tick and grey its items: the main thread asks
// (W16_WM_MENUOPEN), the task sends those to the program's window, then
// tells the main thread to show the popup as it now stands.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

#define HMENU_BASE  0x7800          // HMENU i is HMENU_BASE + 2 * i: even, beside the HRSRCs
#define HMENU_LAST  0x7FFE
#define HACCEL_BASE 0x7600          // HACCEL i is ACCELERATORS resource i
#define SYSMENU     0x7FFF          // GetSystemMenu's: odd, never a loaded one

#define MF_GRAYED     0x0001
#define MF_DISABLED   0x0002
#define MF_CHECKED    0x0008
#define MF_POPUP      0x0010
#define MF_END        0x0080
#define MF_BYPOSITION 0x0400
#define MF_SEP        0x8000        // w16's own mark: a separator

struct w16_mi {
    char *text;
    uint16_t id, flags, sub;        // sub: a popup item's HMENU
};

struct w16_hmenu {
    int used;
    struct w16_mi *it;
    int n, cap;
};

static struct w16_hmenu **g_menus;
static int g_nmenus;

static struct w16_hmenu *menu_of(uint16_t h) {
    if (h < HMENU_BASE || h > HMENU_LAST || (h & 1)) return 0;
    int i = (h - HMENU_BASE) / 2;
    return i < g_nmenus && g_menus[i] && g_menus[i]->used ? g_menus[i] : 0;
}

static uint16_t menu_new(void) {
    int i = 0;
    while (i < g_nmenus && g_menus[i] && g_menus[i]->used) i++;
    if (HMENU_BASE + 2 * i > HMENU_LAST) return 0;   // the handle range is full
    if (i == g_nmenus) {
        struct w16_hmenu **g = realloc(g_menus, sizeof *g * (size_t)(g_nmenus + 1));
        if (!g) return 0;
        g_menus = g;
        g_menus[g_nmenus++] = 0;
    }
    if (!g_menus[i] && !(g_menus[i] = calloc(1, sizeof **g_menus))) return 0;
    memset(g_menus[i], 0, sizeof **g_menus);
    g_menus[i]->used = 1;
    return (uint16_t)(HMENU_BASE + 2 * i);
}

static int menu_append(struct w16_hmenu *m, const char *text, uint16_t id, uint16_t flags, uint16_t sub) {
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        struct w16_mi *g = realloc(m->it, sizeof *g * (size_t)cap);
        if (!g) return -1;
        m->it = g;
        m->cap = cap;
    }
    char *t = strdup(text);
    if (!t) return -1;
    m->it[m->n++] = (struct w16_mi){ t, id, flags, sub };
    return 0;
}

static void menu_destroy(uint16_t h) {
    struct w16_hmenu *m = menu_of(h);
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        if (m->it[i].flags & MF_POPUP) menu_destroy(m->it[i].sub);
        free(m->it[i].text);
    }
    free(m->it);
    memset(m, 0, sizeof *m);
}

// --- a MENU resource ----------------------------------------------------------------

struct reader { const uint8_t *p; uint32_t n, at; int bad; };

static unsigned rd16(struct reader *r) {
    if (r->at + 2 > r->n) { r->bad = 1; return 0; }
    unsigned v = r->p[r->at] | r->p[r->at + 1] << 8;
    r->at += 2;
    return v;
}

static const char *rd_str(struct reader *r) {
    const char *s = (const char *)r->p + r->at;
    while (r->at < r->n && r->p[r->at]) r->at++;
    if (r->at >= r->n) { r->bad = 1; return ""; }
    r->at++;
    return s;
}

// One level of items into `h`, each popup's own items under it, until the
// one carrying MF_END.
static int parse_items(struct reader *r, uint16_t h, int depth) {
    for (;;) {
        unsigned flags = rd16(r);
        uint16_t id = 0, sub = 0;
        if (!(flags & MF_POPUP)) id = (uint16_t)rd16(r);
        const char *text = rd_str(r);
        if (r->bad || depth > 8) return -1;
        if (flags & MF_POPUP) {
            if (!(sub = menu_new()) || parse_items(r, sub, depth + 1) < 0) return -1;
        }
        unsigned keep = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_POPUP);
        if (!(flags & MF_POPUP) && !id && !text[0]) keep |= MF_SEP;
        if (menu_append(menu_of(h), text, id, (uint16_t)keep, sub) < 0) return -1;
        if (flags & MF_END) return 0;
    }
}

// --- the exports --------------------------------------------------------------------

uint16_t w16_load_menu(struct w16 *w, const char *name, uint16_t id) {
    struct ne_resource res;
    uint16_t h = 0;
    if (w16_res_find(w, NE_RT_MENU, 0, name ? 0 : id, name, &res) >= 0) {
        pthread_mutex_lock(&w->lock);
        struct reader r = { w->ne.img + res.off, res.len, 4, 0 };   // after the version and header size
        h = menu_new();
        if (h && parse_items(&r, h, 0) < 0) { menu_destroy(h); h = 0; }
        pthread_mutex_unlock(&w->lock);
    }
    return h;
}

// LoadMenu(hInst, lpName)
static int api_LoadMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint32_t p = w16_arg32(a);
    char name[64] = "";
    if ((p >> 16) && cpu16_str(c, (uint16_t)(p >> 16), (uint16_t)p, name, sizeof name) < 0) return -1;
    c->r[R_AX] = w16_load_menu(w, (p >> 16) ? name : 0, (uint16_t)p);
    return 6;
}

// The item `id` names in `m` -- by command, searching popups too, or by
// position -- and the menu holding it.
static struct w16_mi *find(uint16_t h, uint16_t id, unsigned flags) {
    struct w16_hmenu *m = menu_of(h);
    if (!m) return 0;
    if (flags & MF_BYPOSITION) return id < m->n ? &m->it[id] : 0;
    for (int i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & (MF_POPUP | MF_SEP)) && m->it[i].id == id) return &m->it[i];
        if (m->it[i].flags & MF_POPUP) {
            struct w16_mi *x = find(m->it[i].sub, id, flags);
            if (x) return x;
        }
    }
    return 0;
}

static void bar_changed(struct w16 *w) { w16_task_post(w, W16_POST_PAINT); }

// CheckMenuItem(hMenu, id, flags): the previous state, or -1
static int api_CheckMenuItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), id = w16_arg16(a), flags = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_mi *it = find(h, id, flags);
    int was = it ? it->flags & MF_CHECKED : -1;
    if (it) it->flags = (uint16_t)((it->flags & ~MF_CHECKED) | (flags & MF_CHECKED));
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)was;
    return 6;
}

// EnableMenuItem(hMenu, id, flags): the previous MF_GRAYED|MF_DISABLED, or -1
static int api_EnableMenuItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), id = w16_arg16(a), flags = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_mi *it = find(h, id, flags);
    int was = it ? it->flags & (MF_GRAYED | MF_DISABLED) : -1;
    if (it) it->flags = (uint16_t)((it->flags & ~(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED)));
    pthread_mutex_unlock(&w->lock);
    if (it && h == w->menu) bar_changed(w);
    w->cpu.r[R_AX] = (uint16_t)was;
    return 6;
}

// GetMenuState(hMenu, id, flags) -> the item's MF_* flags (a popup's
// item count in the high byte), or -1
static int api_GetMenuState(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), id = w16_arg16(a), flags = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_mi *it = find(h, id, flags);
    int r = -1;
    if (it) {
        struct w16_hmenu *sub = (it->flags & MF_POPUP) ? menu_of(it->sub) : 0;
        r = (it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_POPUP)) | (it->flags & MF_SEP ? 0x0800 : 0);
        if (sub) r = (r & 0xFF) | (sub->n & 0xFF) << 8;
    }
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)r;
    return 6;
}

// GetMenuItemCount(hMenu) -> its items, or -1
static int api_GetMenuItemCount(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_hmenu *m = menu_of(h);
    w->cpu.r[R_AX] = (uint16_t)(m ? m->n : -1);
    pthread_mutex_unlock(&w->lock);
    return 2;
}

// GetMenuItemID(hMenu, nPos) -> the item's command id; -1 for a popup or none
static int api_GetMenuItemID(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), pos = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_hmenu *m = menu_of(h);
    int r = m && pos < m->n && !(m->it[pos].flags & MF_POPUP) ? m->it[pos].id : -1;
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)r;
    return 4;
}

// GetSystemMenu(hwnd, bRevert): the control menu is w16's own
static int api_GetSystemMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a);
    w->cpu.r[R_AX] = SYSMENU;
    return 4;
}

// GetMenu(hwnd)
static int api_GetMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w->cpu.r[R_AX] = w->menu;
    return 2;
}

// SetMenu(hwnd, hMenu): the bar takes its height from the client area, as
// on Windows -- the window keeps its size and the program hears WM_SIZE.
static int api_SetMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_set_menu(w, w16_arg16(a));
    w->cpu.r[R_AX] = 1;
    return 4;
}

void w16_set_menu(struct w16 *w, uint16_t h) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *win = &w->win;
    int had = win->menubar, has = menu_of(h) != 0;
    w->menu = has ? h : 0;
    win->menubar = has;
    int resized = had != has && win->used && w16_top_fit(win) > 0;
    int cw = win->cw, ch = win->ch;
    pthread_mutex_unlock(&w->lock);
    if (resized) {
        w16_invalidate(w, win, 1);
        w16_post_msg(w, win->hwnd, WM_SIZE, SIZE_RESTORED, (uint32_t)ch << 16 | (uint16_t)cw);
    }
    bar_changed(w);
}

// GetSubMenu(hMenu, nPos)
static int api_GetSubMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a), pos = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    struct w16_hmenu *m = menu_of(h);
    w->cpu.r[R_AX] = m && pos < m->n && (m->it[pos].flags & MF_POPUP) ? m->it[pos].sub : 0;
    pthread_mutex_unlock(&w->lock);
    return 4;
}

// DrawMenuBar(hwnd)
static int api_DrawMenuBar(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    bar_changed(w);
    return 2;
}

// DestroyMenu(hMenu)
static int api_DestroyMenu(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    pthread_mutex_lock(&w->lock);
    int ok = menu_of(h) != 0 && h != w->menu;
    if (ok) menu_destroy(h);
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 2;
}

// LoadAccelerators(hInst, lpName)
static int api_LoadAccelerators(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    uint32_t p = w16_arg32(a);
    char name[64] = "";
    if ((p >> 16) && cpu16_str(c, (uint16_t)(p >> 16), (uint16_t)p, name, sizeof name) < 0) return -1;
    struct ne_resource res;
    int i = w16_res_find(w, NE_RT_ACCELERATOR, 0, (uint16_t)((p >> 16) ? 0 : p), name[0] ? name : 0, &res);
    c->r[R_AX] = i < 0 || i > 0x1FF ? 0 : (uint16_t)(HACCEL_BASE + i);
    return 6;
}

#define FVIRTKEY 0x01
#define FSHIFT   0x04
#define FCONTROL 0x08
#define FALT     0x10

// TranslateAccelerator(hwnd, hAccel, lpMsg): a key an ACCELERATORS entry
// names becomes WM_COMMAND (its id, 1 in lParam's high word) to `hwnd`.
// A virtual key matches WM_KEYDOWN with the same Shift, Ctrl and Alt; a
// character matches WM_CHAR.
static int api_TranslateAccelerator(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hwnd = w16_arg16(a), hacc = w16_arg16(a);
    uint32_t lpmsg = w16_arg32(a);
    c->r[R_AX] = 0;
    const uint8_t *m = cpu16_ptr(c, (uint16_t)(lpmsg >> 16), (uint16_t)lpmsg, 10, 0);
    if (!m) return -1;
    uint16_t msg = (uint16_t)(m[2] | m[3] << 8), wp = (uint16_t)(m[4] | m[5] << 8);
    if (msg != WM_KEYDOWN && msg != 0x0104 && msg != WM_CHAR) return 8;
    struct ne_resource res;
    if (hacc < HACCEL_BASE || ne_resource_at(&w->ne, hacc - HACCEL_BASE, &res) < 0 ||
        res.type != NE_RT_ACCELERATOR)
        return 8;
    const uint8_t *e = w->ne.img + res.off;
    unsigned mods = w->key_mods;
    for (uint32_t at = 0; at + 5 <= res.len; at += 5) {
        unsigned f = e[at], key = e[at + 1] | e[at + 2] << 8, cmd = e[at + 3] | e[at + 4] << 8;
        int hit;
        if (f & FVIRTKEY)
            hit = msg != WM_CHAR && key == wp && !(f & FSHIFT) == !(mods & 1) &&
                  !(f & FCONTROL) == !(mods & 2) && !(f & FALT) == !(mods & 4);
        else
            hit = msg == WM_CHAR && key == wp;
        if (hit) {
            struct w16_msg cm = { hwnd, WM_COMMAND, (uint16_t)cmd, 0x10000u, 0 };
            w16_dispatch(w, &cm);
            c->r[R_AX] = 1;
            return 8;
        }
        if (f & 0x80) break;
    }
    return 8;
}

const struct w16_api g_menu_api[] = {
    { 150, api_LoadMenu },
    { 152, api_DestroyMenu },
    { 154, api_CheckMenuItem },
    { 155, api_EnableMenuItem },
    { 156, api_GetSystemMenu },
    { 157, api_GetMenu },
    { 158, api_SetMenu },
    { 159, api_GetSubMenu },
    { 160, api_DrawMenuBar },
    { 177, api_LoadAccelerators },
    { 178, api_TranslateAccelerator },
    { 250, api_GetMenuState },
    { 263, api_GetMenuItemCount },
    { 264, api_GetMenuItemID },
};
const unsigned g_menu_api_n = sizeof g_menu_api / sizeof g_menu_api[0];

// --- opening a title's popup: the task's half ---------------------------------------

// The main thread asked for title `pos`: the program hears WM_INITMENU
// (when no popup was open) and WM_INITMENUPOPUP, then the popup is shown.
void w16_menu_opening(struct w16 *w, int pos, int first) {
    pthread_mutex_lock(&w->lock);
    struct w16_hmenu *m = menu_of(w->menu);
    uint16_t bar = w->menu, sub = m && pos >= 0 && pos < m->n && (m->it[pos].flags & MF_POPUP) ? m->it[pos].sub : 0;
    pthread_mutex_unlock(&w->lock);
    if (!sub) return;
    if (first) {
        struct w16_msg im = { w->win.hwnd, 0x0116, bar, 0, 0 };            // WM_INITMENU
        w16_dispatch(w, &im);
    }
    struct w16_msg ip = { w->win.hwnd, 0x0117, sub, (uint32_t)(uint16_t)pos, 0 };   // WM_INITMENUPOPUP
    w16_dispatch(w, &ip);
    w16_task_post_arg(w, W16_POST_MENU_SHOW, pos);
}

// --- the bar: the main thread's half, under `lock` -------------------------------------

static int pad(void) { return 8; }             // 3.11's: a title's text 8 in from its box

// A title without its '&' markers (allocated), and where the marked
// letter falls in it, -1 none; "&&" is a plain '&'.
static char *plain_text(const char *t, int *mark) {
    char *out = malloc(strlen(t) + 1);
    int k = 0;
    *mark = -1;
    for (; out && *t; t++) {
        if (*t == '&' && t[1] && t[1] != '&') { *mark = k; continue; }
        if (*t == '&') t++;
        out[k++] = *t;
    }
    if (out) out[k] = 0;
    return out;
}

// Each title's place along the bar, in the bar's 1x pixels; its count.
static int titles(const struct w16_hmenu *m, int *xs, int *ws, int cap) {
    int x = 0, n = 0;                            // the first box starts at the bar's edge
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    for (int i = 0; m && i < m->n && n < cap; i++, n++) {
        int mark;
        char *t = plain_text(m->it[i].text, &mark);
        int tw = t ? ugfx_text_width(t) : 0;
        free(t);
        xs[n] = x;
        ws[n] = tw + 2 * pad();
        x += ws[n];
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return n;
}

int w16_menubar_count(struct w16 *w) {
    struct w16_hmenu *m = menu_of(w->menu);
    return m ? m->n : 0;
}

int w16_menubar_title_at(struct w16 *w, int x, int *tx, int *tw) {
    struct w16_hmenu *m = menu_of(w->menu);
    if (!m) return -1;
    int *xs = malloc(sizeof *xs * (size_t)(m->n + 1)), *ws = malloc(sizeof *ws * (size_t)(m->n + 1));
    int hit = -1;
    if (xs && ws) {
        int n = titles(m, xs, ws, m->n);
        for (int i = 0; i < n; i++)
            if ((x < 0 && i == -x - 1) || (x >= xs[i] && x < xs[i] + ws[i])) {
                hit = i;
                if (tx) *tx = xs[i];
                if (tw) *tw = ws[i];
            }
    }
    free(xs);
    free(ws);
    return hit;
}

int w16_menubar_mnemonic(struct w16 *w, int ch) {
    struct w16_hmenu *m = menu_of(w->menu);
    ch |= 0x20;
    for (int i = 0; m && i < m->n; i++) {
        const char *amp = strchr(m->it[i].text, '&');
        if (amp && (amp[1] | 0x20) == ch) return i;
    }
    return -1;
}

void w16_menubar_draw(struct ugfx_surface *s, struct w16 *w, int x, int y, int width, int open) {
    uint32_t black = w16_rgb(0x000000), white = w16_rgb(0xFFFFFF), navy = w16_rgb(0x000080);
    ugfx_fill_rect(s, x, y, width, W16_MENU_H - 1, white);
    ugfx_fill_rect(s, x, y + W16_MENU_H - 1, width, 1, black);
    struct w16_hmenu *m = menu_of(w->menu);
    if (!m) return;
    int *xs = malloc(sizeof *xs * (size_t)(m->n + 1)), *ws = malloc(sizeof *ws * (size_t)(m->n + 1));
    if (!xs || !ws) { free(xs); free(ws); return; }
    int n = titles(m, xs, ws, m->n);
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    // The cell's top on the bar's: 3.11's 16-row System font in its 18 rows.
    int ch = ugfx_char_h(), ty = y + (W16_MENU_H - 1 - ch - 2) / 2;
    for (int i = 0; i < n && xs[i] < width; i++) {
        int lit = i == open, grey = m->it[i].flags & (MF_GRAYED | MF_DISABLED);
        uint32_t fg = grey ? w16_rgb(0x808080) : lit ? white : black;
        if (lit) ugfx_fill_rect(s, x + xs[i], y, ws[i], W16_MENU_H - 1, navy);
        int mark;
        char *plain = plain_text(m->it[i].text, &mark);
        if (!plain) continue;
        int tx = x + xs[i] + pad();
        ugfx_draw_string_clipped(s, tx, ty, width - xs[i], plain, fg, UGFX_TRANSPARENT);
        if (mark >= 0)
            ugfx_fill_rect(s, tx + ugfx_text_width_n(plain, mark), ty + w16_font_ascent(ugfx_font_current()) + 1, ugfx_text_width_n(plain + mark, 1), 1, fg);
        free(plain);
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    free(xs);
    free(ws);
}

// Title `pos`'s popup as it stands, into `out` (menu16.c's), its items'
// ids the program's own.
void w16_menubar_build(struct w16 *w, int pos, struct w16_menu *out) {
    w16_menu_clear(out);
    struct w16_hmenu *m = menu_of(w->menu);
    if (!m || pos < 0 || pos >= m->n || !(m->it[pos].flags & MF_POPUP)) return;
    struct w16_hmenu *sub = menu_of(m->it[pos].sub);
    for (int i = 0; sub && i < sub->n; i++) {
        const struct w16_mi *it = &sub->it[i];
        if (it->flags & MF_SEP) { w16_menu_add(out, "", "", 0, 0); continue; }
        // "&New\tF2": the part after the tab is the shortcut, right-aligned.
        char *text = strdup(it->text), *tab = text ? strchr(text, '\t') : 0;
        if (!text) continue;
        if (tab) *tab++ = 0;
        unsigned f = (it->flags & (MF_GRAYED | MF_DISABLED) ? W16_MF_GRAYED : 0) |
                     (it->flags & MF_CHECKED ? W16_MF_CHECKED : 0);
        // A nested popup cannot be chosen yet: shown, and grey.
        if (it->flags & MF_POPUP) f |= W16_MF_GRAYED;
        w16_menu_add(out, text, tab ? tab : "", it->id, f);
        free(text);
    }
}
