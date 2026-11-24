// Task Manager, as a RING-3 PROCESS: a navigation rail of three pages --
// Processes, Performance, Services -- which is Windows 11's Task Manager
// shape (22H2 moved it from tabs to a rail) with KDE System Monitor's
// tree and details pane on the Processes page.
//
// THIS FILE IS THE SHELL: the rail, the window, and the PROCESS MODEL
// every page reads (tm_internal.h). The pages are userland/apps/system/taskmgr/,
// one file each, and share nothing with each other but what is declared
// there -- the File Manager's and System Settings' arrangement.
//
// A process is listed ONCE, its threads folded in (tgid != pid), which
// is the unit both Windows and KDE list. It is grouped the way Windows
// groups it: an APP is a program with a desktop entry, or anything
// started from one (a Terminal's shell) or from the desktop; SYSTEM is
// init and what the kernel spawned (ppid 0); everything else is a
// BACKGROUND process. Windows decides "app" by a visible window, which
// no ABI here reports (QUERY_WINDOWS was retired); a desktop entry is
// what the Start menu and taskbar already use to name a program.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "base/ulog.h"
#include "ui/ugfx.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_layout.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_menubar.h"
#include "ui/uui_focus.h"
#include "ui/uui_anim.h"      // uui_anim_ms(): desktop.animations
#include "ui/uui_builder.h"   // the menu bar, from taskmgr.ui
#include "keyboard.h"
#include "base/uconf.h"
#include "base/uopen.h"
#include "base/uappentry.h"
#include "base/ugrow.h"
#include "etc_config.h"
#include "kpath.h"
#include "query_abi.h"
#include "apps/system/taskmgr/tm_internal.h"

// --- the process model -------------------------------------------------

struct tm_proc *g_proc;
int g_proc_cap;
int g_nproc;
int g_desktop_pid;
unsigned g_cpu_pm;
unsigned long long g_mem_used, g_mem_total;
struct umem_comp g_mem_comp;
int g_threads;
struct uprocio g_io;
struct uprocio_rate g_kernel_io;
struct tm_core *g_core;
int g_ncore;
int g_ncore_sched = 1;
static int g_core_cap;

// Last tick's CPU totals, BY PID: rows move as processes come and go, so
// indexing by row would charge one process's time to whichever landed
// in its slot -- nonsense exactly when the table is busiest.
static struct prev { int pid; unsigned long long cpu; } *g_prev;
static int g_nprev;
static unsigned long long g_prev_ns;
static unsigned long long g_load_proc, g_load_kernel;

static struct proc_info *g_info;

int tm_proc_row(int pid) {
    for (int i = 0; i < g_nproc; i++)
        if (g_proc[i].pid == pid) return i;
    return -1;
}

const char *tm_status_text(const struct tm_proc *p) {
    switch (p->state) {
    case PROC_STATE_RUNNING:
    case PROC_STATE_READY:   return "Running";
    case PROC_STATE_STOPPED: return "Stopped";
    case PROC_STATE_ZOMBIE:  return "Exited";
    case PROC_STATE_BLOCKED: break;
    default:                 return "-";
    }
    // Plain words for WHY it waits -- Linux's WCHAN, which Task Manager
    // on Windows cannot show at all.
    switch (p->wait) {
    case PROC_WAIT_EVENT:  return "Waiting for input";
    case PROC_WAIT_PIPE:   return "Reading a pipe";
    case PROC_WAIT_CHILD:  return "Waiting for a child";
    case PROC_WAIT_TIMER:  return "Sleeping";
    case PROC_WAIT_KEY:    return "Waiting for a key";
    case PROC_WAIT_TTY:    return "Writing a terminal";
    case PROC_WAIT_THREAD: return "Joining a thread";
    case PROC_WAIT_NET:    return "Waiting for network";
    case PROC_WAIT_FUTEX:  return "Waiting";
    case PROC_WAIT_SIGNAL: return "Waiting for a signal";
    case PROC_WAIT_LOCK:   return "Waiting for a lock";
    case PROC_WAIT_DISK:   return "Waiting for the disk";
    default:               return "Waiting";
    }
}

