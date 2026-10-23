// Search into subfolders: the "search:<folder>" virtual folder (a
// uui_fileview source) and the worker that fills it. Typing in the
// search box still FILTERS the folder you are in (files.c's
// pane_filter); Enter asks this to walk the tree below it, the way
// Dolphin's filter bar and its "From here (including subfolders)"
// search sit side by side. There is no index to ask, so the walk is
// the search -- on a thread of its own, posting as it finds things,
// with Stop.
//
// THE FILTERS NARROW, THE CONTENTS WIDEN: a row passes the type, date
// and size filters (lib/ufind.h) and then matches by name OR, with
// "Inside text files", by a line in it -- Explorer's search and
// Nautilus's full-text search. An empty query with a filter set lists
// everything the filter keeps ("every picture under here").
#include "fm_internal.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <pthread.h>
#include "kpath.h"
#include "lib/uwalk.h"
#include <caltime.h>
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"
#include "lib/ugrow.h"
#include "lib/ufind.h"
#include <time.h>

struct hit {
    char path[PATH_MAX_LEN];
    struct sys_dirent e;
    int line;                 // the matching line, from 1; 0 = matched by name
    char match[80];           // that line, trimmed to what a column shows
};

// WRITTEN BY THE WORKER under g_lock; read by the main thread under it.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hit *g_hits;                // grown as the walk finds them
static int g_nhits, g_hits_cap;
static int g_full;                        // memory ran out: there were more
static unsigned long long g_seen;         // entries looked at
static char g_where[PATH_MAX_LEN];        // the directory being read, for the strip

static char g_root[PATH_MAX_LEN], g_query[UUI_TEXTBOX_MAX];
static struct ufind_filter g_filter = UFIND_FILTER_ANY;
static int g_contents;                    // "Inside text files"
static long long g_now;                   // the date filter's today, asked once
static volatile int g_running, g_cancel;
static pthread_t g_thread;
static struct uwalk g_walk;               // static: several KB
static unsigned long long g_last_post;

// What the listing last copied out, so `path` and `cell` answer for the
// rows the pane holds even while the worker keeps adding.
static struct hit *g_shown;
static int g_nshown, g_shown_cap;


static int matches(const char *name) {
    int qn = (int)strlen(g_query), n = (int)strlen(name);
    if (!qn) return 1;   // a filter alone
    for (int i = 0; i + qn <= n; i++)
        if (!strncasecmp(name + i, g_query, (size_t)qn)) return 1;
    return 0;
}

