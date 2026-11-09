// Install SaunaOS: the window that puts this running system onto a disk --
// Welcome, Disk, Software, Ready, Installing, Finished on a step rail
// (Calamares' shape, and First-boot setup's frame). The five stages are
// base/uinstall.h's, the ones `install` runs, on a worker thread; this
// window polls how far it has got. Software is base/uinstall.h's choice
// of optional components too, `install --without`'s, drawn as
// uui_appcard's checklist.
//
// THREE WAYS IN, all decided at boot by /etc/services.d/installer, which
// runs `installer --boot` after toywm:
//
//   installer=off           nothing at all
//   a live session          a desktop launcher and a live-session notice
//                           with "Install SaunaOS" (Ubuntu's casper and
//                           Fedora's liveinst put the same two there)
//   installer=on            this window, full screen -- the live boot
//                           menu's "Install SaunaOS" entry
//
// An installed machine gets none of them; the window still runs from
// the Start menu, onto another disk, as `install --disk` does.
//
// NOTHING IS ERASED UNTIL THE CONFIRM on Ready names what goes, and the
// rail and the buttons are held from then until the run returns.
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "rt/sys.h"
#include "query_abi.h"
#include "win_proto.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "base/ulog.h"
#include "ui/uui_wizard.h"
#include "ui/uui_appcard.h"
#include "ui/uui_builder.h"
#include "ui/uui_layout.h"
#include "ui/uui_label.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_segmented.h"
#include "ui/uui_grid.h"
#include "ui/uui_diskplan.h"
#include "ui/uui_stages.h"
#include "ui/uui_dialog.h"
#include "ui/uui_focus.h"
#include "ui/uui_primitives.h"
#include "base/human.h"
#include "base/uinitials.h"
#include "media/icon_cache.h"
#include "media/uimg.h"
#include "base/ugrow.h"
#include "base/uduration.h"
#include "base/ufbs.h"
#include "base/uinitctl.h"
#include "base/uinstall.h"
#include "base/udiskscan.h"
#include "base/udirs.h"

#define SELF "/bin/wm/system/installer"

enum { S_WELCOME, S_DISK, S_SOFTWARE, S_READY, S_INSTALL, S_DONE, S_COUNT };

enum {
    ID_DISKS = 1, ID_TABLE, ID_PLAN, ID_ASKALL, ID_RESTART, ID_DIALOG,
    ID_KEEP, ID_SHUTDOWN, ID_SOFTWARE, ID_SW_ALL, ID_SW_NONE,
};
enum { ASK_CANCEL = 1, ASK_ERASE, ASK_OK };

static struct uapp *g_app;

// --- what the machine has ------------------------------------------------

static struct udisk_scan g_scan;          // ~10 KB: never on the stack
static int *g_usable, g_nusable, g_usable_cap;   // grown to the disks
static uint32_t g_files;                  // what the copy carries, as the Software step chose
static uint64_t g_bytes, g_need, g_left_out;
static struct uinstall_software g_sw;     // removed = left out (base/uinstall.h)
static unsigned g_carry;                  // the pages this session set (base/ufbs.h)
static char g_version[32] = "SaunaOS";

static int g_live;   // a live session: "keep trying" and the USB stick mean something
static struct uinstall_plan g_plan;
static struct uinstall_progress g_prog;
static int g_running;
static uint64_t g_t0, g_took_ms;

static struct uui_grid g_grid;          // the disk cards, behind installer.ui's `app #disks`
static struct uui_segmented *g_table;   // installer.ui's, as is every widget but the three apps
static int g_mbr_at;                    // the table row's MBR segment
static struct uui_wizard *g_wz;
static struct uui_layout *g_root;

// GPT until the window exists: scan() runs before it, to know which disks to offer.
static int cur_mbr(void) { return g_table && g_table->selected == g_mbr_at; }

static struct udisk *chosen(void) {
    int i = g_grid.selected;
    return i >= 0 && i < g_nusable ? &g_scan.disks[g_usable[i]] : 0;
}

static uint64_t g_need_seen;   // the need the disks offered were judged by

static void scan(void) {
    g_need_seen = g_need;
    udisk_scan(&g_scan);
    g_nusable = 0;
    if (!UGROW(g_usable, g_usable_cap, g_scan.ndisks)) {
        ulogf("installer: out of memory listing %d disks\n", g_scan.ndisks);
        return;
    }
    for (int i = 0; i < g_scan.ndisks; i++)
        if (!uinstall_refusal(&g_scan.disks[i], UINSTALL_ESP_DEFAULT, cur_mbr(), g_need))
            g_usable[g_nusable++] = i;
}

// --- the widgets, page by page ------------------------------------------------

static struct uui_label *w_intro, *w_intro_live, *w_none, *w_refused, *w_done_title, *w_done_text;
static char g_refused_text[256], g_none_text[160], g_done_title[96], g_done_text[256];
static struct uui_diskplan g_plan_w;
static struct uui_checkbox *g_askall, *g_restart;
static struct uui_stages g_stages;
static struct uui_button *g_keep, *g_shutdown;
static struct uui_dialog *g_ask;


// Fact cards: a label column and a value column, the summary First-boot
// setup's Ready step draws.
#define FACTS_MAX 6
struct facts { int n; const char *k[FACTS_MAX]; char v[FACTS_MAX][128]; };
static struct facts g_welcome_facts, g_ready_facts, g_done_facts;
static struct uui_custom *g_welcome_card, *g_ready_card, *g_done_card;

static int fact_row_h(void) { return ugfx_char_h() * 2; }

