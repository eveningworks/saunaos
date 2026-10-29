// Notifications: history, Do Not Disturb, per-app settings, the server half
// of WIN_REQ_NOTIFY, and the bell's flyout -- see wm_notify.h.
#include "wm_notify.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_log.h"
#include "ui/ulog.h"
#include "wm_conf.h"
#include "crash_notice.h"
#include "lib/icon_cache.h"
#include "lib/ugrow.h"
#include "lib/udate.h"
#include "lib/uopen.h"
#include "lib/usetting.h"
#include "ui/uui.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NOTIFY_CONF "/etc/notify.conf"
#define SET_DND     "desktop.notifications.dnd"
#define SET_FOR     "desktop.notifications.dnd_for"
// A POLICY CAP, logged when it bites: the oldest row goes. Windows keeps
// a notification center's worth, KDE a few hundred.
#define HISTORY_MAX 200

struct nf_row {
    uint32_t id;
    char app[WIN_NOTIFY_APP_MAX];
    char icon[WIN_NOTIFY_ICON_MAX];
    char title[WIN_NOTIFY_TITLE_MAX];
    char body[WIN_NOTIFY_BODY_MAX];
    time_t when;
    enum nf_target kind;
    char *target;          // owned; NULL for none
    int pid;
    uint32_t cid;
    int unread;
};

static struct nf_row *g_rows;   // oldest first; grown on demand (lib/ugrow.h)
static int g_nrows, g_cap;
static uint32_t g_next_id = 1;

int wm_notify_open;
static int g_tray = -1;
static int g_dnd;
static long g_until;               // seconds since 1970, 0: until turned off
static char g_for[8] = "1h";
static uint32_t g_seen_gen;
static int g_hover = -1;           // see HOVER_* and row indices
static int g_hover_x;              // over the hovered row's x
enum { HOVER_SWITCH = 1000, HOVER_SETTINGS, HOVER_CLEAR, HOVER_FOR0 };   // + 0..2 for the three durations

static const char *const FOR_KEYS[3]   = { "1h", "4h", "off" };
static const char *const FOR_LABELS[3] = { "1 hour", "4 hours", "Until turned off" };

// --- settings ------------------------------------------------------------

static void read_settings(void) {
    char v[16];
    g_dnd = usetting_get(SET_DND, v, sizeof v) && k_strcmp(v, "on") == 0;
    char u[24];
    g_until = wm_conf_get(NOTIFY_CONF, "dnd_until", u, sizeof u) ? strtol(u, 0, 10) : 0;
    if (!usetting_get(SET_FOR, g_for, sizeof g_for)) k_strlcpy(g_for, "1h", sizeof g_for);
}

int wm_notify_dnd(void) { return g_dnd; }

// When it lifts, from now and `dnd_for` -- written whenever it is
// switched on, from here, Settings or `config`, so it is never stale.
static void write_until(int on) {
    long until = 0;
    if (on) {
        long now = (long)time(0);
        if (!k_strcmp(g_for, "1h")) until = now + 3600;
        else if (!k_strcmp(g_for, "4h")) until = now + 4 * 3600;
    }
    char u[24];
    snprintf(u, sizeof u, "%ld", until);
    wm_conf_set(NOTIFY_CONF, "dnd_until", u);
    g_until = until;
}

static void set_dnd(int on) {
    write_until(on);
    usetting_set(SET_DND, on ? "on" : "off");
    wm_logf("notify: do not disturb %s%s\n", on ? "on" : "off", on && g_until ? " for a while" : "");
    read_settings();
}