static int visit(void *ctx, const char *path, const struct sys_dirent *e) {
    (void)ctx;
    if (g_cancel) return -1;
    // Hidden names the way the panes hide them -- and a bin is never
    // searched: what is in it was deleted.
    if (e->name[0] == '.' && (!g_opt.folder.hidden || !strncmp(e->name, ".Trash", 6))) return 0;
    if (!ufind_matches(&g_filter, e, g_now)) return 1;
    int line = 0;
    char text[sizeof g_hits[0].match];
    text[0] = '\0';
    if (matches(e->name) ||
        (g_contents && !e->is_dir && g_query[0] &&
         ufind_contains(path, g_query, &line, text, sizeof text, &g_cancel))) {
        pthread_mutex_lock(&g_lock);
        if (!g_full && UGROW(g_hits, g_hits_cap, g_nhits + 1)) {
            strlcpy(g_hits[g_nhits].path, path, PATH_MAX_LEN);
            g_hits[g_nhits].e = *e;
            g_hits[g_nhits].line = line;
            strlcpy(g_hits[g_nhits].match, text, sizeof g_hits[0].match);
            g_nhits++;
        } else if (!g_full) {
            g_full = 1;
            ulogf("files: out of memory at %d search results\n", g_nhits);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return 1;
}

static void *worker(void *arg) {
    (void)arg;
    if (uwalk_begin(&g_walk, g_root)) {
        while (!g_cancel && uwalk_step(&g_walk, visit, 0)) {
            pthread_mutex_lock(&g_lock);
            g_seen = g_walk.entries;
            if (g_walk.depth > 0) strlcpy(g_where, g_walk.frame[g_walk.depth - 1].path, sizeof g_where);
            pthread_mutex_unlock(&g_lock);
            // Posted at most every ~150 ms: a post per directory would
            // be thousands of events for one search.
            unsigned long long now = sys_monotonic_ns();
            if (now - g_last_post > 150000000ull) {
                g_last_post = now;
                uapp_post(g_app, POST_SEARCH, 0);
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    g_seen = g_walk.entries;
    pthread_mutex_unlock(&g_lock);
    g_running = 0;
    uapp_post(g_app, POST_SEARCH, 1);
    return 0;
}

void search_stop(void) {
    if (!g_running) return;
    g_cancel = 1;
    // The walk checks the flag per entry, so this is short; joined so a
    // new search never shares the state with an old one.
    pthread_join(g_thread, 0);
    g_running = 0;
}

// BEFORE the results are listed: which shape the folder takes
// (search_source) follows `contents`, and a listing can resolve it before
// the walk starts.
void search_set_filter(const struct ufind_filter *f, int contents) {
    search_stop();   // the worker reads them
    g_filter = *f;
    g_contents = contents;
}

void search_start(const char *root, const char *query) {
    search_stop();
    pthread_mutex_lock(&g_lock);
    g_nhits = g_full = 0;
    g_seen = 0;
    g_where[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    strlcpy(g_root, root, sizeof g_root);
    strlcpy(g_query, query, sizeof g_query);
    g_now = (long long)time(0);
    g_cancel = 0;
    g_running = 1;
    g_last_post = 0;
    ulogf("files: search start \"%s\" under %s kind %d age %d size %d contents %d\n",
          g_query, g_root, g_filter.kind, g_filter.age, g_filter.size, g_contents);
    if (pthread_create(&g_thread, 0, worker, 0) != 0) {
        g_running = 0;
        set_note("could not start the search");
    }
}

int search_running(void) { return g_running; }

// What a finished search found, for a test to read: the first few, by
// path and matching line (0 for a name match), so a big search costs the
// log a handful of lines rather than one per file.
void search_log_hits(void) {
    enum { SHOW = 16 };
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nhits && i < SHOW; i++)
        ulogf("files: search hit %d %s\n", g_hits[i].line, g_hits[i].path);
    if (g_nhits > SHOW) ulogf("files: search hit and %d more\n", g_nhits - SHOW);
    pthread_mutex_unlock(&g_lock);
}

// The strip's words: what is happening, or what came of it.
void search_status(char *out, int cap) {
    pthread_mutex_lock(&g_lock);
    int n = g_nhits, full = g_full;
    unsigned long long seen = g_seen;
    pthread_mutex_unlock(&g_lock);
    if (g_running)
        snprintf(out, (size_t)cap, "Searching... %d found, %llu looked at", n, seen);
    else if (full)
        snprintf(out, (size_t)cap, "The first %d found, of %llu looked at", n, seen);
    else
        snprintf(out, (size_t)cap, "%d found, %llu looked at", n, seen);
}

// --- the virtual folder ---------------------------------------------------

// TWO SHAPES OF ONE FOLDER, told apart by the source's ctx: a contents
// search puts Match second, as a pane too narrow for every column drops
// them from the end and the line found is that search's answer.
enum { C_NAME, C_SIZE, C_MODIFIED, C_FOLDER, C_MATCH };
static const struct uui_table_column search_cols[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 20, UUI_TALIGN_LEFT  },
    { "Folder",   20, UUI_TALIGN_LEFT  },
};
static const struct uui_table_column text_cols[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Match",    30, UUI_TALIGN_LEFT  },
    { "Folder",   20, UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 20, UUI_TALIGN_LEFT  },
};
static const int TEXT_FACT[] = { C_NAME, C_MATCH, C_FOLDER, C_SIZE, C_MODIFIED };

static int fact(void *ctx, int col) {
    return ctx && col >= 0 && col < (int)(sizeof TEXT_FACT / sizeof TEXT_FACT[0]) ? TEXT_FACT[col] : col;
}

static int s_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx; (void)dir;
    pthread_mutex_lock(&g_lock);
    g_nshown = g_nhits < cap ? g_nhits : cap;
    if (!UGROW(g_shown, g_shown_cap, g_nshown)) g_nshown = g_shown_cap;
    if (g_nshown) memcpy(g_shown, g_hits, sizeof g_shown[0] * (size_t)g_nshown);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_nshown; i++) out[i] = g_shown[i].e;
    return g_nshown;
}

static int s_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_nshown && (int)strlcpy(out, g_shown[i].path, (size_t)cap) < cap;
}

static void s_cell(void *ctx, int i, int col, char *out, int cap) {
    col = fact(ctx, col);
    out[0] = '\0';
    if (i < 0 || i >= g_nshown) return;
    const struct sys_dirent *e = &g_shown[i].e;
    if (col == C_SIZE && !e->is_dir) human_size(out, (unsigned long)cap, e->size);
    else if (col == C_MODIFIED) udate_format(out, (unsigned long)cap, &e->modified, UDATE_DATE | UDATE_TIME);
    else if (col == C_FOLDER) k_path_dirname(g_shown[i].path, out, (size_t)cap);
    else if (col == C_MATCH && g_shown[i].line)
        snprintf(out, (size_t)cap, "line %d: %s", g_shown[i].line, g_shown[i].match);
}

// By the value, not the text: "9K" sorts after "10K" as a string.
static int s_compare(void *ctx, int a, int b, int col) {
    col = fact(ctx, col);
    const struct hit *x = &g_shown[a], *y = &g_shown[b];
    if (col == C_SIZE) return x->e.size < y->e.size ? -1 : x->e.size > y->e.size;
    if (col == C_MATCH) return strcasecmp(x->match, y->match);
    if (col == C_MODIFIED) {
        uint64_t tx = cal_rtc_to_epoch(&x->e.modified), ty = cal_rtc_to_epoch(&y->e.modified);
        return tx < ty ? -1 : tx > ty;
    }
    return strcmp(x->path, y->path);
}

static const struct uui_fileview_source g_src = {
    .list = s_list, .path = s_path,
    .cols = search_cols, .ncols = 4,
    .cell = s_cell, .compare = s_compare,
};
static const struct uui_fileview_source g_src_text = {
    .ctx = (void *)1,
    .list = s_list, .path = s_path,
    .cols = text_cols, .ncols = 5,
    .cell = s_cell, .compare = s_compare,
};

const struct uui_fileview_source *search_source(void) { return g_contents ? &g_src_text : &g_src; }

int in_search(const struct uui_fileview *fv) {
    const struct uui_fileview_source *s = uui_fileview_source(fv);
    return s == &g_src || s == &g_src_text;
}

// "search:/usr" -> "/usr": the folder a search view searches.
const char *search_scope(const char *dir) {
    return strncmp(dir, FM_SEARCH, sizeof FM_SEARCH - 1) == 0 ? dir + sizeof FM_SEARCH - 1 : 0;
}

// Nothing is created, pasted or renamed in a list of results: they live
// in many folders, and "here" is none of them.
int search_item_flags(int code, unsigned *out) {
    if (!in_search(active())) return 0;
    switch (code) {
    case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE: case CMD_CLIP_PASTE:
    case CMD_RENAME: case CMD_MOVE: case CMD_COPY:
        *out = UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}
