// The Alt+Tab switcher -- see wm_switcher.h.
#include "wm_switcher.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_taskbar.h"
#include "wm_thumb.h"
#include "wm_snap.h"
#include "wm_flyout.h"
#include "wm_glass.h"
#include "wm_log.h"
#include "lib/icon_cache.h"
#include "lib/ugrow.h"
#include "ui/uui.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"

// A tap shorter than this switches without the card ever drawing --
// Plasma's task switcher waits about as long before it shows.
#define SHOW_DELAY_NS (100ull * 1000000ull)

#define CARD_PAD  16
#define CELL_GAP  12
#define CELL_PAD  6     // around a thumbnail box: where the pick's fill shows
#define BOX_MIN   64    // a thumbnail is never narrower, however many windows

struct sw_entry {
    struct wm_thumb t;
    int minimized;
};

static struct sw_entry *g_e;   // grown on demand (lib/ugrow.h)
static int g_cap, g_n;
static int g_active;           // Alt held, picking
static int g_shown;            // the card is drawn
static uint64_t g_opened_ns;
static int g_pick, g_hover = -1;

// SNAP ASSIST (wm_snap.h): the same card laid in the zone left empty,
// listing the windows not yet placed; a click puts one there. `g_more`
// holds a layout's further zones, offered one after another, and
// `g_taken` the windows already placed, which no later card lists.
#define ASSIST_MAX 4
static int g_assist;
static uint16_t g_zone;
static uint16_t g_more[ASSIST_MAX];
static int g_nmore;
static uint32_t g_taken[ASSIST_MAX + 1];
static int g_ntaken;
// The last switch, for `gui switcher`: how long Alt was held and whether
// the card had drawn -- a test over a slow console cannot make a tap.
static int g_last_ms = -1, g_last_drawn;

// --- geometry -------------------------------------------------------------

struct sw_geom {
    int x, y, w, h;
    int bw, bh;                // a thumbnail box
    int cw, ch;                // a cell: header, gap, box, padding
    int cols, rows, head_h;
    int grid_y;                // the first row's top
};

static int header_h(void) { return ugfx_char_h() + 8; }

static void cells_for(struct sw_geom *g, int bw, int room_w) {
    g->bw = bw;
    g->bh = bw * 5 / 8;
    g->head_h = header_h();
    g->cw = bw + 2 * CELL_PAD;
    g->ch = CELL_PAD + g->head_h + CELL_PAD + g->bh + CELL_PAD;
    int room = room_w - 2 * CARD_PAD + CELL_GAP;
    int cols = room / (g->cw + CELL_GAP);
    if (cols < 1) cols = 1;
    if (cols > g_n) cols = g_n > 0 ? g_n : 1;
    g->cols = cols;
    g->rows = (g_n + cols - 1) / cols;
    if (g->rows < 1) g->rows = 1;
    g->w = 2 * CARD_PAD + cols * g->cw + (cols - 1) * CELL_GAP;
    g->h = 2 * CARD_PAD + g->rows * g->ch + (g->rows - 1) * CELL_GAP;
}

// About Windows 11's thumbnail on a 1280 screen, shrunk until every
// window fits on the screen above the taskbar -- Plasma's grid does the
// same rather than scroll.
// Snap Assist's card FILLS its zone, a few pixels in, with the grid
// centred on it.
static void geometry(struct sw_geom *g) {
    int rx = 0, ry = 0, rw = screen_w * 9 / 10, rh = (screen_h - taskbar_h) * 9 / 10;
    if (g_assist) {
        wm_snap_zone_rect(g_zone, &rx, &ry, &rw, &rh);
        rx += 6; ry += 6; rw -= 12; rh -= 12;
    }
    int bw = screen_w * 15 / 100;
    if (bw < 140) bw = 140;
    if (bw > 280) bw = 280;
    for (;;) {
        cells_for(g, bw, rw);
        if ((g->h <= rh && g->w <= rw) || bw <= BOX_MIN) break;
        bw = bw * 9 / 10;
        if (bw < BOX_MIN) bw = BOX_MIN;
    }
    if (g_assist) {
        int grid_h = g->h - 2 * CARD_PAD;
        g->x = rx; g->y = ry; g->w = rw; g->h = rh;
        g->grid_y = ry + (rh - grid_h) / 2;
        return;
    }
    g->x = (screen_w - g->w) / 2;
    g->y = (screen_h - taskbar_h - g->h) / 2;
    if (g->y < 0) g->y = 0;
    g->grid_y = g->y + CARD_PAD;
}