// --- desktop entries -----------------------------------------------------

// Every desktop entry, GROWN ON DEMAND (base/ugrow.h): it held 48.
struct tm_app *g_apps;
int g_napps;
static int g_apps_cap;

// Read ONCE at start: a program installed while Task Manager is open is
// shown under its process name until the next start, which is the
// cheaper wrong answer than a directory walk every tick.
static int add_app(const struct uappentry *e, void *ctx) {
    (void)ctx;
    if (!UGROW(g_apps, g_apps_cap, g_napps + 1)) {
        ulogf("taskmgr: out of memory at %d desktop entries\n", g_napps);
        return 1;   // stop the walk: the rest show under their process names
    }
    struct tm_app *a = &g_apps[g_napps++];
    strlcpy(a->exec, e->exec, sizeof a->exec);
    strlcpy(a->name, e->name, sizeof a->name);
    strlcpy(a->icon, e->icon, sizeof a->icon);
    return 0;
}

// EVERY entry, whatever the GPL switch hides: it names what RUNS.
static void load_apps(void) {
    uappentry_each_all(add_app, 0);
    ulogf("taskmgr: names from %d desktop entries\n", g_napps);
}

static int app_for_path(const char *path) {
    if (!path[0]) return -1;
    for (int i = 0; i < g_napps; i++)
        if (strcmp(g_apps[i].exec, path) == 0) return i;
    return -1;
}

static unsigned long long prev_cpu(int pid) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].pid == pid) return g_prev[i].cpu;
    return 0;   // new since the last tick: all its time is this tick's
}

static void classify(void) {
    g_desktop_pid = 0;
    for (int i = 0; i < g_nproc; i++)
        if (strcmp(g_proc[i].name, "toywm") == 0) g_desktop_pid = g_proc[i].pid;
    for (int i = 0; i < g_nproc; i++) {
        struct tm_proc *p = &g_proc[i];
        p->app = app_for_path(p->path);
        strlcpy(p->title, p->app >= 0 ? g_apps[p->app].name : p->name, sizeof p->title);
    }
    for (int i = 0; i < g_nproc; i++) {
        struct tm_proc *p = &g_proc[i];
        if (p->pid == 1 || p->ppid == 0) { p->group = TM_GROUP_SYSTEM; continue; }
        p->group = TM_GROUP_BACKGROUND;
        if (p->app >= 0) { p->group = TM_GROUP_APPS; continue; }
        // Started by an app, or by the desktop: an app's too. Bounded,
        // since a parent chain is data from the kernel, not a promise.
        int x = p->ppid;
        for (int steps = 0; x > 1 && steps < 8; steps++) {
            if (x == g_desktop_pid) { p->group = TM_GROUP_APPS; break; }
            int r = tm_proc_row(x);
            if (r < 0) break;
            if (g_proc[r].app >= 0) { p->group = TM_GROUP_APPS; break; }
            x = g_proc[r].ppid;
        }
    }
}

// Each core's load since the last read, matched by the kernel's core
// number: the record index would move if a core in the middle went away.
// MATCHED AGAINST A COPY of the last read: g_core is rewritten in place,
// so a core appearing mid-list would push the rest onto slots already
// overwritten -- their last samples lost, or another core's taken.
static struct tm_core *g_core_was;
static int g_core_was_cap;

