// Quick Settings -- see wm_quick.h.
#include "wm_quick.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_notify.h"
#include "network_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "layout_popup.h"
#include "remote_popup.h"
#include "tray_slider_popup.h"
#include "osk.h"
#include "lib/icon_cache.h"
#include "lib/usetting.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"

#define SET_GLASS "desktop.transparency"
#define SET_QUICK "desktop.tray.quick"   // off: the members are their own icons again

// The pages: today's flyouts, shown over the group with Back.
enum { PAGE_NONE, PAGE_NETWORK, PAGE_VOLUME, PAGE_BRIGHTNESS, PAGE_LAYOUT, PAGE_REMOTE };
static const struct {
    const char *name;
    void (*open)(void);
    void (*close)(void);
    int *is_open;
    int (*rect)(int *, int *, int *, int *);
} PAGES[] = {
    { 0, 0, 0, 0, 0 },
    { "network",    network_open_page,    network_close,    &network_open,    network_rect },
    { "volume",     volume_open_now,      volume_close,     &volume_open,     volume_rect },
    { "brightness", brightness_open_now,  brightness_close, &brightness_open, brightness_rect },
    { "layout",     layout_open_page,     layout_close,     &layout_open,     layout_rect },
    { "remote",     remote_open_page,     remote_close,     &remote_open,     remote_rect },
};

enum { T_NETWORK, T_DND, T_LAYOUT, T_REMOTE, T_OSK, T_GLASS };
static const char *const TILE_NAMES[WM_QUICK_TILES] = {
    "network", "dnd", "layout", "remote", "osk", "transparency",
};

static int g_tray = -1;
static int g_main;              // the card is up
static int g_page;              // a page is up in its place
static int g_switching;         // opening a page: its close_others() must not end us
static int g_hover;             // HOVER_*; tiles 1..6 main, 11..16 chevron
static int g_drag;              // 1 volume, 2 brightness: a slider is held
enum { HOVER_BACK = 100, HOVER_SETTINGS, HOVER_SL0 = 200, HOVER_SLC0 = 210 };

// --- the tiles' and sliders' state -----------------------------------------

static int glass_on(void) {
    char v[16];
    return usetting_get(SET_GLASS, v, sizeof v) && !k_strcmp(v, "on");
}

static struct tray_slider_popup *slider(int k) { return k == 0 ? volume_slider() : brightness_slider(); }

static int slider_shown(int k) {
    struct tray_slider_popup *p = slider(k);
    if (k == 1 && (brightness_tray_hidden() || p->unavailable[0])) return 0;
    return p->max > p->min;
}

static int tile_on(int t) {
    switch (t) {   // dispatch-ok: the six tiles the card has
    case T_NETWORK: return network_tile_on();
    case T_DND:     return wm_notify_dnd();
    case T_REMOTE:  return remote_desktop_live();
    case T_OSK:     return osk_open;
    case T_GLASS:   return glass_on();
    default:        return 0;
    }
}

static int tile_page(int t) {
    return t == T_NETWORK ? PAGE_NETWORK : t == T_LAYOUT ? PAGE_LAYOUT
         : t == T_REMOTE && remote_session_count() > 0 ? PAGE_REMOTE : PAGE_NONE;
}

static int tile_disabled(int t) {
    if (t == T_LAYOUT) return layout_count() < 2;   // nothing to switch to; its list still opens
    if (t == T_NETWORK) { char s[48]; network_summary(s, sizeof s); return !k_strcmp(s, "No network"); }
    return 0;
}

static void tile_label(int t, char *out, unsigned cap) {
    switch (t) {   // dispatch-ok: the six tiles the card has
    case T_NETWORK: k_strlcpy(out, network_tile_on() ? "Wired" : "Network off", cap); return;
    case T_DND:     k_strlcpy(out, "Do not disturb", cap); return;
    case T_LAYOUT:  k_strlcpy(out, "Keyboard", cap); return;
    case T_REMOTE:  k_strlcpy(out, "Remote desktop", cap); return;
    case T_OSK:     k_strlcpy(out, "On-screen keyboard", cap); return;
    default:        k_strlcpy(out, "Transparency", cap); return;
    }
}

// --- geometry ----------------------------------------------------------------

