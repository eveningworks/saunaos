// menu16.c -- a Windows 3.1 popup menu, drawn by the MAIN thread into a
// toolkit popup: white, a black edge, a hard black shadow, the bar in the
// highlight navy, greyed items in grey, a mnemonic underlined. The control
// menu is its first user; stage 3's menu bars open the same thing.
//
// THE MENU KEEPS NO PROGRAM STATE: it returns the chosen item's id and the
// caller turns that into a message (WM_SYSCOMMAND for the control menu).
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/ugfx.h"
#include "ui/uui_popup.h"

// 3.11's menu at VGA, measured on Notepad's and Paintbrush's: a label's origin 16 in (room for a
// check mark), the shortcuts a left-aligned column 8 after the longest label, the border 15 after the
// longest label or shortcut, and a shadow one light-grey pixel wide.
#define PAD_L   16
#define PAD_R   15
#define GAP     8
#define SHADOW  1

static uint32_t rgb(uint32_t v) { return w16_rgb(v); }

static int sep_h(void) { return 7; }

// A row's height is the SYSTEM FONT's, measured once when the menu opens
// and kept in `m`: the hit test runs under whatever font is current, and
// asking ugfx_char_h() there put the pointer a row off by the time it
// reached the bottom of a long menu.
static int item_h(const struct w16_menu *m, const struct w16_menu_item *it) {
    return it->flags & W16_MF_SEPARATOR ? sep_h() : m->row_h;
}