static void read_cores(void) {
    struct query_coreload r;
    int n = 0, sched = 0, nwas = 0;
    if (g_ncore > 0 && UGROW(g_core_was, g_core_was_cap, g_ncore)) {
        memcpy(g_core_was, g_core, (size_t)g_ncore * sizeof *g_core);
        nwas = g_ncore;
    }   // else no copy: every core starts again from this read, at 0%
    for (int i = 0; sys_query_record(QUERY_CORELOAD, i, &r, sizeof r) >= (int)sizeof r; i++) {
        if (!UGROW(g_core, g_core_cap, n + 1)) break;   // keeps the cores it has
        struct tm_core was = { 0 };
        int seen = 0;
        for (int j = 0; j < nwas; j++)
            if (g_core_was[j].index == r.index) { was = g_core_was[j]; seen = 1; break; }
        struct tm_core *c = &g_core[n++];
        c->index = (unsigned)r.index;
        c->apic_id = (unsigned)r.apic_id;
        c->scheduling = (r.flags & QUERY_CPU_SCHEDULING) != 0;
        c->pm = 0;
        if (seen && r.now_ns > was.now_ns && r.busy_ns >= was.busy_ns) {
            unsigned long long pm = (r.busy_ns - was.busy_ns) * 1000ULL / (r.now_ns - was.now_ns);
            c->pm = pm > 1000 ? 1000 : (unsigned)pm;
        }
        c->busy_ns = r.busy_ns;
        c->now_ns = r.now_ns;
        sched += c->scheduling;
    }
    g_ncore = n;
    g_ncore_sched = sched > 0 ? sched : 1;   // a kernel without QUERY_CORELOAD: one
}

static void refresh_model(void) {
    unsigned long long now = sys_monotonic_ns();
    unsigned long long elapsed = now - g_prev_ns;
    read_cores();

    int ninfo = 0;
    for (int i = 0; ninfo < g_proc_cap && sys_proc_info(i, &g_info[ninfo]) == 0; i++) {
        if (g_info[ninfo].pid == 0) continue;   // an empty slot: skip, do not stop
        ninfo++;
    }
    g_threads = ninfo;

    int n = 0;
    for (int i = 0; i < ninfo; i++) {
        const struct proc_info *in = &g_info[i];
        if (in->tgid && in->tgid != in->pid) continue;   // a thread: below
        struct tm_proc *p = &g_proc[n++];
        memset(p, 0, sizeof *p);
        p->pid = in->pid;
        p->ppid = in->ppid;
        p->pgid = in->pgid;
        p->uid = in->euid;
        p->state = in->state;
        p->wait = in->wait_reason;
        p->threads = 1;
        p->cpu_ns = in->cpu_ns;
        strlcpy(p->name, in->name, sizeof p->name);
    }
    g_nproc = n;
    for (int i = 0; i < ninfo; i++) {
        const struct proc_info *in = &g_info[i];
        if (!in->tgid || in->tgid == in->pid) continue;
        int r = tm_proc_row(in->tgid);
        if (r < 0) continue;
        g_proc[r].cpu_ns += in->cpu_ns;
        g_proc[r].threads++;
        // A process is running while ANY of its threads is.
        if (in->state == PROC_STATE_RUNNING || in->state == PROC_STATE_READY)
            g_proc[r].state = in->state;
    }

    for (int i = 0; i < n; i++) {
        struct tm_proc *p = &g_proc[i];
        unsigned long long was = prev_cpu(p->pid);
        unsigned long long used = p->cpu_ns > was ? p->cpu_ns - was : 0;
        // A SHARE OF THE WHOLE MACHINE: one busy thread on four cores is
        // 25%, as Utilization says, not 100% beside a 25% header.
        unsigned long long span = elapsed * (unsigned long long)g_ncore_sched;
        p->cpu_pm = span ? (unsigned)(used * 1000ULL / span) : 0;
        if (p->cpu_pm > 1000) p->cpu_pm = 1000;   // a slot reused mid-interval
    }
    g_nprev = n;
    for (int i = 0; i < n; i++) { g_prev[i].pid = g_proc[i].pid; g_prev[i].cpu = g_proc[i].cpu_ns; }
    g_prev_ns = now;

    struct query_procpath pp;
    QUERY_FOREACH(QUERY_PROCPATH, pp, qi) {
        int r = tm_proc_row(pp.pid);
        if (r >= 0) strlcpy(g_proc[r].path, pp.path, sizeof g_proc[r].path);
    }
    // Memory per process: a page-table walk each, so its own fact.
    unsigned long long apps = 0;
    struct query_procmem pm;
    QUERY_FOREACH(QUERY_PROCMEM, pm, mi_) {
        apps += pm.private_bytes;
        int r = tm_proc_row(pm.pid);
        if (r >= 0) { g_proc[r].private_bytes = pm.private_bytes; g_proc[r].shared_bytes = pm.shared_bytes; }
    }
    // I/O per process: two reads of QUERY_PROCIO a refresh apart.
    if (uprocio_update(&g_io) >= 0) {
        for (int i = 0; i < n; i++)
            if (!uprocio_rate(&g_io, g_proc[i].pid, &g_proc[i].io)) memset(&g_proc[i].io, 0, sizeof g_proc[i].io);
        if (!uprocio_rate(&g_io, 0, &g_kernel_io)) memset(&g_kernel_io, 0, sizeof g_kernel_io);
    }
    classify();

    // The machine: the CPU against the kernel's own idle, as the Overview
    // rings measured it; memory in frames.
    struct query_cpuload cl;
    if (sys_query_record(QUERY_CPULOAD, 0, &cl, sizeof cl) >= (int)sizeof cl) {
        unsigned long long dp = cl.proc_ns - g_load_proc, dk = cl.kernel_ns - g_load_kernel;
        if (g_load_proc && dp + dk) g_cpu_pm = (unsigned)(dp * 1000ULL / (dp + dk));
        if (g_cpu_pm > 1000) g_cpu_pm = 1000;
        g_load_proc = cl.proc_ns;
        g_load_kernel = cl.kernel_ns;
    }
    struct query_meminfo mi;
    if (sys_query_record(QUERY_MEMINFO, 0, &mi, sizeof mi) >= (int)sizeof mi) {
        g_mem_total = mi.frame_total * mi.frame_bytes;
        unsigned long long freeb = mi.frame_free * mi.frame_bytes;
        g_mem_used = g_mem_total > freeb ? g_mem_total - freeb : 0;
        umem_comp_from(&mi, apps, &g_mem_comp);
    }
}

