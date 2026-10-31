// See drives_popup.h. The tray's USB drives item and its flyout.
#include <stdlib.h>
#include "wm_internal.h"
#include "drives_popup.h"
#include "crash_notice.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "ui/uui_drive.h"
#include "ui/ulog.h"
#include "kapi.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "lib/udrive.h"
#include <errno.h>

int drives_open = 0;

// How long a flyout that opened itself stays, untouched.
#define AUTO_OPEN_NS (6ull * 1000000000ull)

static int g_tray_id = -1;
static uint32_t g_seen_generation;
static int g_last_present = -1;

// What the last poll read: the volumes, their rows, and the list widget
// that draws them. Allocated to fit and replaced whole each poll. A ROW
// IS NOT A VOLUME: a stick with several volumes is a stick row and a row
// for each, and g_row_vol names the volume a row acts on (a stick row:
// its first, for the disk).
static struct udrive *g_v;
static int g_n;
static struct uui_drive_row *g_rows;
static int *g_row_vol;
static int g_nrows, g_rows_cap;
static struct uui_drive g_list;

// Names that live from one poll to the next, each a growable set of
// short strings: the disks an Eject is under way for, the volumes seen
// mounted, and the disks PLUGGED IN while the desktop runs -- a volume
// newly mounted opens the flyout only on one of those. A stick in the
// port at boot is added before the desktop starts but mounted after it,
// and is not news: Windows' AutoPlay answers a device arrival, not a
// mount.
struct nameset { char (*v)[16]; int n, cap; };
static struct nameset g_busy, g_mounted, g_plugged;

static uint32_t g_event_seq;     // the newest device event already read
static uint64_t g_auto_until;    // the self-opened flyout's deadline; 0 when not
static unsigned g_self_opens;    // `gui drives`: a flyout that closed again still counted
enum { HOVER_NONE, HOVER_TRAY, HOVER_ALL, HOVER_GEAR };
static int g_hover;

// --- name sets -------------------------------------------------------------

static int set_has(const struct nameset *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!k_strcmp(s->v[i], name)) return 1;
    return 0;
}

static void set_add(struct nameset *s, const char *name) {
    if (set_has(s, name)) return;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        char (*g)[16] = realloc(s->v, (size_t)cap * sizeof *s->v);
        if (!g) return;
        s->v = g;
        s->cap = cap;
    }
    k_strlcpy(s->v[s->n++], name, sizeof s->v[0]);
}

static void set_drop(struct nameset *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!k_strcmp(s->v[i], name)) { k_memcpy(s->v[i], s->v[--s->n], sizeof s->v[0]); return; }
}

// --- reading ---------------------------------------------------------------

// The first volume of `disk` in `v`, or NULL.
static const struct udrive *vol_of(const struct udrive *v, int n, const char *disk) {
    for (int i = 0; i < n; i++) if (!k_strcmp(v[i].disk, disk)) return &v[i];
    return 0;
}

static int any_mounted(const struct udrive *v, int n, const char *disk) {
    for (int i = 0; i < n; i++) if (!k_strcmp(v[i].disk, disk) && v[i].point[0]) return 1;
    return 0;
}

// Disks, not volumes: a stick with two partitions is one thing plugged in.
int drives_disk_count(void) {
    int n = 0;
    for (int i = 0; i < g_n; i++) if (vol_of(g_v, g_n, g_v[i].disk) == &g_v[i]) n++;
    return n;
}

static int volumes_on(const char *disk) {
    int n = 0;
    for (int i = 0; i < g_n; i++) n += !k_strcmp(g_v[i].disk, disk);
    return n;
}

