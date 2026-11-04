// System Update -- pull a new build from an update server, with the
// progress on screen.
//
// A WINDOW over userland/update/upd.c, the engine /bin/update also
// runs; the design, and why a library update waits for a restart, is
// docs/update-design.md. The layout is Windows Update's single page: a
// status heading, the server, one overall bar, then What's new / Files /
// Log as tabs (the mockups chosen 2026-09-30 and 2026-10-02). What's new
// is the release notes the engine cut at this machine's build.
//
// THE CHANNEL is the server's own list (upd_resolve()): the heading names
// the one in use and Change channel... opens it as a dialog (the U3
// mockup, 2026-10-18). A channel BEHIND this machine is never installed
// silently: choosing one asks to go back or to wait, and a check of one
// that is behind says it is waiting (V_WAITING). Each row says whether its
// build is newer than this machine's, the same, or older.
//
// THE SERVER is Change server...'s dialog (the S3 mockup, 2026-10-18): the
// address, the recent ones as rows, and Test, which asks the address for
// its channel list WITHOUT changing a setting -- Ubuntu's "Select Best
// Server" shape -- so a typo is found before Use.
//
// THREADS: a check or an install runs on a worker, which owns the plan
// while it runs and talks to this thread ONLY through uapp_post() and
// the log queue below. Everything drawn is touched here.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"   // KEY_PAGE_*, KEY_MOD_CTRL
#include "rt/sys.h"
#include "lib/uinitctl.h"
#include "lib/human.h"
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_markdown.h"
#include "ui/uui_webview.h"
#include "lib/umd.h"
#include "ui/uui_table.h"
#include "ui/uui_tabs.h"
#include "ui/uui_dialog.h"
#include "ui/uui_choicelist.h"
#include "ui/ulog.h"
#include "lib/ugrow.h"
#include "ui/utheme.h"
#include "update/upd.h"

enum { EV_LOG = 1, EV_PROGRESS, EV_DONE, EV_WEB };
enum { JOB_NONE, JOB_CHECK, JOB_APPLY, JOB_TEST };

enum {
    ID_CHANGE = 1, ID_SERVER_EDIT, ID_TEST, ID_SERVERS,
    ID_TABLE, ID_LOGVIEW, ID_TABS, ID_NOTES, ID_CHECK, ID_MAIN, ID_CHANNEL, ID_DIALOG,
    ID_CHOICES,
};
// What the two dialogs answer.
enum { DLG_CANCEL = 1, DLG_USE, DLG_WAIT, DLG_BACK };
enum { PAGE_NOTES, PAGE_FILES, PAGE_LOG, PAGE_COUNT };

// What the window is showing, which decides the heading and the buttons.
enum view {
    V_CHECKING, V_AVAILABLE, V_UPTODATE, V_INSTALLING, V_INSTALLED, V_RESTART, V_BLOCKED,
    V_FAILED, V_CANCELLED,
    V_WAITING,   // the channel's build is older than this machine's; nothing is offered
};

// ---- state shared with the worker -------------------------------------------

static struct upd_plan g_plan;
static char g_server[UPD_URL_MAX];
static struct upd_channels g_chs;   // the server's list; chosen -1 for a server without one
static char g_url[UPD_URL_MAX + 32]; // the channel's own address, what was checked
static char g_want[24];             // a channel to check before the setting is changed, or ""
static int g_go_back;               // installing a build older than this machine's was agreed
static int g_job;                   // JOB_*, set here, cleared here on EV_DONE
static volatile int g_cancel;
static int g_job_rc;
// Test's question and answer (JOB_TEST): the address as typed, and what it said.
static char g_test_url[UPD_URL_MAX];
static struct upd_channels g_test_chs;
static char g_test_err[200];
static int g_test_rc;
static unsigned long long g_test_ns;
static int g_check_after_test;      // Use was pressed while Test was out

// Log lines cross threads through this queue; EV_LOG says "drain it".
#define LOGQ 64
static char g_logq[LOGQ][200];
static int g_logq_n;
static pthread_mutex_t g_logq_lock = PTHREAD_MUTEX_INITIALIZER;

static struct uapp *g_app;
static unsigned long long g_last_post_ns;

// ---- widgets --------------------------------------------------------------

static enum view g_view = V_CHECKING;
static char g_title_text[64], g_detail_text[200], g_amount_text[64], g_rate_text[64];
static struct uui_label g_title, g_detail, g_server_key, g_server_val, g_amount, g_rate, g_spacer;
static struct uui_label g_chan_key, g_chan_val;
static char g_chan_text[40];
static struct uui_button g_change, g_check, g_main, g_chan_btn;
static char g_main_label[48];
static struct uui_dialog g_dlg;
static struct uui_choicelist g_choices;
// One row per channel the server lists, grown with g_chs (lib/ugrow.h);
// g_choices is re-aimed at g_choice every time the rows are built.
struct choice_row { int ch; char tags[24], val[64]; const char *badge; };
static struct uui_choice *g_choice;
static struct choice_row *g_choice_row;
static int g_choice_cap, g_choice_row_cap;
static char g_dlg_line[3][160];
static const char *g_dlg_rows[3] = { g_dlg_line[0], g_dlg_line[1], g_dlg_line[2] };
static int g_back_to = -1;                 // the channel the going-back question is about
static int g_log_dialog;                   // report the open dialog's rects at the next tick
static struct uui_item g_choices_item = { .ops = &uui_choicelist_ops, .widget = &g_choices,
                                          .id = ID_CHOICES, .name = "channels" };

