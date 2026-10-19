// dlgwin16.c -- a dialog's own toywm window, on the MAIN thread: an
// owned, modal uapp window with no chrome of toywm's (UAPP_WIN_NO_DECOR),
// in which the dialog and its controls are painted the way Windows 3.1
// drew them -- the thick dialog frame, white, bevelled grey buttons, a
// boxed edit with its caret -- from the state dialog16.c keeps. A press, a
// release, a drag and a key are posted to the task as messages for the
// dialog; nothing here decides what a control does.
//
// Painted at 1x into a buffer and scaled (scale16.c), as the program's
// own window is (w16.c's on_draw).
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"

#define BS_TYPE    0x0F
#define SS_TYPE    0x0F
#define SS_CENTER  0x01
#define SS_RIGHT   0x02
#define SS_ICON    0x03
#define SS_NOPREFIX 0x80
#define WS_BORDER  0x00800000u

static uint32_t rgb(uint32_t v) { return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v); }

void w16_dlg_units(int *bx, int *by) {
    static int x, y;
    pthread_mutex_lock(&g_w16_text_lock);
    if (!x) {
        const struct ugfx_font *was = ugfx_set_font(w16_system_font());
        x = (ugfx_text_width("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") + 26) / 52;
        y = ugfx_char_h();
        ugfx_set_font(was);
        if (x < 1) x = 8;
        if (y < 1) y = 16;
    }
    pthread_mutex_unlock(&g_w16_text_lock);
    *bx = x;
    *by = y;
}

// --- painting (under the w16 lock and the text lock) ---------------------------

// `t` without its '&' markers, and where the marked letter falls (-1 none).
static int strip_amp(const char *t, char *out, size_t cap, int prefix) {
    int mark = -1;
    size_t n = 0;
    for (; *t && n + 1 < cap; t++) {
        if (prefix && *t == '&' && t[1] != '&' && t[1]) { mark = (int)n; continue; }
        if (prefix && *t == '&' && t[1] == '&') t++;
        out[n++] = *t;
    }
    out[n] = 0;
    return mark;
}

// One line at (x, y), its mnemonic underlined.
static void label(struct ugfx_surface *s, int x, int y, int w, const char *line, int mark, uint32_t fg,
                  uint32_t bg) {
    ugfx_draw_string_clipped(s, x, y, w, line, fg, bg);
    if (mark >= 0 && line[mark]) {
        char pre[256], ch[2] = { line[mark], 0 };
        size_t m = (size_t)mark < sizeof pre - 1 ? (size_t)mark : sizeof pre - 1;
        memcpy(pre, line, m);
        pre[m] = 0;
        int ux = x + ugfx_text_width(pre);
        ugfx_fill_rect(s, ux, y + ugfx_char_h() - 2, ugfx_text_width(ch), 1, fg);
    }
}

// Text wrapped at words inside (x, y, w, h), each line placed by `align`:
// 0 left, 1 centred, 2 right. '\n' breaks a line.
static void text_box(struct ugfx_surface *s, int x, int y, int w, int h, const char *text, int align,
                     int prefix, uint32_t fg, uint32_t bg) {
    char *buf = malloc(strlen(text) + 1);
    if (!buf) return;
    int mark = strip_amp(text, buf, strlen(text) + 1, prefix), lh = ugfx_char_h(), at = 0;
    const char *p = buf;
    for (int ly = y; *p && ly + lh <= y + h + lh / 2; ly += lh) {
        // As many words as fit, or one cut short when none does.
        int end = 0, last_fit = 0;
        while (p[end] && p[end] != '\n') {
            int e = end;
            while (p[e] == ' ') e++;
            while (p[e] && p[e] != ' ' && p[e] != '\n') e++;
            char *t = strndup(p, (size_t)e);
            int tw = t ? ugfx_text_width(t) : 0;
            free(t);
            if (tw > w && last_fit) break;
            last_fit = end = e;
            if (tw > w) break;
        }
        char *line = strndup(p, (size_t)end);
        if (!line) break;
        int lw = ugfx_text_width(line), lx = align == 1 ? x + (w - lw) / 2 : align == 2 ? x + w - lw : x;
        int m = mark >= at && mark < at + end ? mark - at : -1;
        label(s, lx < x ? x : lx, ly, w, line, m, fg, bg);
        free(line);
        at += end;
        p += end;
        while (*p == ' ') { p++; at++; }
        if (*p == '\n') { p++; at++; }
    }
    free(buf);
}