static void add_row(int vol, int kind) {
    if (g_nrows == g_rows_cap) {
        int cap = g_rows_cap ? g_rows_cap * 2 : 8;
        struct uui_drive_row *r = realloc(g_rows, (size_t)cap * sizeof *r);
        if (r) g_rows = r;
        int *m = r ? realloc(g_row_vol, (size_t)cap * sizeof *m) : 0;
        if (m) g_row_vol = m;
        if (!r || !m) return;   // the list stays as long as memory allowed
        g_rows_cap = cap;
    }
    struct uui_drive_row *row = &g_rows[g_nrows];
    if (kind == 0) uui_drive_fill(row, &g_v[vol]);
    else if (kind == 1) uui_drive_fill_stick(row, &g_v[vol], volumes_on(g_v[vol].disk));
    else uui_drive_fill_volume(row, &g_v[vol]);
    row->busy = set_has(&g_busy, g_v[vol].disk);
    g_row_vol[g_nrows++] = vol;
}

// Each stick in the order its first volume came, with its volumes under it.
static void build_rows(void) {
    g_nrows = 0;
    for (int i = 0; i < g_n; i++) {
        if (vol_of(g_v, g_n, g_v[i].disk) != &g_v[i]) continue;   // its stick is listed
        if (volumes_on(g_v[i].disk) == 1) { add_row(i, 0); continue; }
        add_row(i, 1);
        for (int k = i; k < g_n; k++)
            if (!k_strcmp(g_v[k].disk, g_v[i].disk)) add_row(k, 2);
    }
    if (!g_list.rows) uui_drive_init(&g_list, g_rows, 0, "Plug in a USB stick to see it here.");
    g_list.rows = g_rows;
    g_list.count = g_nrows;
}

// What the kernel said happened to a disk since the last poll, told in a
// card -- read BEFORE the list is replaced, so the old list still names it.
static void read_events(void) {
    struct query_devevent e;
    uint32_t newest = g_event_seq;
    for (int i = 0; sys_query_record(QUERY_DEVEVENT, i, &e, sizeof e) >= (int)sizeof e; i++) {
        if (e.seq <= g_event_seq) continue;
        if (e.seq > newest) newest = e.seq;
        if (k_strncmp(e.device_id, "blk:", 4) != 0) continue;
        const char *disk = e.device_id + 4;
        if (e.kind == QUERY_DEVEV_ADDED) set_add(&g_plugged, disk);
        if (e.kind != QUERY_DEVEV_EJECTED && e.kind != QUERY_DEVEV_REMOVED) continue;
        set_drop(&g_plugged, disk);
        const struct udrive *d = vol_of(g_v, g_n, disk);
        if (!d) continue;   // not a stick this item listed
        char title[96];
        if (e.kind == QUERY_DEVEV_EJECTED) {
            k_snprintf(title, sizeof title, "%s can be pulled out", d->name);
            crash_notice_info(title, "Everything saved to it is on it.", "drive-usb", 0, 0);
        } else if (any_mounted(g_v, g_n, disk)) {
            // Honest about what quick removal promises (usb-storage-design.md):
            // a finished write is on the stick; a copy mid-way is not.
            static const char *const SUBS[] = {
                "It was not ejected first. Everything saved before that is on it; "
                "a copy still running when it went did not finish.",
                "It was not ejected first. A copy still running then did not finish.",
            };
            k_snprintf(title, sizeof title, "%s was pulled out", d->name);
            crash_notice_info(title, crash_notice_sub_fits(SUBS[0]) ? SUBS[0] : SUBS[1],
                              "drive-usb", "OK", 1);
        }
        set_drop(&g_busy, disk);
        ulogf("wm: drives: %s %s\n", disk, e.kind == QUERY_DEVEV_EJECTED ? "ejected" : "removed");
    }
    g_event_seq = newest;
}