// Entry k's cell. The LAST ROW IS CENTRED, as Windows 11's is.
static void cell_rect(const struct sw_geom *g, int k, int *x, int *y) {
    int row = k / g->cols, col = k % g->cols;
    int in_row = row == g->rows - 1 ? g_n - row * g->cols : g->cols;
    int row_w = in_row * g->cw + (in_row - 1) * CELL_GAP;
    *x = g->x + CARD_PAD + (g->w - 2 * CARD_PAD - row_w) / 2 + col * (g->cw + CELL_GAP);
    *y = g->grid_y + row * (g->ch + CELL_GAP);
}

static void thumb_box(const struct sw_geom *g, int k, int *bx, int *by) {
    int cx, cy;
    cell_rect(g, k, &cx, &cy);
    *bx = cx + CELL_PAD;
    *by = cy + CELL_PAD + g->head_h + CELL_PAD;
}

// --- the list -------------------------------------------------------------

static void drop_all(void) {
    for (int k = 0; k < g_n; k++) wm_thumb_free(&g_e[k].t);
    g_n = 0;
}

static int add(int i, int minimized) {
    if (!UGROW(g_e, g_cap, g_n + 1)) {
        wm_logf("switcher: out of memory at %d windows\n", g_n);
        return 0;
    }
    struct sw_entry *e = &g_e[g_n++];
    e->t.img.px = 0;
    if (!windows[i].open_seq) windows[i].open_seq = wm_next_open_seq();
    wm_thumb_reset(&e->t, windows[i].open_seq);
    e->minimized = minimized;
    return 1;
}

static int taken(uint32_t seq) {
    for (int k = 0; k < g_ntaken; k++)
        if (seq && g_taken[k] == seq) return 1;
    return 0;
}

// MOST RECENTLY USED FIRST: windows[] is activation order, newest last.
// Minimized windows after the rest.
static void build(int same_app, int focus) {
    drop_all();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = window_count - 1; i >= 0; i--) {
            if (!taskbar_lists_window(i)) continue;
            int mini = windows[i].state == WIN_MINIMIZED;
            if (mini != pass) continue;
            if (same_app && (focus < 0 || !taskbar_same_app(i, focus))) continue;
            if (g_assist && taken(windows[i].open_seq)) continue;
            if (!add(i, mini)) return;
        }
    }
}

// --- open, step, commit ---------------------------------------------------

int wm_switcher_active(void) { return g_active && !g_assist; }
int wm_switcher_is_open(void) { return g_active; }

void wm_switcher_damage(void) { wm_overlay_damage("switcher"); }

void wm_switcher_close(void) {
    if (!g_active) return;
    g_active = 0;     // where it was drawn is damaged by the core (wm_overlay.h)
    g_assist = 0;
    g_shown = 0;
    g_hover = -1;
    drop_all();
}

int wm_switcher_step(int dir, int same_app) {
    if (g_active && g_assist) {
        g_nmore = 0;          // Alt+Tab over an assist card ends the layout
        wm_switcher_close();
    }
    if (!g_active) {
        int focus = wm_focus_index();
        build(same_app, focus);
        if (g_n == 0) return 0;
        wm_overlay_close_others("switcher");
        g_active = 1;
        g_shown = 0;
        g_hover = -1;
        g_opened_ns = sys_monotonic_ns();
        // The first press picks the window BEFORE the focused one -- or
        // the first, when nothing is focused (all minimized, or the
        // desktop has the keyboard). Shift+Tab first picks the last.
        int first_is_focus = focus >= 0 && windows[focus].open_seq == g_e[0].t.seq;
        if (dir < 0) g_pick = g_n - 1;
        else g_pick = first_is_focus && g_n > 1 ? 1 : 0;
        return 1;
    }
    g_pick = ((g_pick + (dir < 0 ? -1 : 1)) % g_n + g_n) % g_n;
    wm_switcher_damage();
    return 1;
}