static void dotted_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i += 2) { ugfx_fill_rect(s, x + i, y, 1, 1, c); ugfx_fill_rect(s, x + i, y + h - 1, 1, 1, c); }
    for (int i = 0; i < h; i += 2) { ugfx_fill_rect(s, x, y + i, 1, 1, c); ugfx_fill_rect(s, x + w - 1, y + i, 1, 1, c); }
}

// A 3.1 push button: black edge with its corners cut, a grey face raised
// by a white and a dark-grey bevel, sunk while held. The default -- or a
// focused button -- has the edge doubled.
static void button(struct ugfx_surface *s, const struct w16_wnd *c, int x, int y, int thick) {
    int w = c->w, h = c->h;
    uint32_t black = rgb(0x000000), hi = rgb(0xFFFFFF), lo = rgb(0x808080);
    ugfx_fill_rect(s, x + 1, y, w - 2, 1, black);
    ugfx_fill_rect(s, x + 1, y + h - 1, w - 2, 1, black);
    ugfx_fill_rect(s, x, y + 1, 1, h - 2, black);
    ugfx_fill_rect(s, x + w - 1, y + 1, 1, h - 2, black);
    int t = thick ? 2 : 1;
    if (thick) ugfx_draw_rect(s, x + 1, y + 1, w - 2, h - 2, black);
    int fx = x + t, fy = y + t, fw = w - 2 * t, fh = h - 2 * t;
    ugfx_fill_rect(s, fx, fy, fw, fh, rgb(0xC0C0C0));
    if (c->pressed) {
        ugfx_fill_rect(s, fx, fy, fw, 1, lo);
        ugfx_fill_rect(s, fx, fy, 1, fh, lo);
    } else {
        ugfx_fill_rect(s, fx, fy, fw - 1, 1, hi);
        ugfx_fill_rect(s, fx, fy, 1, fh - 1, hi);
        ugfx_fill_rect(s, fx, fy + fh - 2, fw, 2, lo);
        ugfx_fill_rect(s, fx + fw - 2, fy, 2, fh, lo);
    }
    char buf[128];
    int mark = strip_amp(c->text ? c->text : "", buf, sizeof buf, 1);
    int tw = ugfx_text_width(buf), lh = ugfx_char_h(), sh = c->pressed ? 1 : 0;
    int tx = x + (w - tw) / 2 + sh, ty = y + (h - lh) / 2 + sh;
    label(s, tx, ty, w - 4, buf, mark, black, rgb(0xC0C0C0));
    if (c->focused) dotted_rect(s, tx - 2, ty - 1, tw + 4, lh + 2, black);
}

static void edit(struct ugfx_surface *s, struct w16_wnd *c, int x, int y) {
    uint32_t white = rgb(0xFFFFFF), black = rgb(0x000000), navy = rgb(0x000080);
    ugfx_fill_rect(s, x, y, c->w, c->h, white);
    if (c->style & WS_BORDER) ugfx_draw_rect(s, x, y, c->w, c->h, black);
    const char *t = c->text ? c->text : "";
    int n = (int)strlen(t), lh = ugfx_char_h(), ty = y + (c->h - lh) / 2, room = c->w - 4;
    // Scrolled so the caret stays in the box (ES_AUTOHSCROLL's job).
    if (c->scroll > c->caret) c->scroll = c->caret;
    for (;;) {
        char *seen = strndup(t + c->scroll, (size_t)(c->caret - c->scroll));
        int sw = seen ? ugfx_text_width(seen) : 0;
        free(seen);
        if (sw <= room - 1 || c->scroll >= c->caret) break;
        c->scroll++;
    }
    int lo = c->caret < c->anchor ? c->caret : c->anchor, hi = c->caret ^ c->anchor ^ lo;
    int cx = x + 2, caret_x = cx;
    char ch[2] = { 0, 0 };
    for (int i = c->scroll; i < n; i++) {
        ch[0] = t[i];
        int cw = ugfx_text_width(ch);
        if (cx + cw > x + c->w - 2) break;
        int sel = c->focused && i >= lo && i < hi;
        if (sel) ugfx_fill_rect(s, cx, ty, cw, lh, navy);
        ugfx_draw_string(s, cx, ty, ch, sel ? white : black, UGFX_TRANSPARENT);
        if (i + 1 == c->caret) caret_x = cx + cw;
        cx += cw;
    }
    if (c->caret <= c->scroll) caret_x = x + 2;
    if (c->focused && lo == hi) ugfx_fill_rect(s, caret_x, ty, 1, lh, black);
}