// --- the shell ----------------------------------------------------------

enum { PAGE_PROCS, PAGE_PERF, PAGE_SERVICES, PAGE_COUNT };
static struct tm_page g_pages[PAGE_COUNT];
static int g_page;

static struct uui_sidebar_row g_rail_rows[PAGE_COUNT];
static struct uui_sidebar g_rail;

// The one context menu, a second uui_menubar with no bar of its own
// (docs/conventions/gui.md). The Processes and Services pages fill it.
struct uui_menubar g_ctx;

// The menu bar, taskmgr.ui's: File, and View with Refresh now and
// Update speed -- Windows 10's Task Manager's View menu.
static struct uui_menubar g_menu;
enum { CMD_EXIT = 1, CMD_REFRESH, CMD_SPEED,      // CMD_SPEED + TM_SPEED_*
       CMD_SHOW = CMD_SPEED + TM_SPEEDS };            // CMD_SHOW + TM_SHOW_*
static const struct uui_builder_name MENU_NAMES[] = {
    { "exit", CMD_EXIT }, { "refresh", CMD_REFRESH },
    { "speed_high", CMD_SPEED + TM_SPEED_HIGH }, { "speed_normal", CMD_SPEED + TM_SPEED_NORMAL },
    { "speed_low", CMD_SPEED + TM_SPEED_LOW }, { "speed_paused", CMD_SPEED + TM_SPEED_PAUSED },
    { "show_all", CMD_SHOW + TM_SHOW_ALL }, { "show_mine", CMD_SHOW + TM_SHOW_MINE },
    { "show_system", CMD_SHOW + TM_SHOW_SYSTEM },
};

// The window is a column -- the menu bar over BODY -- and BODY the row
// the window was before it: the rail and the page. Routed: the bar, BODY
// and the context menu, LAST so it is hit-tested first -- its popup
// covers whatever is under it.
static struct uui_item ITEMS[2];        // BODY's: the rail, the page
static struct uui_layout BODY;
static struct uui_item TOP_ITEMS[3];    // the bar, BODY, the context menu
static struct uui_layout LAYOUT;

#define FOCUS_MAX 12
static struct uui_focusable g_focusables[FOCUS_MAX];
static struct uui_focus g_focus;

static struct uapp *g_app;