static int assist_open(uint16_t zone) {
    g_assist = 1;
    g_zone = zone;
    build(0, -1);
    if (g_n == 0) { g_assist = 0; return 0; }
    wm_overlay_close_others("switcher");
    g_active = 1;
    g_shown = 1;
    g_hover = -1;
    g_pick = -1;     // nothing picked until an arrow: a click is the usual answer
    wm_switcher_damage();
    return 1;
}

int wm_switcher_assist(uint16_t zone, uint32_t except_seq, const uint16_t *more, int n_more) {
    if (g_active) wm_switcher_close();
    g_ntaken = 0;
    if (except_seq) g_taken[g_ntaken++] = except_seq;
    g_nmore = 0;
    for (int k = 0; k < n_more && g_nmore < ASSIST_MAX; k++) g_more[g_nmore++] = more[k];
    return assist_open(zone);
}

static void switch_to(int k) {
    int idx = k >= 0 && k < g_n ? wm_window_by_seq(g_e[k].t.seq) : -1;
    if (g_assist) {
        uint16_t zone = g_zone;
        wm_switcher_close();
        if (idx < 0) return;
        uint32_t seq = windows[idx].open_seq;
        if (wm_snap_window(idx, zone)) wm_bring_forward(wm_window_by_seq(seq));
        if (g_ntaken < ASSIST_MAX + 1) g_taken[g_ntaken++] = seq;
        // THE LAYOUT'S NEXT ZONE, until one has a window left to offer.
        while (g_nmore > 0) {
            uint16_t next = g_more[0];
            for (int j = 1; j < g_nmore; j++) g_more[j - 1] = g_more[j];
            g_nmore--;
            if (assist_open(next)) break;
        }
        return;
    }
    wm_switcher_close();
    if (idx >= 0) wm_bring_forward(idx);
}

void wm_switcher_commit(void) {
    if (!g_active || g_pick < 0) return;
    g_last_ms = (int)((sys_monotonic_ns() - g_opened_ns) / 1000000);
    g_last_drawn = g_shown;
    switch_to(g_pick);
}

void wm_switcher_tick(void) {
    if (!g_active) return;
    uint64_t now_ns = sys_monotonic_ns();
    if (!g_shown && now_ns - g_opened_ns >= SHOW_DELAY_NS) {
        g_shown = 1;
        wm_switcher_damage();
    }
    // A WINDOW THAT CLOSED UNDER THE CARD LEAVES IT; the pick stays on
    // the same window, or the one that moved into its place.
    int kept = 0, gone = 0;
    for (int k = 0; k < g_n; k++) {
        if (wm_window_by_seq(g_e[k].t.seq) < 0) {
            wm_thumb_free(&g_e[k].t);
            if (k < g_pick) g_pick--;
            gone = 1;
            continue;
        }
        if (kept != k) {
            g_e[kept] = g_e[k];   // the thumbnail moves with it
            g_e[k].t.img.px = 0;
        }
        kept++;
    }
    if (gone) {
        wm_switcher_damage();   // the bigger card it was
        g_n = kept;
        if (!g_n) { wm_switcher_close(); return; }
        if (g_pick >= g_n) g_pick = g_n - 1;
        if (g_pick < 0) g_pick = 0;
        g_hover = -1;
    }
    if (!g_shown) return;
    struct sw_geom g;
    geometry(&g);
    uint64_t now = sys_ticks();
    int changed = gone;
    for (int k = 0; k < g_n; k++) changed |= wm_thumb_update(&g_e[k].t, g.bw, g.bh, now);
    if (changed) wm_switcher_damage();
}

int wm_switcher_wait_ms(void) {
    if (!g_active || g_shown) return -1;
    uint64_t now = sys_monotonic_ns(), due = g_opened_ns + SHOW_DELAY_NS;
    return now >= due ? 0 : (int)((due - now + 999999) / 1000000);
}

void wm_switcher_presented(uint32_t open_seq) {
    for (int k = 0; k < g_n; k++)
        if (g_e[k].t.seq == open_seq) g_e[k].t.dirty = 1;
}

// --- the overlay's ops ----------------------------------------------------