// The app's own setting, keyed by its name made fit for a key (a person's
// name has spaces). An app is written down the first time it notifies, so
// System Settings can list every one.
static void app_key(const char *app, char *out, int cap) {
    int n = 0;
    for (const char *p = "app."; *p && n < cap - 1; p++) out[n++] = *p;
    for (const char *p = app; *p && n < cap - 1; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
}

enum { POLICY_ON, POLICY_QUIET, POLICY_OFF };
static int policy_of(const char *app) {
    char key[WIN_NOTIFY_APP_MAX + 4], v[16];
    app_key(app, key, sizeof key);
    if (!key[4]) return POLICY_ON;
    if (!wm_conf_get(NOTIFY_CONF, key, v, sizeof v)) {
        wm_conf_set(NOTIFY_CONF, key, "on");
        return POLICY_ON;
    }
    return !k_strcmp(v, "off") ? POLICY_OFF : !k_strcmp(v, "quiet") ? POLICY_QUIET : POLICY_ON;
}

// --- history -----------------------------------------------------------------

static int unread_count(void) {
    int n = 0;
    for (int i = 0; i < g_nrows; i++) n += g_rows[i].unread;
    return n;
}

static void badge(void) {
    if (g_tray < 0) return;
    tray_set_icon(g_tray, g_dnd ? "tray-bell-dnd" : "tray-bell");
    tray_set_badge(g_tray, unread_count());
}

static void drop_row(int i) {
    free(g_rows[i].target);
    for (int k = i; k < g_nrows - 1; k++) g_rows[k] = g_rows[k + 1];
    g_nrows--;
}

static int row_of(uint32_t id) {
    for (int i = 0; i < g_nrows; i++) if (g_rows[i].id == id) return i;
    return -1;
}

static uint32_t add_row(const struct nf_note *n, uint32_t id) {
    if (g_nrows >= HISTORY_MAX) {
        wm_logf("notify: history full at %d -- the oldest goes\n", HISTORY_MAX);
        drop_row(0);
    }
    if (!UGROW(g_rows, g_cap, g_nrows + 1)) {
        wm_logf("notify: out of memory -- not kept in the history\n");
        return 0;
    }
    struct nf_row *r = &g_rows[g_nrows++];
    k_memset(r, 0, sizeof *r);
    r->id = id ? id : g_next_id++;
    k_strlcpy(r->app, n->app ? n->app : "Notification", sizeof r->app);
    k_strlcpy(r->icon, n->icon ? n->icon : "tray-bell", sizeof r->icon);
    k_strlcpy(r->title, n->title ? n->title : "", sizeof r->title);
    k_strlcpy(r->body, n->body ? n->body : "", sizeof r->body);
    r->when = time(0);
    r->kind = n->target_kind;
    if (n->target && n->target[0]) {
        size_t l = k_strlen(n->target) + 1;
        r->target = malloc(l);
        if (r->target) k_memcpy(r->target, n->target, l);
        else r->kind = NF_TARGET_NONE;
    }
    r->pid = n->pid;
    r->cid = n->cid;
    r->unread = !wm_notify_open;
    return r->id;
}

int wm_notify_note(const struct nf_note *n, uint32_t *hist) {
    *hist = 0;
    const char *app = n->app ? n->app : "Notification";
    int critical = n->urgency == WIN_NOTIFY_URGENCY_CRITICAL;
    int policy = policy_of(app);
    if (policy == POLICY_OFF && !critical) {
        wm_logf("notify: %s is off -- \"%s\" not shown\n", app, n->title ? n->title : "");
        return 0;
    }
    if (!n->transient) *hist = add_row(n, 0);
    badge();
    if (wm_notify_open) wm_notify_damage();
    int show = critical || (!g_dnd && policy == POLICY_ON);
    ulogf("wm: notify %s \"%s\" %s\n", app, n->title ? n->title : "",
          show ? "shown" : g_dnd ? "held (do not disturb)" : "held (quiet)");
    return show;
}

void wm_notify_forget(uint32_t hist) {
    int i = hist ? row_of(hist) : -1;
    if (i < 0) return;
    drop_row(i);
    badge();
    if (wm_notify_open) wm_notify_damage();
}

void wm_notify_answer(int pid, uint32_t id, int action) {
    if (pid <= 0 || !id) return;
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_NOTIFY;
    ev.a = (int32_t)id;
    ev.b = action;
    wm_client_push_event(pid, &ev);
}

// --- the server half -----------------------------------------------------

// One notification being assembled per sender; grown on demand.
struct nf_asm {
    int pid;
    char app[WIN_NOTIFY_APP_MAX], icon[WIN_NOTIFY_ICON_MAX];
    char title[WIN_NOTIFY_TITLE_MAX], body[WIN_NOTIFY_BODY_MAX];
    char action[WIN_NOTIFY_ACTIONS][WIN_NOTIFY_ACTION_MAX];
};
static struct nf_asm *g_asm;
static int g_nasm, g_asm_cap;

static struct nf_asm *asm_of(int pid, int make) {
    for (int i = 0; i < g_nasm; i++) if (g_asm[i].pid == pid) return &g_asm[i];
    if (!make || !UGROW(g_asm, g_asm_cap, g_nasm + 1)) return 0;
    struct nf_asm *a = &g_asm[g_nasm++];
    k_memset(a, 0, sizeof *a);
    a->pid = pid;
    return a;
}

static void asm_done(int pid) {
    for (int i = 0; i < g_nasm; i++)
        if (g_asm[i].pid == pid) { g_asm[i] = g_asm[--g_nasm]; return; }
}

// Appends a piece; a field past its bound is CUT, never refused (the
// sender cut between characters already, so this only ever trims a
// misbehaving one).
static void append(char *dst, int cap, const char *piece) {
    int l = (int)k_strlen(dst);
    if (l < cap - 1) k_strlcpy(dst + l, piece, (size_t)(cap - l));
}

static uint32_t post(int pid, struct nf_asm *a, int flags, uint32_t replaces) {
    if (!a->title[0]) return 0;
    // A sender may name its own icon; otherwise the app's, by its window.
    const char *icon = a->icon[0] ? a->icon : 0;
    if (!icon) {
        for (int i = 0; i < window_count; i++)
            if (windows[i].client_pid == pid && !windows[i].popup) { icon = wm_window_icon_name(i); break; }
    }
    // REPLACED IN PLACE: the old card and row go, and the new one takes
    // their id, so the sender's answer still names it.
    uint32_t id = 0;
    if (replaces) {
        int r = row_of(replaces);
        if (r >= 0 && g_rows[r].pid == pid) { id = replaces; drop_row(r); }
        crash_notice_drop_client(pid, replaces);
    }
    if (!id) id = g_next_id++;
    const char *acts[WIN_NOTIFY_ACTIONS];
    int nact = 0;
    for (int k = 0; k < WIN_NOTIFY_ACTIONS; k++) if (a->action[k][0]) acts[nact++] = a->action[k];
    crash_notice_client(pid, id, a->app[0] ? a->app : "Notification", icon, a->title, a->body,
                        acts, nact, flags & WIN_NOTIFY_URGENCY_MASK, (flags & WIN_NOTIFY_F_TRANSIENT) != 0);
    return id;
}

int wm_notify_piece(int pid, int a, int b, int c, const char *text, int *reply) {
    int field = a & 0xff;
    if (field == WIN_NOTIFY_FIELD_END) {
        struct nf_asm *as = asm_of(pid, 1);
        *reply = as ? (int)post(pid, as, b, (uint32_t)c) : 0;
        asm_done(pid);
        return 1;
    }
    struct nf_asm *as = asm_of(pid, 1);
    if (!as) return 0;
    if (field == WIN_NOTIFY_FIELD_APP) append(as->app, sizeof as->app, text);
    else if (field == WIN_NOTIFY_FIELD_ICON) append(as->icon, sizeof as->icon, text);
    else if (field == WIN_NOTIFY_FIELD_TITLE) append(as->title, sizeof as->title, text);
    else if (field == WIN_NOTIFY_FIELD_BODY) append(as->body, sizeof as->body, text);
    else if (field >= WIN_NOTIFY_FIELD_ACTION && field < WIN_NOTIFY_FIELD_ACTION + WIN_NOTIFY_ACTIONS)
        append(as->action[field - WIN_NOTIFY_FIELD_ACTION], WIN_NOTIFY_ACTION_MAX, text);
    return 0;
}

void wm_notify_close_request(int pid, uint32_t id) {
    int r = row_of(id);
    if (r >= 0 && g_rows[r].pid == pid) wm_notify_forget(id);
    crash_notice_drop_client(pid, id);
}

// --- the tray item and its poll ------------------------------------------

void wm_notify_tray_init(void) {
    read_settings();
    g_seen_gen = wm_setting_generation();
    g_tray = tray_register_icon("tray-bell");
    badge();
}

void wm_notify_poll(void) {
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_gen) {
        g_seen_gen = gen;
        int was = g_dnd;
        read_settings();
        if (was != g_dnd) {
            write_until(g_dnd);   // switched elsewhere: it lasts from now
            badge();
            if (wm_notify_open) wm_notify_damage();
        }
    }
    // DO NOT DISTURB LIFTS ON ITS OWN when its hour is up.
    if (g_dnd && g_until && (long)time(0) >= g_until) {
        set_dnd(0);
        badge();
        if (wm_notify_open) wm_notify_damage();
    }
}

