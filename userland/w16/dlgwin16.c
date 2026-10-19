// dlgwin16.c -- a dialog's own toywm window, on the MAIN thread: an owned
// uapp window with no chrome of toywm's (UAPP_WIN_NO_DECOR), modal for a
// DialogBox, in which the Windows 3.1 dialog frame and caption are drawn
// round the dialog's client buffer -- which the task painted, controls
// and all. A press, a release, a drag and a key are posted to the task as
// messages for the control under the pointer or with the focus; nothing
// here decides what a control does.
//
// Painted at 1x into a buffer and scaled (scale16.c), as the program's
// own window is (w16.c's on_draw).
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"

// The System font's base units: 3.11's at VGA, 8 by 16 -- fixed rather
// than measured off the stand-in face, so a program's dialogs come out the
// size they were whatever face draws them.
void w16_dlg_units(int *bx, int *by) {
    *bx = 8;
    *by = 16;
}

// The frame and caption round the client buffer (under the w16 lock and
// the text lock): the thick dialog frame, navy while active, the title
// centred in white and the control-menu box at its left as a 3.1 window
// has it.
static void paint_frame(const struct w16_wnd *d, uint32_t *px) {
    struct ugfx_surface s = ugfx_surface_for_pixels(px, d->w, d->h);
    uint32_t black = w16_rgb(0x000000), white = w16_rgb(0xFFFFFF);
    int f = w16_dlg_frame(d), top = w16_dlg_frame_top(d), cap = w16_dlg_caption(d);
    ugfx_fill_rect(&s, 0, 0, d->w, d->h, black);
    if (f > 1) {
        uint32_t band = d->active ? w16_rgb(0x000080) : w16_rgb(0xC0C0C0);
        ugfx_fill_rect(&s, 1, 1, d->w - 2, d->h - 2, band);
        ugfx_draw_rect(&s, f - 1, top - 1, d->w - 2 * f + 2, d->h - top - f + 2, white);
        // The white line's top edge is the caption's: with none, 3.11's frame
        // is navy to the client there.
        if (!cap) ugfx_fill_rect(&s, f - 1, top - 1, d->w - 2 * f + 2, 1, band);
    }
    if (cap) {
        int cw = d->w - 2 * f;
        uint32_t bar = d->active ? w16_rgb(0x000080) : white, ink = d->active ? white : black;
        ugfx_fill_rect(&s, f, top, cw, cap - 1, bar);
        ugfx_fill_rect(&s, f, top + cap - 1, cw, 1, black);
        int box = cap - 1;
        w16_draw_sysbox(&s, f, top, box, box, 0);   // the main window's own, frame16.c's
        ugfx_fill_rect(&s, f + box, top, 1, box, black);
        const char *t = d->text ? d->text : "";
        int tw = ugfx_text_width(t);
        ugfx_draw_string_clipped(&s, f + box + (cw - box - tw) / 2, top + (cap - 1 - ugfx_char_h()) / 2,
                                 cw - box - 2, t, ink, bar);
    }
    if (d->fb) ugfx_blit(&s, f, top + cap, d->fbw, d->fbh, d->fb, d->fbw);
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
            paint_frame(d, buf);
            ugfx_set_font(was);
            pthread_mutex_unlock(&g_w16_text_lock);
            w16_scale_blit(s, 0, 0, buf, d->w, d->h, d->w);
        }
    }
    pthread_mutex_unlock(&g_w16.lock);
}

// Pointer events in the dialog's CLIENT coordinates; the caption drags.
static void post_mouse(struct uapp_window *u, uint16_t msg, int x, int y, unsigned buttons) {
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *d = w16_wnd_get(&g_w16, hwnd_of(u));
    int f = d ? w16_dlg_frame(d) : 0, top = d ? w16_dlg_frame_top(d) : 0, cap = d ? w16_dlg_caption(d) : 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (!d) return;
    x = w16_down(x) - f;
    y = w16_down(y) - top - cap;
    if (msg == WM_LBUTTONDOWN && y < 0 && y >= -cap && !g_w16.capture) { uapp_window_move_start(u, 0); return; }
    w16_route_mouse(&g_w16, d, msg, (uint16_t)(buttons & 1 ? 0x0001 : 0), x, y);
}

static void on_press(struct uapp_window *u, int x, int y, unsigned buttons) {
    if (buttons & 1) post_mouse(u, WM_LBUTTONDOWN, x, y, buttons);
}
static void on_motion(struct uapp_window *u, int x, int y, unsigned buttons) {
    post_mouse(u, WM_MOUSEMOVE, x, y, buttons);
}
static void on_release(struct uapp_window *u, int x, int y, unsigned buttons) {
    post_mouse(u, WM_LBUTTONUP, x, y, buttons & ~1u);
}

static void on_key(struct uapp_window *u, int key, unsigned mods) {
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *d = w16_wnd_get(&g_w16, hwnd_of(u));
    pthread_mutex_unlock(&g_w16.lock);
    if (d) w16_route_key(&g_w16, d, key, mods, 0);
}

static void on_close(struct uapp_window *u) {
    w16_post_msg(&g_w16, hwnd_of(u), WM_CLOSE, 0, 0);
}

void w16_dlgwin_open(struct w16 *w, struct uapp *a, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *d = w16_wnd_get(w, hwnd);
    int dw = d ? d->w : 0, dh = d ? d->h : 0, modal = d ? d->modal : 0;
    pthread_mutex_unlock(&w->lock);
    if (!d) return;
    struct uapp_window_desc desc = {
        .title = d->text && d->text[0] ? d->text : "Dialog",
        .w = w16_up(dw),
        .h = w16_up(dh),
        .flags = (modal ? UAPP_WIN_MODAL : 0) | UAPP_WIN_NO_DECOR,
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
        if (!d->ended) d->ended = 1;
        pthread_cond_broadcast(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
    if (u) w16_post_msg(w, hwnd, W16_WM_ACTIVATE, 1, 0);
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

void w16_dlgwin_redraw_all(struct w16 *w) {
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *d = w->wnds[i];
        if (d && d->used && d->kind == W16_K_DIALOG && d->uwin) uapp_window_redraw(d->uwin);
    }
    pthread_mutex_unlock(&w->lock);
}