// The server dialog's body: the address and Test, the recent ones, and
// what Test found.
static struct uui_textbox g_edit;
static struct uui_button g_test;
static struct uui_label g_addr_key, g_recent_key, g_test_line, g_test_line2;
static char g_test_text[200], g_test_text2[160];
static struct uui_choicelist g_servers;
static struct uui_choice g_server_rows[UPD_RECENT_MAX + 1];
static char g_recent_urls[UPD_RECENT_MAX + 1][UPD_URL_MAX];
static struct uui_item g_addr_items[] = {
    // FILL_H: the field takes the row's height, the Test button's.
    { .ops = &uui_textbox_ops, .widget = &g_edit, .id = ID_SERVER_EDIT, .name = "serveredit",
      .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_button_ops,  .widget = &g_test, .id = ID_TEST, .name = "test" },
};
static struct uui_layout g_addr_row = {
    .dir = UUI_ROW, .items = g_addr_items, .count = sizeof g_addr_items / sizeof g_addr_items[0],
};
static struct uui_item g_srv_items[] = {
    { .ops = &uui_label_ops,      .widget = &g_addr_key },
    { .ops = &uui_layout_ops,     .widget = &g_addr_row, .flags = UUI_FILL_W },
    { .ops = &uui_label_ops,      .widget = &g_recent_key },
    { .ops = &uui_choicelist_ops, .widget = &g_servers, .id = ID_SERVERS, .name = "servers",
      .flags = UUI_FILL_W },
    { .ops = &uui_label_ops,      .widget = &g_test_line, .name = "testresult", .flags = UUI_FILL_W },
    { .ops = &uui_label_ops,      .widget = &g_test_line2, .name = "testdetail", .flags = UUI_FILL_W },
};
static struct uui_layout g_srv_col = {
    .dir = UUI_COLUMN, .gap = 4, .items = g_srv_items, .count = sizeof g_srv_items / sizeof g_srv_items[0],
};
static struct uui_item g_srv_item = { .ops = &uui_layout_ops, .widget = &g_srv_col, .name = "server" };
static struct uui_progress g_bar;
static struct uui_table g_table;
static struct uui_textview g_log;
static char g_log_buf[32768];
static int *g_rows;                      // plan index per table row, grown to the plan
static int g_row_count, g_row_cap;
static struct uui_markdown g_notes;
// What's new as a web page when the engine is installed (ui/uui_webview.h):
// app:notes/<gen> is the notes, the "Under the hood" fold a link to
// app:notes/hood -- NetSurf has no <details> (the stage 5 canvas's N2).
// Without the engine (NOGPL=1), g_notes draws them as before.
static struct uui_webview g_web;
static int g_web_on;
static char *g_notes_md;            // a copy: the worker frees the plan's
static unsigned g_notes_gen;
static char g_files_label[24] = "Files";
static struct uui_tab g_tab_list[PAGE_COUNT] = {
    { "What's new", 0, 0 }, { g_files_label, 0, 0 }, { "Log", 0, 0 },
};
static struct uui_tabs g_tabs;

// Speed, from the bytes done since the install started.
static unsigned long long g_apply_start_ns;

static const struct uui_table_column COLS[] = {
    { "File", 0, UUI_TALIGN_LEFT },
    { "Size", 8, UUI_TALIGN_RIGHT },
    { "Status", 12, UUI_TALIGN_LEFT },
};

// The labels FILL the row's height, which centres their text on the
// buttons beside them (uui_label draws centred in a stretched box).
static struct uui_item g_server_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_server_key, .name = "serverkey", .flags = UUI_FILL_H },
    { .ops = &uui_label_ops,  .widget = &g_server_val, .name = "server",
      .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_label_ops,  .widget = &g_chan_key, .name = "channelkey", .flags = UUI_FILL_H },
    { .ops = &uui_label_ops,  .widget = &g_chan_val, .name = "channel", .flags = UUI_FILL_H },
    { .ops = &uui_button_ops, .widget = &g_chan_btn, .id = ID_CHANNEL, .name = "changechannel" },
    { .ops = &uui_button_ops, .widget = &g_change, .id = ID_CHANGE, .name = "change" },
};
static struct uui_layout g_server_row = {
    .dir = UUI_ROW, .items = g_server_items,
    .count = sizeof g_server_items / sizeof g_server_items[0],
};
static struct uui_item g_amount_items[] = {
    { .ops = &uui_label_ops, .widget = &g_amount, .name = "amount", .flags = UUI_FILL_W },
    { .ops = &uui_label_ops, .widget = &g_rate, .name = "rate" },
};
static struct uui_layout g_amount_row = {
    .dir = UUI_ROW, .items = g_amount_items,
    .count = sizeof g_amount_items / sizeof g_amount_items[0],
};
static struct uui_item g_button_items[] = {
    { .ops = &uui_label_ops,  .widget = &g_spacer, .flags = UUI_FILL_W },
    { .ops = &uui_button_ops, .widget = &g_check, .id = ID_CHECK, .name = "check" },
    { .ops = &uui_button_ops, .widget = &g_main, .id = ID_MAIN, .name = "main" },
};
static struct uui_layout g_button_row = {
    .dir = UUI_ROW, .items = g_button_items,
    .count = sizeof g_button_items / sizeof g_button_items[0],
};

// The tab strip and the one page under it, tight against it.
static struct uui_item g_page_items[] = {
    { .ops = &uui_tabs_ops,     .widget = &g_tabs, .id = ID_TABS, .name = "tabs",
      .flags = UUI_FILL_W },
    { .ops = &uui_markdown_ops, .widget = &g_notes, .id = ID_NOTES, .name = "notes",
      .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_table_ops,    .widget = &g_table, .id = ID_TABLE, .name = "files",
      .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 },
    { .ops = &uui_textview_ops, .widget = &g_log, .id = ID_LOGVIEW, .name = "log",
      .flags = UUI_FILL_W | UUI_FILL_H, .hidden = 1 },
};
static struct uui_layout g_page = {
    .dir = UUI_COLUMN, .gap = 1, .items = g_page_items,
    .count = sizeof g_page_items / sizeof g_page_items[0],
};

static struct uui_item g_items[] = {
    { .ops = &uui_label_ops,    .widget = &g_title, .name = "title" },
    { .ops = &uui_label_ops,    .widget = &g_detail, .name = "detail", .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_server_row, .flags = UUI_FILL_W },
    { .ops = &uui_progress_ops, .widget = &g_bar, .name = "bar", .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_amount_row, .flags = UUI_FILL_W },
    { .ops = &uui_layout_ops,   .widget = &g_page, .flags = UUI_FILL_W | UUI_FILL_H },
    { .ops = &uui_layout_ops,   .widget = &g_button_row, .flags = UUI_FILL_W },
    // NOT LAID OUT -- the layout's count stops before it -- but routed,
    // and last so it draws over the page.
    { .ops = &uui_dialog_ops,   .widget = &g_dlg, .id = ID_DIALOG, .name = "dialog" },
};
static struct uui_layout g_root = {
    .dir = UUI_COLUMN, .items = g_items, .count = sizeof g_items / sizeof g_items[0] - 1,
};

static struct uui_focusable g_focusables[] = {
    { &g_chan_btn, &uui_button_ops },
    { &g_change, &uui_button_ops },
    { &g_table, &uui_table_ops },
    { &g_check, &uui_button_ops },
    { &g_main, &uui_button_ops },
};
static struct uui_focus g_focus;

// ---- the worker ------------------------------------------------------------

static void w_log(void *ctx, const char *line) {
    (void)ctx;
    pthread_mutex_lock(&g_logq_lock);
    if (g_logq_n < LOGQ) snprintf(g_logq[g_logq_n++], sizeof g_logq[0], "%s", line);
    pthread_mutex_unlock(&g_logq_lock);
    uapp_post(g_app, EV_LOG, 0);
}