// --- the flyout ------------------------------------------------------------

struct nf_geom {
    int x, y, w, h;
    int pad, row_h, rows_y, nshow;   // rows drawn: the newest that fit
    int dnd_y, dnd_h, sw_x, sw_y;
    int for_y, for_x[3], for_w[3];
    int foot_y, foot_h, set_x, set_w, clr_x, clr_w, btn_y, btn_h;
    int tray_x, tray_y, tray_w, tray_h;
};

static void geometry(struct nf_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    g->pad = m.pad;
    g->w = ugfx_char_w() * 38;
    g->row_h = ugfx_char_h() * 2 + m.pad;
    int head = m.hero_h;
    g->dnd_h = m.row_h;
    int for_h = utheme_control_h() + m.pad / 2;
    g->foot_h = m.foot_h;
    // THE NEWEST THAT FIT above the taskbar, with room for a "none" line.
    int room = (screen_h - taskbar_h) * 3 / 4 - (head + g->dnd_h + for_h + g->foot_h + 2 * m.vpad);
    int fit = room / g->row_h;
    if (fit < 1) fit = 1;
    g->nshow = g_nrows < fit ? g_nrows : fit;
    int list_h = g->nshow ? g->nshow * g->row_h : g->row_h;
    g->h = head + g->dnd_h + for_h + m.vpad + list_h + m.vpad + g->foot_h;
    if (!tray_item_rect(g_tray, &g->tray_x, &g->tray_y, &g->tray_w, &g->tray_h)) {
        g->tray_x = screen_w - 64; g->tray_y = screen_h - taskbar_h; g->tray_w = 24; g->tray_h = taskbar_h;
    }
    wm_popup_place(g->tray_x + g->tray_w - g->w, screen_h - taskbar_h - g->h, g->w, g->h, &g->x, &g->y);
    g->dnd_y = g->y + head;
    g->sw_x = g->x + g->w - m.pad - m.sw_w;
    g->sw_y = g->dnd_y + (g->dnd_h - m.sw_h) / 2;
    g->for_y = g->dnd_y + g->dnd_h;
    int fx = g->x + m.pad;
    for (int k = 0; k < 3; k++) {
        g->for_w[k] = wm_flyout_button_w(FOR_LABELS[k], 0);
        g->for_x[k] = fx;
        fx += g->for_w[k] + m.pad / 2;
    }
    g->rows_y = g->for_y + for_h + m.vpad;
    g->foot_y = g->y + g->h - g->foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (g->foot_h - m.btn_h) / 2;
    g->clr_w = wm_flyout_button_w("Clear all", 0);
    g->clr_x = g->x + g->w - m.pad - g->clr_w;
    g->set_w = wm_flyout_button_w("Notification settings...", 0);
    g->set_x = g->x + m.pad;
}