static void draw_facts(struct ugfx_surface *s, const struct uui_custom *c) {
    const struct facts *f = c->state;
    int r = ugfx_char_h() / 2, pad = ugfx_char_w() * 2;
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, r - 1, UTHEME_WHITE);
    int lw = ugfx_char_advance('n') * 13, rh = fact_row_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    for (int i = 0; i < f->n; i++) {
        int y = c->y + i * rh;
        if (i) ugfx_fill_rect(s, c->x + pad, y, c->w - 2 * pad, 1, UTHEME_WINDOW_BG);
        int ty = y + (rh - ugfx_char_h()) / 2;
        ugfx_draw_string_clipped(s, c->x + pad, ty, lw, f->k[i], dim, UTHEME_WHITE);
        ugfx_draw_string_clipped(s, c->x + pad + lw, ty, c->w - 2 * pad - lw, f->v[i], UTHEME_TEXT,
                                 UTHEME_WHITE);
    }
}

static void fact(struct facts *f, const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fact(struct facts *f, const char *k, const char *fmt, ...) {
    if (f->n >= FACTS_MAX) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->v[f->n], sizeof f->v[f->n], fmt, ap);
    va_end(ap);
    f->k[f->n++] = k;
}

static void card_fit(struct uui_custom *c, const struct facts *f) {
    c->h = fact_row_h() * (f->n ? f->n : 1);
}

// The pages the session set, as a list a person reads: "Keyboard, Time
// zone". Four or more would not fit a fact row, so those are said by
// what is left out instead.
static void carried_text(char *out, size_t cap) {
    int n = 0;
    for (int p = 0; p < UFBS_PAGES; p++) if (g_carry & (1u << p)) n++;
    out[0] = 0;
    if (n == UFBS_PAGES) { snprintf(out, cap, "every setup page"); return; }
    int listing_set = n < 4;
    if (!listing_set) strlcat(out, "every setup page but ", cap);
    int first = 1;
    for (int p = 0; p < UFBS_PAGES; p++) {
        if ((int)((g_carry >> p) & 1u) != listing_set) continue;
        if (!first) strlcat(out, ", ", cap);
        strlcat(out, ufbs_page_title(p), cap);
        first = 0;
    }
}

static void disk_card(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx;
    if (index < 0 || index >= g_nusable) return;
    const struct udisk *d = &g_scan.disks[g_usable[index]];
    int pad = ugfx_char_w(), lh = ugfx_char_h(), sel = state & UUI_GRID_SELECTED;
    uint32_t ink = sel ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, ground = sel ? UTHEME_ACCENT : uui_grid_c_bg(&g_grid);
    char line1[64], line2[96], size[24];
    human_size_iec(size, sizeof size, d->sectors * 512);
    snprintf(line1, sizeof line1, "%s", d->model[0] ? d->model : d->name);
    int used = 0;
    for (int k = 0; k < d->nparts; k++) if (d->parts[k].fs[0] || d->parts[k].number) used++;
    snprintf(line2, sizeof line2, "%s  -  %s  -  %s", d->name, size,
             used ? (d->kind == QUERY_PART_GPT ? "GPT, has partitions" : "has partitions") : "empty");
    int px = lh * 2;
    const struct uimg *ico = icon_get("drive", px);
    if (ico) ugfx_blit_alpha(s, x + pad, y + (h - ico->h) / 2, ico->w, ico->h, ico->px, ico->w);
    int tx = x + pad * 2 + px, room = w - (tx - x) - pad, ty = y + (h - lh * 2 - 2) / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, tx, ty, room, line1, ink, ground);
    ugfx_set_font(was);
    ugfx_draw_string_clipped(s, tx, ty + lh + 2, room, line2,
                             sel ? ink : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), ground);
}

// --- the Software step ----------------------------------------------------------
// A row per optional component (uui_appcard's CHECKS), ticked as this
// session has them; a tick or an untick goes through the engine, so what
// Needs= ties together moves together.

static struct uui_appcards g_checks;      // behind installer.ui's `app #software`
static struct uui_appcard *g_rows;        // one per component, grown to the set
struct row_text { char kind[96], badge[24], size[16], glyph[12]; };
static struct row_text *g_row_text;
static int g_rows_cap;
static struct uui_label *w_sw_total;
static struct uui_button *g_sw_all, *g_sw_none;
static char g_sw_total[128];

// What the row says after the name: what it needs, else its project, else
// its summary -- and for one removed here, that.
static void software_kind(int i, char *out, size_t cap) {
    const struct ucomponent *c = &g_sw.set.c[i];
    if (g_sw.absent[i]) { snprintf(out, cap, "removed on this system"); return; }
    const char *w = c->needs;
    while (*w == ' ') w++;
    size_t n = strcspn(w, " ");
    if (n && n < UCOMPONENT_ID_MAX) {
        char id[UCOMPONENT_ID_MAX];
        memcpy(id, w, n);
        id[n] = '\0';
        int k = ucomponent_find(&g_sw.set, id);
        if (k >= 0) { snprintf(out, cap, "Needs the %s", g_sw.set.c[k].name); return; }
    }
    snprintf(out, cap, "%s", c->project[0] ? c->project : c->summary[0] ? c->summary
                                                                       : ucomponent_category_label(c->category));
}

