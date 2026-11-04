// Snap layouts -- see wm_snaplayout.h.
#include "wm_snaplayout.h"
#include "wm_snap.h"
#include "wm_switcher.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_flyout.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"

#define Z WM_SNAP_ZONE
// Reading order, three to a row: Windows 11's set on a wide screen.
static const uint16_t L0[] = { Z(0, 3, 0, 6), Z(3, 6, 0, 6) };
static const uint16_t L1[] = { Z(0, 3, 0, 6), Z(3, 6, 0, 3), Z(3, 6, 3, 6) };
static const uint16_t L2[] = { Z(0, 3, 0, 3), Z(0, 3, 3, 6), Z(3, 6, 0, 6) };
static const uint16_t L3[] = { Z(0, 3, 0, 3), Z(3, 6, 0, 3), Z(0, 3, 3, 6), Z(3, 6, 3, 6) };
static const uint16_t L4[] = { Z(0, 2, 0, 6), Z(2, 4, 0, 6), Z(4, 6, 0, 6) };
static const uint16_t L5[] = { Z(0, 4, 0, 6), Z(4, 6, 0, 6) };
#undef Z
static const struct { const uint16_t *z; int n; } g_layouts[] = {
    { L0, 2 }, { L1, 3 }, { L2, 3 }, { L3, 4 }, { L4, 3 }, { L5, 2 },
};
#define NLAYOUTS ((int)(sizeof g_layouts / sizeof g_layouts[0]))
#define COLS 3
#define NZONES (2 + 3 + 3 + 4 + 3 + 2)

// The rest before it opens: shorter than a tooltip's, about Windows'.
#define OPEN_DELAY_NS (400ull * 1000000ull)
// How long it survives the pointer leaving the button and the card --
// the crossing from one to the other.
#define GRACE_NS      (300ull * 1000000ull)

static int g_open;
static int g_kb;                  // opened by Super+Z: no hover grace
static uint32_t g_seq;            // the window it lays out
static int g_hot = -1;            // flat zone index
static uint32_t g_btn_seq;        // whose Maximize button the pointer rests on
static uint64_t g_btn_since, g_left_since;
static int g_suppress;            // after a click, until the pointer leaves the button

int wm_snaplayout_is_open(void) { return g_open; }
void wm_snaplayout_damage(void) { wm_overlay_damage("snaplayout"); }

static void flat_zone(int k, int *layout, uint16_t *zone) {
    for (int l = 0; l < NLAYOUTS; l++) {
        if (k < g_layouts[l].n) { *layout = l; *zone = g_layouts[l].z[k]; return; }
        k -= g_layouts[l].n;
    }
    *layout = -1; *zone = 0;
}

static int first_of(int layout) {
    int k = 0;
    for (int l = 0; l < layout; l++) k += g_layouts[l].n;
    return k;
}

// --- geometry -------------------------------------------------------------

struct sl_geom { int x, y, w, h, tw, th, pad, gap; };

static void geometry(struct sl_geom *g) {
    int idx = wm_window_by_seq(g_seq);
    g->pad = 12; g->gap = 10;
    g->tw = ugfx_char_w() * 11;
    if (g->tw < 72) g->tw = 72;
    g->th = g->tw * (screen_h - taskbar_h) / (screen_w > 0 ? screen_w : 1);
    g->w = 2 * g->pad + COLS * g->tw + (COLS - 1) * g->gap;
    g->h = 2 * g->pad + 2 * g->th + g->gap;
    int ax = screen_w / 2, ay = 0;
    if (idx >= 0) {
        struct btn_rects r = title_buttons(&windows[idx]);
        ax = r.max_x + r.w / 2;
        ay = r.y + r.h + 6;
    }
    wm_popup_place(ax - g->w / 2, ay, g->w, g->h, &g->x, &g->y);
}

static void tile_rect(const struct sl_geom *g, int l, int *x, int *y) {
    *x = g->x + g->pad + (l % COLS) * (g->tw + g->gap);
    *y = g->y + g->pad + (l / COLS) * (g->th + g->gap);
}