// Throttled: a download calls this every 32 KiB, and ten repaints a
// second is all a person can see.
static void w_progress(void *ctx, const struct upd_plan *p, int idx) {
    (void)ctx;
    unsigned long long now = sys_monotonic_ns();
    int milestone = idx >= 0 && p->files[idx].status != UPD_FETCHING;
    if (!milestone && now - g_last_post_ns < 100000000ULL) return;
    g_last_post_ns = now;
    uapp_post(g_app, EV_PROGRESS, idx);
}

static int w_cancelled(void *ctx) { (void)ctx; return g_cancel; }

static const struct upd_hooks HOOKS = {
    .log = w_log, .progress = w_progress, .cancelled = w_cancelled,
};

static void *worker(void *arg) {
    int job = (int)(long)arg;
    if (job == JOB_TEST) {
        unsigned long long t0 = sys_monotonic_ns();
        g_test_err[0] = 0;
        g_test_rc = upd_channels_fetch(g_test_url, &g_test_chs, g_test_err, sizeof g_test_err);
        g_test_ns = sys_monotonic_ns() - t0;
    } else if (job == JOB_CHECK) {
        upd_plan_free(&g_plan);
        char err[200] = "";
        if (upd_resolve(g_server, g_want[0] ? g_want : NULL, g_url, sizeof g_url, &g_chs,
                        err, sizeof err) != 0) {
            memset(&g_plan, 0, sizeof g_plan);
            snprintf(g_plan.error, sizeof g_plan.error, "%s", err);
            w_log(0, err);
            g_job_rc = -1;
        } else {
            g_job_rc = upd_check(g_url, &g_plan, &HOOKS);
        }
    } else {
        g_job_rc = upd_apply(&g_plan, &HOOKS);
    }
    uapp_post(g_app, EV_DONE, job);
    return 0;
}

static void refresh(struct uapp *a);
static void show_notes(void);

static void start_job(struct uapp *a, int job) {
    if (g_job != JOB_NONE) return;
    g_job = job;
    g_cancel = 0;
    g_view = job == JOB_CHECK ? V_CHECKING : V_INSTALLING;
    if (job == JOB_APPLY) g_apply_start_ns = sys_monotonic_ns();
    if (job == JOB_CHECK) {
        g_row_count = 0;
        show_notes();   // the worker frees the plan the page points into
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, worker, (void *)(long)job) != 0) {
        worker((void *)(long)job);   // no thread: the window freezes for it, but it still works
    }
    ulogf("sysupdate: %s started\n", job == JOB_CHECK ? "check" : "install");
    refresh(a);
}

// ---- presenting --------------------------------------------------------------

static const char *status_word(const struct upd_file *f) {
    static char pct[16];
    switch (f->status) {
    case UPD_FETCHING:
        snprintf(pct, sizeof pct, "%d%%", f->size ? (int)(f->got * 100 / f->size) : 100);
        return pct;
    case UPD_STAGED:    return "Downloaded";
    case UPD_INSTALLED: return "Installed";
    case UPD_AT_BOOT:   return "At restart";
    case UPD_REMOVED:   return "Removed";
    case UPD_FAILED:    return g_view == V_CANCELLED ? "Stopped" : "Failed";
    default:            return f->change == UPD_NEW ? "New"
                             : f->change == UPD_REMOVE ? "To remove" : "Queued";
    }
}

static void cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (row < 0 || row >= g_row_count) return;
    const struct upd_file *f = &g_plan.files[g_rows[row]];
    if (col == 0) snprintf(out, (size_t)cap, "%s", f->path);
    else if (col == 1 && f->change != UPD_REMOVE) human_size(out, (unsigned long)cap, f->size);
    else snprintf(out, (size_t)cap, "%s", status_word(f));
}

// The row being fetched is tinted, so the eye finds it in a long list.
static uint32_t tint(void *ctx, int row) {
    (void)ctx;
    if (row < 0 || row >= g_row_count) return 0;
    return g_plan.files[g_rows[row]].status == UPD_FETCHING ? UTHEME_SELECTION : 0;
}

static void collect_rows(void) {
    g_row_count = 0;
    if (g_plan.count > g_row_cap) {
        int *g = realloc(g_rows, sizeof *g_rows * (size_t)g_plan.count);
        if (g) { g_rows = g; g_row_cap = g_plan.count; }
    }
    for (int i = 0; i < g_plan.count && g_row_count < g_row_cap; i++) {
        int c = g_plan.files[i].change;
        if (c == UPD_CHANGED || c == UPD_NEW || c == UPD_REMOVE) g_rows[g_row_count++] = i;
    }
    uui_table_set_rows(&g_table, g_row_count);
}

// What the What's new page says when the engine has no notes to give.
static void show_notes(void) {
    const char *t = g_plan.notes;
    if (g_job == JOB_CHECK) t = "Checking for release notes...";
    else if (!t && g_plan.notes_since) t = "Nothing new since this machine's build.";
    else if (!t) t = "This server publishes no release notes.";
    if (!g_web_on) {
        uui_markdown_set_text(&g_notes, t, (int)strlen(t));
        return;
    }
    free(g_notes_md);
    g_notes_md = strdup(t);
    // A new address each time: the page is the notes AS THEY ARE NOW.
    char url[48];
    snprintf(url, sizeof url, "app:notes/%u", ++g_notes_gen);
    uui_webview_navigate(&g_web, url);
}

static void notes_page(struct uui_webview *v, uint32_t id, const char *url) {
    const char *md = g_notes_md ? g_notes_md : "";
    int hood = !strcmp(url, "app:notes/hood");
    struct umd_html_opts o = { NULL, NULL, hood ? UMD_HTML_DETAILS_ONLY : UMD_HTML_DETAILS_LINK,
                               hood ? "app:notes/main" : "app:notes/hood" };
    int n = 0;
    char *page = uui_webview_markdown_page("What's new", md, (int)strlen(md), &o, &n);
    if (page) uui_webview_scheme_reply(v, id, 200, "text/html", page, (uint32_t)n, 1);
    else uui_webview_scheme_reply(v, id, 404, NULL, NULL, 0, 1);
    free(page);
}

static void notes_changed(struct uui_webview *v, unsigned what) {
    (void)v;
    if (!g_app) return;
    // Where the page's links are, for a test (ui/uui_describe.h), as the
    // fold's row was reported when the notes were drawn here.
    if (what & (UUI_WV_LINKS | UUI_WV_URL)) uapp_log_layout(g_app, "sysupdate");
    uapp_redraw(g_app);
}

