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

#define PAD_L   18                 // room for a check mark, as 3.1 left it
#define PAD_R   14
#define SHADOW  2

static uint32_t rgb(uint32_t v) { return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v); }

static int row_h(void) { return ugfx_char_h() + 4; }
static int sep_h(void) { return 7; }

static int item_h(const struct w16_menu_item *it) {
    return it->flags & W16_MF_SEPARATOR ? sep_h() : row_h();
}

// The label without its '&', and where the mnemonic falls in it (-1 none).
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
    char t[sizeof it->text];
    int at = plain(it->text, t, sizeof t);
    return at >= 0 ? lower(t[at]) : 0;
}

// Measured in the system font, under the text lock the task's GDI shares.
static void measure(struct w16_menu *m) {
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int lw = 0, aw = 0, h = 4;
    for (int i = 0; i < m->n; i++) {
        char t[sizeof m->item[i].text];
        plain(m->item[i].text, t, sizeof t);
        int tw = ugfx_text_width(t), kw = ugfx_text_width(m->item[i].accel);
        if (tw > lw) lw = tw;
        if (kw > aw) aw = kw;
        h += item_h(&m->item[i]);
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    m->w = PAD_L + lw + (aw ? 3 * ugfx_char_w() + aw : 0) + PAD_R + SHADOW;
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
    int S = g_w16_scale;                         // the popup is real pixels; m's numbers are 1x
    m->popup = uui_popup_open(ax * S, ay * S, aw * S, ah * S, m->w * S, m->h * S, UUI_POPUP_BELOW,
                              UUI_POPUP_GRAB | UUI_POPUP_NO_DECOR, dismissed, m, &x, &y);
    m->x = x / S;
    m->y = y / S;
    m->armed = 0;
    m->sel = -1;
    // Opened from the keyboard, the first item that can be chosen is lit,
    // as 3.1's Alt+Space did; opened by the pointer, nothing until it moves.
    if (by_key)
        for (int i = 0; i < m->n; i++)
            if (!(m->item[i].flags & (W16_MF_SEPARATOR | W16_MF_GRAYED))) { m->sel = i; break; }
}

void w16_menu_close(struct w16_menu *m) {
    if (m->popup > 0) uui_popup_close(m->popup);
    m->popup = 0;
    m->sel = -1;
    m->armed = 0;
}

static int item_at(const struct w16_menu *m, int x, int y) {
    if (x < m->x || x >= m->x + m->w - SHADOW || y < m->y) return -1;
    int top = m->y + 2;
    for (int i = 0; i < m->n; i++) {
        int h = item_h(&m->item[i]);
        if (y >= top && y < top + h) return i;
        top += h;
    }
    return -1;
}

static int choosable(const struct w16_menu *m, int i) {
    return i >= 0 && i < m->n && !(m->item[i].flags & (W16_MF_SEPARATOR | W16_MF_GRAYED));
}

static void draw_menu(struct w16_menu *m, struct ugfx_surface *s);

// At 2x the menu is drawn at 1x and every pixel doubled, as the window is.
void w16_menu_draw(struct w16_menu *m) {
    if (m->popup <= 0) return;
    struct ugfx_surface *s = uui_popup_surface(m->popup);
    if (!s) return;
    if (g_w16_scale == 1) { draw_menu(m, s); return; }
    uint32_t *px = malloc((size_t)m->w * m->h * 4);
    if (!px) return;
    struct ugfx_surface t = ugfx_surface_for_pixels(px, m->w, m->h);
    draw_menu(m, &t);
    w16_scale_blit(s, 0, 0, px, m->w, m->h, m->w, g_w16_scale);
    free(px);
}

static void draw_menu(struct w16_menu *m, struct ugfx_surface *s) {
    uint32_t black = rgb(0x000000), white = rgb(0xFFFFFF);
    int W = m->w - SHADOW, H = m->h - SHADOW;
    // The shadow is the surface's black right and bottom bands: an opaque
    // popup cannot leave its two far corners transparent, as 3.1's could.
    ugfx_fill_rect(s, 0, 0, m->w, m->h, black);
    ugfx_fill_rect(s, 0, 0, W, H, white);
    ugfx_draw_rect(s, 0, 0, W, H, black);

    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int y = 2, ch = ugfx_char_h();
    for (int i = 0; i < m->n; i++) {
        const struct w16_menu_item *it = &m->item[i];
        int h = item_h(it);
        if (it->flags & W16_MF_SEPARATOR) {
            ugfx_fill_rect(s, 1, y + h / 2, W - 2, 1, black);
            y += h;
            continue;
        }
        int lit = i == m->sel;
        uint32_t fg = it->flags & W16_MF_GRAYED ? rgb(0x808080) : lit ? white : black;
        if (lit) ugfx_fill_rect(s, 1, y, W - 2, h, rgb(0x000080));
        char t[sizeof it->text];
        int at = plain(it->text, t, sizeof t);
        int ty = y + (h - ch) / 2;
        ugfx_draw_string_clipped(s, PAD_L, ty, W - PAD_L - PAD_R, t, fg, UGFX_TRANSPARENT);
        if (at >= 0) {
            int ux = PAD_L + ugfx_text_width_n(t, at), uw = ugfx_text_width_n(t + at, 1);
            ugfx_fill_rect(s, ux, ty + ch - 2, uw, 1, fg);
        }
        if (it->accel[0])
            ugfx_draw_string(s, W - PAD_R - ugfx_text_width(it->accel), ty, it->accel, fg,
                             UGFX_TRANSPARENT);
        y += h;
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

unsigned w16_menu_motion(struct w16_menu *m, int x, int y) {
    if (m->popup <= 0) return 0;
    int i = item_at(m, x, y);
    int sel = choosable(m, i) ? i : -1;
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
    if (key == KEY_ARROW_DOWN || key == KEY_ARROW_UP) {
        int step = key == KEY_ARROW_DOWN ? 1 : -1;
        for (int k = 1; k <= m->n; k++) {
            int i = ((m->sel < 0 ? (step > 0 ? -1 : 0) : m->sel) + step * k + m->n * 2) % m->n;
            if (choosable(m, i)) { m->sel = i; break; }
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

static void add(struct w16_menu *m, const char *text, const char *accel, unsigned id, int grayed) {
    if (m->n >= W16_MENU_MAX) return;
    struct w16_menu_item *it = &m->item[m->n++];
    memset(it, 0, sizeof *it);
    strlcpy(it->text, text, sizeof it->text);
    strlcpy(it->accel, accel, sizeof it->accel);
    it->id = id;
    it->flags = (text[0] ? 0 : W16_MF_SEPARATOR) | (grayed ? W16_MF_GRAYED : 0);
}

// Move and Size are 3.1's KEYBOARD move and size, which need toywm to run
// a drag without a held button; until it can, they are shown grey rather
// than dropped, so the menu is the one a 3.1 user knows.
void w16_sysmenu_build(struct w16_menu *m, const struct w16_wnd *win) {
    int popup = m->popup, x = m->x, y = m->y, w = m->w, h = m->h;
    memset(m, 0, sizeof *m);
    m->popup = popup; m->x = x; m->y = y; m->w = w; m->h = h;
    m->sel = -1;
    int max = win->maximized;
    add(m, "&Restore", "", SC_RESTORE, !max);
    add(m, "&Move", "", SC_MOVE, 1);
    add(m, "&Size", "", SC_SIZE, 1);
    add(m, "Mi&nimize", "", SC_MINIMIZE, !(win->style & WS_MINIMIZEBOX));
    add(m, "Ma&ximize", "", SC_MAXIMIZE, max || !(win->style & WS_MAXIMIZEBOX));
    add(m, "", "", 0, 0);
    add(m, "&Close", "Alt+F4", SC_CLOSE, 0);
}