// A zone's rect on its tile: the tile's inside in sixths, each zone two
// pixels in, so neighbours stand four apart.
static void zone_rect(const struct sl_geom *g, int l, uint16_t z, int *x, int *y, int *w, int *h) {
    int tx, ty;
    tile_rect(g, l, &tx, &ty);
    int ix = tx + 3, iy = ty + 3, iw = g->tw - 6, ih = g->th - 6;
    int x0 = ix + (z & 0xF) * iw / 6, x1 = ix + ((z >> 4) & 0xF) * iw / 6;
    int y0 = iy + ((z >> 8) & 0xF) * ih / 6, y1 = iy + ((z >> 12) & 0xF) * ih / 6;
    *x = x0 + 2; *y = y0 + 2; *w = x1 - x0 - 4; *h = y1 - y0 - 4;
}

int wm_snaplayout_rect(int *x, int *y, int *w, int *h) {
    if (!g_open) return 0;
    struct sl_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

static int zone_at(int mx, int my) {
    struct sl_geom g;
    geometry(&g);
    for (int k = 0; k < NZONES; k++) {
        int l, x, y, w, h;
        uint16_t z;
        flat_zone(k, &l, &z);
        zone_rect(&g, l, z, &x, &y, &w, &h);
        if (uui_hit(x - 2, y - 2, w + 4, h + 4, mx, my)) return k;
    }
    return -1;
}

// --- open and close -------------------------------------------------------

static int can_snap(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    const struct window *w = &windows[idx];
    return w->resizable && !w->popup && !w->fullscreen && !w->no_decor &&
           w->state != WIN_MINIMIZED;
}

static void set_hot(int k) {
    if (k == g_hot) return;
    g_hot = k;
    int l;
    uint16_t z;
    flat_zone(k, &l, &z);
    wm_snap_preview(k >= 0 ? z : 0);   // where it would go, on the screen itself
    wm_snaplayout_damage();
}

static void open_for(int idx, int kb) {
    if (!windows[idx].open_seq) windows[idx].open_seq = wm_next_open_seq();
    wm_overlay_close_others("snaplayout");
    g_seq = windows[idx].open_seq;
    g_open = 1;
    g_kb = kb;
    g_hot = -1;
    g_left_since = 0;
    if (kb) set_hot(0);
    wm_snaplayout_damage();
}

void wm_snaplayout_close(void) {
    if (!g_open) return;
    g_open = 0;    // where it was drawn is damaged by the core (wm_overlay.h)
    g_kb = 0;
    g_hot = -1;
    g_suppress = 1;
    wm_snap_preview(0);
}

int wm_snaplayout_open_for(int idx) {
    if (!can_snap(idx)) return 0;
    open_for(idx, 1);
    return 1;
}

void wm_snaplayout_poll(int mx, int my, uint8_t buttons) {
    uint64_t now = sys_monotonic_ns();
    int on_btn = title_hover_win >= 0 && title_hover_kind == 1;
    if (g_open) {
        int idx = wm_window_by_seq(g_seq);
        if (!can_snap(idx)) { wm_snaplayout_close(); return; }
        if (g_kb) return;
        int x = 0, y = 0, w = 0, h = 0;
        wm_snaplayout_rect(&x, &y, &w, &h);
        if ((on_btn && title_hover_win == idx) || uui_hit(x, y, w, h, mx, my)) {
            g_left_since = 0;
        } else if (!g_left_since) {
            g_left_since = now;
        } else if (now - g_left_since >= GRACE_NS) {
            wm_snaplayout_close();
        }
        return;
    }
    if (!on_btn || !can_snap(title_hover_win) || wm_overlay_any_open()) {
        g_btn_seq = 0;
        g_suppress = 0;
        return;
    }
    if (buttons) { g_suppress = 1; return; }   // a press on Maximize is an answer
    if (g_suppress) return;
    struct window *w = &windows[title_hover_win];
    if (!w->open_seq) w->open_seq = wm_next_open_seq();
    if (w->open_seq != g_btn_seq) {
        g_btn_seq = w->open_seq;
        g_btn_since = now;
    } else if (now - g_btn_since >= OPEN_DELAY_NS) {
        open_for(title_hover_win, 0);
    }
}

int wm_snaplayout_wait_ms(void) {
    uint64_t now = sys_monotonic_ns(), due;
    if (g_open && !g_kb && g_left_since) due = g_left_since + GRACE_NS;
    else if (!g_open && g_btn_seq && !g_suppress) due = g_btn_since + OPEN_DELAY_NS;
    else return -1;
    return now >= due ? 0 : (int)((due - now + 999999) / 1000000);
}

// --- acting ---------------------------------------------------------------

// The window goes to the zone; the layout's OTHER zones are then offered
// in reading order through Snap Assist.
static void commit(int k) {
    int l;
    uint16_t z;
    flat_zone(k, &l, &z);
    int idx = wm_window_by_seq(g_seq);
    uint32_t seq = g_seq;
    wm_snaplayout_close();
    if (l < 0 || idx < 0 || !wm_snap_window(idx, z)) return;
    uint16_t rest[4];
    int n = 0;
    for (int j = 0; j < g_layouts[l].n && n < 4; j++)
        if (g_layouts[l].z[j] != z) rest[n++] = g_layouts[l].z[j];
    if (n) wm_switcher_assist(rest[0], seq, rest + 1, n - 1);
}

int wm_snaplayout_hover_at(int mx, int my) {
    if (!g_open) return 0;
    int k = zone_at(mx, my);
    if (k >= 0 || !g_kb) set_hot(k);
    return k + 1;
}

int wm_snaplayout_click(int mx, int my) {
    if (!g_open) return 0;
    int k = zone_at(mx, my);
    if (k >= 0) { commit(k); return 1; }
    int x = 0, y = 0, w = 0, h = 0;
    wm_snaplayout_rect(&x, &y, &w, &h);
    if (uui_hit(x, y, w, h, mx, my)) return 1;   // between tiles: inert, but ours
    wm_snaplayout_close();
    return 0;
}

int wm_snaplayout_key(int key, uint8_t mods) {
    (void)mods;
    if (!g_open) return 0;
    if (key == 0x1B) { wm_snaplayout_close(); return 1; }
    if ((key == '\n' || key == '\r') && g_hot >= 0) { commit(g_hot); return 1; }
    int cur = g_hot < 0 ? 0 : g_hot;
    if (key == KEY_ARROW_RIGHT || key == KEY_ARROW_LEFT) {
        set_hot((cur + (key == KEY_ARROW_RIGHT ? 1 : NZONES - 1)) % NZONES);
        return 1;
    }
    if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN) {
        int l;
        uint16_t z;
        flat_zone(cur, &l, &z);
        l = (l + (key == KEY_ARROW_DOWN ? COLS : NLAYOUTS - COLS)) % NLAYOUTS;
        set_hot(first_of(l));
        return 1;
    }
    return 0;
}