// automountd's answers: only a refusal needs saying -- success shows as
// the stick leaving.
static void read_replies(void) {
    struct udrive_msg m;
    while (udrive_take_reply(&m)) {
        if (m.verb != UDRIVE_EJECT) continue;
        set_drop(&g_busy, m.dev);
        if (m.result >= 0) continue;
        const struct udrive *d = vol_of(g_v, g_n, m.dev);
        char title[96];
        k_snprintf(title, sizeof title, "%s was not ejected", d ? d->name : m.dev);
        crash_notice_info(title, m.result == -EBUSY
                              ? "A file on it is still open. Close it and try again."
                              : "The drive did not answer. It is still in use.",
                          "drive-usb", 0, 0);
        ulogf("wm: drives: eject %s refused (%d)\n", m.dev, (int)m.result);
    }
}

// Replaces the list; returns 1 when a volume was newly seen mounted.
static int read_list(void) {
    struct udrive *v;
    int n = udrive_list(&v);
    if (n < 0) return 0;
    free(g_v);
    g_v = v;
    g_n = n;
    int arrived = 0;
    for (int i = 0; i < g_n; i++) {
        if (!g_v[i].point[0] || set_has(&g_mounted, g_v[i].dev)) continue;
        set_add(&g_mounted, g_v[i].dev);
        if (!set_has(&g_plugged, g_v[i].disk) || g_v[i].no_automount) continue;   // a test's disk is not news
        set_drop(&g_plugged, g_v[i].disk);   // once per plug-in: a remount is not news either
        arrived = 1;
    }
    // A volume that went is forgotten, so plugging it back opens the flyout.
    for (int i = 0; i < g_mounted.n;) {
        int still = 0;
        for (int k = 0; k < g_n && !still; k++) still = !k_strcmp(g_v[k].dev, g_mounted.v[i]);
        if (still) i++;
        else set_drop(&g_mounted, g_mounted.v[i]);
    }
    // ...and a disk no longer plugged in is not being ejected.
    for (int i = 0; i < g_busy.n;) {
        if (vol_of(g_v, g_n, g_busy.v[i])) i++;
        else set_drop(&g_busy, g_busy.v[i]);
    }
    build_rows();
    return arrived;
}

static void drives_open_now(int itself);

void drives_poll(void) {
    read_replies();
    read_events();
    int arrived = read_list();

    int present = g_n > 0;
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation || present != g_last_present) {
        g_seen_generation = gen;
        g_last_present = present;
        tray_set_hidden(g_tray_id, !tray_want_shown("usb", present));
    }
    // The card goes with its icon: the last stick ejected leaves nothing
    // for it to hang from.
    if (drives_open && tray_is_hidden(g_tray_id)) drives_close();

    // OPENS ITSELF on an arrival, unless a menu or dialog is up -- the
    // person is in the middle of something, and the icon is there.
    if (arrived && !drives_open && !tray_is_hidden(g_tray_id) && !wm_overlay_any_open() &&
        !wm_overlay_modal_open()) {
        drives_open_now(1);
        g_self_opens++;
        ulogf("wm: drives: opened itself\n");
    }
    if (drives_open && g_auto_until && sys_monotonic_ns() >= g_auto_until && g_hover == HOVER_NONE &&
        g_list.hover_row < 0) {
        drives_close();
        return;
    }
    // Free space moves with every write, so an open card is redrawn each poll.
    if (drives_open) drives_damage();
}

void drives_tray_init(void) {
    g_tray_id = tray_register_icon("tray-usb");
    // The events, the volumes and the disks already here at startup are
    // not news -- nor a volume of one mounted after it (g_plugged).
    struct query_devevent e;
    for (int i = 0; sys_query_record(QUERY_DEVEVENT, i, &e, sizeof e) >= (int)sizeof e; i++)
        if (e.seq > g_event_seq) g_event_seq = e.seq;
    read_list();
    g_last_present = g_n > 0;
    tray_set_hidden(g_tray_id, !tray_want_shown("usb", g_last_present));
}

