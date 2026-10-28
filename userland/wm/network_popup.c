// See network_popup.h. A tray flyout over QUERY_NETDEV, on the shared
// card (wm_flyout.h); its actions are /bin/netctl's.
#include <stdlib.h>
#include "lib/unum.h"
#include "wm_internal.h"
#include "network_popup.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "lib/icon_cache.h"
#include "lib/uchan.h"
#include "lib/uclip.h"
#include "lib/uconf.h"
#include "lib/unetctl.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "wm_anim.h"        // wm_anim_enabled()

int network_open = 0;

static int g_tray_id = -1;
static const char *g_icon = "tray-network-off";
static struct network_view g_view;
static int g_hover;
#define BUSY_PERIOD_MS 1400   // one sweep of the busy bar
static uint32_t g_seen_generation;

// What the card says beyond the device table: the lease netd keeps on
// disk and the DNS server it wrote. Read when the panel opens and when
// the address changes -- small files, never once a second.
static char g_lease[48], g_dns[20];

// TRAFFIC, a sample a second for the last minute -- Task Manager's
// per-second rate (tm_perf.c), kept here because the card draws it.
#define HIST 60
static unsigned long long g_rx_hist[HIST], g_tx_hist[HIST];
static int g_hist_n;
static unsigned long long g_last_rx, g_last_tx, g_last_ns;

// THE SWITCH ANSWERS AT ONCE. netd acts on its next pass, so for a few
// seconds the card shows what was asked rather than flicking back.
static int g_want_down = -1;
// The card the switch last acted on, kept on the panel while it is off
// -- or switching it off would hand the panel to the next card, and it
// could not be switched back on from here.
static char g_pin[NET_ABI_NAME_MAX];
static unsigned long long g_want_until;

// CONNECTING: the limited glyph's links drawing in, then held -- the
// change it is waiting for, drawn as it happens. Frame 0 and the last
// three ARE the limited and connected icons; gen_icons.py draws the
// four between and the dotted still.
#define NET_CONNECTING_S 30
static const char *const CONNECTING_FRAMES[] = {
    "tray-network-limited", "tray-network-connecting-1", "tray-network-connecting-2",
    "tray-network-connecting-3", "tray-network-connecting-4",
    "tray-network", "tray-network", "tray-network",
};
static const struct tray_anim CONNECTING = {
    CONNECTING_FRAMES, sizeof CONNECTING_FRAMES / sizeof CONNECTING_FRAMES[0], 150,
    "tray-network-connecting",
};

// --- what netd says (lib/unetctl.h) ----------------------------------
//
// WATCHED, NEVER ASKED: netd blocks for seconds in a DHCP exchange, and
// a compositor waiting on its answer would freeze. It pushes each card's
// phase into this inbox instead, drained on the once-a-second poll. A
// netd that is not there, or died, is looked for again every few
// seconds; meanwhile nothing is "asking", which is today's view.
#define NETD_RETRY_NS (5ull * 1000000000ull)

struct netd_phase {
    char dev[NET_ABI_NAME_MAX];
    uint32_t phase;
    uint64_t since_ns;
};

static struct uchan_client g_netd;
static int g_netd_up;
static uint64_t g_netd_next;
static struct netd_phase *g_phases;   // one per card netd has told us of
static int g_phase_n, g_phase_cap;

static void phase_put(const struct netctl_event *e) {
    int i = 0;
    while (i < g_phase_n && k_strcmp(g_phases[i].dev, e->dev)) i++;
    if (e->phase == NETCTL_PHASE_GONE) {
        if (i < g_phase_n) g_phases[i] = g_phases[--g_phase_n];
        return;
    }
    if (i == g_phase_n) {
        if (g_phase_n == g_phase_cap) {
            int cap = g_phase_cap ? g_phase_cap * 2 : 4;
            struct netd_phase *p = realloc(g_phases, sizeof *p * (size_t)cap);
            if (!p) return;
            g_phases = p;
            g_phase_cap = cap;
        }
        g_phase_n++;
    }
    k_strlcpy(g_phases[i].dev, e->dev, sizeof g_phases[i].dev);
    g_phases[i].phase = e->phase;
    g_phases[i].since_ns = e->since_ns;
}