// The choice, for a test and for the log: in=, out=, absent= and the rows'
// order=, each a comma list of ids. Longer than ulogf()'s line with many
// components, so written whole.
static void software_log(void) {
    size_t cap = (size_t)(g_sw.set.count + 1) * (UCOMPONENT_ID_MAX + 1) * 4 + 64;
    char *line = malloc(cap);
    if (!line) return;
    static const char *const KEY[] = { "in", "out", "absent", "order" };
    size_t len = (size_t)snprintf(line, cap, "installer: software");
    for (int k = 0; k < 4; k++) {
        len += (size_t)snprintf(line + len, cap - len, " %s=", KEY[k]);
        int first = 1;
        for (int i = 0; i < g_sw.set.count; i++) {
            int in = uinstall_software_in(&g_sw, i);
            if (k == 0 ? !in : k == 1 ? in : k == 2 ? !g_sw.absent[i] : 0) continue;
            len += (size_t)snprintf(line + len, cap - len, "%s%s", first ? "" : ",", g_sw.set.c[i].id);
            first = 0;
        }
    }
    snprintf(line + len, cap - len, "\n");
    sys_eprint(line);
    free(line);
}

// The rows, the total under them and the two buttons, from the choice --
// and the room the root needs, which follows it.
static void refresh_software(void) {
    int n = g_sw.set.count;
    if (n > g_rows_cap) {
        struct uui_appcard *r = realloc(g_rows, sizeof *r * (size_t)n);
        if (r) g_rows = r;
        struct row_text *t = realloc(g_row_text, sizeof *t * (size_t)n);
        if (t) g_row_text = t;
        if (!r || !t) { ulogf("installer: out of memory listing %d components\n", n); return; }
        g_rows_cap = n;
    }
    for (int i = 0; i < n; i++) {
        const struct ucomponent *c = &g_sw.set.c[i];
        struct row_text *t = &g_row_text[i];
        struct uui_appcard *a = &g_rows[i];
        memset(a, 0, sizeof *a);
        software_kind(i, t->kind, sizeof t->kind);
        ucomponent_short_licence(c, t->badge, sizeof t->badge);
        human_size_iec(t->size, sizeof t->size, c->size);
        uinitials(c->name, t->glyph, sizeof t->glyph);
        a->name = c->name;
        a->kind = t->kind;
        a->badge = t->badge;
        a->badge_warm = ucomponent_is_gpl(c);
        a->size = t->size;
        a->icon = c->icon[0] ? c->icon : 0;
        a->glyph = t->glyph;
        a->checked = uinstall_software_in(&g_sw, i);
        a->button_greyed = g_sw.absent[i];
    }
    uui_appcards_set(&g_checks, g_rows, n);
    uinstall_software_totals(&g_sw, &g_files, &g_bytes, &g_left_out);
    g_need = uinstall_root_need(g_bytes);
    char size[16], out[16];
    human_size_iec(size, sizeof size, g_bytes);
    human_size_iec(out, sizeof out, g_left_out);
    if (g_left_out)
        snprintf(g_sw_total, sizeof g_sw_total, "Copies %u files, %s \xc2\xb7 %s left out", (unsigned)g_files, size, out);
    else
        snprintf(g_sw_total, sizeof g_sw_total, "Copies %u files, %s", (unsigned)g_files, size);
    // Greyed when they would change nothing (userland/CLAUDE.md, rule 7).
    if (g_sw_all) g_sw_all->disabled = uinstall_software_count_in(&g_sw) == uinstall_software_count_here(&g_sw);
    if (g_sw_none) g_sw_none->disabled = uinstall_software_count_in(&g_sw) == 0;
    software_log();
}

static void toggle_software(int i) {
    if (i < 0 || i >= g_sw.set.count) return;
    int in = !uinstall_software_in(&g_sw, i);
    if (uinstall_software_choose(&g_sw, i, in) < 0)
        ulogf("installer: %s is not on this system, so it cannot go on the disk\n", g_sw.set.c[i].id);
    refresh_software();
}

// --- keeping the pages true ----------------------------------------------------

// The plan for the disk and table chosen, and the After row drawn from it.
static const char *refresh_plan(void) {
    struct udisk *d = chosen();
    if (!d) {
        uui_diskplan_set(&g_plan_w, 0, 0, 0);
        return "Choose a disk.";
    }
    const char *why = uinstall_plan(&g_plan, &g_scan, d->name, UINSTALL_ESP_DEFAULT, cur_mbr(), g_need);
    if (!why) why = uinstall_payload_problem(cur_mbr());
    struct uui_diskplan_new after[3];
    int n = 0;
    if (!g_plan.nparts) {
        uui_diskplan_set(&g_plan_w, d, 0, 0);
        return why;
    }
    for (int i = 0; i < g_plan.nparts; i++) {
        const struct uinstall_part *q = &g_plan.part[i];
        after[n++] = (struct uui_diskplan_new){
            q->start, q->sectors,
            i == g_plan.root ? UDISK_ROLE_DATA : i == g_plan.boot ? UDISK_ROLE_ESP : UDISK_ROLE_BIOS,
            i == g_plan.root ? "SaunaOS" : i == g_plan.boot ? "/boot" : "BIOS",
            q->fs ? (i == g_plan.root ? "TFS3" : "FAT32") : 0,
        };
    }
    uui_diskplan_set(&g_plan_w, d, after, n);
    return why;
}

static void refresh_refused(void) {
    g_refused_text[0] = 0;
    for (int i = 0; i < g_scan.ndisks; i++) {
        const struct udisk *d = &g_scan.disks[i];
        const char *why = uinstall_refusal(d, UINSTALL_ESP_DEFAULT, cur_mbr(), g_need);
        if (!why) continue;
        char one[96];
        snprintf(one, sizeof one, "%s%s (%s): %s", g_refused_text[0] ? "; " : "Not offered: ",
                 d->name, d->persistent ? (d->model[0] ? d->model : d->driver) : "live image", why);
        strlcat(g_refused_text, one, sizeof g_refused_text);
    }
    snprintf(g_none_text, sizeof g_none_text, "%s",
             g_nusable ? "" : "No disk here can take SaunaOS. Attach one, then reopen this window.");
}

