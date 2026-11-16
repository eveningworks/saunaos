// barmode16.c -- THE menu bar that is open or lit, in whichever of the program's top-level windows
// it is: the main window's (w16.c) or a framed popup's (dlgwin16.c). One at a time, as 3.1 had one
// menu mode for the whole system. Main thread only.
//
// A title's popup is asked of the task first (W16_WM_MENUOPEN: menubar16.c sends WM_INITMENUPOPUP)
// and shown when W16_POST_MENU_SHOW comes back. From the keyboard it is 3.1's MENU-BAR MODE: a title
// lit with nothing dropped -- Alt alone or F10 (DefWindowProc's SC_KEYMENU), or Esc out of a popup
// opened from the keyboard. Left and Right move it; Down, Up or Enter drop it; a title's letter drops
// that one; Esc, F10 or Alt again leaves.
//
// KEYS THAT COME WHILE A TITLE IS ASKED FOR WAIT FOR IT: 3.1's menu loop read them from the queue
// once the popup was up, so a journal's Alt, Down, Down lands on the popup's second item.
//
// `menu_keys` in struct w16 (under `lock`) is the window whose menus have the keyboard, for the task:
// a key a journal plays then comes here (W16_POST_KEY) and not to the program. The task sets it
// itself when it asks for SC_KEYMENU (user16.c), so a key it plays next cannot overtake the ask.
#include "w16_internal.h"

#include <stdlib.h>
#include "keyboard.h"
#include "base/ugrow.h"
#include "ui/uapp.h"

static struct w16_menu g_bar;
static uint16_t g_hwnd;                 // the window whose bar it is; 0 none
static int g_pos = -1;                  // the title asked for or dropped, -1 none
static int g_pending;                   // asked of the task, not shown yet
static int g_by_key;                    // dropped from the keyboard: Esc steps back to the bar
static int g_hot = -1;                  // menu-bar mode's lit title, -1 not in it
static int g_alt_armed;                 // Alt went down in menu mode: its release leaves it
static int *g_wait, g_nwait, g_wait_cap;   // keys that came while a title was asked for

static int open_p(void) { return g_bar.popup > 0 || g_pending; }
static int active(void) { return g_hwnd && (open_p() || g_hot >= 0); }

static void redraw(struct w16 *w, uint16_t hwnd) {
    if (!hwnd) return;
    if (hwnd == w->win.hwnd) {
        if (w->app) uapp_redraw(w->app);
        return;
    }
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    struct uapp_window *u = d ? d->uwin : 0;
    pthread_mutex_unlock(&w->lock);
    if (u) uapp_window_redraw(u);
}

// While an SC_KEYMENU the task asked for is still on its way, the task's menu_keys stands.
static void sync(struct w16 *w) {
    pthread_mutex_lock(&w->lock);
    if (active()) w->menu_keys = g_hwnd;
    else if (!w->menu_asks) w->menu_keys = 0;
    pthread_mutex_unlock(&w->lock);
}

void w16_bar_heard(struct w16 *w) {
    pthread_mutex_lock(&w->lock);
    if (w->menu_asks > 0) w->menu_asks--;
    w->menu_lit = 0;                             // the bar's own state says from here
    pthread_mutex_unlock(&w->lock);
}

// The bar's title count, or a title's letter (-1 none) when ch is one; under nothing.
static int titles(struct w16 *w, uint16_t hwnd, int ch, int *mnemonic) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    int n = d && d->menubar ? w16_menubar_count(d) : 0;
    if (mnemonic) *mnemonic = n && ch > ' ' && ch < 0x7F ? w16_menubar_mnemonic(d, ch) : -1;
    pthread_mutex_unlock(&w->lock);
    return n;
}

int w16_bar_active(void) { return active(); }
int w16_bar_has(uint16_t hwnd) { return hwnd && hwnd == g_hwnd && active(); }
int w16_bar_open_in(uint16_t hwnd) { return hwnd && hwnd == g_hwnd && open_p(); }
int w16_bar_pos(uint16_t hwnd) { return w16_bar_open_in(hwnd) ? g_pos : -1; }
int w16_bar_lit(uint16_t hwnd) {
    if (hwnd && hwnd == g_hwnd) return open_p() ? g_pos : g_hot;
    return hwnd && hwnd == g_w16.menu_lit ? 0 : -1;   // asked lit by the task, not yet heard (under `lock`)
}
struct w16_menu *w16_bar_shown(uint16_t *hwnd) {
    *hwnd = g_hwnd;
    return g_hwnd && g_bar.popup > 0 ? &g_bar : 0;
}
struct w16_menu *w16_bar_menu(uint16_t hwnd) { return hwnd && hwnd == g_hwnd && g_bar.popup > 0 ? &g_bar : 0; }

void w16_bar_close(struct w16 *w) {
    uint16_t h = g_hwnd;
    w16_menu_close(&g_bar);
    g_hwnd = 0;
    g_pos = g_hot = -1;
    g_pending = g_nwait = g_alt_armed = 0;
    sync(w);
    redraw(w, h);
}

void w16_bar_request(struct w16 *w, uint16_t hwnd, int pos, int by_key) {
    int first = !w16_bar_open_in(hwnd);
    if (g_hwnd && g_hwnd != hwnd) w16_bar_close(w);
    w16_menu_close(&g_bar);
    g_hwnd = hwnd;
    g_pos = pos;
    g_pending = 1;
    g_by_key = by_key;
    g_hot = -1;
    w16_post_msg(w, hwnd, W16_WM_MENUOPEN, (uint16_t)pos, (uint32_t)first);
    sync(w);
    redraw(w, hwnd);
}