// A link in the notes to the web opens in the Web Browser.
static void notes_external(struct uui_webview *v, const char *url) {
    (void)v;
    if (g_app) uapp_spawn(g_app, "/bin/wm/apps/browser", url);
}

static const struct uui_webview_host NOTES_HOST = {
    .changed = notes_changed,
    .external = notes_external,
    .scheme = notes_page,
};

static void show_page(struct uapp *a, int page) {
    g_page_items[1].hidden = page != PAGE_NOTES;
    g_page_items[2].hidden = page != PAGE_FILES;
    g_page_items[3].hidden = page != PAGE_LOG;
    uui_tabs_select(&g_tabs, page);
    if (a) {
        uui_layout_run(&g_root, 0, 0, uapp_width(a), uapp_height(a));
        uapp_redraw(a);
    }
}

static void tab_selected(void *ctx, int index) { show_page(ctx, index); }

static void set_button(struct uui_button *b, const char *label, int enabled) {
    b->label = label;
    b->disabled = !enabled;
}

static void relayout(struct uapp *a) {
    uui_layout_run(&g_root, 0, 0, uapp_width(a), uapp_height(a));
}

static void refresh(struct uapp *a) {
    char total[16], done[16];
    human_size(total, sizeof total, g_plan.bytes);
    human_size(done, sizeof done, g_plan.done_bytes);
    const char *title = "";
    g_detail_text[0] = g_amount_text[0] = g_rate_text[0] = '\0';
    char built[96] = "", removals[48] = "";
    if (g_plan.removals)
        snprintf(removals, sizeof removals, ", %d stale file%s %s", g_plan.removals,
                 g_plan.removals == 1 ? "" : "s", g_view == V_INSTALLED ? "removed" : "to remove");
    if (g_plan.built[0])
        snprintf(built, sizeof built, ", build of %s%s%s%s", g_plan.built,
                 g_plan.version[0] ? " (" : "", g_plan.version, g_plan.version[0] ? ")" : "");

    switch (g_view) {
    case V_CHECKING:
        title = "Checking for updates";
        snprintf(g_detail_text, sizeof g_detail_text, "Comparing this machine with the server");
        uui_progress_set_busy(&g_bar);
        break;
    case V_AVAILABLE:
        title = "Updates available";
        snprintf(g_detail_text, sizeof g_detail_text, "%d of %d files changed, %s%s%s",
                 g_plan.changed, g_plan.count - g_plan.removals, total, removals, built);
        uui_progress_set(&g_bar, 0);
        snprintf(g_amount_text, sizeof g_amount_text, "%s to download", total);
        break;
    case V_UPTODATE:
        title = "Up to date";
        snprintf(g_detail_text, sizeof g_detail_text, "All %d files match the server%s",
                 g_plan.count - g_plan.removals, built);
        uui_progress_set(&g_bar, 1000);
        break;
    case V_INSTALLING:
        title = "Installing updates";
        snprintf(g_detail_text, sizeof g_detail_text, "%d of %d files changed, %s%s",
                 g_plan.changed, g_plan.count, total, built);
        uui_progress_set(&g_bar, g_plan.bytes ? (int)(g_plan.done_bytes * 1000 / g_plan.bytes) : 0);
        snprintf(g_amount_text, sizeof g_amount_text, "%s of %s", done, total);
        {
            unsigned long long el = sys_monotonic_ns() - g_apply_start_ns;
            unsigned long long ms = el / 1000000ULL;
            if (ms > 500 && g_plan.done_bytes) {
                unsigned long long rate = g_plan.done_bytes * 1000ULL / ms;
                unsigned long long left = g_plan.bytes > g_plan.done_bytes
                                          ? (g_plan.bytes - g_plan.done_bytes) / (rate ? rate : 1) : 0;
                char r[16];
                human_size(r, sizeof r, rate);
                snprintf(g_rate_text, sizeof g_rate_text, "%s/s, about %llu s left", r,
                         left + 1);
            }
        }
        break;
    case V_INSTALLED:
        title = "Updates installed";
        snprintf(g_detail_text, sizeof g_detail_text, "%d file%s updated%s%s", g_plan.changed,
                 g_plan.changed == 1 ? "" : "s", removals, built);
        uui_progress_set(&g_bar, 1000);
        snprintf(g_amount_text, sizeof g_amount_text, "%s", total);
        break;
    case V_RESTART:
        title = "Restart to finish";
        if (g_plan.staged_for_boot && !g_plan.at_boot)
            snprintf(g_detail_text, sizeof g_detail_text,
                     "An update is waiting; it is applied while the machine starts");
        else
            snprintf(g_detail_text, sizeof g_detail_text,
                     "%d files downloaded and verified%s. They are put in place while "
                     "the machine restarts, because %s", g_plan.changed, built,
                     g_plan.kernel_installed ? "the kernel changed" : "a library changed");
        uui_progress_set(&g_bar, 1000);
        if (g_plan.at_boot || g_plan.kernel_installed)
            snprintf(g_amount_text, sizeof g_amount_text, "%s downloaded", total);
        break;
    case V_BLOCKED:
        title = "The kernel cannot be updated";
        snprintf(g_detail_text, sizeof g_detail_text,
                 "GRUB on this machine shows no menu, so the previous kernel could not be "
                 "chosen if a new one failed. Nothing will be installed.");
        uui_progress_set(&g_bar, 0);
        break;
    case V_CANCELLED:
        title = "Update cancelled";
        snprintf(g_detail_text, sizeof g_detail_text, "Nothing was changed");
        uui_progress_set(&g_bar, 0);
        break;
    case V_WAITING: {
        const struct upd_channel *c = g_chs.chosen >= 0 ? &g_chs.ch[g_chs.chosen] : 0;
        char mv[48], mb[48];
        upd_machine_build(mv, sizeof mv, mb, sizeof mb);
        snprintf(g_title_text, sizeof g_title_text, "Waiting for %s to reach %s",
                 c ? c->title : "the server", mv);
        title = g_title_text;
        snprintf(g_detail_text, sizeof g_detail_text,
                 "Its newest build, %s%s%s, is older than this machine's. Nothing is installed "
                 "until it passes this build.", g_plan.version[0] ? g_plan.version : "?",
                 g_plan.built[0] ? " of " : "", g_plan.built);
        uui_progress_set(&g_bar, 1000);
        break;
    }
    case V_FAILED:
        title = "Update failed";
        snprintf(g_detail_text, sizeof g_detail_text, "%s", g_plan.error[0] ? g_plan.error : "see the log");
        uui_progress_set(&g_bar, 0);
        break;
    }
    // THE HEADING NAMES THE CHANNEL -- "Up to date on Main" -- for a
    // server that declares them.
    const char *on = g_chs.chosen >= 0 && g_view != V_WAITING && g_view != V_CHECKING
                     ? g_chs.ch[g_chs.chosen].title : 0;
    if (title != g_title_text) {
        if (on) snprintf(g_title_text, sizeof g_title_text, "%s on %s", title, on);
        else snprintf(g_title_text, sizeof g_title_text, "%s", title);
    }
    uui_label_set_text(&g_title, g_title_text);
    snprintf(g_chan_text, sizeof g_chan_text, "%s",
             g_chs.chosen >= 0 ? g_chs.ch[g_chs.chosen].title : "-");
    uui_label_set_text(&g_chan_val, g_chan_text);
    uui_label_set_text(&g_detail, g_detail_text);
    uui_label_set_text(&g_amount, g_amount_text);
    uui_label_set_text(&g_rate, g_rate_text);
    uui_label_set_text(&g_server_val, g_server);

    int busy = g_job != JOB_NONE;
    set_button(&g_check, "Check again", !busy);
    set_button(&g_change, "Change server...", !busy);
    set_button(&g_chan_btn, "Change channel...", !busy && g_chs.count > 0);
    switch (g_view) {
    case V_CHECKING: case V_INSTALLING: set_button(&g_main, "Cancel", 1); break;
    case V_AVAILABLE:
        if (g_plan.behind) {
            snprintf(g_main_label, sizeof g_main_label, "Go back to %s", g_plan.version);
            set_button(&g_main, g_main_label, 1);
        } else {
            set_button(&g_main, "Install", 1);
        }
        break;
    case V_WAITING:
        snprintf(g_main_label, sizeof g_main_label, "Go back to %s", g_plan.version);
        set_button(&g_main, g_main_label, g_plan.changed || g_plan.removals);
        break;
    case V_RESTART:                     set_button(&g_main, "Restart now", 1); break;
    default:                            set_button(&g_main, "Install", 0); break;
    }
    uui_table_set_rows(&g_table, g_row_count);
    if (g_row_count) snprintf(g_files_label, sizeof g_files_label, "Files (%d)", g_row_count);
    else snprintf(g_files_label, sizeof g_files_label, "Files");
    show_notes();
    relayout(a);
    uapp_redraw(a);
}