// The disks offered, judged again -- by the table chosen, and by the room
// the software chosen needs.
static void rescan(void) {
    int keep = g_grid.selected;
    scan();
    uui_grid_set(&g_grid, g_nusable, disk_card, 0);
    uui_grid_select(&g_grid, keep < g_nusable ? keep : g_nusable ? 0 : -1);
    refresh_refused();
}

static void refresh_welcome(void) {
    struct facts *f = &g_welcome_facts;
    f->n = 0;
    char size[16], need[16], carried[128];
    human_size_iec(size, sizeof size, g_bytes);
    human_size_iec(need, sizeof need, g_need + UINSTALL_ESP_DEFAULT * 1024ull * 1024);
    fact(f, "Installs", "%s", g_version);
    fact(f, "Copies", "%u files, %s", (unsigned)g_files, size);
    fact(f, "Needs", "%s of disk, which is erased", need);
    carried_text(carried, sizeof carried);
    if (carried[0]) fact(f, "Carried over", "%s", carried);
    else fact(f, "Carried over", "nothing yet -- setup asks on the first start");
    card_fit(g_welcome_card, f);
}

static void refresh_ready(void) {
    struct facts *f = &g_ready_facts;
    f->n = 0;
    const struct udisk *d = chosen();
    if (!d) { card_fit(g_ready_card, f); return; }
    char size[16], lost[128] = "", root[16], total[16], carried[128];
    human_size_iec(size, sizeof size, d->sectors * 512);
    fact(f, "Disk", "%s, %s (%s)", d->name, size, d->model[0] ? d->model : d->driver);
    for (int k = 0; k < d->nparts; k++) {
        char t[40], one[64], ps[16];
        udisk_part_title(&d->parts[k], t, sizeof t);
        human_size_iec(ps, sizeof ps, d->parts[k].count * 512);
        snprintf(one, sizeof one, "%s%s %s", lost[0] ? ", " : "", t, ps);
        strlcat(lost, one, sizeof lost);
    }
    fact(f, "Erases", "%s", lost[0] ? lost : "nothing -- the disk is empty");
    human_size_iec(root, sizeof root, g_plan.part[g_plan.root].sectors * 512);
    fact(f, "Layout", "%s, /boot %llu MiB, root %s", g_plan.mbr ? "MBR" : "GPT",
         (unsigned long long)g_plan.esp_mib, root);
    human_size_iec(total, sizeof total, g_bytes);
    fact(f, "Copies", "%u files, %s", (unsigned)g_files, total);
    char sw[128];
    uinstall_software_describe(&g_sw, sw, sizeof sw);
    fact(f, "Software", "%s", sw);
    ulogf("installer: ready: software %s\n", sw);
    carried_text(carried, sizeof carried);
    fact(f, "Carried over", "%s", carried[0] ? carried : "nothing -- setup asks everything");
    card_fit(g_ready_card, f);
}

static void refresh_done(void) {
    struct facts *f = &g_done_facts;
    f->n = 0;
    char took[16], total[16], root[16];
    uduration_clock(g_took_ms, took, sizeof took);
    if (g_prog.ok) {
        snprintf(g_done_title, sizeof g_done_title, "SaunaOS is installed on %s", g_plan.disk);
        const char *asks = g_carry && !g_askall->checked ? "only what this session did not set"
                                                        : "how to set it up";
        if (g_live)
            snprintf(g_done_text, sizeof g_done_text,
                     "Remove the USB stick or disc, then restart. The computer starts from %s, and "
                     "First-boot setup asks %s.", g_plan.disk, asks);
        else
            snprintf(g_done_text, sizeof g_done_text,
                     "%s starts SaunaOS on its own now; this computer still starts from the disk it "
                     "did. On %s, First-boot setup asks %s.", g_plan.disk, g_plan.disk, asks);
        human_size_iec(total, sizeof total, g_bytes);
        human_size_iec(root, sizeof root, g_plan.part[g_plan.root].sectors * 512);
        fact(f, "Disk", "%s", g_plan.disk);
        fact(f, "Layout", "%s, root %s", g_plan.mbr ? "MBR" : "GPT", root);
        fact(f, "Copied", "%u files, %s", (unsigned)g_files, total);
        fact(f, "Took", "%s", took);
    } else {
        snprintf(g_done_title, sizeof g_done_title, "The install did not finish");
        snprintf(g_done_text, sizeof g_done_text,
                 "%s is in an unknown state and will not start SaunaOS. Go back to try again, "
                 "perhaps with the other partition table.", g_plan.disk);
        fact(f, "Stopped at", "%s", uinstall_stage_name(g_prog.stage < UINSTALL_STAGES ? g_prog.stage
                                                                                      : UINSTALL_BOOTLOADER));
        fact(f, "Because", "%s", g_prog.error[0] ? g_prog.error : "no reason was given");
        if (g_prog.errors > 1) fact(f, "Errors", "%d in all", g_prog.errors);
    }
    card_fit(g_done_card, f);
    w_done_title->text = g_done_title;
    w_done_text->text = g_done_text;
}

// --- the run ------------------------------------------------------------------

static char g_stage_detail[UINSTALL_STAGES][128];