// Moves keyboard focus to `widget` if it is a stop on the ring.
void tm_focus(void *widget) {
    for (int i = 0; i < g_focus.count; i++)
        if (g_focus.items[i].widget == widget) { uui_focus_set(&g_focus, i); return; }
}

// A context menu has no key slot of its own (uui_menubar_ops), and the
// focus ring is offered every key before on_key -- so while a page's menu
// is open the ring is PARKED, and the arrows and Enter reach the menu
// through on_key instead of moving the table underneath it.
int tm_focus_park(void) {
    int was = g_focus.current;
    uui_focus_set(&g_focus, -1);
    return was;
}

void tm_focus_restore(int index) { uui_focus_set(&g_focus, index); }

void tm_relayout(void) {
    if (g_app) uui_layout_run(&LAYOUT, 0, 0, uapp_width(g_app), uapp_height(g_app));
}

// --- the update speed, and the clock that samples and glides -----------
//
// ONE TIMER, re-armed at every wake for the NEXT thing due: a sample, on
// a fixed schedule of the update speed's interval; or, while a graph
// glides, the moment its trace has moved a whole pixel (uui_chart.h).
// So the process wakes exactly as often as something changes, and not at
// a frame rate: about six times a second for the CPU chart at High in a
// default window, twice a second when nothing on screen glides.

static const struct { const char *word; unsigned ms; } SPEEDS[TM_SPEEDS] = {
    [TM_SPEED_HIGH] = { "high", TM_REFRESH_MS }, [TM_SPEED_NORMAL] = { "normal", 1000 },
    [TM_SPEED_LOW] = { "low", 4000 }, [TM_SPEED_PAUSED] = { "paused", 0 },
};
#define TM_PAUSED_MS 60000   // nothing to do: a long wait, since 0 is no timer at all

static int g_speed = TM_SPEED_HIGH;
static unsigned g_run_ms = TM_REFRESH_MS;   // the last speed that samples
static unsigned long long g_next_sample_ns;
static int g_animate = 1;   // desktop.animations
static int g_glide_live = -1;

unsigned tm_interval_ms(void) { return g_run_ms; }

static int glide_on(void) { return g_animate && g_speed != TM_SPEED_PAUSED; }

// Only what can be SEEN glides: the Performance page, in a window that
// is not minimized, on a desktop that has the screen (WIN_EV_SUSPENDED).
static int glide_live(struct uapp *a) {
    return glide_on() && g_page == PAGE_PERF && a && !uapp_suspended(a);
}

static void arm(struct uapp *a) {
    if (!a) return;
    unsigned long long now = sys_monotonic_ns();
    long long wait = -1;
    if (g_speed != TM_SPEED_PAUSED)
        wait = g_next_sample_ns > now ? (long long)((g_next_sample_ns - now + 999999ull) / 1000000ull) : 1;
    int live = glide_live(a);
    if (live != g_glide_live) {
        g_glide_live = live;
        ulogf("taskmgr: glide %s\n", live ? "on" : "off");
    }
    if (live) {
        int g = tm_perf_glide_next_ms(now);
        if (g >= 0 && (wait < 0 || g < wait)) wait = g;
    }
    uapp_set_tick(a, wait < 0 ? TM_PAUSED_MS : wait < 1 ? 1u : (unsigned)wait);
}

// Everything re-read, every page told. History is collected whether or
// not anybody is looking at it, so a page opened after a spike still
// shows the spike.
static void sample(struct uapp *a) {
    static unsigned long long services_ns;
    unsigned long long now = sys_monotonic_ns();
    refresh_model();
    if (now - services_ns >= 2000000000ull) {   // two seconds; it is a file
        services_ns = now;
        tm_services_read();
    }
    tm_perf_sample();
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_pages[i].tick) g_pages[i].tick(a, i == g_page);
    uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    // The glide starts again from the new samples (and files its count);
    // the whole frame is repainted anyway.
    tm_perf_glide(0, now);
}