// Menu-bar mode on title t of hwnd's bar, nothing dropped.
static void light(struct w16 *w, uint16_t hwnd, int t) {
    if (g_hwnd && g_hwnd != hwnd) w16_bar_close(w);
    w16_menu_close(&g_bar);
    g_hwnd = hwnd;
    g_pos = -1;
    g_pending = g_nwait = 0;
    g_hot = t;
    sync(w);
    redraw(w, hwnd);
}

void w16_bar_unlight(struct w16 *w, uint16_t hwnd) {
    if (g_hwnd && (!hwnd || hwnd == g_hwnd) && g_hot >= 0 && !open_p()) w16_bar_close(w);
}

void w16_bar_forget(struct w16 *w, uint16_t hwnd) {
    if (hwnd && hwnd == g_hwnd) w16_bar_close(w);
}

void w16_bar_choose(struct w16 *w, unsigned id) {
    uint16_t h = g_hwnd;
    w16_bar_close(w);
    if (h) w16_post_msg(w, h, WM_COMMAND, (uint16_t)id, 0);
}

int w16_bar_show(struct w16 *w, uint16_t hwnd, int pos) {
    if (!g_pending || hwnd != g_hwnd || pos != g_pos) return 0;
    g_pending = 0;
    int bx = 0, by = 0, bw, bh = 0, tx = 0, ty = 0, tw = 0, ok = 0;
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    struct uapp_window *u = d && d != &w->win ? d->uwin : 0;
    if (d && (d == &w->win || u)) {
        w16_frame_menubar(d, &bx, &by, &bw, &bh);
        w16_menubar_build(d, pos, &g_bar);
        ok = w16_menubar_title_at(d, -(pos + 1), 0, &tx, &ty, &tw) >= 0;
    }
    pthread_mutex_unlock(&w->lock);
    if (ok && g_bar.n) {
        unsigned prev = u ? uapp_window_popup_begin(u) : 0;   // in its own window, not the main one
        w16_menu_open(&g_bar, bx + tx, by + ty, tw, W16_MENU_H - 1, g_by_key);   // under its title's row
        if (u) uapp_window_popup_end(prev);
    } else {
        g_pos = -1;                              // a title that is a command of its own: nothing shows
    }
    int n = g_nwait, *v = g_wait;
    g_wait = 0;
    g_nwait = g_wait_cap = 0;
    if (!active()) n = 0;                        // nothing open for them: what 3.1's loop never saw
    for (int i = 0; i < n; i++) w16_bar_key(w, v[i]);
    free(v);
    sync(w);
    redraw(w, hwnd);
    return 1;
}

int w16_bar_key(struct w16 *w, int key) {
    if (!active()) return 0;
    uint16_t h = g_hwnd;
    g_alt_armed = 0;                             // Alt+letter in menu mode: the letter's, not a leave
    if (g_pending) {
        if (key == 0x1B) w16_bar_close(w);       // a task that never answers cannot hold the keyboard
        else if (UGROW(g_wait, g_wait_cap, g_nwait + 1)) g_wait[g_nwait++] = key;
        return 1;
    }
    if (g_bar.popup > 0) {
        unsigned id = w16_menu_key(&g_bar, key);
        if (id == W16_MENU_LEFT || id == W16_MENU_RIGHT) {
            int n = titles(w, h, 0, 0);
            if (n) w16_bar_request(w, h, (g_pos + (id == W16_MENU_LEFT ? n - 1 : 1)) % n, 1);
        } else if (id == (unsigned)-1) {
            int was = g_pos;
            if (g_by_key) light(w, h, was);       // Esc steps back to the bar, not out of it
            else w16_bar_close(w);
        } else if (id) {
            w16_bar_choose(w, id);
        }
        redraw(w, h);
        return 1;
    }
    int t, n = titles(w, h, key, &t), hot = g_hot;
    if (n && key == KEY_ARROW_LEFT) light(w, h, (hot + n - 1) % n);
    else if (n && key == KEY_ARROW_RIGHT) light(w, h, (hot + 1) % n);
    else if (key == KEY_ARROW_DOWN || key == KEY_ARROW_UP || key == '\n' || key == '\r') w16_bar_request(w, h, hot, 1);
    else if (t >= 0) w16_bar_request(w, h, t, 1);
    else if (key == 0x1B || key == KEY_F10 || !n) w16_bar_close(w);
    // anything else is ignored, as 3.1 beeped and stayed
    return 1;
}

int w16_bar_alt(struct w16 *w, int down) {
    if (!active()) {
        g_alt_armed = 0;
        return 0;
    }
    if (down) g_alt_armed = 1;
    else if (g_alt_armed) w16_bar_close(w);
    return 1;
}

void w16_bar_keymenu(struct w16 *w, uint16_t hwnd, int title) {
    int n = titles(w, hwnd, 0, 0);
    if (!n) { sync(w); return; }                 // no bar: 3.1 lit the control-menu box, which w16 lacks
    if (title < 0) light(w, hwnd, 0);
    else if (title < n) w16_bar_request(w, hwnd, title, 1);
    else sync(w);
}