static void drain_log(void) {
    pthread_mutex_lock(&g_logq_lock);
    for (int i = 0; i < g_logq_n; i++) {
        for (const char *c = g_logq[i]; *c; c++) utext_putc(&g_log.tb, *c);
        utext_putc(&g_log.tb, '\n');
    }
    g_logq_n = 0;
    pthread_mutex_unlock(&g_logq_lock);
}

static void test_done(struct uapp *a);

static void job_done(struct uapp *a, int job) {
    g_job = JOB_NONE;
    if (job == JOB_TEST) {
        test_done(a);
        return;
    }
    if (job == JOB_CHECK) {
        upd_server_get(g_server, sizeof g_server);   // an old .../stable may have been split
        g_want[0] = '\0';
        collect_rows();
        int changes = g_plan.changed || g_plan.removals;
        if (g_job_rc != 0) g_view = g_cancel ? V_CANCELLED : V_FAILED;
        else if (g_plan.staged_for_boot) g_view = V_RESTART;
        else if (g_plan.kernel_blocked) g_view = V_BLOCKED;
        else if (g_plan.behind && changes && !g_go_back) g_view = V_WAITING;
        else g_view = changes ? V_AVAILABLE : V_UPTODATE;
        ulogf("sysupdate: channel %s of %d, behind %d\n",
              g_chs.chosen >= 0 ? g_chs.ch[g_chs.chosen].id : "-", g_chs.count, g_plan.behind);
        // "Go back to X" was the answer already: install what the check found.
        if (g_view == V_AVAILABLE && g_plan.behind && g_go_back) {
            refresh(a);
            start_job(a, JOB_APPLY);
            return;
        }
    } else {
        g_go_back = 0;
        if (g_job_rc != 0) g_view = g_cancel ? V_CANCELLED : V_FAILED;
        else if (g_plan.at_boot || g_plan.kernel_installed) g_view = V_RESTART;
        else g_view = V_INSTALLED;
    }
    // What a test reads: the view, and the counts it was drawn from.
    ulogf("sysupdate: view %d changed %d removals %d staged %d installed %s\n", (int)g_view,
          g_plan.changed, g_plan.removals, g_plan.at_boot, g_job_rc == 0 ? "ok" : "failed");
    ulogf("sysupdate: notes %d hood %d quiet %d since %d\n", g_plan.notes_count,
          g_plan.notes_hood, g_plan.notes_quiet, g_plan.notes_since);
    refresh(a);
    uapp_log_layout(a, "sysupdate");   // the tab slots, for a test to click (ui/uui_describe.h)
}

// ---- the server dialog ----------------------------------------------------------

static void open_dialog(struct uapp *a, const char *title, int rows,
                        const struct uui_dialog_button *b, int n, int dflt);

static void test_say(const char *line1, const char *line2) {
    snprintf(g_test_text, sizeof g_test_text, "%s", line1);
    snprintf(g_test_text2, sizeof g_test_text2, "%s", line2);
    uui_label_set_text(&g_test_line, g_test_text);
    uui_label_set_text(&g_test_line2, g_test_text2);
}

// The recent addresses as rows, the one in use badged; the field's
// address selected when it is one of them.
static void server_rows(void) {
    int n = upd_recent(g_recent_urls, UPD_RECENT_MAX), have = 0;
    for (int i = 0; i < n; i++) have |= !strcmp(g_recent_urls[i], g_server);
    if (!have && g_server[0]) {   // in use but never Used here: an image's default
        memmove(g_recent_urls[1], g_recent_urls[0], (size_t)n * sizeof g_recent_urls[0]);
        snprintf(g_recent_urls[0], sizeof g_recent_urls[0], "%s", g_server);
        n++;
    }
    int sel = -1;
    for (int i = 0; i < n; i++) {
        int cur = !strcmp(g_recent_urls[i], g_server);
        g_server_rows[i] = (struct uui_choice){ .title = g_recent_urls[i], .badge = cur ? "in use" : 0 };
        if (!strcmp(g_recent_urls[i], uui_textbox_text(&g_edit))) sel = i;
    }
    uui_choicelist_init(&g_servers, g_server_rows, n, sel);
}