struct qs_geom {
    int x, y, w, h, pad, gap;
    int tile_w, tile_h, label_h, tiles_y;
    int sl_y, row_h, nsl;
    int foot_y, foot_h, btn_y, btn_h, set_x, set_w;
    int gx, gy, gw, gh;
};

static void geometry(struct qs_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    g->pad = m.pad;
    g->gap = m.pad * 2 / 3;
    g->w = ugfx_char_w() * 34;
    g->tile_w = (g->w - 2 * g->pad - 2 * g->gap) / 3;
    g->tile_h = utheme_control_h() + ugfx_char_h() / 2;
    g->label_h = ugfx_char_h() + 6;
    g->row_h = m.row_h;
    g->nsl = slider_shown(0) + slider_shown(1);
    g->foot_h = m.foot_h;
    g->h = m.vpad + 2 * (g->tile_h + g->label_h) + g->gap + m.vpad + g->nsl * g->row_h + m.vpad + g->foot_h;
    if (!tray_item_rect(g_tray, &g->gx, &g->gy, &g->gw, &g->gh)) {
        g->gx = screen_w - 160; g->gy = screen_h - taskbar_h; g->gw = 96; g->gh = taskbar_h;
    }
    wm_popup_place(g->gx + g->gw - g->w, screen_h - taskbar_h - g->h, g->w, g->h, &g->x, &g->y);
    g->tiles_y = g->y + m.vpad;
    g->sl_y = g->tiles_y + 2 * (g->tile_h + g->label_h) + g->gap + m.vpad;
    g->foot_y = g->y + g->h - g->foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (g->foot_h - m.btn_h) / 2;
    g->set_w = wm_flyout_button_w("All settings", "tb-gear");
    g->set_x = g->x + g->w - g->pad - g->set_w;
}

static void tile_rect(const struct qs_geom *g, int t, int *x, int *y, int *w, int *cx, int *cw) {
    int r = t / 3, c = t % 3;
    *x = g->x + g->pad + c * (g->tile_w + g->gap);
    *y = g->tiles_y + r * (g->tile_h + g->label_h + g->gap);
    *cw = tile_page(t) || t == T_LAYOUT ? g->tile_h * 2 / 3 : 0;
    if (t == T_REMOTE) *cw = g->tile_h * 2 / 3;   // its sessions, or its settings
    *w = g->tile_w - *cw;
    *cx = *x + *w;
}

// Slider k's row: the icon cell, the track, the number, the chevron.
static int slider_rect(const struct qs_geom *g, int k, int *ix, int *tx, int *tw, int *ry,
                       int *cx, int *cs) {
    if (!slider_shown(k)) return 0;
    int row = k == 1 && !slider_shown(0) ? 0 : k;
    *ry = g->sl_y + row * g->row_h;
    *ix = g->x + g->pad;
    *cs = g->row_h * 2 / 3;
    *cx = g->x + g->w - g->pad - *cs;
    int num_w = ugfx_text_width("100") + 6;
    *tx = *ix + g->row_h + 4;
    *tw = *cx - num_w - 6 - *tx - ugfx_char_h() / 2 - 2;   // room for the knob at the end
    return 1;
}

int wm_quick_is_open(void) {
    if (g_page && !*PAGES[g_page].is_open) g_page = 0;   // the page closed itself
    return g_main || g_page;
}

static void back_rect(int *x, int *y, int *s) {
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int px = 0, py = 0, pw = 0, ph = 0;
    if (g_page) PAGES[g_page].rect(&px, &py, &pw, &ph);
    *s = m.badge;
    *x = px + m.pad;
    *y = py + (m.hero_h - m.badge) / 2;
}