static void netd_sync(uint64_t now) {
    if (g_netd_up && now >= g_netd_next) {
        g_netd_next = now + NETD_RETRY_NS;
        if (!uchan_client_server_alive(&g_netd)) {
            uchan_client_close(&g_netd);
            g_netd_up = 0;
        }
    }
    if (!g_netd_up && now >= g_netd_next) {
        g_netd_next = now + NETD_RETRY_NS;
        g_phase_n = 0;   // a new netd tells us everything again
        if (uchan_client_open(&g_netd, NETCTL_SERVICE) == 0) {
            struct netctl_msg m;
            k_memset(&m, 0, sizeof m);
            m.verb = NETCTL_WATCH;
            if (uchan_send(&g_netd, &m, sizeof m) == 0) g_netd_up = 1;
            else uchan_client_close(&g_netd);
        }
    }
    struct netctl_event e;
    while (g_netd_up && uchan_client_recv(&g_netd, &e, sizeof e)) {
        e.dev[sizeof e.dev - 1] = 0;
        phase_put(&e);
    }
}

// When the switch last went on, from here or from anywhere: a link
// that is down for the next few seconds is renegotiating, not unplugged.
#define NET_NEGOTIATE_NS (20ull * 1000000000ull)
static uint64_t g_on_ns;

// AN ADDRESS netd IS STILL ASKING ABOUT IS NOT A CONNECTION: after a
// switch-on the card holds its old address while the server is asked
// again, and "Connected" before the answer is the claim that came back
// suspiciously fast. A renew that is answered within a pass is never
// seen asking at all.
static void fill_phase(struct network_view *v, uint64_t now) {
    v->asking = 0; v->connecting = 0; v->asking_ms = 0;
    v->negotiating = 0;
    if (!v->have_device || v->admin_down) return;
    v->negotiating = v->cable_out && g_on_ns && now - g_on_ns < NET_NEGOTIATE_NS;
    v->connecting = v->negotiating;
    if (v->cable_out) return;
    for (int i = 0; i < g_phase_n; i++) {
        if (k_strcmp(g_phases[i].dev, v->name) || g_phases[i].phase != NETCTL_PHASE_ASKING)
            continue;
        v->asking = 1;
        v->connected = 0;
        v->asking_ms = now > g_phases[i].since_ns ? (now - g_phases[i].since_ns) / 1000000ull : 0;
        v->connecting = v->asking_ms < NET_CONNECTING_S * 1000ull;
    }
}

#define HOVER_TRAY    1
#define HOVER_SWITCH  2
#define HOVER_COPY    3
#define HOVER_RENEW   4
#define HOVER_DETAILS 5
#define HOVER_GEAR    6

// --- reading ----------------------------------------------------------

// 169.254/16. `udhcp.c` hands one of these out when no server answered,
// so an address in this range means "configured itself", not "on a
// network".
#define IS_LINK_LOCAL(ip) (((ip) & 0xFFFF0000u) == 0xA9FE0000u)