int wm_notify_rect(int *x, int *y, int *w, int *h) {
    if (!wm_notify_open) return 0;
    struct nf_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void wm_notify_damage(void) { wm_overlay_damage("notify"); }

static void open_now(void) {
    wm_overlay_close_others("notify");
    wm_notify_open = 1;
    g_hover = -1;
    for (int i = 0; i < g_nrows; i++) g_rows[i].unread = 0;   // seen
    badge();
    wm_notify_damage();
}

void wm_notify_close(void) {
    if (!wm_notify_open) return;
    wm_notify_open = 0;   // the core damages where it was
    g_hover = -1;
}

// The row drawn at slot k is the k-th NEWEST.
static int row_index(const struct nf_geom *g, int k) { (void)g; return g_nrows - 1 - k; }

static int row_at(const struct nf_geom *g, int mx, int my) {
    if (mx < g->x || mx >= g->x + g->w || my < g->rows_y) return -1;
    int k = (my - g->rows_y) / g->row_h;
    return k < g->nshow ? k : -1;
}

static int close_box(const struct nf_geom *g, int k, int *x, int *y, int *s) {
    *s = ugfx_char_h() + 4;
    *x = g->x + g->w - g->pad - *s;
    *y = g->rows_y + k * g->row_h + (g->row_h - *s) / 2;
    return 1;
}

static void act_on(int i) {
    struct nf_row r = g_rows[i];
    g_rows[i].target = 0;           // the copy owns it now
    drop_row(i);
    badge();
    if (r.kind == NF_TARGET_FILE && r.target) {
        int pid = uopen_spawn(r.target);
        if (pid > 0) wm_track_launched(pid);
    } else if (r.kind == NF_TARGET_REPORT && r.target) {
        char args[300];
        snprintf(args, sizeof args, "--report %s", r.target);
        int pid = sys_spawn("/bin/wm/apps/crashreports", args, -1);
        if (pid > 0) wm_track_launched(pid);
    } else if (r.kind == NF_TARGET_CLIENT) {
        wm_notify_answer(r.pid, r.cid, WIN_NOTIFY_CLICKED);
        crash_notice_drop_client(r.pid, r.cid);
    }
    free(r.target);
}

int wm_notify_click(int mx, int my) {
    struct nf_geom g;
    geometry(&g);
    int on_tray = g_tray >= 0 && uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my);
    if (!wm_notify_open) {
        if (on_tray) { open_now(); return 1; }
        return 0;
    }
    if (on_tray) { wm_notify_close(); return 1; }   // a second click on the bell closes it
    if (uui_hit(g.sw_x - g.pad, g.dnd_y, g.w - (g.sw_x - g.x) + g.pad, g.dnd_h, mx, my) ||
        uui_hit(g.x, g.dnd_y, g.w, g.dnd_h, mx, my)) {
        set_dnd(!g_dnd);
        badge();
        wm_notify_damage();
        return 1;
    }
    for (int k = 0; k < 3; k++) {
        if (!uui_hit(g.for_x[k], g.for_y, g.for_w[k], utheme_control_h(), mx, my)) continue;
        usetting_set(SET_FOR, FOR_KEYS[k]);
        k_strlcpy(g_for, FOR_KEYS[k], sizeof g_for);
        if (g_dnd) set_dnd(1);      // on already: from now, for the new length
        wm_notify_damage();
        return 1;
    }
    if (uui_hit(g.clr_x, g.btn_y, g.clr_w, g.btn_h, mx, my)) {
        if (!g_nrows) return 1;
        // Clearing answers nobody: a client's notification CLOSED, as
        // KDE's Clear All closes each one.
        for (int i = 0; i < g_nrows; i++)
            if (g_rows[i].kind == NF_TARGET_CLIENT) wm_notify_answer(g_rows[i].pid, g_rows[i].cid, WIN_NOTIFY_CLOSED);
        while (g_nrows) drop_row(g_nrows - 1);
        badge();
        wm_notify_damage();
        return 1;
    }
    if (uui_hit(g.set_x, g.btn_y, g.set_w, g.btn_h, mx, my)) {
        wm_notify_close();
        int pid = sys_spawn("/bin/wm/system/settings", "desktop.notifications.dnd", -1);
        if (pid > 0) wm_track_launched(pid);
        return 1;
    }
    int k = row_at(&g, mx, my);
    if (k >= 0) {
        int i = row_index(&g, k);
        int bx, by, bs;
        close_box(&g, k, &bx, &by, &bs);
        if (uui_hit(bx, by, bs, bs, mx, my)) {
            if (g_rows[i].kind == NF_TARGET_CLIENT)
                wm_notify_answer(g_rows[i].pid, g_rows[i].cid, WIN_NOTIFY_CLOSED);
            drop_row(i);
            badge();
        } else {
            wm_notify_close();
            act_on(i);
        }
        wm_notify_damage();
        return 1;
    }
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 1;   // inert, but ours
    wm_notify_close();
    return my < screen_h - taskbar_h;   // a click on the taskbar is still the taskbar's
}