static void on_stage(void *ctx, int s, const char *detail) {
    (void)ctx;
    if (s >= 0 && s < UINSTALL_STAGES) snprintf(g_stage_detail[s], sizeof g_stage_detail[s], "%s", detail);
    ulogf("installer: [%d/%d] %s -- %s\n", s + 1, UINSTALL_STAGES, uinstall_stage_name(s), detail);
}

static void on_line(void *ctx, const char *line) {
    (void)ctx;
    ulogf("installer:   %s\n", line);
}

static const struct uinstall_ops OPS = { 0, on_stage, on_line };

static void *worker(void *arg) {
    (void)arg;
    uinstall_run(&g_plan, g_askall->checked ? UINSTALL_ASK_ALL : 0, &g_sw, &g_prog, &OPS);
    return 0;
}

static void start_install(void) {
    memset(&g_prog, 0, sizeof g_prog);
    memset(g_stage_detail, 0, sizeof g_stage_detail);
    g_prog.files_total = g_files;
    g_prog.bytes_total = g_bytes;
    uui_stages_set(&g_stages, 0, -1);
    snprintf(g_stages.line, sizeof g_stages.line, "%s", "Starting");
    ulogf("installer: installing onto %s (%s, ESP %llu MiB)%s\n", g_plan.disk, g_plan.mbr ? "MBR" : "GPT",
          (unsigned long long)g_plan.esp_mib, g_askall->checked ? ", setup asks everything" : "");
    g_running = 1;
    g_t0 = sys_monotonic_ns();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, worker, 0) != 0) {
        // Still "running": the next tick reports it as a finished failure.
        g_prog.finished = 1;
        g_prog.ok = 0;
        snprintf(g_prog.error, sizeof g_prog.error, "could not start the install's thread");
    }
}

// From the tick: copy how far it has got into the stage list.
// ONCE PER RUN: g_running is what says the end has not been reported yet,
// so Took is measured once and Finished entered (and a restart asked
// for) once, not every tick after.
static int poll_install(void) {
    if (!g_running) return 0;
    int s = g_prog.stage;
    for (int i = 0; i < UINSTALL_STAGES; i++)
        snprintf(g_stages.detail[i], sizeof g_stages.detail[i], "%s", i <= s ? g_stage_detail[i] : "");
    if (s == UINSTALL_COPY && g_prog.bytes_total) {
        char a[16], b[16];
        human_size_iec(a, sizeof a, g_prog.bytes_done);
        human_size_iec(b, sizeof b, g_prog.bytes_total);
        snprintf(g_stages.line, sizeof g_stages.line, "%u of %u files, %s of %s",
                 (unsigned)g_prog.files_done, (unsigned)g_prog.files_total, a, b);
        uui_stages_set(&g_stages, s, (int)(g_prog.bytes_done * 1000 / g_prog.bytes_total));
    } else if (s < UINSTALL_STAGES) {
        snprintf(g_stages.line, sizeof g_stages.line, "%s", uinstall_stage_name(s));
        uui_stages_set(&g_stages, s, -1);
    }
    if (!g_prog.finished) {
        uui_stages_tick(&g_stages);
        return 1;
    }
    g_running = 0;
    g_took_ms = (sys_monotonic_ns() - g_t0) / 1000000;
    ulogf("installer: %s in %llu ms%s%s\n", g_prog.ok ? "done" : "FAILED", (unsigned long long)g_took_ms,
          g_prog.ok ? "" : " -- ", g_prog.ok ? "" : g_prog.error);
    if (g_prog.ok) {
        uui_stages_set(&g_stages, UINSTALL_STAGES, 1000);
        g_stages.line[0] = 0;
    }
    refresh_done();
    // Past the copy there is no going back to Ready; a failure may.
    uui_wizard_hold(g_wz, 0, 0, 0);
    uui_wizard_go(g_wz, S_DONE);
    uui_wizard_hold(g_wz, g_prog.ok, !g_prog.ok, 0);
    if (g_prog.ok && g_restart->checked) {
        ulogf("installer: restarting, as asked\n");
        uinitctl_shutdown(1);
    }
    return 1;
}

// --- the confirm ----------------------------------------------------------------

static char g_ask_rows[UDISK_PARTS_MAX + 2][96];
static const char *g_ask_ptrs[UDISK_PARTS_MAX + 2];