// Up, with the field, rows and result as they are: a commit closes the
// dialog, so a refused Use puts it back this way.
static void show_server(struct uapp *a) {
    snprintf(g_dlg_line[0], sizeof g_dlg_line[0],
             "The address this machine checks for updates, starting http:// or https://.");
    static const struct uui_dialog_button b[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Use", DLG_USE, UUI_DLG_PRIMARY },
    };
    open_dialog(a, "Update server", 1, b, 2, 1);
    int bw, bh;
    uui_layout_ops.natural_size(&g_srv_col, &bw, &bh);
    if (bw < ugfx_char_w() * 60) bw = ugfx_char_w() * 60;
    uui_dialog_set_body(&g_dlg, &g_srv_item, bw, bh);
    struct uui_item *tabs[] = { &g_addr_items[0], &g_addr_items[1], &g_srv_items[3] };
    uui_dialog_set_tabs(&g_dlg, tabs, 3);
    uui_dialog_focus(&g_dlg, &g_addr_items[0]);
}

static void open_server(struct uapp *a) {
    if (g_job != JOB_NONE) return;
    uui_textbox_set_text(&g_edit, g_server);
    server_rows();
    test_say("Test asks the address what it offers, without changing anything.", "");
    show_server(a);
    ulogf("sysupdate: server dialog, %d rows\n", g_servers.count);
}

static void start_test(struct uapp *a) {
    if (g_job != JOB_NONE) return;
    snprintf(g_test_url, sizeof g_test_url, "%s", uui_textbox_text(&g_edit));
    size_t n = strlen(g_test_url);
    while (n && g_test_url[n - 1] == '/') g_test_url[--n] = '\0';
    if (strncmp(g_test_url, "http://", 7) && strncmp(g_test_url, "https://", 8)) {
        test_say("Not tested: the address must start with http:// or https://", "");
        uapp_redraw(a);
        return;
    }
    char line[200];
    snprintf(line, sizeof line, "Testing %s ...", g_test_url);
    test_say(line, "");
    g_job = JOB_TEST;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, worker, (void *)(long)JOB_TEST) != 0) worker((void *)(long)JOB_TEST);
    refresh(a);
}

// "Release, Main and Dev" -- the channels this machine would list.
static void channel_names(const struct upd_channels *c, char *out, size_t cap) {
    int n = 0, internal = upd_show_internal();
    int *shown = malloc(sizeof *shown * (size_t)(c->count > 0 ? c->count : 1));
    if (!shown) { out[0] = 0; return; }
    for (int i = 0; i < c->count; i++)
        if (!c->ch[i].internal || internal) shown[n++] = i;
    out[0] = 0;
    for (int k = 0; k < n; k++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", k == 0 ? "" : k == n - 1 ? " and " : ", ", c->ch[shown[k]].title);
    }
    free(shown);
}

static void test_done(struct uapp *a) {
    unsigned ms = (unsigned)(g_test_ns / 1000000ULL);
    char l1[200], l2[160] = "";
    if (g_test_rc < 0) {
        snprintf(l1, sizeof l1, "No answer from %s: %s", g_test_url, g_test_err);
    } else if (g_test_rc == 0) {
        snprintf(l1, sizeof l1, "Reached in %u ms. It lists no channels, so the address itself is checked.", ms);
    } else {
        char names[120];
        channel_names(&g_test_chs, names, sizeof names);
        snprintf(l1, sizeof l1, "Reached in %u ms. Offers %s.", ms, names);
        // The channel this machine would check there: its setting, else
        // the server's default.
        char want[24] = "";
        upd_channel_get(want, sizeof want);
        int i = -1;
        for (int k = 0; k < g_test_chs.count && want[0]; k++)
            if (!strcmp(g_test_chs.ch[k].id, want)) i = k;
        if (i < 0) i = g_test_chs.dflt >= 0 ? g_test_chs.dflt : 0;
        const struct upd_channel *c = &g_test_chs.ch[i];
        if (c->version[0])
            snprintf(l2, sizeof l2, "%s's newest: %s%s%s", c->title, c->version,
                     c->built[0] ? ", built " : "", c->built);
        else
            snprintf(l2, sizeof l2, "%s has nothing published yet.", c->title);
    }
    test_say(l1, l2);
    ulogf("sysupdate: test %s rc %d channels %d\n", g_test_url, g_test_rc,
          g_test_rc > 0 ? g_test_chs.count : 0);
    if (g_check_after_test) {
        g_check_after_test = 0;
        start_job(a, JOB_CHECK);
        return;
    }
    refresh(a);
}

// 1 when the field's address was saved; 0 with the dialog saying why.
static int use_server(struct uapp *a) {
    const char *url = uui_textbox_text(&g_edit);
    if (upd_server_set(url) != 0) {
        test_say("Not saved: the address must start with http:// or https:// and fit in 63 characters", "");
        ulogf("sysupdate: server refused\n");
        uapp_redraw(a);
        return 0;
    }
    snprintf(g_server, sizeof g_server, "%s", url);
    ulogf("sysupdate: server %s\n", g_server);
    return 1;
}

// ---- the channel dialog, and going back ---------------------------------------

static void open_dialog(struct uapp *a, const char *title, int rows,
                        const struct uui_dialog_button *b, int n, int dflt) {
    uui_dialog_set_bounds(&g_dlg, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_dlg, title, g_dlg_rows, rows, b, n, dflt, DLG_CANCEL);
    // Placed by the next frame; a test reads its rows and buttons after it.
    g_log_dialog = 1;
    uapp_redraw(a);
}