int drives_tray_hidden(void) { return tray_is_hidden(g_tray_id); }
int drives_count(void) { return g_nrows; }
const char *drives_row_title(int i) { return i >= 0 && i < g_nrows ? g_rows[i].title : ""; }
const char *drives_row_sub(int i) { return i >= 0 && i < g_nrows ? g_rows[i].sub : ""; }
const char *drives_row_point(int i) {
    return i >= 0 && i < g_nrows && g_rows[i].can_open ? g_v[g_row_vol[i]].point : "";
}
int drives_row_busy(int i) { return i >= 0 && i < g_nrows && g_rows[i].busy; }
int drives_row_indent(int i) { return i >= 0 && i < g_nrows && g_rows[i].indent; }
int drives_opened_itself(void) { return drives_open && g_auto_until != 0; }
unsigned drives_self_opens(void) { return g_self_opens; }

// --- geometry ----------------------------------------------------------------

static const char ALL[] = "Eject all";

void drives_geometry(struct drives_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();

    int lw, lh;
    uui_drive_ops.natural_size(&g_list, &lw, &lh);
    int w = ch * 22;
    int foot_w = 2 * (m.pad - 6) + wm_flyout_button_w(ALL, "tb-eject") + 4 + m.btn_h;
    if (w < foot_w) w = foot_w;
    if (w < lw + 2 * (m.pad - ugfx_char_w())) w = lw + 2 * (m.pad - ugfx_char_w());
    int max_w = screen_w - 2 * m.pad;
    if (w > max_w) w = max_w;
    int h = m.hero_h + 1 + m.vpad / 2 + lh + m.vpad / 2 + m.foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - ugfx_px(32); ty = screen_h - taskbar_h; tw = ugfx_px(16); th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->pad = m.pad; g->hero_h = m.hero_h;
    g->list_y = y + m.hero_h + 1 + m.vpad / 2;
    g->list_h = lh;
    g->foot_h = m.foot_h;
    g->foot_y = y + h - m.foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (m.foot_h - m.btn_h) / 2;
    g->all_x = x + m.pad - 6;
    g->all_w = wm_flyout_button_w(ALL, "tb-eject");
    g->gear_w = m.btn_h;
    g->gear_x = x + w - (m.pad - 6) - g->gear_w;
    // The rows lay themselves out across the card, inset by their own padding.
    uui_drive_ops.set_geometry(&g_list, x + m.pad - ugfx_char_w(), g->list_y,
                               w - 2 * (m.pad - ugfx_char_w()), lh);
}

int drives_button(int row, int btn, int r[4]) {
    struct drives_geom g;
    drives_geometry(&g);
    return uui_drive_button_rect(&g_list, row, btn, &r[0], &r[1], &r[2], &r[3]);
}

// --- state -------------------------------------------------------------------