// The label without its '&', and where the mnemonic falls in it (-1 none).
// `out` holds strlen(text) + 1.
static int plain(const char *text, char *out, int cap) {
    int n = 0, at = -1;
    for (const char *p = text; *p && n < cap - 1; p++) {
        if (*p == '&' && p[1] && p[1] != '&') { at = n; continue; }
        if (*p == '&') p++;
        out[n++] = *p;
    }
    out[n] = 0;
    return at;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int mnemonic(const struct w16_menu_item *it) {
    char *t = malloc(strlen(it->text) + 1);
    if (!t) return 0;
    int at = plain(it->text, t, (int)strlen(it->text) + 1), c = at >= 0 ? lower(t[at]) : 0;
    free(t);
    return c;
}

// Measured in the system font, under the text lock the task's GDI shares.
static void measure(struct w16_menu *m) {
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int lw = 0, aw = 0, h = 2;                   // the border's two lines
    m->row_h = ugfx_char_h() + 2;                // 18 in 3.11's System font, rows from the border on
    for (int i = 0; i < m->n; i++) {
        char *t = malloc(strlen(m->item[i].text) + 1);
        if (t) plain(m->item[i].text, t, (int)strlen(m->item[i].text) + 1);
        int tw = t ? ugfx_text_width(t) : 0, kw = ugfx_text_width(m->item[i].accel);
        free(t);
        if (tw > lw) lw = tw;
        if (kw > aw) aw = kw;
        h += item_h(m, &m->item[i]);
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    m->accel_x = PAD_L + lw + GAP;
    m->w = PAD_L + lw + (aw ? GAP + aw : 0) + PAD_R + SHADOW;
    m->h = h + SHADOW;
}

int w16_menu_open_p(const struct w16_menu *m) { return m->popup > 0; }

// The compositor dismissed it: a press in another program. The id is dead.
static void dismissed(void *owner) {
    struct w16_menu *m = owner;
    m->popup = 0;
    m->sel = -1;
    m->armed = 0;
}

void w16_menu_open(struct w16_menu *m, int ax, int ay, int aw, int ah, int by_key) {
    if (m->popup) return;
    measure(m);
    int x = ax, y = ay + ah;
    // The popup is real pixels; m's numbers are 1x.
    m->popup = uui_popup_open(w16_up(ax), w16_up(ay), w16_up(aw), w16_up(ah), w16_up(m->w), w16_up(m->h),
                              UUI_POPUP_BELOW, UUI_POPUP_GRAB | UUI_POPUP_NO_DECOR, dismissed, m, &x, &y);
    m->x = w16_down(x);
    m->y = w16_down(y);
    m->armed = 0;
    m->sel = -1;
    // Opened from the keyboard, the first item is lit even when it is grey
    // (3.11's Edit > Undo, its control menu's Restore); opened by the
    // pointer, nothing until it moves.
    if (by_key)
        for (int i = 0; i < m->n; i++)
            if (!(m->item[i].flags & W16_MF_SEPARATOR)) { m->sel = i; break; }
}

void w16_menu_close(struct w16_menu *m) {
    if (m->popup > 0) uui_popup_close(m->popup);
    m->popup = 0;
    m->sel = -1;
    m->armed = 0;
}

static int item_at(const struct w16_menu *m, int x, int y) {
    if (x < m->x || x >= m->x + m->w - SHADOW || y < m->y) return -1;
    int top = m->y + 1;
    for (int i = 0; i < m->n; i++) {
        int h = item_h(m, &m->item[i]);
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}

// The bar can rest on a grey item, as 3.1's did; only an enabled one acts.
static int selectable(const struct w16_menu *m, int i) {
    return i >= 0 && i < m->n && !(m->item[i].flags & W16_MF_SEPARATOR);
}

static int choosable(const struct w16_menu *m, int i) {
    return selectable(m, i) && !(m->item[i].flags & W16_MF_GRAYED);
}

static void draw_menu(struct w16_menu *m, struct ugfx_surface *s);

// Scaled, the menu is drawn at 1x and then scaled, as the window is.
void w16_menu_draw(struct w16_menu *m) {
    if (m->popup <= 0) return;
    struct ugfx_surface *s = uui_popup_surface(m->popup);
    if (!s) return;
    if (g_w16_q == 4) { draw_menu(m, s); return; }
    uint32_t *px = malloc((size_t)m->w * m->h * 4);
    if (!px) return;
    struct ugfx_surface t = ugfx_surface_for_pixels(px, m->w, m->h);
    draw_menu(m, &t);
    w16_scale_blit(s, 0, 0, px, m->w, m->h, m->w);
    free(px);
}

static void draw_menu(struct w16_menu *m, struct ugfx_surface *s) {
    uint32_t black = rgb(0x000000), white = rgb(0xFFFFFF);
    int W = m->w - SHADOW, H = m->h - SHADOW;
    // The shadow is the surface's light-grey right column and bottom row, from the second pixel on. An
    // opaque popup cannot leave its two far corners transparent, as 3.1's did: the top right is black,
    // the menu bar's line a dropdown lies on, the bottom left white, the client under it as a rule.
    ugfx_fill_rect(s, 0, 0, m->w, m->h, rgb(0xC0C0C0));
    ugfx_fill_rect(s, 0, 0, W, H, white);
    ugfx_draw_rect(s, 0, 0, W, H, black);
    ugfx_fill_rect(s, W, 0, SHADOW, 1, black);
    ugfx_fill_rect(s, 0, H, 1, SHADOW, white);

    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int y = 1, ch = ugfx_char_h();
    for (int i = 0; i < m->n; i++) {
        const struct w16_menu_item *it = &m->item[i];
        int h = item_h(m, it);
        if (it->flags & W16_MF_SEPARATOR) {
            ugfx_fill_rect(s, 1, y + h / 2, W - 2, 1, black);
            y += h;
            continue;
        }
        // A grey item: light grey -- COLOR_GRAYTEXT at VGA -- or, lit, white dithered as GrayString did.
        int lit = i == m->sel, grey = (it->flags & W16_MF_GRAYED) != 0;
        uint32_t fg = grey && !lit ? rgb(0xC0C0C0) : lit ? white : black;
        if (lit) ugfx_fill_rect(s, 1, y, W - 2, h, rgb(0x000080));
        char *t = malloc(strlen(it->text) + 1);
        if (!t) { y += h; continue; }
        int at = plain(it->text, t, (int)strlen(it->text) + 1);
        int ty = y;                              // the cell at the row's top, its 2 spare rows below, as 3.11
        if (it->flags & W16_MF_CHECKED) {       // 3.1's tick, in the room left of the text
            int cx = 5, cy = ty + ch / 2;
            for (int k = 0; k < 3; k++) ugfx_fill_rect(s, cx + k, cy - 1 + k, 1, 2, fg);
            for (int k = 0; k < 5; k++) ugfx_fill_rect(s, cx + 3 + k, cy + 1 - k, 1, 2, fg);
        }
        ugfx_draw_string_clipped(s, PAD_L, ty, W - PAD_L - PAD_R, t, fg, UGFX_TRANSPARENT);
        if (at >= 0) {
            int ux = PAD_L + ugfx_text_width_n(t, at), uw = ugfx_text_width_n(t + at, 1);
            ugfx_fill_rect(s, ux, ty + w16_font_ascent(ugfx_font_current()) + 1, uw, 1, fg);
        }
        if (it->accel[0])
            ugfx_draw_string(s, m->accel_x, ty, it->accel, fg, UGFX_TRANSPARENT);
        if (grey && lit) w16_gray_dither(s, 1, y, W - 2, h, white, rgb(0x000080));
        free(t);
        y += h;
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

unsigned w16_menu_motion(struct w16_menu *m, int x, int y) {
    if (m->popup <= 0) return 0;
    int i = item_at(m, x, y);
    int sel = selectable(m, i) ? i : -1;
    if (sel != m->sel && (sel >= 0 || m->armed)) m->sel = sel;
    return 0;
}

int w16_menu_press(struct w16_menu *m, int x, int y) {
    if (m->popup <= 0) return 0;
    if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h) {
        m->armed = 1;
        w16_menu_motion(m, x, y);
        return 1;
    }
    w16_menu_close(m);
    return 0;
}

unsigned w16_menu_release(struct w16_menu *m, int x, int y) {
    if (m->popup <= 0) return 0;
    int i = item_at(m, x, y);
    if (!choosable(m, i)) return 0;        // a release off the items, or on a grey one, keeps it open
    unsigned id = m->item[i].id;
    w16_menu_close(m);
    return id;
}

unsigned w16_menu_key(struct w16_menu *m, int key) {
    if (m->popup <= 0) return 0;
    if (key == 0x1B) { w16_menu_close(m); return (unsigned)-1; }
    if (key == KEY_ARROW_LEFT) return W16_MENU_LEFT;
    if (key == KEY_ARROW_RIGHT) return W16_MENU_RIGHT;
    if (key == KEY_ARROW_DOWN || key == KEY_ARROW_UP) {
        int step = key == KEY_ARROW_DOWN ? 1 : -1;
        for (int k = 1; k <= m->n; k++) {
            int i = ((m->sel < 0 ? (step > 0 ? -1 : 0) : m->sel) + step * k + m->n * 2) % m->n;
            if (selectable(m, i)) { m->sel = i; break; }
        }
        return 0;
    }
    if (key == '\n' || key == '\r') {
        if (!choosable(m, m->sel)) return 0;
        unsigned id = m->item[m->sel].id;
        w16_menu_close(m);
        return id;
    }
    if (key > ' ' && key < 0x7F) {
        for (int i = 0; i < m->n; i++)
            if (choosable(m, i) && mnemonic(&m->item[i]) == lower((char)key)) {
                unsigned id = m->item[i].id;
                w16_menu_close(m);
                return id;
            }
    }
    return 0;
}

// --- the control menu ---------------------------------------------------------

void w16_menu_clear(struct w16_menu *m) {
    for (int i = 0; i < m->n; i++) { free(m->item[i].text); free(m->item[i].accel); }
    m->n = 0;
    m->sel = -1;
}

// An empty `text` is a separator. An item that cannot be added (out of
// memory) is left off.
void w16_menu_add(struct w16_menu *m, const char *text, const char *accel, unsigned id, unsigned flags) {
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 16;
        struct w16_menu_item *g = realloc(m->item, sizeof *g * (size_t)cap);
        if (!g) return;
        m->item = g;
        m->cap = cap;
    }
    char *t = strdup(text), *k = strdup(accel ? accel : "");
    if (!t || !k) { free(t); free(k); return; }
    struct w16_menu_item *it = &m->item[m->n++];
    it->text = t;
    it->accel = k;
    it->id = id;
    it->flags = flags | (text[0] ? 0 : W16_MF_SEPARATOR);
}

static void add(struct w16_menu *m, const char *text, const char *accel, unsigned id, int grayed) {
    w16_menu_add(m, text, accel, id, grayed ? W16_MF_GRAYED : 0);
}

// Move and Size are 3.1's KEYBOARD move and size, which need toywm to run
// a drag without a held button; until it can, they are shown grey rather
// than dropped, so the menu is the one a 3.1 user knows.
// A dialog's control menu, as 3.11's: Move and Close, nothing between them.
void w16_dlg_sysmenu_build(struct w16_menu *m) {
    w16_menu_clear(m);
    add(m, "&Move", "", SC_MOVE, 0);
    add(m, "&Close", "Alt+F4", SC_CLOSE, 0);
}

void w16_sysmenu_build(struct w16_menu *m, struct w16 *w) {
    const struct w16_wnd *win = &w->win;
    w16_menu_clear(m);
    int max = win->maximized;
    add(m, "&Restore", "", SC_RESTORE, !max);
    add(m, "&Move", "", SC_MOVE, 1);
    add(m, "&Size", "", SC_SIZE, 1);
    add(m, "Mi&nimize", "", SC_MINIMIZE, !(win->style & WS_MINIMIZEBOX));
    add(m, "Ma&ximize", "", SC_MAXIMIZE, max || !(win->style & WS_MAXIMIZEBOX));
    add(m, "", "", 0, 0);
    add(m, "&Close", "Alt+F4", SC_CLOSE, 0);
    w16_sysmenu_extra(w, m);
}