static void set_speed(struct uapp *a, int sp, int save) {
    if (sp < 0 || sp >= TM_SPEEDS) return;
    g_speed = sp;
    if (SPEEDS[sp].ms) g_run_ms = SPEEDS[sp].ms;
    tm_perf_set_interval(g_run_ms);
    tm_procs_set_interval(g_run_ms);
    // Paused shows the newest sample at the edge: a frozen glide would
    // keep it out of sight.
    tm_perf_set_smooth(glide_on());
    g_next_sample_ns = sys_monotonic_ns() + (unsigned long long)g_run_ms * 1000000ull;
    if (save && !uconf_set(TM_CONF, "update_speed", SPEEDS[sp].word))
        ulogf("taskmgr: could not save the update speed to %s\n", TM_CONF);
    ulogf("taskmgr: update speed %s, interval %u ms\n", SPEEDS[sp].word, SPEEDS[sp].ms);
    arm(a);
    if (a) uapp_redraw(a);
}

static void load_speed(void) {
    char w[16];
    if (!uconf_get(TM_CONF, "update_speed", w, sizeof w)) return;
    for (int i = 0; i < TM_SPEEDS; i++)
        if (!strcmp(w, SPEEDS[i].word)) {
            g_speed = i;
            if (SPEEDS[i].ms) g_run_ms = SPEEDS[i].ms;
        }
}

static void refresh_now(struct uapp *a) {
    sample(a);
    g_next_sample_ns = sys_monotonic_ns() + (unsigned long long)g_run_ms * 1000000ull;
    ulogf("taskmgr: refreshed\n");
    arm(a);
    uapp_redraw(a);
}

static unsigned menu_flags(int code) {
    if (code >= CMD_SPEED && code < CMD_SPEED + TM_SPEEDS)
        return code - CMD_SPEED == g_speed ? UUI_MI_CHECKED : 0;
    if (code >= CMD_SHOW && code < CMD_SHOW + TM_SHOWS)
        return code - CMD_SHOW == tm_procs_show() ? UUI_MI_CHECKED : 0;
    return 0;   // Exit and Refresh now always act (Refresh now samples even Paused)
}

static void command(struct uapp *a, int code) {
    if (code == CMD_EXIT) { uapp_quit(a, 0); return; }
    if (code == CMD_REFRESH) { refresh_now(a); return; }
    if (code >= CMD_SPEED && code < CMD_SPEED + TM_SPEEDS) set_speed(a, code - CMD_SPEED, 1);
    if (code >= CMD_SHOW && code < CMD_SHOW + TM_SHOWS) {
        tm_procs_set_show(code - CMD_SHOW, 1);
        uapp_redraw(a);
    }
}

static void select_page(struct uapp *a, int index) {
    if (index < 0 || index >= PAGE_COUNT) return;
    g_page = index;
    const struct tm_page *p = &g_pages[index];
    ITEMS[1].ops = p->root->ops;
    ITEMS[1].widget = p->root->widget;
    ITEMS[1].name = p->root->name;

    // The ring: the rail, then the page's own stops.
    g_focusables[0] = (struct uui_focusable){ &g_rail, &uui_sidebar_ops };
    int n = 1 + (p->focusables ? p->focusables(g_focusables + 1, FOCUS_MAX - 1) : 0);
    uui_focus_init(&g_focus, g_focusables, n);
    uui_focus_set(&g_focus, n > 1 ? 1 : 0);

    if (p->open) p->open(a);
    if (a) uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    arm(a);   // the Performance page glides; the others need only samples
    ulogf("taskmgr: page %d\n", index);
}

void tm_show_page(struct uapp *a, int index) {
    uui_sidebar_select_id(&g_rail, index);
    select_page(a, index);
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (id == TM_ID_MENU) {
        int code = uui_menubar_take_code(&g_menu);   // parked in the widget
        if (code > 0) command(a, code);
        uapp_redraw(a);
        return;
    }
    if (id == TM_ID_RAIL) {
        int want = uui_sidebar_selected_id(&g_rail);
        if (want != g_page) select_page(a, want);
        uapp_redraw(a);
        return;
    }
    const struct tm_page *p = &g_pages[g_page];
    if (p->widget && p->widget(a, id, reason)) uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->action && p->action(a, code)) uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_menubar_key(&g_menu, key, &code)) {   // F10, and the bar's keys while open
        if (code > 0) command(a, code);
        uapp_redraw(a);
        return;
    }
    if (key == KEY_F5) { refresh_now(a); return; }
    const struct tm_page *p = &g_pages[g_page];
    if (p->key && p->key(a, key, mods)) uapp_redraw(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->press) p->press(a, x, y, buttons);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    const struct tm_page *p = &g_pages[g_page];
    if (p->release) p->release(a, x, y, buttons);
}