int drives_rect(int *x, int *y, int *w, int *h) {
    struct drives_geom g;
    drives_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void drives_damage(void) { wm_overlay_damage("drives"); }

static void drives_open_now(int itself) {
    wm_overlay_close_others("drives");
    read_list();             // never open on a stale list
    drives_open = 1;
    g_hover = HOVER_NONE;
    g_list.hover_row = g_list.hover_btn = -1;
    g_auto_until = itself ? sys_monotonic_ns() + AUTO_OPEN_NS : 0;
    drives_damage();
}

void drives_close(void) {
    if (!drives_open) return;
    drives_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_hover = HOVER_NONE;
    g_auto_until = 0;
}

// --- input -------------------------------------------------------------------

static void eject(const char *disk) {
    if (set_has(&g_busy, disk)) return;
    if (udrive_ask_eject(disk) < 0) {
        crash_notice_info("Could not eject", "The drive service (automountd) is not running.",
                          "drive-usb", 0, 0);
        return;
    }
    set_add(&g_busy, disk);
    ulogf("wm: drives: eject %s asked\n", disk);
}

static void spawn(const char *path, const char *arg) {
    int pid = sys_spawn(path, arg, -1);
    if (pid > 0) wm_track_launched(pid);
}

int drives_handle_click(int mx, int my) {
    struct drives_geom g;
    drives_geometry(&g);

    if (!drives_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            drives_open_now(0);
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        drives_close();      // a second click on the icon closes it
        return 1;
    }
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        // A flyout that OPENED ITSELF never takes the click that closes
        // it: the person was aiming at something else, not dismissing
        // a card they did not ask for. Otherwise a click on the TASKBAR
        // is still the taskbar's.
        int itself = g_auto_until != 0;
        drives_close();
        return !itself && my < screen_h - taskbar_h;
    }
    g_auto_until = 0;        // touched: it stays until closed
    // A ROW'S BUTTON ACTS ON THE PRESS, as every overlay here does -- the
    // compositor hands an overlay one click, not a press and a release.
    if (uui_drive_press(&g_list, mx, my) && uui_drive_release(&g_list, mx, my) == 2) {
        int r = g_list.fired_row, b = g_list.fired_btn;
        g_list.fired_row = -1;
        if (r >= 0 && r < g_nrows) {
            const struct udrive *d = &g_v[g_row_vol[r]];
            if (b == UUI_DRIVE_OPEN && d->point[0]) {
                drives_close();
                spawn("/bin/wm/apps/files", d->point);
                return 1;
            }
            if (b == UUI_DRIVE_EJECT) eject(d->disk);
        }
        build_rows();
        drives_damage();
        return 1;
    }
    if (g_n && uui_hit(g.all_x, g.btn_y, g.all_w, g.btn_h, mx, my)) {
        for (int i = 0; i < g_n; i++) eject(g_v[i].disk);
        build_rows();
        drives_damage();
        return 1;
    }
    if (uui_hit(g.gear_x, g.btn_y, g.gear_w, g.btn_h, mx, my)) {
        drives_close();
        spawn("/bin/wm/apps/disks", 0);
        return 1;
    }
    return 1;   // the rest is ours, and inert
}

int drives_hover_at(int mx, int my) {
    if (tray_is_hidden(g_tray_id)) return 0;
    struct drives_geom g;
    drives_geometry(&g);
    int was = g_hover;
    g_hover = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my) ? HOVER_TRAY : HOVER_NONE;
    if (drives_open) {
        if (g_n && uui_hit(g.all_x, g.btn_y, g.all_w, g.btn_h, mx, my)) g_hover = HOVER_ALL;
        else if (uui_hit(g.gear_x, g.btn_y, g.gear_w, g.btn_h, mx, my)) g_hover = HOVER_GEAR;
        // The pointer over the card holds a flyout that opened itself.
        if (g_auto_until && uui_hit(g.x, g.y, g.w, g.h, mx, my))
            g_auto_until = sys_monotonic_ns() + AUTO_OPEN_NS;
        if (uui_drive_hover(&g_list, mx, my) || was != g_hover) drives_damage();
    }
    return g_hover == HOVER_TRAY;
}

// --- drawing -----------------------------------------------------------------

void drives_draw(int mx, int my) {
    (void)mx; (void)my;
    struct drives_geom g;
    drives_geometry(&g);

    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);
    char sub[64];
    if (!g_n) k_strlcpy(sub, "Nothing plugged in", sizeof sub);
    else k_snprintf(sub, sizeof sub, "%d plugged in \xb7 safe to pull once ejected", drives_disk_count());
    wm_flyout_hero(g.x, g.y, g.w - g.pad, g_n ? UTHEME_ACCENT
                                             : ugfx_blend(utheme_current()->text_muted, UTHEME_WHITE, 80),
                   "tray-usb", "USB drives", sub);
    wm_flyout_rule(g.x, g.y + g.hero_h, g.w);
    uui_drive_draw(wm_surface(), &g_list, wm_flyout_ink());

    wm_flyout_button(g.all_x, g.btn_y, ALL, "tb-eject", 1, g_hover == HOVER_ALL, !g_n);
    wm_flyout_icon_button(g.gear_x, g.btn_y, g.gear_w, "tb-gear", g_hover == HOVER_GEAR);
}