// The server's channels as rows: an internal one only where this machine
// lists them, or when it is the one in use.
static void open_channels(struct uapp *a) {
    if (g_job != JOB_NONE || !g_chs.count) return;
    int n = 0, sel = 0, internal = upd_show_internal();
    char mv[48], mb[48];
    upd_machine_build(mv, sizeof mv, mb, sizeof mb);
    if (!UGROW(g_choice, g_choice_cap, g_chs.count) ||
        !UGROW(g_choice_row, g_choice_row_cap, g_chs.count)) {
        ulogf("sysupdate: out of memory for %d channel rows\n", g_chs.count);
        return;
    }
    for (int i = 0; i < g_chs.count; i++) {
        const struct upd_channel *c = &g_chs.ch[i];
        struct choice_row *r = &g_choice_row[n];
        if (c->internal && !internal && i != g_chs.chosen) continue;
        snprintf(r->tags, sizeof r->tags, "%s%s%s", i == g_chs.dflt ? "default" : "",
                 i == g_chs.dflt && c->internal ? " " : "", c->internal ? "internal" : "");
        // The date AND time: two builds a day on one channel are normal.
        int tone = UUI_BADGE_PLAIN;
        r->badge = 0;
        if (c->version[0]) {
            snprintf(r->val, sizeof r->val, "%s%s%s", c->version,
                     c->built[0] ? " - " : "", c->built);
            if (mv[0]) {
                int cmp = upd_build_cmp(c->version, c->built, mv, mb);
                r->badge = cmp > 0 ? "newer" : cmp < 0 ? "older" : "this build";
                tone = cmp > 0 ? UUI_BADGE_ACCENT : cmp < 0 ? UUI_BADGE_WARN : UUI_BADGE_PLAIN;
            }
        } else {
            snprintf(r->val, sizeof r->val, "nothing yet");
        }
        g_choice[n] = (struct uui_choice){ c->title, r->tags, c->desc, r->val,
                                           r->badge, tone };
        ulogf("sysupdate: channel row %d %s '%s' %s\n", n, c->id, r->val,
              r->badge ? r->badge : "-");
        r->ch = i;
        if (i == g_chs.chosen) sel = n;
        n++;
    }
    uui_choicelist_init(&g_choices, g_choice, n, sel);
    if (mv[0])
        snprintf(g_dlg_line[0], sizeof g_dlg_line[0], "%s offers %d channel%s. This machine runs %s%s%s.",
                 g_server, n, n == 1 ? "" : "s", mv, mb[0] ? ", built " : "", mb);
    else
        snprintf(g_dlg_line[0], sizeof g_dlg_line[0], "%s offers %d channel%s.", g_server, n,
                 n == 1 ? "" : "s");
    static const struct uui_dialog_button b[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Use", DLG_USE, UUI_DLG_PRIMARY },
    };
    open_dialog(a, "Update channel", 1, b, 2, 1);
    int bw, bh;
    uui_choicelist_natural_size(&g_choices, &bw, &bh);
    if (bw < ugfx_char_w() * 52) bw = ugfx_char_w() * 52;
    uui_dialog_set_body(&g_dlg, &g_choices_item, bw, bh);
    struct uui_item *tabs[] = { &g_choices_item };
    uui_dialog_set_tabs(&g_dlg, tabs, 1);
    uui_dialog_focus(&g_dlg, &g_choices_item);
    ulogf("sysupdate: channel dialog, %d rows\n", n);
}

static void check_channel(struct uapp *a, int i, int go_back) {
    snprintf(g_want, sizeof g_want, "%s", g_chs.ch[i].id);
    upd_channel_set(g_chs.ch[i].id);
    g_go_back = go_back;
    ulogf("sysupdate: channel -> %s%s\n", g_chs.ch[i].id, go_back ? " (going back)" : "");
    start_job(a, JOB_CHECK);
}

// A channel was chosen. One whose build is OLDER than this machine's
// asks first: ChromeOS waits for the channel to catch up, Windows wants
// a reinstall; here only the differing files go back, so both are offered.
static void channel_chosen(struct uapp *a, int i) {
    const struct upd_channel *c = &g_chs.ch[i];
    char mv[48], mb[48];
    upd_machine_build(mv, sizeof mv, mb, sizeof mb);
    if (i == g_chs.chosen) return;
    if (!c->version[0] || upd_build_cmp(c->version, c->built, mv, mb) >= 0) {
        check_channel(a, i, 0);
        return;
    }
    g_back_to = i;
    snprintf(g_dlg_line[0], sizeof g_dlg_line[0], "This machine runs %s%s%s. %s's newest is %s%s%s.",
             mv, mb[0] ? ", built " : "", mb, c->title, c->version, c->built[0] ? " of " : "", c->built);
    snprintf(g_dlg_line[1], sizeof g_dlg_line[1],
             "Going back replaces only the files that differ. Your files in /etc and /home are kept.");
    static char wait[48], back[48];
    snprintf(wait, sizeof wait, "Wait for %s", mv);
    snprintf(back, sizeof back, "Go back to %s", c->version);
    static struct uui_dialog_button b[3];
    b[0] = (struct uui_dialog_button){ "Cancel", DLG_CANCEL, 0 };
    b[1] = (struct uui_dialog_button){ wait, DLG_WAIT, 0 };
    b[2] = (struct uui_dialog_button){ back, DLG_BACK, UUI_DLG_PRIMARY };
    static char title[64];
    snprintf(title, sizeof title, "%s is behind this machine", c->title);
    open_dialog(a, title, 2, b, 3, 1);
    ulogf("sysupdate: %s is behind this machine\n", c->id);
}

static void dialog_answer(struct uapp *a, int code) {
    if (g_dlg.body == &g_srv_item) {
        // A refused address keeps the dialog up, saying why.
        if (code == DLG_USE && !use_server(a)) { show_server(a); return; }
        uui_dialog_close(&g_dlg);
        uui_dialog_set_body(&g_dlg, 0, 0, 0);
        if (code == DLG_USE) {
            if (g_job == JOB_TEST) g_check_after_test = 1;
            else start_job(a, JOB_CHECK);
        }
        refresh(a);
        return;
    }
    int chooser = g_dlg.body == &g_choices_item;
    uui_dialog_close(&g_dlg);
    uui_dialog_set_body(&g_dlg, 0, 0, 0);
    if (chooser) {
        int r = g_choices.selected;
        if (code == DLG_USE && r >= 0 && r < g_choices.count) channel_chosen(a, g_choice_row[r].ch);
    } else if (g_back_to >= 0 && (code == DLG_WAIT || code == DLG_BACK)) {
        check_channel(a, g_back_to, code == DLG_BACK);
        g_back_to = -1;
    }
    refresh(a);
}

// ---- callbacks ----------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    // A fold opened or the notes scrolled: their rows moved, and a test
    // reads where from the layout log (ui/uui_describe.h).
    if (id == ID_NOTES && (reason == UUI_REASON_RELEASE || reason == UUI_REASON_WHEEL)) {
        uapp_log_layout(a, "sysupdate");
        return;
    }
    if (id == ID_DIALOG) {
        int code = uui_dialog_take_code(&g_dlg);
        if (code > 0) dialog_answer(a, code);
        uapp_redraw(a);
        return;
    }
    if (id == ID_CHOICES) {
        if (reason == UUI_REASON_PRESS) uui_dialog_focus(&g_dlg, &g_choices_item);
        uapp_redraw(a);
        return;
    }
    if (id == ID_SERVERS) {
        if (reason == UUI_REASON_PRESS) uui_dialog_focus(&g_dlg, &g_srv_items[3]);
        int r = g_servers.selected;
        if (r >= 0 && r < g_servers.count) uui_textbox_set_text(&g_edit, g_recent_urls[r]);
        uapp_redraw(a);
        return;
    }
    if (id == ID_SERVER_EDIT && reason == UUI_REASON_PRESS) {
        uui_dialog_focus(&g_dlg, &g_addr_items[0]);
        uapp_redraw(a);
        return;
    }
}