static void ask_erase(void) {
    const struct udisk *d = chosen();
    if (!d) return;
    char size[16];
    human_size_iec(size, sizeof size, d->sectors * 512);
    int n = 0;
    snprintf(g_ask_rows[n++], sizeof g_ask_rows[0], "%s, %s.%s", d->model[0] ? d->model : d->name, size,
             d->nparts ? " These are lost:" : " It holds no partitions.");
    for (int k = 0; k < d->nparts && n < UDISK_PARTS_MAX; k++) {
        const struct udisk_part *p = &d->parts[k];
        char t[40], ps[16];
        udisk_part_title(p, t, sizeof t);
        human_size_iec(ps, sizeof ps, p->count * 512);
        snprintf(g_ask_rows[n++], sizeof g_ask_rows[0], "    %s -- %s%s%s, %s", p->dev[0] ? p->dev : "-", t,
                 udisk_part_fs(p)[0] ? ", " : "", udisk_part_fs(p), ps);
    }
    snprintf(g_ask_rows[n++], sizeof g_ask_rows[0], "This can't be undone.");
    for (int i = 0; i < n; i++) g_ask_ptrs[i] = g_ask_rows[i];
    static const struct uui_dialog_button btns[] = {
        { "Cancel", ASK_CANCEL, UUI_DLG_PLAIN },
        { "Erase and install", ASK_ERASE, UUI_DLG_DANGER },
    };
    static char title[64];
    snprintf(title, sizeof title, "Erase %s and install SaunaOS?", d->name);
    uui_dialog_set_bounds(g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    // Cancel is the default: Return must never erase a disk.
    uui_dialog_open(g_ask, title, g_ask_ptrs, n, btns, 2, 0, ASK_CANCEL);
    uapp_redraw(g_app);
}

static void say(const char *title, const char *line) {
    static const char *rows[1];
    rows[0] = line;
    static const struct uui_dialog_button ok[] = { { "OK", ASK_OK, UUI_DLG_PRIMARY } };
    uui_dialog_set_bounds(g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(g_ask, title, rows, 1, ok, 1, 0, ASK_OK);
    uapp_redraw(g_app);
}

// --- the focus ring follows the page ------------------------------------------------

static struct uui_focusable g_focusables[10];
static struct uui_focus g_focus;

static void focus_for(int step) {
    int n = 0;
    struct uui_focusable *f = g_focusables;
    switch (step) {
    case S_DISK:
        f[n++] = (struct uui_focusable){ &g_grid, &uui_grid_ops };
        f[n++] = (struct uui_focusable){ g_table, &uui_segmented_ops };
        break;
    case S_SOFTWARE:
        f[n++] = (struct uui_focusable){ &g_checks, &uui_appcards_ops };
        f[n++] = (struct uui_focusable){ g_sw_all, &uui_button_ops };
        f[n++] = (struct uui_focusable){ g_sw_none, &uui_button_ops };
        break;
    case S_READY:
        f[n++] = (struct uui_focusable){ g_askall, &uui_checkbox_ops };
        break;
    case S_INSTALL:
        f[n++] = (struct uui_focusable){ g_restart, &uui_checkbox_ops };
        break;
    case S_DONE:
        f[n++] = (struct uui_focusable){ g_keep, &uui_button_ops };
        f[n++] = (struct uui_focusable){ g_shutdown, &uui_button_ops };
        break;
    }
    // Next first among the buttons, so Tab from the page lands on it.
    const struct uui_widget_ops *ops[3];
    void *w[3];
    int k = uui_wizard_focusables(g_wz, ops, w, 3);
    int order[3] = { 1, 0, 2 };
    for (int j = 0; j < k; j++) {
        int o = order[j] < k ? order[j] : j;
        f[n++] = (struct uui_focusable){ w[o], ops[o] };
    }
    uui_focus_init(&g_focus, g_focusables, n);
    uui_focus_set(&g_focus, 0);
}

// THE CHECKLIST IS AS TALL AS ITS ROWS, until the page is shorter than
// that, and then it scrolls. uui_layout grows a UUI_FILL_H child as well as
// shrinking it, and a full-screen installer would draw five rows in a box
// the height of the screen: so laid out once filling, to learn the room,
// then pinned to its rows when they fit.
static struct uui_item *g_sw_item;   // the wizard's copy of `app #software`

static void lay_out(void) {
    if (!g_app) return;
    int w = uapp_width(g_app), h = uapp_height(g_app);
    if (g_sw_item) {
        g_sw_item->flags |= UUI_FILL_H;
        g_sw_item->main_size = 0;
    }
    uui_layout_run(g_root, 0, 0, w, h);
    if (!g_sw_item) return;
    int nw, nh;
    uui_appcards_ops.natural_size(&g_checks, &nw, &nh);
    if (g_checks.h <= nh) return;
    g_sw_item->flags &= ~UUI_FILL_H;
    g_sw_item->main_size = nh;
    uui_layout_run(g_root, 0, 0, w, h);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a; (void)w; (void)h;
    lay_out();
}

// --- the wizard's callbacks ---------------------------------------------------------

static void on_enter(int step) {
    ulogf("installer: step %d\n", step);
    if (step == S_WELCOME) refresh_welcome();
    if (step == S_DISK) {
        if (g_need != g_need_seen) rescan();   // back from Software, having changed what fits
        refresh_refused();
        refresh_plan();
    }
    if (step == S_SOFTWARE) refresh_software();
    if (step == S_READY) refresh_ready();
    // A fact card's height follows its rows, so the page is laid out again.
    lay_out();
    focus_for(step);
    if (g_app) uapp_redraw(g_app);
}

static int g_confirmed;   // the erase was confirmed: Ready may be left

static int can_leave(int step) {
    if (step == S_DISK) {
        const char *why = refresh_plan();
        if (why) { say("This disk can't be used", why); return 0; }
    }
    // What was ticked may no longer fit the disk chosen a step ago.
    if (step == S_SOFTWARE) {
        const char *why = refresh_plan();
        if (why) { say("That does not fit on this disk", why); return 0; }
    }
    if (step == S_READY) {
        if (!g_confirmed) { ask_erase(); return 0; }
        g_confirmed = 0;
    }
    return 1;
}

static void on_finish(void) {
    ulogf("installer: restarting\n");
    uinitctl_shutdown(1);
}

static void quit(void) {
    if (g_running) return;
    uapp_quit(g_app, 0);
}

// --- uapp -------------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_DIALOG) {
        if (uui_dialog_take_code(g_ask) == ASK_ERASE) {
            g_confirmed = 1;
            uui_wizard_go(g_wz, S_INSTALL);
            if (uui_wizard_step(g_wz) == S_INSTALL) {
                uui_wizard_hold(g_wz, 1, 1, 1);
                start_install();
            }
        }
    } else if (id == ID_DISKS || id == ID_TABLE) {
        if (id == ID_TABLE) rescan();
        refresh_plan();
    } else if (id == ID_SOFTWARE) {
        toggle_software(uui_appcards_take_button(&g_checks));
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    if (code == ID_KEEP) quit();
    else if (code == ID_SHUTDOWN) uinitctl_shutdown(0);
    else if (code == ID_SW_ALL || code == ID_SW_NONE) {
        uinstall_software_all(&g_sw, code == ID_SW_ALL);
        refresh_software();
    } else uui_wizard_action(g_wz, code);
    uapp_redraw(a);
}

// A close while the disk is being written is refused, and says why:
// stopping half way leaves a disk that boots nothing.
static int on_close(struct uapp *a) {
    (void)a;
    if (!g_running) return 1;
    if (!uapp_question_open(a))
        say("The install is still running", "Closing now would leave the disk unusable. It takes a minute or so.");
    return 0;
}

// The geometry a test drives the window by (uapp_log_layout()), and where
// the wizard and the run are: step, stage, finished, ok.
static void on_draw(struct uapp *a, struct uapp_draw *dr) {
    (void)dr;
    uapp_log_layout(a, "installer");
    uapp_logf_layout("installer: layout state %d %d %d %d\n", uui_wizard_step(g_wz), (int)g_prog.stage,
                     (int)g_prog.finished, (int)g_prog.ok);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return poll_install();
}

// --- building it ---------------------------------------------------------------------
// The window is installer.ui's; these join it to the program.

static const struct uui_builder_name NAMES[] = {
    { "disks", ID_DISKS }, { "table", ID_TABLE }, { "plan", ID_PLAN }, { "ask_all", ID_ASKALL },
    { "restart", ID_RESTART }, { "dialog", ID_DIALOG }, { "keep", ID_KEEP }, { "shutdown", ID_SHUTDOWN },
    { "software", ID_SOFTWARE }, { "sw_all", ID_SW_ALL }, { "sw_none", ID_SW_NONE },
};

static int load(struct uui_builder *ui, struct uapp_desc *desc) {
    g_wz = uui_builder_wizard(ui, "main", "wizard", S_COUNT);
    g_ask = uui_builder_widget(ui, "main", "dialog", "dialog");
    w_intro = uui_builder_widget(ui, "main", "intro", "label");
    w_intro_live = uui_builder_widget(ui, "main", "intro_live", "label");
    g_welcome_card = uui_builder_widget(ui, "main", "facts", "custom");
    w_none = uui_builder_widget(ui, "main", "none", "label");
    w_refused = uui_builder_widget(ui, "main", "refused", "label");
    g_table = uui_builder_widget(ui, "main", "table", "segmented");
    g_mbr_at = uui_builder_choice(ui, "main", "table", "mbr");
    uui_builder_choice(ui, "main", "table", "gpt");
    g_ready_card = uui_builder_widget(ui, "main", "summary", "custom");
    g_askall = uui_builder_widget(ui, "main", "ask_all", "checkbox");
    g_restart = uui_builder_widget(ui, "main", "restart", "checkbox");
    w_done_title = uui_builder_widget(ui, "main", "done_title", "label");
    w_done_text = uui_builder_widget(ui, "main", "done_text", "label");
    g_done_card = uui_builder_widget(ui, "main", "done_facts", "custom");
    g_keep = uui_builder_widget(ui, "main", "keep", "button");
    g_shutdown = uui_builder_widget(ui, "main", "shutdown", "button");
    struct uui_item *disks = uui_builder_item(ui, "main", "disks");
    struct uui_item *plan = uui_builder_item(ui, "main", "plan");
    struct uui_item *stages = uui_builder_item(ui, "main", "stages");
    struct uui_item *software = uui_builder_item(ui, "main", "software");
    w_sw_total = uui_builder_widget(ui, "main", "sw_total", "label");
    g_sw_all = uui_builder_widget(ui, "main", "sw_all", "button");
    g_sw_none = uui_builder_widget(ui, "main", "sw_none", "button");
    if (uui_builder_error(ui)) return 0;

    software->ops = &uui_appcards_ops;
    software->widget = &g_checks;
    uui_appcards_init(&g_checks, UUI_APPCARD_CHECKS);
    g_checks.empty = "This build has no optional components: everything goes on the disk.";
    uui_label_set_text(w_sw_total, g_sw_total);
    refresh_software();

    disks->ops = &uui_grid_ops;
    disks->widget = &g_grid;
    // Measured when the window opens, before on_open has the font to size
    // its cards: a zeroed grid divides by its 0 cell width.
    uui_grid_init(&g_grid, 0, 0);
    plan->ops = &uui_diskplan_ops;
    plan->widget = &g_plan_w;
    stages->ops = &uui_stages_ops;
    stages->widget = &g_stages;
    uui_diskplan_init(&g_plan_w);
    uui_stages_init(&g_stages);
    g_stages.bg = UUI_ROLE(window_bg);
    const char *names[UINSTALL_STAGES];
    for (int i = 0; i < UINSTALL_STAGES; i++) names[i] = uinstall_stage_name(i);
    uui_stages_set_names(&g_stages, names, UINSTALL_STAGES);

    // What a live session says, and what an installed system does.
    uui_builder_item(ui, "main", "intro_live")->hidden = !g_live;
    uui_builder_item(ui, "main", "intro")->hidden = g_live;
    if (g_live) {
        g_wz->skip.label = "Quit -- keep trying SaunaOS";
        g_keep->label = "Keep trying SaunaOS";
    }
    // Why no disk is offered, or why some are not: one of the two.
    uui_label_set_text(w_none, g_none_text);
    uui_label_set_text(w_refused, g_refused_text);
    uui_builder_item(ui, "main", "none")->hidden = g_nusable > 0;
    uui_builder_item(ui, "main", "refused")->hidden = !g_nusable;
    uui_label_set_text(w_done_title, g_done_title);
    uui_label_set_text(w_done_text, g_done_text);
    g_askall->bg = g_restart->bg = UUI_ROLE(window_bg);

    g_wz->d.can_leave = can_leave;
    g_wz->d.on_enter = on_enter;
    g_wz->d.on_finish = on_finish;
    g_wz->d.on_skip = quit;
    if (!uui_builder_window(ui, "main", desc)) return 0;
    g_root = desc->layout;
    return 1;
}

static int g_fullscreen;
static void on_size(int *w, int *h) {
    *w = ugfx_char_advance('n') * 116;
    *h = ugfx_char_h() * 36;
}

// EVERY SIZE FROM THE FONT IS SET HERE, not in main(): the session font
// arrives with the window.
static void on_open(struct uapp *a) {
    g_app = a;
    int cw = ugfx_char_advance('n'), lh = ugfx_char_h();
    *g_welcome_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts,
                                           .state = &g_welcome_facts };
    *g_ready_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts,
                                         .state = &g_ready_facts };
    *g_done_card = (struct uui_custom){ .w = cw * 70, .h = fact_row_h(), .draw = draw_facts,
                                        .state = &g_done_facts };
    uui_grid_init(&g_grid, cw * 60, lh * 3);
    g_grid.bg = UUI_ROLE(panel_bg);
    uui_grid_set(&g_grid, g_nusable, disk_card, 0);
    uui_grid_select(&g_grid, g_nusable ? 0 : -1);
    struct uui_item *disks = 0;
    for (int i = 0; i < g_wz->d.count; i++) {
        struct uui_layout *page = uui_wizard_page(g_wz, i)->widget;
        for (int k = 0; k < page->count; k++) {
            if (page->items[k].widget == &g_grid) disks = &page->items[k];
            if (page->items[k].widget == &g_checks) g_sw_item = &page->items[k];
        }
    }
    if (disks) disks->main_size = lh * 3 * (g_nusable > 2 ? 2 : g_nusable ? g_nusable : 1) + 2;
    w_done_title->font = ugfx_font_display();
    refresh_software();
    lay_out();
    if (g_fullscreen) uapp_set_fullscreen(a, 1);
    on_enter(uui_wizard_step(g_wz));
}