int wm_notify_hover_at(int mx, int my) {
    if (!wm_notify_open) return 0;
    struct nf_geom g;
    geometry(&g);
    int h = -1;
    if (uui_hit(g.x, g.dnd_y, g.w, g.dnd_h, mx, my)) h = HOVER_SWITCH;
    for (int k = 0; k < 3; k++)
        if (uui_hit(g.for_x[k], g.for_y, g.for_w[k], utheme_control_h(), mx, my)) h = HOVER_FOR0 + k;
    if (uui_hit(g.clr_x, g.btn_y, g.clr_w, g.btn_h, mx, my)) h = HOVER_CLEAR;
    if (uui_hit(g.set_x, g.btn_y, g.set_w, g.btn_h, mx, my)) h = HOVER_SETTINGS;
    int k = row_at(&g, mx, my);
    if (k >= 0) h = k;
    g_hover_x = mx;
    if (h != g_hover) { g_hover = h; wm_notify_damage(); }
    int bx, by, bs;
    return h < 0 ? 0 : h * 2 + 1 + (h >= 0 && h < 1000 && close_box(&g, h, &bx, &by, &bs) &&
                                   uui_hit(bx, by, bs, bs, mx, my));
}

int wm_notify_key(int key, uint8_t mods) {
    (void)mods;
    if (!wm_notify_open) return 0;
    if (key == 0x1B) { wm_notify_close(); return 1; }
    return 0;
}