static void on_action(struct uapp *a, int code) {
    switch (code) {
    case ID_CHANGE:      open_server(a); break;
    case ID_CHANNEL:     open_channels(a); break;
    case ID_TEST:        start_test(a); break;
    case ID_CHECK:       start_job(a, JOB_CHECK); break;
    case ID_MAIN:
        if (g_view == V_CHECKING || g_view == V_INSTALLING) g_cancel = 1;
        else if (g_view == V_AVAILABLE) start_job(a, JOB_APPLY);
        else if (g_view == V_WAITING) { g_go_back = 1; start_job(a, JOB_APPLY); }
        else if (g_view == V_RESTART) { sys_sync(); uinitctl_shutdown(1); }
        break;
    }
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (uui_dialog_is_open(&g_dlg)) return;
    // Ctrl+PgUp/PgDn steps the tabs, as in the Terminal.
    if ((mods & KEY_MOD_CTRL) && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        int step = key == KEY_PAGE_DOWN ? 1 : PAGE_COUNT - 1;
        show_page(a, (g_tabs.selected + step) % PAGE_COUNT);
        return;
    }}

static int on_user(struct uapp *a, int kind, int arg) {
    if (kind == EV_WEB) return uui_webview_on_post(a, kind, arg);
    if (kind == EV_LOG) { drain_log(); return 1; }
    if (kind == EV_DONE) { drain_log(); job_done(a, arg); return 1; }
    if (kind == EV_PROGRESS) {
        if (g_job == JOB_CHECK && g_plan.count && g_row_count == 0) {
            snprintf(g_detail_text, sizeof g_detail_text, "Comparing %d files", g_plan.count);
            uui_label_set_text(&g_detail, g_detail_text);
        }
        if (g_job == JOB_APPLY) refresh(a);
        return 1;
    }
    return 0;
}

static int on_tick(struct uapp *a) {
    if (g_log_dialog) { g_log_dialog = 0; uapp_log_layout(a, "sysupdate"); }
    return uui_progress_tick(&g_bar);
}

// Closing mid-install would leave the worker writing into a process
// that is going away; ask it to stop first, and let the next close go.
static int on_close(struct uapp *a) {
    (void)a;
    if (g_job == JOB_NONE) {
        if (g_web_on) uui_webview_destroy(&g_web);
        return 1;
    }
    g_cancel = 1;
    return 0;
}

// Font-derived, so here rather than in main(): the session font is not
// mapped until uapp_run() fetches it.
static void on_size(int *w, int *h) {
    g_title.font = ugfx_font_session(UGFX_FONT_BOLD);
    *w = ugfx_char_w() * 76;
    *h = ugfx_char_h() * 28;
}

static void on_open(struct uapp *a) {
    g_app = a;
    g_tabs.ctx = a;
    if (g_web_on && uui_webview_setup(a, EV_WEB) != 0) ulogf("sysupdate: no page channel\n");
    start_job(a, JOB_CHECK);
}

int main(void) {
    if (!ugfx_font_init()) return 2;
    upd_server_get(g_server, sizeof g_server);

    uui_label_init(&g_title, g_title_text);
    uui_label_init(&g_detail, g_detail_text);
    uui_label_set_wrap(&g_detail, 2);
    uui_label_init(&g_server_key, "Server");
    uui_label_init(&g_server_val, g_server);
    uui_label_init(&g_chan_key, "Channel");
    uui_label_init(&g_chan_val, g_chan_text);
    uui_button_init(&g_chan_btn, 0, 0, 0, 0, "Change channel...", UUI_ROLE(control_bg), UUI_ROLE(text),
                    ID_CHANNEL);
    g_chan_btn.outlined = 1;
    uui_dialog_init(&g_dlg);
    g_chs.chosen = g_chs.dflt = -1;
    uui_label_init(&g_amount, g_amount_text);
    uui_label_init(&g_rate, g_rate_text);
    uui_label_init(&g_spacer, "");
    uui_button_init(&g_change, 0, 0, 0, 0, "Change server...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_CHANGE);
    uui_button_init(&g_test, 0, 0, 0, 0, "Test", UUI_ROLE(control_bg), UUI_ROLE(text), ID_TEST);
    uui_button_init(&g_check, 0, 0, 0, 0, "Check again", UUI_ROLE(control_bg), UUI_ROLE(text), ID_CHECK);
    uui_button_init(&g_main, 0, 0, 0, 0, "Restart now", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_MAIN);
    g_change.outlined = g_test.outlined = 1;
    g_check.outlined = 1;
    uui_textbox_init(&g_edit, g_server);
    uui_label_init(&g_addr_key, "Address");
    uui_label_init(&g_recent_key, "Recent");
    uui_label_init(&g_test_line, g_test_text);
    uui_label_init(&g_test_line2, g_test_text2);
    uui_progress_init(&g_bar);
    uui_table_init(&g_table, 0, 0, 0, 0, COLS, (int)(sizeof COLS / sizeof COLS[0]), cell, 0);
    uui_table_set_tint(&g_table, tint);
    uui_textview_init(&g_log, 0, 0, 0, 0, UUI_ROLE(text), UUI_ROLE(field_bg), UUI_ROLE(selection_bg),
                      g_log_buf, (int)sizeof g_log_buf);
    uui_markdown_init(&g_notes);
    g_web_on = uui_webview_available();
    if (g_web_on) {
        uui_webview_init(&g_web, &NOTES_HOST, NULL);
        g_web.scheme = 1;
        g_web.visible = 1;
        g_page_items[1].ops = &uui_webview_ops;
        g_page_items[1].widget = &g_web;
    }
    uui_tabs_init(&g_tabs, g_tab_list, PAGE_COUNT, 0);
    g_tabs.on_select = tab_selected;
    show_page(0, PAGE_NOTES);
    uui_focus_init(&g_focus, g_focusables, (int)(sizeof g_focusables / sizeof g_focusables[0]));

    struct uapp_desc desc = {
        .title = "System Update",
        .app_id = "sysupdate",
        .layout = &g_root,
        .widgets = g_items,
        .widget_count = sizeof g_items / sizeof g_items[0],
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_open = on_open,
        .on_user = on_user,
        .on_tick = on_tick,
        .tick_ms = 50,
        .on_close = on_close,
        .flags = UAPP_RESIZABLE | UAPP_SINGLE_INSTANCE,
        .on_size = on_size,
    };
    return uapp_run(&desc);
}