// A message box's icon (1 hand, 2 question, 3 exclamation, 4 asterisk),
// or 0xFF: the program's own.
static void icon(struct ugfx_surface *s, struct w16 *w, int kind, int x, int y) {
    if (kind == 1) { w16_stop_sign(s, x, y); return; }
    if (kind == 0xFF) {
        static uint32_t px[32 * 32];
        static int iw = -1, ih;
        if (iw < 0 && w16_icon(&w->ne, px, &iw, &ih) < 0) iw = 0;
        if (iw > 0) ugfx_blit_alpha(s, x + (32 - iw) / 2, y + (32 - ih) / 2, iw, ih, px, iw);
        return;
    }
    uint32_t disc = kind == 3 ? rgb(0xFFFF00) : rgb(0xFFFFFF), ink = kind == 3 ? rgb(0x000000) : rgb(0x000080);
    ugfx_fill_circle(s, x + 16, y + 16, 15, rgb(0x000000));
    ugfx_fill_circle(s, x + 16, y + 16, 14, disc);
    const char *g = kind == 2 ? "?" : kind == 3 ? "!" : "i";
    ugfx_draw_string(s, x + 16 - ugfx_text_width(g) / 2, y + 16 - ugfx_char_h() / 2, g, ink, disc);
}

void w16_dlg_paint(struct w16 *w, const struct w16_wnd *d, uint32_t *px) {
    struct ugfx_surface s = ugfx_surface_for_pixels(px, d->w, d->h);
    uint32_t black = rgb(0x000000), white = rgb(0xFFFFFF);
    int f = w16_dlg_frame(d), cap = w16_dlg_caption(d);
    ugfx_fill_rect(&s, 0, 0, d->w, d->h, black);
    if (f > 1) {
        uint32_t band = d->active ? rgb(0x000080) : rgb(0xC0C0C0);
        ugfx_fill_rect(&s, 1, 1, d->w - 2, d->h - 2, band);
        if (cap) ugfx_fill_rect(&s, f - 1, f - 1, d->w - 2 * f + 2, d->h - 2 * f + 2, black);
    }
    int ox = f, oy = f + cap;
    if (cap) {
        // The caption: navy while active, the title centred in white, and
        // the control-menu box at its left as a 3.1 window has it.
        int cw = d->w - 2 * f;
        uint32_t bar = d->active ? rgb(0x000080) : white, ink = d->active ? white : black;
        ugfx_fill_rect(&s, f, f, cw, cap - 1, bar);
        ugfx_fill_rect(&s, f, f + cap - 1, cw, 1, black);
        int box = cap - 1;
        ugfx_fill_rect(&s, f, f, box, box, rgb(0xC0C0C0));
        ugfx_fill_rect(&s, f + box, f, 1, box, black);
        ugfx_fill_rect(&s, f + 4, f + box / 2 - 1, box - 7, 3, rgb(0x808080));
        ugfx_fill_rect(&s, f + 3, f + box / 2 - 2, box - 7, 3, white);
        const char *t = d->text ? d->text : "";
        int tw = ugfx_text_width(t);
        ugfx_draw_string_clipped(&s, f + box + (cw - box - tw) / 2, f + (cap - 1 - ugfx_char_h()) / 2,
                                 cw - box - 2, t, ink, bar);
    }
    ugfx_fill_rect(&s, ox, oy, d->cw, d->ch, white);
    // A focused button is the default while it has the focus (3.1's rule).
    const struct w16_wnd *fc = w16_wnd_get(w, d->focus);
    int focus_button = fc && fc->kind == W16_K_BUTTON;
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (!c || !c->used || c->parent != d->hwnd || c->kind == W16_K_DIALOG) continue;
        int x = ox + c->x, y = oy + c->y;
        struct ugfx_surface clip = s;
        switch (c->kind) {
        case W16_K_BUTTON:
            button(&clip, c, x, y, c->focused || (c->is_default && !focus_button));
            break;
        case W16_K_EDIT:
            edit(&clip, c, x, y);
            break;
        case W16_K_STATIC:
            if ((c->style & SS_TYPE) == SS_ICON) { icon(&clip, w, (int)(c->style >> 8 & 0xFF), x, y); break; }
            text_box(&clip, x, y, c->w, c->h, c->text ? c->text : "",
                     (c->style & SS_TYPE) == SS_CENTER ? 1 : (c->style & SS_TYPE) == SS_RIGHT ? 2 : 0,
                     !(c->style & SS_NOPREFIX), black, white);
            break;
        default:
            break;
        }
    }
}