// "now", "5 min", or the time of day once it is an hour old.
static void when_text(time_t t, char *out, int cap) {
    long d = (long)(time(0) - t);
    if (d < 60) { k_strlcpy(out, "now", (size_t)cap); return; }
    if (d < 3600) { snprintf(out, (size_t)cap, "%ld min", d / 60); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    udate_format_tm(out, (unsigned long)cap, &tm, UDATE_TIME);
}

void wm_notify_draw(int mx, int my) {
    (void)my;
    if (!wm_notify_open) return;
    struct nf_geom g;
    geometry(&g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    struct ugfx_surface *s = wm_surface();
    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);

    char sub[64];
    if (g_dnd && g_until) {
        struct tm tm;
        time_t u = (time_t)g_until;
        localtime_r(&u, &tm);
        char t[16];
        udate_format_tm(t, sizeof t, &tm, UDATE_TIME);
        snprintf(sub, sizeof sub, "Do not disturb until %s", t);
    } else if (g_dnd) {
        k_strlcpy(sub, "Do not disturb is on", sizeof sub);
    } else if (g_nrows) {
        snprintf(sub, sizeof sub, "%d kept", g_nrows);
    } else {
        k_strlcpy(sub, "None", sizeof sub);
    }
    wm_flyout_hero(g.x, g.y, g.w - g.pad, g_dnd ? UTHEME_OUTLINE : UTHEME_ACCENT,
                   g_dnd ? "tray-bell-dnd" : "tray-bell", "Notifications", sub);

    // DO NOT DISTURB: the switch, and how long the next one lasts.
    wm_flyout_rule(g.x, g.dnd_y, g.w);
    ugfx_draw_string_clipped(s, g.x + g.pad, g.dnd_y + (g.dnd_h - ugfx_char_h()) / 2,
                             g.sw_x - g.x - 2 * g.pad, "Do not disturb", wm_flyout_ink(), UGFX_TRANSPARENT);
    wm_flyout_switch(g.sw_x, g.sw_y, g_dnd, g_hover == HOVER_SWITCH);
    for (int k = 0; k < 3; k++)
        wm_flyout_button(g.for_x[k], g.for_y, FOR_LABELS[k], 0, !k_strcmp(g_for, FOR_KEYS[k]),
                         g_hover == HOVER_FOR0 + k, 0);
    wm_flyout_rule(g.x, g.rows_y - m.vpad / 2, g.w);

    if (!g.nshow) {
        const char *none = "No notifications";
        ugfx_draw_string(s, g.x + (g.w - ugfx_text_width(none)) / 2,
                         g.rows_y + (g.row_h - ugfx_char_h()) / 2, none, wm_flyout_dim(), UGFX_TRANSPARENT);
    }
    int isz = ugfx_char_h() + ugfx_char_h() / 2;
    for (int k = 0; k < g.nshow; k++) {
        const struct nf_row *r = &g_rows[row_index(&g, k)];
        int ry = g.rows_y + k * g.row_h;
        if (g_hover == k)
            uui_fill_round_rect(s, g.x + g.pad / 2, ry, g.w - g.pad, g.row_h, 6,
                                uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
        const struct uimg *ico = icon_get(r->icon, isz);
        if (ico) ugfx_blit_alpha(s, g.x + g.pad, ry + (g.row_h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
        int tx = g.x + g.pad + isz + g.pad;
        int bx, by, bs;
        close_box(&g, k, &bx, &by, &bs);
        int tw = (g_hover == k ? bx : g.x + g.w - g.pad) - tx - 4;
        char when[24], line[WIN_NOTIFY_BODY_MAX + 64];
        when_text(r->when, when, sizeof when);
        int ty = ry + (g.row_h - 2 * ugfx_char_h()) / 2;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_elided(s, tx, ty, tw, r->title, wm_flyout_ink(), UGFX_TRANSPARENT);
        ugfx_set_font(was);
        if (r->body[0]) snprintf(line, sizeof line, "%s \xc2\xb7 %s \xc2\xb7 %s", r->body, r->app, when);
        else snprintf(line, sizeof line, "%s \xc2\xb7 %s", r->app, when);
        ugfx_draw_string_elided(s, tx, ty + ugfx_char_h(), tw, line, wm_flyout_dim(), UGFX_TRANSPARENT);
        if (g_hover == k) {
            int on = uui_hit(bx, by, bs, bs, g_hover_x, my);
            if (on) uui_fill_round_rect(s, bx, by, bs, bs, 4, uui_state_bg(wm_flyout_ground(), UUI_STATE_PRESSED));
            int c = bs / 2, a = bs / 5;
            ugfx_draw_line(s, bx + c - a, by + c - a, bx + c + a, by + c + a, UTHEME_TEXT, GEOM_AA);
            ugfx_draw_line(s, bx + c + a, by + c - a, bx + c - a, by + c + a, UTHEME_TEXT, GEOM_AA);
        }
    }
    (void)mx;
    wm_flyout_button(g.set_x, g.btn_y, "Notification settings...", 0, 0, g_hover == HOVER_SETTINGS, 0);
    wm_flyout_button(g.clr_x, g.btn_y, "Clear all", 0, 1, g_hover == HOVER_CLEAR, g_nrows == 0);
}

int wm_notify_state(int *unread, int *dnd, int *rx, int *ry, int *rw, int *rh,
                    int sw[4], int clear[4], int bell[4],
                    struct wm_notify_row_info *out, int max) {
    struct nf_geom g;
    geometry(&g);
    *unread = unread_count();
    *dnd = g_dnd;
    *rx = *ry = *rw = *rh = 0;
    if (wm_notify_open) { *rx = g.x; *ry = g.y; *rw = g.w; *rh = g.h; }
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    sw[0] = g.sw_x; sw[1] = g.sw_y; sw[2] = m.sw_w; sw[3] = m.sw_h;
    clear[0] = g.clr_x; clear[1] = g.btn_y; clear[2] = g.clr_w; clear[3] = g.btn_h;
    bell[0] = g.tray_x; bell[1] = g.tray_y; bell[2] = g.tray_w; bell[3] = g.tray_h;
    int n = 0;
    for (int i = g_nrows - 1; i >= 0 && n < max; i--, n++) {
        int k = g_nrows - 1 - i;
        out[n].id = g_rows[i].id;
        out[n].app = g_rows[i].app;
        out[n].title = g_rows[i].title;
        out[n].body = g_rows[i].body;
        int shown = wm_notify_open && k < g.nshow;
        out[n].x = shown ? g.x : 0;
        out[n].y = shown ? g.rows_y + k * g.row_h : 0;
        out[n].w = shown ? g.w : 0;
        out[n].h = shown ? g.row_h : 0;
        out[n].cx = out[n].cy = out[n].cs = 0;
        if (shown) close_box(&g, k, &out[n].cx, &out[n].cy, &out[n].cs);
    }
    return n;
}