// A sample when one is due (on a fixed schedule: a late wake does not
// push the next one back), a glide step when one is, then the timer for
// whichever comes next. Repaints only when either changed something.
static int on_tick(struct uapp *a) {
    unsigned long long now = sys_monotonic_ns();
    int repaint = 0;
    if (g_speed != TM_SPEED_PAUSED && now + 1000000ull >= g_next_sample_ns) {
        sample(a);
        unsigned long long step = (unsigned long long)g_run_ms * 1000000ull;
        g_next_sample_ns += step;
        if (g_next_sample_ns <= now) g_next_sample_ns = now + step;
        repaint = 1;
    }
    // A glide step alone repaints only the traces that moved.
    if (glide_live(a) && !repaint) tm_perf_glide(a, now);
    arm(a);
    return repaint;
}

// desktop.animations, followed live (WIN_EV_SETTING): Off is stepped.
static void on_setting(struct uapp *a) {
    int on = uui_anim_ms(100) != 0;
    if (on == g_animate) return;
    g_animate = on;
    tm_perf_set_smooth(glide_on());
    ulogf("taskmgr: animations %s\n", on ? "on" : "off");
    arm(a);
}

// Minimized or behind a text VT: no glide; back: it starts again.
static void on_suspend(struct uapp *a, int suspended) {
    (void)suspended;
    arm(a);
}

// Before the widgets draw and after every layout, so a resize that
// leaves the table too narrow drops the details pane in the same frame.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uui_menubar_set_bounds(&g_menu, 0, 0, uapp_width(a), uapp_height(a));   // an in-window popup's room
    if (g_page == PAGE_PROCS && tm_procs_fit())
        uui_layout_run(&LAYOUT, 0, 0, uapp_width(a), uapp_height(a));
    uapp_log_layout(a, "taskmgr");
}

static void on_open(struct uapp *a) {
    g_app = a;
    g_prev_ns = sys_monotonic_ns();
    g_animate = uui_anim_ms(100) != 0;
    set_speed(a, g_speed, 0);   // the saved speed: intervals, smooth, the schedule
    refresh_model();
    tm_services_read();
    tm_perf_sample();
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_pages[i].tick) g_pages[i].tick(a, i == g_page);
    select_page(a, g_page);
    ulogf("taskmgr: rows %d\n", g_nproc);
}

// Static, not main()'s local: on_size() sets the minimum from the font,
// which is known only once uapp_run() has started.
static struct uapp_desc g_desc;

static void on_size(int *w, int *h) {
    // FROM WHAT THE WIDGETS ASK FOR, plus room for names: the table's Name
    // column stretches, so its natural width is only its title, and a
    // width fixed in widest-glyph units starved it to nothing under the
    // laptop's larger face while fitting in QEMU.
    tm_procs_size();
    int nw = 0, nh = 0;
    uui_layout_natural_size(&LAYOUT, &nw, &nh);
    // Room for names beyond the Name title the table asks for. Not for
    // the filter box as well: the Read and Write columns took that width,
    // and a default wider than the screen leaves nothing to resize into.
    *w = nw + ugfx_char_advance('0') * 14;
    *h = ugfx_char_h() * 34;
    if (*h < nh) *h = nh;

    // A MINIMUM, so a resize -- or a remembered size -- cannot squeeze the
    // toolbar off the window: the rail, the page's toolbar, the margins.
    int rw = 0, rh = 0;
    uui_sidebar_ops.natural_size(&g_rail, &rw, &rh);
    g_desc.min_w = rw + tm_procs_min_width() + 3 * uui_layout_margin(&BODY)
                   + uui_layout_gap(&BODY);
    g_desc.min_h = ugfx_char_h() * 16 + uui_menubar_height(&g_menu);
}

