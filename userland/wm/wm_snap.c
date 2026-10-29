// Window snapping -- see wm_snap.h.
#include "wm_snap.h"
#include "wm_internal.h"
#include "wm_switcher.h"
#include "wm_shadow.h" // wm_damage_window_rect()
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "kfmt.h"
#include "kapi.h"

static int nib(uint16_t z, int k) { return (z >> (4 * k)) & 0xF; }

void wm_snap_zone_rect(uint16_t zone, int *x, int *y, int *w, int *h) {
    int wa_h = screen_h - taskbar_h;
    // Each edge from its own sixth, so two zones that meet share a pixel
    // column exactly -- rounding a width instead leaves a one-pixel gap.
    int x0 = nib(zone, 0) * screen_w / 6, x1 = nib(zone, 1) * screen_w / 6;
    int y0 = nib(zone, 2) * wa_h / 6,     y1 = nib(zone, 3) * wa_h / 6;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

static int zone_ok(uint16_t z) {
    return nib(z, 0) < nib(z, 1) && nib(z, 1) <= 6 && nib(z, 2) < nib(z, 3) && nib(z, 3) <= 6;
}

// Where a window goes, by the one rule both kinds obey: a client is
// PROPOSED the content size and adopts it with its next present; a
// kernel-space app is simply resized.
static void place(struct window *w, int x, int y, int ww, int hh) {
    wm_damage_window_rect(w->x, w->y, w->w, w->h);
    w->x = x;
    w->y = y;
    if (wm_client_is_client_window(w))
        wm_client_send_resize(w, ww - wm_chrome_w(w), hh - wm_chrome_h(w));
    else { w->w = ww; w->h = hh; }
    wm_damage_window_rect(x, y, ww, hh);
    WM_REDRAW();
}

int wm_snap_window(int idx, uint16_t zone) {
    if (idx < 0 || idx >= window_count) return 0;
    struct window *w = &windows[idx];
    if (!w->resizable || w->popup || w->fullscreen) return 0;
    if (zone && !zone_ok(zone)) return 0;
    if (w->state == WIN_MINIMIZED) wm_window_unminimize(idx);
    if (!zone) {
        if (!w->snap) return 0;
        w->snap = 0;
        place(w, w->saved_x, w->saved_y, w->saved_w, w->saved_h);
        return 1;
    }
    // THE RESTORE RECT IS TAKEN ONCE: a window going from one zone to
    // another, or from maximized (whose saved_* already holds it), keeps
    // the size it had before the first of them.
    if (!w->snap && w->state != WIN_MAXIMIZED) {
        w->saved_x = w->x; w->saved_y = w->y;
        wm_window_heading_size(w, &w->saved_w, &w->saved_h);
    }
    if (w->state == WIN_MAXIMIZED) w->state = WIN_NORMAL;
    w->snap = zone;
    int x, y, ww, hh;
    wm_snap_zone_rect(zone, &x, &y, &ww, &hh);
    place(w, x, y, ww, hh);
    return 1;
}

void wm_snap_relayout(void) {
    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        if (!w->snap || w->fullscreen || w->state == WIN_MAXIMIZED) continue;
        int x, y, ww, hh;
        wm_snap_zone_rect(w->snap, &x, &y, &ww, &hh);
        place(w, x, y, ww, hh);
    }
}

// The half opposite a half -- where Snap Assist offers the rest.
static uint16_t opposite(uint16_t z) {
    if (z == WM_SNAP_LEFT) return WM_SNAP_RIGHT;
    if (z == WM_SNAP_RIGHT) return WM_SNAP_LEFT;
    return 0;
}

// SNAP ASSIST, after a half: unless a window is already in the other
// half, its card lists the rest there (wm_switcher.h).
static void assist_after(int idx, uint16_t zone) {
    uint16_t other = opposite(zone);
    if (!other || idx < 0) return;
    for (int i = 0; i < window_count; i++)
        if (windows[i].snap == other && windows[i].state != WIN_MINIMIZED) return;
    wm_switcher_assist(other, windows[idx].open_seq, 0, 0);
}

int wm_snap_window_assisted(int idx, uint16_t zone) {
    if (idx < 0 || idx >= window_count) return 0;
    uint32_t seq = windows[idx].open_seq;
    if (!wm_snap_window(idx, zone)) return 0;
    assist_after(wm_window_by_seq(seq), zone);
    return 1;
}

// --- Super+arrows ---------------------------------------------------------