int wm_switcher_rect(int *x, int *y, int *w, int *h) {
    if (!g_active || !g_shown) return 0;
    struct sw_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

static int entry_at(int mx, int my) {
    if (!g_shown) return -1;
    struct sw_geom g;
    geometry(&g);
    for (int k = 0; k < g_n; k++) {
        int cx, cy;
        cell_rect(&g, k, &cx, &cy);
        if (uui_hit(cx, cy, g.cw, g.ch, mx, my)) return k;
    }
    return -1;
}

int wm_switcher_hover_at(int mx, int my) {
    if (!g_active) return 0;
    g_hover = entry_at(mx, my);
    return g_hover + 1;
}

// A click on an entry switches to it; anywhere else cancels and falls
// through, as every popup's dismissing click does.
int wm_switcher_click(int mx, int my) {
    if (!g_active) return 0;
    int k = entry_at(mx, my);
    if (k >= 0) { switch_to(k); return 1; }
    int x, y, w, h;
    int on_card = wm_switcher_rect(&x, &y, &w, &h) && uui_hit(x, y, w, h, mx, my);
    if (g_assist && on_card) return 1;   // between cards: inert, but ours
    g_nmore = 0;
    wm_switcher_close();
    return on_card;
}

// Tab itself never gets here (wm.c steps before the overlays): Esc,
// the arrows by one and by a row, and Enter to switch now.
int wm_switcher_key(int key, uint8_t mods) {
    (void)mods;
    if (!g_active) return 0;
    if (key == 0x1B) { g_nmore = 0; wm_switcher_close(); return 1; }
    if (key == '\n' || key == '\r') {
        if (g_assist) { if (g_pick >= 0) switch_to(g_pick); }
        else wm_switcher_commit();
        return 1;
    }
    struct sw_geom g;
    geometry(&g);
    int step = 0;
    if (key == KEY_ARROW_RIGHT) step = 1;
    else if (key == KEY_ARROW_LEFT) step = -1;
    else if (key == KEY_ARROW_DOWN) step = g.cols;
    else if (key == KEY_ARROW_UP) step = -g.cols;
    if (!step) return 0;
    if (g_pick < 0) { g_pick = 0; wm_switcher_damage(); return 1; }
    int next = g_pick + step;
    if (step == 1 || step == -1) next = (next % g_n + g_n) % g_n;
    else if (next < 0 || next >= g_n) return 1;   // no row there: stay
    g_pick = next;
    wm_switcher_damage();
    return 1;
}

// --- drawing --------------------------------------------------------------

static void draw_entry(struct ugfx_surface *s, const struct sw_geom *g, int k) {
    int idx = wm_window_by_seq(g_e[k].t.seq);
    if (idx < 0) return;
    int cx, cy;
    cell_rect(g, k, &cx, &cy);
    int r = uui_popup_radius() > 4 ? uui_popup_radius() - 2 : 2;
    // THE PICK IS FILLED IN THE SELECTION AND RINGED IN THE ACCENT, two
    // pixels -- what Super's release (Alt's here) lands on must read
    // from across the room; a hover gets the state wash only.
    if (g_assist) {
        // ON ASSIST'S DARK SHEET the pick and the hover are washes of the
        // accent, the pick ringed -- the S2 mockup's.
        if (k == g_pick || k == g_hover)
            wm_snap_sheet(cx, cy, g->cw, g->ch, r, UTHEME_ACCENT, k == g_pick ? 150 : 100);
        if (k == g_pick)
            uui_stroke_round_rect(s, cx, cy, g->cw, g->ch, r,
                                  ugfx_blend(UTHEME_ACCENT, ugfx_rgb(255, 255, 255), 110));
    } else if (k == g_pick) {
        uui_fill_round_rect(s, cx, cy, g->cw, g->ch, r, UTHEME_ACCENT);
        uui_fill_round_rect(s, cx + 2, cy + 2, g->cw - 4, g->ch - 4, r > 2 ? r - 2 : 1,
                            UTHEME_SELECTION);
    } else if (k == g_hover) {
        uui_fill_round_rect(s, cx, cy, g->cw, g->ch, r,
                            uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
    }

    // THE TITLE ROW: icon, then the title, bold on the pick.
    int hy = cy + CELL_PAD;
    int isz = g->head_h - 8;
    int tx = cx + CELL_PAD;
    const char *iname = wm_window_icon_name(idx);
    const struct uimg *ico = iname ? icon_get(iname, isz) : 0;
    if (ico) {
        ugfx_blit_alpha(s, tx, hy + (g->head_h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
        tx += ico->w + 6;
    }
    uint32_t ink = g_e[k].minimized && k != g_pick ? wm_flyout_dim() : UTHEME_TEXT;
    if (g_assist) ink = g_e[k].minimized ? ugfx_rgb(176, 180, 190) : ugfx_rgb(255, 255, 255);
    const struct ugfx_font *was = 0;
    if (k == g_pick) was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, tx, hy + (g->head_h - ugfx_char_h()) / 2,
                            cx + g->cw - CELL_PAD - tx, windows[idx].title, ink,
                            UGFX_TRANSPARENT);
    if (was) ugfx_set_font(was);

    // THE THUMBNAIL, centred in its box. A window with no buffer to
    // scale -- a kernel-space app, a client not drawn yet -- shows its
    // icon at half the box.
    int bx, by;
    thumb_box(g, k, &bx, &by);
    const struct uimg *t = &g_e[k].t.img;
    if (t->px) {
        int px = bx + (g->bw - t->w) / 2, py = by + (g->bh - t->h) / 2;
        ugfx_blit(s, px, py, t->w, t->h, t->px, t->w);
        ugfx_draw_rect(s, px, py, t->w, t->h, g_e[k].minimized ? UTHEME_SEPARATOR : UTHEME_OUTLINE);
    } else {
        uui_fill_round_rect(s, bx, by, g->bw, g->bh, 4, UTHEME_WINDOW_BG);
        const struct uimg *big = iname ? icon_get(iname, g->bh / 2) : 0;
        if (big)
            ugfx_blit_alpha(s, bx + (g->bw - big->w) / 2, by + (g->bh - big->h) / 2,
                            big->w, big->h, big->px, big->w);
    }
}

void wm_switcher_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!g_active || !g_shown) return;
    struct sw_geom g;
    geometry(&g);
    struct ugfx_surface *s = wm_surface();
    if (g_assist) {
        // The empty zone DIMMED, not covered: Windows 11's Snap Assist.
        wm_snap_sheet(g.x, g.y, g.w, g.h, 8, ugfx_rgb(20, 24, 32), 180);
        uui_stroke_round_rect(s, g.x, g.y, g.w, g.h, 8, ugfx_rgb(96, 102, 116));
    } else {
        wm_flyout_card(g.x, g.y, g.w, g.h, 0);
    }
    for (int k = 0; k < g_n; k++) draw_entry(s, &g, k);
}

void wm_switcher_last(int *ms, int *drawn) { *ms = g_last_ms; *drawn = g_last_drawn; }
uint16_t wm_switcher_assist_zone(void) { return g_active && g_assist ? g_zone : 0; }

int wm_switcher_state(int *shown, int *pick, int *x, int *y, int *w, int *h,
                      struct wm_switcher_entry_info *out, int max) {
    *shown = g_shown;
    *pick = g_active ? g_pick : -1;
    *x = *y = *w = *h = 0;
    if (!g_active) return 0;
    struct sw_geom g;
    geometry(&g);
    if (g_shown) { *x = g.x; *y = g.y; *w = g.w; *h = g.h; }
    int n = 0;
    for (int k = 0; k < g_n && n < max; k++) {
        int idx = wm_window_by_seq(g_e[k].t.seq);
        if (idx < 0) continue;
        int cx, cy, bx, by;
        cell_rect(&g, k, &cx, &cy);
        thumb_box(&g, k, &bx, &by);
        const struct uimg *t = &g_e[k].t.img;
        out[n].title = windows[idx].title;
        out[n].minimized = g_e[k].minimized;
        out[n].x = cx; out[n].y = cy; out[n].w = g.cw; out[n].h = g.ch;
        out[n].thumb_w = t->px ? t->w : 0;
        out[n].thumb_h = t->px ? t->h : 0;
        out[n].thumb_x = bx + (g.bw - out[n].thumb_w) / 2;
        out[n].thumb_y = by + (g.bh - out[n].thumb_h) / 2;
        n++;
    }
    return n;
}