// --- at boot --------------------------------------------------------------------------

// A live session's two ways in: an icon on the desktop, and a notice that
// says what a live session is (WIN_NOTICE_LIVE, whose action opens this).
// The launcher lives in the RAM image, so it is gone with the session,
// and an install leaves it behind (UINSTALL_LAUNCHER). installer=on gets
// the launcher too but no notice: the window is the way back in after
// "Keep trying SaunaOS", and the notice would only be dropped again.
static void offer(int notice) {
    static const char ENTRY[] =
        "# Written by `installer --boot` in a live session (docs/commands/install.md).\n"
        "Name=Install SaunaOS\n"
        "Exec=" SELF "\n"
        "Category=system\n"
        "Comment=Put this system on a disk in this computer\n"
        "Icon=installer\n";
    char launcher[128];
    snprintf(launcher, sizeof launcher, "%s/" UINSTALL_LAUNCHER, udir_path(UDIR_DESKTOP));
    struct sys_stat st;
    if (sys_stat(launcher, &st) != 0) {
        int fd = open(launcher, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (write(fd, ENTRY, sizeof ENTRY - 1) != (long)sizeof ENTRY - 1)
                ulogf("installer: could not write %s\n", launcher);
            close(fd);
        }
    }
    int sent = notice && uapp_notice(0, WIN_NOTICE_LIVE, 0, "live");
    ulogf("installer: live session -- launcher at %s, notice %s\n", launcher,
          sent ? "sent" : notice ? "not sent" : "not asked for");
}