int wm_quick_rect(int *x, int *y, int *w, int *h) {
    if (!wm_quick_is_open()) return 0;
    if (g_page) {   // the page is its owner's to damage; ours is Back
        int s;
        back_rect(x, y, &s);
        *w = *h = s;
        return 1;
    }
    struct qs_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void wm_quick_damage(void) { wm_overlay_damage("quick"); }

// --- open, page, close ---------------------------------------------------------

static void open_main(void) {
    wm_overlay_close_others("quick");
    g_main = 1;
    g_page = 0;
    g_hover = 0;
    wm_quick_damage();
}

static void open_page(int p) {
    if (p <= PAGE_NONE) return;
    g_switching = 1;
    wm_quick_damage();       // where the card was
    g_main = 0;
    PAGES[p].open();         // its close_others() reaches our close: ignored
    g_switching = 0;
    g_page = *PAGES[p].is_open ? p : 0;
}

void wm_quick_close(void) {
    if (g_switching) return;
    if (g_page && *PAGES[g_page].is_open) PAGES[g_page].close();
    g_page = 0;
    g_main = 0;
    g_drag = 0;
    g_hover = 0;
}

static uint32_t g_seen_gen;
static int g_grouped = 1;

static int want_grouped(void) {
    char v[16];
    return !(usetting_get(SET_QUICK, v, sizeof v) && !k_strcmp(v, "off"));
}

void wm_quick_poll(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_gen) return;
    g_seen_gen = gen;
    int want = want_grouped();
    if (want == g_grouped) return;
    g_grouped = want;
    if (!want && wm_quick_is_open()) wm_quick_close();
    tray_group_set(g_tray, want);
}

void wm_quick_init(void) {
    int shown[3], n = 0, silent[2], ns = 0;
    shown[n++] = network_tray_id();
    shown[n++] = volume_slider()->tray_id;
    shown[n++] = brightness_slider()->tray_id;
    silent[ns++] = layout_tray_id();
    silent[ns++] = osk_tray_id();
    g_tray = tray_register_group(shown, n, silent, ns);
    g_seen_gen = wm_setting_generation();
    g_grouped = want_grouped();
    if (!g_grouped) tray_group_set(g_tray, 0);
}

// --- input ---------------------------------------------------------------------

static void set_slider(int k, int mx, int commit) {
    struct qs_geom g;
    geometry(&g);
    int ix, tx, tw, ry, cx, cs;
    if (!slider_rect(&g, k, &ix, &tx, &tw, &ry, &cx, &cs) || tw <= 0) return;
    struct tray_slider_popup *p = slider(k);
    int f = mx - tx;
    if (f < 0) f = 0;
    if (f > tw) f = tw;
    tray_slider_set_level(p, p->min + (p->max - p->min) * f / tw, commit);
    wm_quick_damage();
}

static int hit_at(const struct qs_geom *g, int mx, int my) {
    for (int t = 0; t < WM_QUICK_TILES; t++) {
        int x, y, w, cx, cw;
        tile_rect(g, t, &x, &y, &w, &cx, &cw);
        if (cw && uui_hit(cx, y, cw, g->tile_h, mx, my)) return 11 + t;
        if (uui_hit(x, y, w, g->tile_h + g->label_h, mx, my)) return 1 + t;
    }
    for (int k = 0; k < 2; k++) {
        int ix, tx, tw, ry, cx, cs;
        if (!slider_rect(g, k, &ix, &tx, &tw, &ry, &cx, &cs)) continue;
        if (uui_hit(cx, ry + (g->row_h - cs) / 2, cs, cs, mx, my)) return HOVER_SLC0 + k;
        if (uui_hit(tx - 6, ry, tw + 12, g->row_h, mx, my)) return HOVER_SL0 + k;
    }
    if (uui_hit(g->set_x, g->btn_y, g->set_w, g->btn_h, mx, my)) return HOVER_SETTINGS;
    return 0;
}

static void tile_click(int t) {
    switch (t) {   // dispatch-ok: the six tiles the card has
    case T_NETWORK: network_toggle(); break;
    case T_DND:     wm_notify_set_dnd(!wm_notify_dnd()); break;
    case T_LAYOUT:  layout_next(); break;
    case T_REMOTE:
        if (remote_session_count() > 0) { open_page(PAGE_REMOTE); return; }
        wm_quick_close();
        { int pid = sys_spawn("/bin/wm/system/settings", "remotedesktop", -1); if (pid > 0) wm_track_launched(pid); }
        return;
    case T_OSK:     osk_toggle(); break;
    case T_GLASS:   usetting_set(SET_GLASS, glass_on() ? "off" : "on"); break;
    default: break;
    }
    wm_quick_damage();
}