int wm_snap_key(int idx, int key) {
    if (idx < 0 || idx >= window_count) return 0;
    struct window *w = &windows[idx];
    if (!w->resizable || w->popup || w->fullscreen) return 0;
    uint16_t s = w->snap;
    int max = w->state == WIN_MAXIMIZED;
    if (key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT) {
        int left = key == KEY_ARROW_LEFT;
        // A window on the far side comes back to its own rect first, as
        // on Windows: a second press then takes the near half.
        if (s && (left ? nib(s, 0) >= 3 : nib(s, 1) <= 3)) return wm_snap_window(idx, 0);
        if (s && (left ? nib(s, 1) <= 3 : nib(s, 0) >= 3)) return 1;   // already there
        return wm_snap_window_assisted(idx, left ? WM_SNAP_LEFT : WM_SNAP_RIGHT);
    }
    if (key == KEY_ARROW_UP) {
        if (s == WM_SNAP_LEFT)  return wm_snap_window(idx, WM_SNAP_TL);
        if (s == WM_SNAP_RIGHT) return wm_snap_window(idx, WM_SNAP_TR);
        if (s == WM_SNAP_BL)    return wm_snap_window(idx, WM_SNAP_LEFT);
        if (s == WM_SNAP_BR)    return wm_snap_window(idx, WM_SNAP_RIGHT);
        if (s == WM_SNAP_TL || s == WM_SNAP_TR) return 1;
        if (!max) wm_set_maximized(idx, 1);
        return 1;
    }
    if (key == KEY_ARROW_DOWN) {
        if (max) { wm_set_maximized(idx, 0); return 1; }
        if (s == WM_SNAP_LEFT)  return wm_snap_window(idx, WM_SNAP_BL);
        if (s == WM_SNAP_RIGHT) return wm_snap_window(idx, WM_SNAP_BR);
        if (s == WM_SNAP_TL)    return wm_snap_window(idx, WM_SNAP_LEFT);
        if (s == WM_SNAP_TR)    return wm_snap_window(idx, WM_SNAP_RIGHT);
        if (s && s != WM_SNAP_BL && s != WM_SNAP_BR) return wm_snap_window(idx, 0);
        wm_window_minimize(idx);
        return 1;
    }
    return 0;
}

// --- the drag -------------------------------------------------------------

static uint16_t g_preview;

// The drop targets: a band along each side edge and the top, with the
// corners a fifth of the work area's height (and an eighth of the width
// along the top) -- about where Windows and KWin put theirs.
uint16_t wm_snap_zone_at(int mx, int my) {
    int band = 4;
    int wa_h = screen_h - taskbar_h;
    if (my >= wa_h) return 0;
    int corner_h = wa_h / 5, corner_w = screen_w / 8;
    int at_l = mx < band, at_r = mx >= screen_w - band, at_t = my < band;
    if (at_l || at_r) {
        if (my < corner_h)        return at_l ? WM_SNAP_TL : WM_SNAP_TR;
        if (my >= wa_h - corner_h) return at_l ? WM_SNAP_BL : WM_SNAP_BR;
        return at_l ? WM_SNAP_LEFT : WM_SNAP_RIGHT;
    }
    if (at_t) {
        if (mx < corner_w)             return WM_SNAP_TL;
        if (mx >= screen_w - corner_w) return WM_SNAP_TR;
        return WM_SNAP_MAXIMIZE;
    }
    return 0;
}

// The preview's rect: the zone (or the work area, for maximize) inset by
// a few pixels, so it reads as a target rather than as a window.
static int preview_rect(uint16_t z, int *x, int *y, int *w, int *h) {
    if (!z) return 0;
    if (z == WM_SNAP_MAXIMIZE) { *x = 0; *y = 0; *w = screen_w; *h = screen_h - taskbar_h; }
    else wm_snap_zone_rect(z, x, y, w, h);
    int in = 6;
    *x += in; *y += in; *w -= 2 * in; *h -= 2 * in;
    return *w > 0 && *h > 0;
}

void wm_snap_preview(uint16_t z) {
    if (z == g_preview) return;
    int x, y, w, h;
    if (preview_rect(g_preview, &x, &y, &w, &h)) wm_damage_rect(x, y, w, h);
    g_preview = z;
    if (preview_rect(g_preview, &x, &y, &w, &h)) wm_damage_rect(x, y, w, h);
    WM_REDRAW();
}

uint16_t wm_snap_preview_zone(void) { return g_preview; }

// A ROUNDED SEE-THROUGH SHEET: `color` laid at `alpha` over what is
// there, the corners cut row by row so the wash does not spill past a
// ring drawn on the same rect.
void wm_snap_sheet(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha) {
    struct ugfx_surface *s = wm_surface();
    for (int row = 0; row < h; row++) {
        int dy = row < r ? r - row : row >= h - r ? row - (h - r - 1) : 0;
        int cut = 0;
        if (dy) {
            // r - sqrt(r^2 - dy^2), in integers.
            int k = 0;
            while ((k + 1) * (k + 1) <= r * r - dy * dy) k++;
            cut = r - k;
        }
        ugfx_blend_hspan(s, x + cut, y + row, w - 2 * cut, color, 0, alpha);
    }
}

// THE ACCENT, SEE-THROUGH, on a rounded rect ringed in it -- Windows
// 11's snap preview and KWin's outline are both a translucent sheet of
// the accent.
void wm_snap_preview_draw(void) {
    int x, y, w, h;
    if (!preview_rect(g_preview, &x, &y, &w, &h)) return;
    struct ugfx_surface *s = wm_surface();
    int r = 8;
    uint32_t accent = UTHEME_ACCENT;
    wm_snap_sheet(x, y, w, h, r, accent, 80);
    uui_stroke_round_rect(s, x, y, w, h, r, accent);
    uui_stroke_round_rect(s, x + 1, y + 1, w - 2, h - 2, r - 1, accent);
}

void wm_snap_name(uint16_t z, char *out, int cap) {
    static const struct { uint16_t z; const char *n; } names[] = {
        { WM_SNAP_LEFT, "left" }, { WM_SNAP_RIGHT, "right" },
        { WM_SNAP_TL, "tl" }, { WM_SNAP_TR, "tr" }, { WM_SNAP_BL, "bl" }, { WM_SNAP_BR, "br" },
    };
    if (!z) { if (cap > 0) out[0] = 0; return; }
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].z == z) { k_strlcpy(out, names[i].n, (size_t)cap); return; }
    k_snprintf(out, (size_t)cap, "%d-%d,%d-%d", nib(z, 0), nib(z, 1), nib(z, 2), nib(z, 3));
}