// --- drawing --------------------------------------------------------------

void wm_snaplayout_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!g_open) return;
    struct sl_geom g;
    geometry(&g);
    wm_flyout_card(g.x, g.y, g.w, g.h, 0);
    struct ugfx_surface *s = wm_surface();
    for (int l = 0; l < NLAYOUTS; l++) {
        int tx, ty;
        tile_rect(&g, l, &tx, &ty);
        uui_fill_round_rect(s, tx, ty, g.tw, g.th, 6, UTHEME_WHITE);
        uui_stroke_round_rect(s, tx, ty, g.tw, g.th, 6, UTHEME_SEPARATOR);
    }
    for (int k = 0; k < NZONES; k++) {
        int l, x, y, w, h;
        uint16_t z;
        flat_zone(k, &l, &z);
        zone_rect(&g, l, z, &x, &y, &w, &h);
        uui_fill_round_rect(s, x, y, w, h, 3, k == g_hot ? UTHEME_ACCENT : UTHEME_BUTTON_BG);
    }
}

int wm_snaplayout_state(int *x, int *y, int *w, int *h, int *hot,
                        struct wm_snaplayout_zone_info *out, int max) {
    *hot = g_open ? g_hot : -1;
    if (!wm_snaplayout_rect(x, y, w, h)) { *x = *y = *w = *h = 0; return 0; }
    struct sl_geom g;
    geometry(&g);
    int n = 0;
    for (int k = 0; k < NZONES && n < max; k++) {
        int l;
        uint16_t z;
        flat_zone(k, &l, &z);
        out[n].layout = l;
        out[n].zone = z;
        zone_rect(&g, l, z, &out[n].x, &out[n].y, &out[n].w, &out[n].h);
        n++;
    }
    return n;
}