int main(int argc, char **argv) {
    int boot = argc > 1 && !strcmp(argv[1], "--boot");
    if (boot) {
        int word = uinstall_boot_word();
        if (word == 2) return 0;                         // installer=off: nothing at all
        if (word != 1) {
            if (udisk_live_root()) offer(1);
            return 0;
        }
        if (udisk_live_root()) offer(0);
        g_fullscreen = 1;                                // installer=on: this window, now
    }
    if (argc > 1 && !strcmp(argv[1], "--fullscreen")) g_fullscreen = 1;
    g_live = udisk_live_root();

    struct query_version kv;
    if (sys_query_record(QUERY_VERSION, 0, &kv, sizeof kv) >= (int)sizeof kv)
        snprintf(g_version, sizeof g_version, "SaunaOS %s", kv.version);
    g_carry = ufbs_stored();
    uinstall_software_load(&g_sw);
    if (uinstall_measure(&g_sw, &g_files, &g_bytes) == 0) g_need = uinstall_root_need(g_bytes);
    scan();

    struct uui_builder *ui = uui_builder_load("installer", NAMES, (int)(sizeof NAMES / sizeof NAMES[0]));
    if (!ui) return 1;
    struct uapp_desc desc = {
        .on_size      = on_size,
        .focus        = &g_focus,
        .flags        = UAPP_SINGLE_INSTANCE,
        .tick_ms      = 250,
        .on_open      = on_open,
        .on_widget    = on_widget,
        .on_action    = on_action,
        .on_close     = on_close,
        .on_tick      = on_tick,
        .on_draw      = on_draw,
        .on_resize    = on_resize,
    };
    if (!load(ui, &desc)) return uui_builder_run_error(ui);
    return uapp_run(&desc);
}