int wm_quick_click(int mx, int my) {
    struct qs_geom g;
    geometry(&g);
    // The group's REAL rect: hidden (`desktop.tray.quick` off) it has none,
    // and geometry()'s fallback would sit over the icons it gave back.
    int bx, by, bw, bh;
    int on_group = g_tray >= 0 && tray_item_rect(g_tray, &bx, &by, &bw, &bh) &&
                   uui_hit(bx, by, bw, bh, mx, my);
    if (!wm_quick_is_open()) {
        if (on_group) { open_main(); return 1; }
        return 0;
    }
    if (on_group) { wm_quick_close(); return 1; }   // a second click closes it
    if (g_page) {
        int bx, by, bs;
        back_rect(&bx, &by, &bs);
        if (uui_hit(bx, by, bs, bs, mx, my)) {
            g_switching = 1;
            PAGES[g_page].close();
            g_switching = 0;
            open_main();
            return 1;
        }
        return 0;   // the page's own
    }
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        wm_quick_close();
        return my < screen_h - taskbar_h;   // a click on the taskbar is still the taskbar's
    }
    int h = hit_at(&g, mx, my);
    if (h >= 11 && h < 11 + WM_QUICK_TILES) {
        int t = h - 11;
        if (t == T_REMOTE) tile_click(t);
        else open_page(tile_page(t) ? tile_page(t) : PAGE_LAYOUT);
        return 1;
    }
    if (h >= 1 && h <= WM_QUICK_TILES) {
        if (!tile_disabled(h - 1)) tile_click(h - 1);
        return 1;
    }
    if (h == HOVER_SL0 || h == HOVER_SL0 + 1) {
        g_drag = h - HOVER_SL0 + 1;
        set_slider(g_drag - 1, mx, 0);
        return 1;
    }
    if (h == HOVER_SLC0 || h == HOVER_SLC0 + 1) {
        open_page(h == HOVER_SLC0 ? PAGE_VOLUME : PAGE_BRIGHTNESS);
        return 1;
    }
    if (h == HOVER_SETTINGS) {
        wm_quick_close();
        int pid = sys_spawn("/bin/wm/system/settings", "", -1);
        if (pid > 0) wm_track_launched(pid);
        return 1;
    }
    return 1;   // inert, but ours
}

void wm_quick_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    if (!g_drag) return;
    if (buttons & 1) { set_slider(g_drag - 1, mx, 0); return; }
    set_slider(g_drag - 1, mx, 1);   // the release writes at once
    g_drag = 0;
}

int wm_quick_hover_at(int mx, int my) {
    if (!wm_quick_is_open()) return 0;
    int h = 0;
    if (g_page) {
        int bx, by, bs;
        back_rect(&bx, &by, &bs);
        h = uui_hit(bx, by, bs, bs, mx, my) ? HOVER_BACK : 0;
    } else {
        struct qs_geom g;
        geometry(&g);
        h = hit_at(&g, mx, my);
    }
    g_hover = h;
    return h;
}

int wm_quick_key(int key, uint8_t mods) {
    (void)mods;
    if (!g_main) return 0;   // a page's keys are its own
    if (key == 0x1B) { wm_quick_close(); return 1; }
    return 0;
}

// --- drawing -------------------------------------------------------------------

static void chevron(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t ink, int left) {
    int cx = x + w / 2, cy = y + h / 2, a = h / 6 > 2 ? h / 6 : 2;
    int d = left ? -1 : 1;
    ugfx_draw_line(s, cx - d * a / 2, cy - a, cx + d * a / 2, cy, ink, GEOM_AA);
    ugfx_draw_line(s, cx + d * a / 2, cy, cx - d * a / 2, cy + a, ink, GEOM_AA);
}