// --- the window -----------------------------------------------------------------

static uint16_t hwnd_of(struct uapp_window *u) { return (uint16_t)(uintptr_t)uapp_window_state(u); }

static void on_draw(struct uapp_window *u, struct uapp_draw *dr) {
    static uint32_t *buf;
    static size_t cap;
    struct ugfx_surface *s = uapp_surface(dr);
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *d = w16_wnd_get(&g_w16, hwnd_of(u));
    if (d) {
        size_t need = (size_t)d->w * d->h;
        if (need > cap) {
            uint32_t *g = realloc(buf, need * 4);
            if (g) { buf = g; cap = need; }
        }
        if (need <= cap) {
            pthread_mutex_lock(&g_w16_text_lock);
            const struct ugfx_font *was = ugfx_set_font(w16_system_font());
            w16_dlg_paint(&g_w16, d, buf);
            ugfx_set_font(was);
            pthread_mutex_unlock(&g_w16_text_lock);
            w16_scale_blit(s, 0, 0, buf, d->w, d->h, d->w);
        }
    }
    pthread_mutex_unlock(&g_w16.lock);
}

// Pointer events in the dialog's CLIENT coordinates; the caption drags.
static void post_mouse(struct uapp_window *u, uint16_t msg, int x, int y) {
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *d = w16_wnd_get(&g_w16, hwnd_of(u));
    int f = d ? w16_dlg_frame(d) : 0, cap = d ? w16_dlg_caption(d) : 0;
    uint16_t h = d ? d->hwnd : 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (!h) return;
    x = w16_down(x) - f;
    y = w16_down(y) - f - cap;
    if (msg == WM_LBUTTONDOWN && y < 0 && y >= -cap) { uapp_window_move_start(u, 0); return; }
    w16_post_msg(&g_w16, h, msg, 0, (uint32_t)(uint16_t)y << 16 | (uint16_t)x);
}

static void on_press(struct uapp_window *u, int x, int y, unsigned buttons) {
    if (buttons & 1) post_mouse(u, WM_LBUTTONDOWN, x, y);
}
static void on_motion(struct uapp_window *u, int x, int y, unsigned buttons) {
    if (buttons & 1) post_mouse(u, WM_MOUSEMOVE, x, y);
}
static void on_release(struct uapp_window *u, int x, int y, unsigned buttons) {
    (void)buttons;
    post_mouse(u, WM_LBUTTONUP, x, y);
}

static void on_key(struct uapp_window *u, int key, unsigned mods) {
    w16_post_msg(&g_w16, hwnd_of(u), W16_WM_DLGKEY, (uint16_t)key, mods);
}

static void on_close(struct uapp_window *u) {
    w16_post_msg(&g_w16, hwnd_of(u), WM_CLOSE, 0, 0);
}

void w16_dlgwin_open(struct w16 *w, struct uapp *a, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    int dw = d ? d->w : 0, dh = d ? d->h : 0;
    pthread_mutex_unlock(&w->lock);
    if (!d) return;
    struct uapp_window_desc desc = {
        .title = d->text && d->text[0] ? d->text : "Dialog",
        .w = w16_up(dw),
        .h = w16_up(dh),
        .flags = UAPP_WIN_MODAL | UAPP_WIN_NO_DECOR,
        .state = (void *)(uintptr_t)hwnd,
        .on_key = on_key,
        .on_close = on_close,
        .on_draw = on_draw,
        .on_press = on_press,
        .on_motion = on_motion,
        .on_release = on_release,
        .log_prefix = "w16-dialog",
    };
    struct uapp_window *u = uapp_window_open(a, &desc);
    pthread_mutex_lock(&w->lock);
    d->uwin = u;
    d->active = u != 0;
    if (!u) {                                    // no window to show it in: the dialog is cancelled
        d->result = -1;
        d->ended = 1;
        pthread_cond_broadcast(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
}

void w16_dlgwin_close(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    struct uapp_window *u = d ? d->uwin : 0;
    if (d) d->uwin = 0;
    pthread_mutex_unlock(&w->lock);
    if (u) uapp_window_close(u);
    pthread_mutex_lock(&w->lock);
    if (d) d->ended = 2;                         // the task may free it now
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

void w16_dlgwin_redraw(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    struct uapp_window *u = d ? d->uwin : 0;
    pthread_mutex_unlock(&w->lock);
    if (u) uapp_window_redraw(u);
}