int main(void) {
    // The tables are as big as the process limit, which depends on the RAM.
    g_proc_cap = sys_proc_max();
    g_proc = g_proc_cap > 0 ? calloc((size_t)g_proc_cap, sizeof *g_proc) : NULL;
    g_prev = g_proc_cap > 0 ? calloc((size_t)g_proc_cap, sizeof *g_prev) : NULL;
    g_info = g_proc_cap > 0 ? calloc((size_t)g_proc_cap, sizeof *g_info) : NULL;
    if (!g_proc || !g_prev || !g_info) {
        sys_eprint("taskmgr: cannot size the process table\n");
        return 1;
    }
    // Never freed: the menu rows live in the builder.
    struct uui_builder *menus = uui_builder_load("taskmgr", MENU_NAMES,
                                                (int)(sizeof MENU_NAMES / sizeof MENU_NAMES[0]));
    int bar_count;
    const struct uui_menu_item *bar_rows = uui_builder_menu(menus, "bar", &bar_count);
    if (uui_builder_error(menus)) return uui_builder_run_error(menus);
    uui_menubar_init(&g_menu, bar_rows, bar_count);
    g_menu.item_flags = menu_flags;
    load_speed();   // before the pages: their charts take its interval
    load_apps();
    tm_procs_init(&g_pages[PAGE_PROCS]);
    tm_perf_init(&g_pages[PAGE_PERF]);
    tm_services_init(&g_pages[PAGE_SERVICES]);

    for (int i = 0; i < PAGE_COUNT; i++)
        g_rail_rows[i] = (struct uui_sidebar_row){ .label = g_pages[i].label,
                                                   .kind = UUI_SIDEBAR_TOP,
                                                   .icon = g_pages[i].icon, .id = i };
    uui_sidebar_init(&g_rail, 0, 0, 0, 0, g_rail_rows, PAGE_COUNT);
    uui_sidebar_select_id(&g_rail, PAGE_PROCS);
    uui_menubar_init(&g_ctx, 0, 0);

    ITEMS[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_rail,
                                   .id = TM_ID_RAIL, .name = "rail", .flags = UUI_FILL_H };
    ITEMS[1] = (struct uui_item){ .ops = g_pages[0].root->ops, .widget = g_pages[0].root->widget,
                                   .name = g_pages[0].root->name,
                                   .flags = UUI_FILL_W | UUI_FILL_H };
    // EDGE TO EDGE, as Help and Crash Reports are: 1,
    // because a margin or gap at or below 0 means "the default".
    BODY = (struct uui_layout){ .dir = UUI_ROW, .items = ITEMS, .count = 2, .margin = 1, .gap = 1 };
    TOP_ITEMS[0] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &g_menu,
                                       .id = TM_ID_MENU, .name = "menu", .flags = UUI_FILL_W };
    TOP_ITEMS[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &BODY,
                                       .flags = UUI_FILL_W | UUI_FILL_H };
    TOP_ITEMS[2] = (struct uui_item){ .ops = &uui_menubar_ops, .widget = &g_ctx,
                                       .id = TM_ID_CTX, .name = "ctxmenu" };
    // The bar on the window's edge: no margin, no gap -- 1 hundredth of a
    // line is 0 px, where 0 itself would mean "the default".
    LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = TOP_ITEMS, .count = 2,
                                  .margin = 1, .margin_unit = UUI_LH, .gap = 1, .gap_unit = UUI_LH };

    g_desc = (struct uapp_desc){
        .title = "Task Manager",
        .app_id = "taskmgr",
        .layout = &LAYOUT,
        .tick_ms = TM_REFRESH_MS,
        .on_size = on_size,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .widgets = TOP_ITEMS,
        .widget_count = 3,
        .focus = &g_focus,
        .on_draw = on_draw,
        .on_key = on_key,
        .on_press = on_press,
        .on_release = on_release,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_setting = on_setting,
        .on_suspend = on_suspend,
    };
    return uapp_run(&g_desc);
}