static void draw_tile(struct ugfx_surface *s, const struct qs_geom *g, int t) {
    int x, y, w, cx, cw;
    tile_rect(g, t, &x, &y, &w, &cx, &cw);
    int on = tile_on(t), off = tile_disabled(t);
    int hot = g_hover == 1 + t, chot = g_hover == 11 + t;
    uint32_t ground = wm_flyout_ground();
    uint32_t face = on ? UTHEME_ACCENT : UTHEME_WHITE;
    uint32_t ink = on ? UTHEME_ACCENT_TEXT : off ? wm_flyout_dim() : UTHEME_TEXT;
    int r = 6;
    // THE TILE: one rounded button, split by a hairline when it has a page.
    uui_fill_round_rect(s, x, y, w + cw, g->tile_h, r, on ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, x + 1, y + 1, w + cw - 2, g->tile_h - 2, r - 1, face);
    if (hot && !off) uui_fill_round_rect(s, x + 1, y + 1, w - 1, g->tile_h - 2, r - 1, uui_state_bg(face, UUI_STATE_HOVER));
    if (cw) {
        if (chot) uui_fill_round_rect(s, cx, y + 1, cw - 1, g->tile_h - 2, r - 1, uui_state_bg(face, UUI_STATE_HOVER));
        ugfx_fill_rect(s, cx, y + 4, 1, g->tile_h - 8, on ? ugfx_blend(UTHEME_ACCENT_TEXT, UTHEME_ACCENT, 90) : UTHEME_SEPARATOR);
        chevron(s, cx, y, cw, g->tile_h, ink, 0);
    }
    int isz = g->tile_h * 5 / 9;
    if (t == T_LAYOUT) {
        const char *c = layout_code(layout_active());
        char code[8];
        unsigned k = 0;
        for (; c && c[k] && k + 1 < sizeof code; k++) code[k] = (char)(c[k] >= 'a' && c[k] <= 'z' ? c[k] - 32 : c[k]);
        code[k] = 0;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string(s, x + (w - ugfx_text_width(code)) / 2, y + (g->tile_h - ugfx_char_h()) / 2,
                         code, ink, UGFX_TRANSPARENT);
        ugfx_set_font(was);
    } else if (t == T_GLASS) {
        // Two panes, the front one see-through: no icon file says it.
        int a = isz * 2 / 3, ix = x + (w - isz) / 2, iy = y + (g->tile_h - isz) / 2;
        uui_stroke_round_rect(s, ix, iy, a, a, 3, ink);
        uui_stroke_round_rect(s, ix + isz - a, iy + isz - a, a, a, 3, ink);
    } else {
        static const char *const ICONS[] = { 0, "tray-bell-dnd", 0, "tray-remote", "tray-keyboard", 0 };
        const char *name = t == T_NETWORK ? tray_icon_now(network_tray_id()) : ICONS[t];
        const struct uimg *ico = name ? icon_get(name, isz) : 0;
        if (ico) ugfx_blit_tinted(s, x + (w - ico->w) / 2, y + (g->tile_h - ico->h) / 2,
                                  ico->w, ico->h, ico->px, ico->w, ink);
    }
    char label[32];
    tile_label(t, label, sizeof label);
    int lw = ugfx_text_width(label);
    int lx = x + (g->tile_w - lw) / 2;
    if (lx < x) lx = x;
    ugfx_draw_string_clipped(s, lx, y + g->tile_h + 3, g->tile_w, label,
                             off ? wm_flyout_dim() : UTHEME_TEXT, UGFX_TRANSPARENT);
    (void)ground;
}