static void fmt_ip(char *out, unsigned cap, unsigned long long ip) {
    if (!ip) { k_strlcpy(out, "none", cap); return; }
    k_snprintf(out, cap, "%llu.%llu.%llu.%llu",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Gb/s to one decimal, because 2.5 Gb/s is not 2 -- the split
// /bin/netctl makes, and for the same reason.
static void fmt_speed(char *out, unsigned cap, const struct network_view *v) {
    unsigned long long b = v->link_bps;
    if (!b) { out[0] = 0; return; }
    if (b >= 1000000000ull && b % 1000000000ull) {
        k_snprintf(out, cap, "%llu.%llu Gb/s", b / 1000000000ull, b % 1000000000ull / 100000000ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else if (b >= 1000000000ull) k_snprintf(out, cap, "%llu Gb/s", b / 1000000000ull);
    else k_snprintf(out, cap, "%llu Mb/s", b / 1000000ull);
}

static void fmt_rate(char *out, unsigned cap, unsigned long long bps) {
    if (bps >= 1000000ull) {
        k_snprintf(out, cap, "%llu.%llu MB/s", bps / 1000000ull, bps % 1000000ull / 100000ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else if (bps >= 1000ull) {
        k_snprintf(out, cap, "%llu.%llu KB/s", bps / 1000ull, bps % 1000ull / 100ull);
        unum_localize(out, (unsigned long)cap, 0);
    } else k_snprintf(out, cap, "%llu B/s", bps);
}

static void read_files(void) {
    g_lease[0] = 0;
    if (g_view.have_device) {
        char path[64], secs[16], server[20];
        k_snprintf(path, sizeof path, "/var/dhcp-%s.lease", g_view.name);
        uint32_t s = 0;
        if (uconf_get(path, "seconds", secs, sizeof secs) && k_parse_u32(secs, &s) && s &&
            uconf_get(path, "server", server, sizeof server)) {
            if (s % 3600 == 0) k_snprintf(g_lease, sizeof g_lease, "%u h, from %s", s / 3600, server);
            else k_snprintf(g_lease, sizeof g_lease, "%u s, from %s", s, server);
        }
    }
    if (!uconf_get("/etc/resolv.conf", "nameserver", g_dns, sizeof g_dns)) g_dns[0] = 0;
}

// THE FIRST CARD THAT IS UP WITH AN ADDRESS, else the first device at
// all -- unless the switch put a card down, which stays on the panel
// while it is off. A tray item is one glyph and cannot show two cards;
// picking the one actually carrying traffic is what a person means by
// "am I online". The count is reported so the panel can say there are
// others.
static void read_devices(struct network_view *v, struct query_netdev *out) {
    struct query_netdev d, best, pinned;
    int have_best = 0, have_pin = 0, count = 0;

    k_memset(v, 0, sizeof *v);
    k_memset(&best, 0, sizeof best);
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        int live = d.ip && !d.admin_down && !(d.link_known && !d.link_up);
        if (count == 0 || (!have_best && live)) { best = d; have_best = live; }
        if (g_pin[0] && !k_strcmp(d.name, g_pin) && d.admin_down) { pinned = d; have_pin = 1; }
        count++;
    }
    if (have_pin) best = pinned;
    else g_pin[0] = 0;   // back up, or gone: the panel follows traffic again
    v->device_count = count;
    if (out) *out = best;
    if (!count) return;

    v->have_device = 1;
    v->link_known = best.link_known != 0;
    v->link_up = best.link_up != 0;
    v->link_bps = best.link_bps;
    v->admin_down = best.admin_down != 0;
    v->link_local = best.ip && IS_LINK_LOCAL((unsigned)best.ip);
    // CONNECTED MEANS AN ADDRESS ON A LINK NOT KNOWN TO BE DOWN. The
    // address alone is not enough -- a card keeps it with the cable out
    // -- and the link flag alone is not either: link_known is 0 on the
    // e1000 every default QEMU guest has.
    v->cable_out = v->link_known && !v->link_up;
    v->connected = best.ip != 0 && !v->link_local && !v->cable_out;
    k_strlcpy(v->name, best.name, sizeof v->name);
    k_strlcpy(v->driver, best.driver, sizeof v->driver);
    k_strlcpy(v->location, best.location, sizeof v->location);
    fmt_ip(v->ip, sizeof v->ip, best.ip);
    fmt_ip(v->mask, sizeof v->mask, best.netmask);
    fmt_ip(v->gw, sizeof v->gw, best.gateway);
    unsigned long long m = best.netmask;
    for (int b = 0; b < 32; b++) v->prefix += (int)((m >> b) & 1);
    unsigned long long mac = best.mac;
    k_snprintf(v->mac, sizeof v->mac, "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
               mac & 0xFF, (mac >> 8) & 0xFF, (mac >> 16) & 0xFF,
               (mac >> 24) & 0xFF, (mac >> 32) & 0xFF, (mac >> 40) & 0xFF);
}

static const char *icon_for(const struct network_view *v) {
    if (!v->have_device || v->admin_down) return "tray-network-off";
    if (v->connected) return "tray-network";
    return "tray-network-limited";
}

static int shown_down(void) {
    if (g_want_down >= 0 && sys_monotonic_ns() < g_want_until) return g_want_down;
    return g_view.admin_down;
}

// TWO CLOCKS, because the two questions have different answers. THE
// DEVICE is read on a once-a-second CADENCE: there is no netdev
// generation in the ABI, and the read is a memcpy out of a live kernel
// table with no I/O behind it. VISIBILITY is on the SETTINGS GENERATION,
// because tray_want_shown() reads /etc/desktop.conf.
void network_poll(void) {
    struct network_view now;
    struct query_netdev d;
    read_devices(&now, &d);
    unsigned long long t = sys_monotonic_ns();
    netd_sync(t);
    if (g_view.have_device && g_view.admin_down && !now.admin_down) g_on_ns = t;
    fill_phase(&now, t);

    // The traffic sample, a rate over the measured interval.
    if (g_last_ns && now.have_device && !k_strcmp(now.name, g_view.name) &&
        d.rx_bytes >= g_last_rx && d.tx_bytes >= g_last_tx) {
        unsigned long long dt = t - g_last_ns;
        if (dt) {
            now.rx_rate = (d.rx_bytes - g_last_rx) * 1000000000ull / dt;
            now.tx_rate = (d.tx_bytes - g_last_tx) * 1000000000ull / dt;
            for (int i = 0; i < HIST - 1; i++) {
                g_rx_hist[i] = g_rx_hist[i + 1];
                g_tx_hist[i] = g_tx_hist[i + 1];
            }
            g_rx_hist[HIST - 1] = now.rx_rate;
            g_tx_hist[HIST - 1] = now.tx_rate;
            if (g_hist_n < HIST) g_hist_n++;
        }
    }
    g_last_rx = d.rx_bytes; g_last_tx = d.tx_bytes; g_last_ns = t;

    int changed = now.have_device != g_view.have_device ||
                  now.connected != g_view.connected ||
                  now.link_local != g_view.link_local ||
                  now.link_up != g_view.link_up ||
                  now.link_known != g_view.link_known ||
                  now.admin_down != g_view.admin_down ||
                  now.device_count != g_view.device_count ||
                  now.asking != g_view.asking || now.connecting != g_view.connecting ||
                  now.cable_out != g_view.cable_out || now.negotiating != g_view.negotiating ||
                  k_strcmp(now.ip, g_view.ip) != 0 ||
                  k_strcmp(now.name, g_view.name) != 0;
    g_view = now;
    if (g_want_down >= 0 && (g_view.admin_down == g_want_down || t >= g_want_until))
        g_want_down = -1;

    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation || changed) {
        g_seen_generation = gen;
        tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
    }
    if (changed) {
        if (network_open) read_files();
        g_icon = icon_for(&g_view);
        tray_set_icon(g_tray_id, g_icon);
        if (g_view.connecting) tray_set_anim(g_tray_id, &CONNECTING);
    }
    // The graph moves every second while the card is up.
    if (network_open) network_damage();
}

void network_view(struct network_view *v) { *v = g_view; }

// The busy bar's frames, while the card is open on a card that is
// connecting: only the header strip is redrawn.
#define BUSY_FRAME_MS 33
static uint64_t g_busy_slice;

static int busy_running(void) {
    return network_open && g_view.connecting && !shown_down() && wm_anim_enabled();
}

void network_animate(void) {
    if (!busy_running()) return;
    uint64_t slice = sys_monotonic_ns() / (BUSY_FRAME_MS * 1000000ull);
    if (slice == g_busy_slice) return;
    g_busy_slice = slice;
    struct network_geom g;
    network_geometry(&g);
    WM_REDRAW();
    wm_damage_rect(g.x, g.y, g.w, g.hero_h + 1);
}

int network_wait_ms(void) {
    if (!busy_running()) return -1;
    return BUSY_FRAME_MS - (int)(sys_monotonic_ns() / 1000000ull % BUSY_FRAME_MS);
}
int network_tray_hidden(void) { return tray_is_hidden(g_tray_id); }

void network_tray_init(void) {
    read_devices(&g_view, 0);
    netd_sync(sys_monotonic_ns());
    g_icon = icon_for(&g_view);
    g_tray_id = tray_register_icon(g_icon);
    tray_set_hidden(g_tray_id, !tray_want_shown("network", g_view.have_device));
}

// --- geometry ---------------------------------------------------------

// The DETAILS rows. Built here rather than in draw() so that drawing,
// the panel's height and `gui network --json` cannot disagree about
// what is on it -- the one-geometry-function rule every flyout follows.
#define NET_ROWS 7

static int build_rows(char keys[NET_ROWS][16], char vals[NET_ROWS][48]) {
    int n = 0;
    k_strlcpy(keys[n], "IPv4 address", 16);
    if (g_view.ip[0] && k_strcmp(g_view.ip, "none"))
        k_snprintf(vals[n], 48, "%s/%d%s", g_view.ip, g_view.prefix,
                   g_view.link_local ? " (self-assigned)" :
                   g_view.cable_out ? " (not in use)" :
                   g_view.asking ? " (not confirmed)" : "");
    else k_strlcpy(vals[n], "none", 48);
    n++;
    if (g_view.asking) {
        unsigned long long s = g_view.asking_ms / 1000;
        k_strlcpy(keys[n], "Asking", 16);
        if (s < 60) k_snprintf(vals[n], 48, "for %llu s", s);
        else if (s < 3600) k_snprintf(vals[n], 48, "for %llu min %llu s", s / 60, s % 60);
        else k_snprintf(vals[n], 48, "for %llu h %llu min", s / 3600, s / 60 % 60);
        n++;
    }
    if (k_strcmp(g_view.gw, "none")) {
        k_strlcpy(keys[n], "Gateway", 16); k_strlcpy(vals[n], g_view.gw, 48); n++;
    }
    if (g_dns[0]) { k_strlcpy(keys[n], "DNS", 16); k_strlcpy(vals[n], g_dns, 48); n++; }
    // The lease FILE outlives the lease: while asking it describes one
    // this card no longer holds.
    if (g_lease[0] && !g_view.asking) { k_strlcpy(keys[n], "Lease", 16); k_strlcpy(vals[n], g_lease, 48); n++; }
    k_strlcpy(keys[n], "Adapter", 16);
    k_snprintf(vals[n], 48, "%s%s%s", g_view.driver, g_view.location[0] ? " at " : "",
               g_view.location);
    n++;
    k_strlcpy(keys[n], "MAC", 16); k_strlcpy(vals[n], g_view.mac, 48); n++;
    return n;
}

void network_geometry(struct network_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();

    char keys[NET_ROWS][16], vals[NET_ROWS][48];
    int rows = g_view.have_device ? build_rows(keys, vals) : 0;
    // AT LEAST AS WIDE AS THE FOOTER'S BUTTONS, which at a small font
    // are wider than the details: a fixed width overlapped them.
    int w = ch * 21;
    int foot_w = 2 * (m.pad - 6) + wm_flyout_button_w("Copy address", "tb-copy") + 6 +
                 wm_flyout_button_w("Renew", "tb-refresh") + 12 +
                 wm_flyout_button_w("Details...", 0) + 4 + m.btn_h;
    if (w < foot_w) w = foot_w;
    int key_w = ugfx_text_width("IPv4 address") + m.pad;
    int h = m.hero_h + 1;
    int graph = g_view.have_device && !shown_down();
    if (!g_view.have_device) h += m.vpad + 2 * (ch + 4) + m.vpad;
    if (graph) h += m.vpad + m.cap_h + ch * 3 + 4 + ch + 4 + m.vpad + 1;
    if (rows) h += m.vpad + m.cap_h + rows * m.row_h + m.vpad;
    h += m.foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = m.pad; g->row_h = m.row_h;
    g->hero_h = m.hero_h;
    if (g_view.have_device) {
        g->sw_w = m.sw_w; g->sw_h = m.sw_h;
        g->sw_x = x + w - m.pad - m.sw_w;
        g->sw_y = y + (m.hero_h - m.sw_h) / 2;
    }

    int cy = y + m.hero_h + 1;
    if (!g_view.have_device) { g->note_y = cy + m.vpad; cy += m.vpad + 2 * (ch + 4) + m.vpad; }
    if (graph) {
        g->cap1_y = cy + m.vpad;
        g->graph_x = x + m.pad;
        g->graph_y = g->cap1_y + m.cap_h;
        g->graph_w = w - 2 * m.pad;
        g->graph_h = ch * 3;
        g->legend_y = g->graph_y + g->graph_h + 4;
        cy = g->legend_y + ch + 4 + m.vpad;
        g->rule2_y = cy;
        cy += 1;
    }
    if (rows) {
        g->cap2_y = cy + m.vpad;
        g->kv_y = g->cap2_y + m.cap_h;
        g->kv_rows = rows;
        g->kv_key_w = key_w;
    }

    g->foot_h = m.foot_h;
    g->foot_y = y + h - m.foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (m.foot_h - m.btn_h) / 2;
    int bx = x + m.pad - 6;
    if (g_view.have_device) {
        g->copy_x = bx; g->copy_w = wm_flyout_button_w("Copy address", "tb-copy");
        bx += g->copy_w + 6;
        g->renew_x = bx; g->renew_w = wm_flyout_button_w("Renew", "tb-refresh");
    }
    g->gear_w = m.btn_h;
    g->gear_x = x + w - (m.pad - 6) - g->gear_w;
    if (g_view.have_device) {
        g->details_w = wm_flyout_button_w("Details...", 0);
        g->details_x = g->gear_x - 4 - g->details_w;
    }
}

// --- state ------------------------------------------------------------

int network_rect(int *x, int *y, int *w, int *h) {
    struct network_geom g;
    network_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

// The rules -- the shadow, and the rect it last occupied -- are
// the core's (wm_overlay.h).
void network_damage(void) { wm_overlay_damage("network"); }

static void network_open_now(void) {
    wm_overlay_close_others("network");
    network_poll();          // never show a stale address on opening
    read_files();
    network_open = 1;
    g_hover = 0;
    network_damage();
}

void network_close(void) {
    if (!network_open) return;
    network_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_hover = 0;
}

// --- input ------------------------------------------------------------

static int hit(int x, int w, const struct network_geom *g, int mx, int my) {
    return w > 0 && uui_hit(x, g->btn_y, w, g->btn_h, mx, my);
}

// netctl, as a child, never waited on: `args` are its command line.
static void run_netctl(const char *verb) {
    char args[64];
    k_snprintf(args, sizeof args, "%s %s --no-wait", verb, g_view.name);
    { int pid_ = sys_spawn("/bin/netctl", args, -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
}

int network_handle_click(int mx, int my) {
    struct network_geom g;
    network_geometry(&g);

    if (!network_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            network_open_now();
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        network_close();     // a second click on the icon closes it
        return 1;
    }
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        network_close();
        // A click on the TASKBAR still belongs to the taskbar.
        return my < screen_h - taskbar_h;
    }
    if (g.sw_w && uui_hit(g.sw_x - 4, g.sw_y - 4, g.sw_w + 8, g.sw_h + 8, mx, my)) {
        // ONE REQUEST IN FLIGHT: each runs in its own netctl, and netd
        // takes its rings in no particular order, so a quick second
        // click could be applied first.
        if (g_want_down >= 0 && sys_monotonic_ns() < g_want_until) return 1;
        int down = !shown_down();
        if (down) k_strlcpy(g_pin, g_view.name, sizeof g_pin);
        run_netctl(down ? "down" : "up");
        if (!down) g_on_ns = sys_monotonic_ns();
        g_want_down = down;
        g_want_until = sys_monotonic_ns() + 8000000000ull;
        network_damage();
    } else if (hit(g.copy_x, g.copy_w, &g, mx, my) && g_view.connected && !shown_down()) {
        uclip_set_text(g_view.ip, (int)k_strlen(g_view.ip));
    } else if (hit(g.renew_x, g.renew_w, &g, mx, my) && !shown_down()) {
        run_netctl("renew");
    } else if (hit(g.details_x, g.details_w, &g, mx, my)) {
        network_close();
        { int pid_ = sys_spawn("/bin/wm/system/taskmgr", "", -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
    } else if (hit(g.gear_x, g.gear_w, &g, mx, my)) {
        network_close();
        { int pid_ = sys_spawn("/bin/wm/system/settings", "system.net.recover", -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
    }
    return 1;   // anything else inside the card is swallowed
}

int network_hover_at(int mx, int my) {
    struct network_geom g;
    network_geometry(&g);
    if (tray_is_hidden(g_tray_id)) return 0;
    g_hover = 0;
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) g_hover = HOVER_TRAY;
    else if (!network_open) g_hover = 0;
    else if (g.sw_w && uui_hit(g.sw_x - 4, g.sw_y - 4, g.sw_w + 8, g.sw_h + 8, mx, my))
        g_hover = HOVER_SWITCH;
    else if (hit(g.copy_x, g.copy_w, &g, mx, my)) g_hover = HOVER_COPY;
    else if (hit(g.renew_x, g.renew_w, &g, mx, my)) g_hover = HOVER_RENEW;
    else if (hit(g.details_x, g.details_w, &g, mx, my)) g_hover = HOVER_DETAILS;
    else if (hit(g.gear_x, g.gear_w, &g, mx, my)) g_hover = HOVER_GEAR;
    return g_hover;
}

// --- drawing ----------------------------------------------------------

static void draw_graph(const struct network_geom *g) {
    struct ugfx_surface *s = wm_surface();
    uui_fill_round_rect(s, g->graph_x, g->graph_y, g->graph_w, g->graph_h, 4, UTHEME_OUTLINE);
    uui_fill_round_rect(s, g->graph_x + 1, g->graph_y + 1, g->graph_w - 2, g->graph_h - 2, 3,
                        UTHEME_WHITE);
    unsigned long long top = 1000;   // a quiet card still has a scale: 1 KB/s
    for (int i = 0; i < HIST; i++) {
        if (g_rx_hist[i] > top) top = g_rx_hist[i];
        if (g_tx_hist[i] > top) top = g_tx_hist[i];
    }
    int xs[HIST], ys_rx[HIST], ys_tx[HIST];
    int ih = g->graph_h - 6;
    for (int i = 0; i < HIST; i++) {
        xs[i] = g->graph_x + 2 + i * (g->graph_w - 5) / (HIST - 1);
        ys_rx[i] = g->graph_y + 3 + ih - (int)(g_rx_hist[i] * (unsigned long long)ih / top);
        ys_tx[i] = g->graph_y + 3 + ih - (int)(g_tx_hist[i] * (unsigned long long)ih / top);
    }
    ugfx_draw_polyline(s, xs, ys_tx, HIST, 0, utheme_action(UTHEME_ACT_ARRANGE), GEOM_AA);
    ugfx_draw_polyline(s, xs, ys_rx, HIST, 0, UTHEME_ACCENT, GEOM_AA);

    // The legend: a swatch and the last second's rate for each.
    char rate[24], line[48];
    int ch = ugfx_char_h(), half = g->graph_w / 2;
    fmt_rate(rate, sizeof rate, g_view.rx_rate);
    k_snprintf(line, sizeof line, "Receiving %s", rate);
    uui_fill_round_rect(s, g->graph_x, g->legend_y + ch / 2 - 3, 10, 6, 2, UTHEME_ACCENT);
    ugfx_draw_string_clipped(s, g->graph_x + 14, g->legend_y, half - 16, line,
                             wm_flyout_dim(), wm_flyout_ink());
    fmt_rate(rate, sizeof rate, g_view.tx_rate);
    k_snprintf(line, sizeof line, "Sending %s", rate);
    uui_fill_round_rect(s, g->graph_x + half, g->legend_y + ch / 2 - 3, 10, 6, 2,
                        utheme_action(UTHEME_ACT_ARRANGE));
    ugfx_draw_string_clipped(s, g->graph_x + half + 14, g->legend_y, half - 16, line,
                             wm_flyout_dim(), wm_flyout_ink());
}

void network_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!network_open) return;
    struct network_geom g;
    network_geometry(&g);
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();
    int down = shown_down();

    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);

    // THE HEADER: the state in a word, then how.
    char title[32], sub[64], speed[24];
    uint32_t badge = UTHEME_ACCENT;
    fmt_speed(speed, sizeof speed, &g_view);
    if (!g_view.have_device) {
        k_strlcpy(title, "No network device", sizeof title);
        k_strlcpy(sub, "Nothing to connect with", sizeof sub);
        badge = ugfx_blend(utheme_current()->text_muted, UTHEME_WHITE, 80);   // nothing to report
    } else if (down) {
        k_strlcpy(title, "Switched off", sizeof title);
        k_snprintf(sub, sizeof sub, "Wired, %s", g_view.name);
        badge = ugfx_blend(utheme_current()->text_muted, UTHEME_WHITE, 80);   // nothing to report
    } else if (g_view.connected) {
        k_strlcpy(title, "Connected", sizeof title);
        k_snprintf(sub, sizeof sub, "Wired, %s%s%s", g_view.name, speed[0] ? ", " : "", speed);
    } else if (g_view.negotiating) {
        k_strlcpy(title, "Connecting...", sizeof title);
        k_snprintf(sub, sizeof sub, "Waiting for the link, %s", g_view.name);
    } else if (g_view.connecting) {
        k_strlcpy(title, "Connecting...", sizeof title);
        k_snprintf(sub, sizeof sub, "Asking for an address, %s%s%s",
                   g_view.name, speed[0] ? ", " : "", speed);
    } else if (g_view.cable_out) {
        k_strlcpy(title, "Cable unplugged", sizeof title);
        k_snprintf(sub, sizeof sub, "Wired, %s", g_view.name);
        badge = utheme_severity(UTHEME_SEV_WARNING);
    } else {
        k_strlcpy(title, "No address", sizeof title);
        k_snprintf(sub, sizeof sub, "%s, %s%s%s",
                   g_view.asking ? "Nobody answered" : "Waiting for one",
                   g_view.name, speed[0] ? ", " : "", speed);
        badge = utheme_severity(UTHEME_SEV_WARNING);
    }
    wm_flyout_hero(g.x, g.y, g.w - (g.sw_w ? g.sw_w + g.pad + 8 : g.pad), badge,
                   g_view.connecting ? tray_icon_now(g_tray_id) : icon_for(&g_view), title, sub);
    if (g.sw_w) wm_flyout_switch(g.sw_x, g.sw_y, !down, g_hover == HOVER_SWITCH);
    wm_flyout_rule(g.x, g.y + g.hero_h, g.w);
    if (g_view.connecting && !down && wm_anim_enabled()) {
        // THE BUSY BAR, on the rule: a block sweeping the card's width,
        // placed from the clock rather than counted, so a late frame
        // lands where it should have been.
        int bw = g.w / 4;
        int pos = (int)(sys_monotonic_ns() / 1000000ull % BUSY_PERIOD_MS) * (g.w + bw) /
                  BUSY_PERIOD_MS - bw;
        int x0 = pos < 0 ? 0 : pos, x1 = pos + bw > g.w ? g.w : pos + bw;
        if (x1 > x0) ugfx_fill_rect(s, g.x + x0, g.y + g.hero_h - 1, x1 - x0, 2, UTHEME_ACCENT);
    }

    if (!g_view.have_device) {
        ugfx_draw_string_clipped(s, g.x + g.pad, g.note_y, g.w - 2 * g.pad,
                                 "No wired adapter was found. A USB Ethernet",
                                 wm_flyout_dim(), wm_flyout_ink());
        ugfx_draw_string_clipped(s, g.x + g.pad, g.note_y + ch + 4, g.w - 2 * g.pad,
                                 "adapter works when plugged in.", wm_flyout_dim(), wm_flyout_ink());
    }
    if (g.graph_h) {
        wm_flyout_caption(g.graph_x, g.cap1_y, g.graph_w, "TRAFFIC");
        draw_graph(&g);
        wm_flyout_rule(g.x, g.rule2_y, g.w);
    }
    if (g.kv_rows) {
        char keys[NET_ROWS][16], vals[NET_ROWS][48];
        build_rows(keys, vals);
        wm_flyout_caption(g.x + g.pad, g.cap2_y, g.w - 2 * g.pad, "DETAILS");
        for (int i = 0; i < g.kv_rows; i++)
            wm_flyout_kv(g.x + g.pad, g.kv_y + i * g.row_h, g.w - 2 * g.pad, g.kv_key_w,
                         keys[i], vals[i]);
    }

    if (g.copy_w) wm_flyout_button(g.copy_x, g.btn_y, "Copy address", "tb-copy", 1,
                                   g_hover == HOVER_COPY, !g_view.connected || down);
    if (g.renew_w) wm_flyout_button(g.renew_x, g.btn_y, "Renew", "tb-refresh", 1,
                                    g_hover == HOVER_RENEW, down);
    if (g.details_w) wm_flyout_button(g.details_x, g.btn_y, "Details...", 0, 0,
                                      g_hover == HOVER_DETAILS, 0);
    wm_flyout_icon_button(g.gear_x, g.btn_y, g.gear_w, "tb-gear", g_hover == HOVER_GEAR);
}