static void draw_slider(struct ugfx_surface *s, const struct qs_geom *g, int k) {
    int ix, tx, tw, ry, cx, cs;
    if (!slider_rect(g, k, &ix, &tx, &tw, &ry, &cx, &cs)) return;
    struct tray_slider_popup *p = slider(k);
    int isz = g->row_h * 3 / 5;
    const char *icon = tray_icon_now(p->tray_id);
    const struct uimg *ico = icon ? icon_get(icon, isz) : 0;
    if (ico) ugfx_blit_tinted(s, ix + (g->row_h - ico->w) / 2, ry + (g->row_h - ico->h) / 2,
                              ico->w, ico->h, ico->px, ico->w, UTHEME_TEXT);
    int range = p->max - p->min > 0 ? p->max - p->min : 1;
    int fill = (p->level - p->min) * tw / range;
    int ty = ry + g->row_h / 2 - 2;
    uui_fill_round_rect(s, tx, ty, tw, 4, 2, UTHEME_SEPARATOR);
    if (fill > 0) uui_fill_round_rect(s, tx, ty, fill, 4, 2, UTHEME_ACCENT);
    int kn = ugfx_char_h();
    int hot = g_hover == HOVER_SL0 + k || g_drag == k + 1;
    uui_fill_round_rect(s, tx + fill - kn / 2, ry + (g->row_h - kn) / 2, kn, kn, kn / 2,
                        hot ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, tx + fill - kn / 2 + 2, ry + (g->row_h - kn) / 2 + 2, kn - 4, kn - 4,
                        kn / 2 - 2, UTHEME_WHITE);
    char num[8];
    k_snprintf(num, sizeof num, "%d", (p->level - p->min) * 100 / range);
    ugfx_draw_string(s, cx - 6 - ugfx_text_width(num), ry + (g->row_h - ugfx_char_h()) / 2, num,
                     UTHEME_TEXT, UGFX_TRANSPARENT);
    int cy = ry + (g->row_h - cs) / 2;
    if (g_hover == HOVER_SLC0 + k)
        uui_fill_round_rect(s, cx, cy, cs, cs, 4, uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
    chevron(s, cx, cy, cs, cs, UTHEME_TEXT, 0);
}

void wm_quick_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!wm_quick_is_open()) return;
    struct ugfx_surface *s = wm_surface();
    if (g_page) {
        // BACK, over the page's badge: the way to the tiles again.
        int bx, by, bs;
        back_rect(&bx, &by, &bs);
        // A ROUND button, anti-aliased, on a disc of the card's ground a
        // pixel wider -- or the badge it covers fringes out round it.
        int cx = bx + bs / 2, cy = by + bs / 2;
        ugfx_fill_circle(s, cx, cy, bs / 2 + 1, wm_flyout_ground());
        ugfx_fill_circle(s, cx, cy, bs / 2 - 1,
                         g_hover == HOVER_BACK ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_HOVER) : UTHEME_BUTTON_BG);
        chevron(s, bx, by, bs, bs, UTHEME_TEXT, 1);
        return;
    }
    struct qs_geom g;
    geometry(&g);
    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);
    for (int t = 0; t < WM_QUICK_TILES; t++) draw_tile(s, &g, t);
    for (int k = 0; k < 2; k++) draw_slider(s, &g, k);
    char sum[48];
    network_summary(sum, sizeof sum);
    ugfx_draw_string_clipped(s, g.x + g.pad, g.foot_y + (g.foot_h - ugfx_char_h()) / 2,
                             g.set_x - g.x - 2 * g.pad, sum, wm_flyout_dim(), UGFX_TRANSPARENT);
    wm_flyout_button(g.set_x, g.btn_y, "All settings", "tb-gear", 0, g_hover == HOVER_SETTINGS, 0);
}

void wm_quick_state(struct wm_quick_info *o) {
    k_memset(o, 0, sizeof *o);
    struct qs_geom g;
    geometry(&g);
    o->open = wm_quick_is_open();
    o->page = g_page;
    o->page_name = g_page ? PAGES[g_page].name : "";
    if (g_main) { o->x = g.x; o->y = g.y; o->w = g.w; o->h = g.h; }
    o->bx = g.gx; o->by = g.gy; o->bw = g.gw; o->bh = g.gh;
    if (g_page) back_rect(&o->back_x, &o->back_y, &o->back_s);
    for (int t = 0; t < WM_QUICK_TILES; t++) {
        struct wm_quick_tile_info *ti = &o->tile[t];
        ti->name = TILE_NAMES[t];
        ti->on = tile_on(t);
        ti->disabled = tile_disabled(t);
        int x, y, w, cx, cw;
        tile_rect(&g, t, &x, &y, &w, &cx, &cw);
        if (g_main) { ti->x = x; ti->y = y; ti->w = w; ti->h = g.tile_h; ti->cx = cx; ti->cw = cw; }
    }
    for (int k = 0; k < 2; k++) {
        struct wm_quick_slider_info *si = &o->slider[k];
        struct tray_slider_popup *p = slider(k);
        si->name = k ? "brightness" : "volume";
        si->level = p->level; si->min = p->min; si->max = p->max;
        int ix, tx, tw, ry, cx, cs;
        si->shown = slider_rect(&g, k, &ix, &tx, &tw, &ry, &cx, &cs);
        if (si->shown && g_main) {
            si->x = tx; si->y = ry; si->w = tw; si->h = g.row_h;
            si->cx = cx; si->cy = ry + (g.row_h - cs) / 2; si->cs = cs;
        }
    }
    if (g_main) { o->set_x = g.set_x; o->set_y = g.btn_y; o->set_w = g.set_w; o->set_h = g.btn_h; }
}
