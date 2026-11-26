// Bookmarks and history. See ns_marks.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "apps/apps/browser/ns_marks.h"
#include "net/ubookmarks.h"
#include "net/uvisits.h"
#include "base/ulog.h"
#include "base/ugrow.h"

// The last rows built and each row's address (heap, "" for a heading),
// grown to the list (base/ugrow.h).
static struct uui_entry *g_rows;
static char **g_url;
static int g_nrows, g_rows_cap, g_url_cap;
static struct ubookmarks g_marks;
static struct uvisits g_visits_db;
static char *g_marks_path, *g_hist_path;   // heap, set once
static int g_ready, g_marks_dirty;

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int ns_marks_init(const char *dir) {
    mkdir(dir, 0755);
    if (asprintf(&g_marks_path, "%s/bookmarks.tsv", dir) < 0 || asprintf(&g_hist_path, "%s/history.tsv", dir) < 0)
        return 0;
    time_t now = time(NULL);
    // NetSurf's own files, from before the engine left this process, are
    // read the first time and then left alone.
    if (!exists(g_marks_path)) {
        char *old = 0;
        int n = asprintf(&old, "%s/bookmarks", dir) < 0 ? 0 : ubookmarks_import_netsurf(&g_marks, old);
        free(old);
        if (n) ulogf("browser: %d bookmarks taken from NetSurf's list\n", n);
        g_marks_dirty = 1;
        g_ready = 1;
    } else {
        g_ready = ubookmarks_load(&g_marks, g_marks_path) == 0;
    }
    if (!exists(g_hist_path)) {
        char *old = 0;
        int n = asprintf(&old, "%s/history", dir) < 0 ? 0 : uvisits_import_netsurf(&g_visits_db, old, now);
        free(old);
        if (n) ulogf("browser: %d history entries taken from NetSurf's\n", n);
    } else {
        uvisits_load(&g_visits_db, g_hist_path, now);
    }
    return g_ready;
}

static void save_marks(void) {
    if (g_ready && ubookmarks_save(&g_marks, g_marks_path) == 0) g_marks_dirty = 0;
}

void ns_marks_fini(void) {
    if (g_marks_dirty) save_marks();
    if (g_visits_db.dirty && g_hist_path) uvisits_save(&g_visits_db, g_hist_path);
}

void ns_marks_visited(const char *url, const char *title) {
    uvisits_visit(&g_visits_db, url, title, time(NULL));
    // Saved as it happens: a browser killed with its window loses nothing.
    if (g_hist_path) uvisits_save(&g_visits_db, g_hist_path);
}

int ns_bookmarked(const char *url) { return url && url[0] && ubookmarks_find(&g_marks, url) >= 0; }

int ns_bookmark_toggle(const char *url, const char *title) {
    if (!url || !url[0] || !g_ready) return 0;
    int i = ubookmarks_find(&g_marks, url);
    if (i >= 0) ubookmarks_remove(&g_marks, i);
    else ubookmarks_add(&g_marks, url, title && title[0] ? title : url);
    save_marks();
    return i < 0;
}

static int matches(const char *filter, const char *title, const char *url) {
    if (!filter || !filter[0]) return 1;
    size_t n = strlen(filter);
    for (const char *s = title; s && *s; s++)
        if (!strncasecmp(s, filter, n)) return 1;
    for (const char *s = url; s && *s; s++)
        if (!strncasecmp(s, filter, n)) return 1;
    return 0;
}

// The row's dim line: the address without its scheme, as a person reads it.
static void shown_url(char *out, size_t cap, const char *url) {
    const char *p = strstr(url, "://");
    snprintf(out, cap, "%s", p ? p + 3 : url);
}

struct visit { time_t when; const char *title, *url; };
static struct visit *g_visits;
static int g_nvisits, g_capvisits;

static int newest_first(const void *a, const void *b) {
    time_t x = ((const struct visit *)a)->when, y = ((const struct visit *)b)->when;
    return x < y ? 1 : x > y ? -1 : 0;
}

static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June",
                                        "July", "August", "September", "October", "November",
                                        "December" };

// "Today", "Yesterday", or the date.
static void day_heading(time_t t, time_t now, char *out, size_t cap) {
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    time_t yday = now - 86400;
    struct tm y;
    localtime_r(&yday, &y);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, cap, "Today");
    else if (a.tm_year == y.tm_year && a.tm_yday == y.tm_yday) snprintf(out, cap, "Yesterday");
    else snprintf(out, cap, "%d %s %d", a.tm_mday, MONTHS[a.tm_mon % 12], a.tm_year + 1900);
}

// One more row: its title (and dim line, from `url`), or a heading.
static int add_row(const char *title, const char *url, int heading) {
    if (!UGROW(g_rows, g_rows_cap, g_nrows + 1) || !UGROW(g_url, g_url_cap, g_nrows + 1)) return 0;
    char *u = strdup(url ? url : "");
    if (!u) return 0;
    struct uui_entry *r = &g_rows[g_nrows];
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "%s", title);   // a line shown: cut is fine
    if (url) shown_url(r->sub, sizeof r->sub, url);
    r->heading = heading;
    g_url[g_nrows++] = u;
    return 1;
}

static int rows_out(struct uui_entry **rows, int ok) {
    *rows = g_rows;
    if (!ok) ulogf("browser: out of memory at %d sidebar rows\n", g_nrows);
    return ok ? g_nrows : -1;
}

int ns_marks_rows(int which, const char *filter, struct uui_entry **rows) {
    for (int i = 0; i < g_nrows; i++) free(g_url[i]);
    g_nrows = 0;
    int ok = 1;
    if (which == NS_MARKS_BOOKMARKS) {
        for (int i = 0; i < g_marks.n && ok; i++) {
            const struct ubookmark *b = &g_marks.b[i];
            if (matches(filter, b->title, b->url)) ok = add_row(b->title, b->url, 0);
        }
        return rows_out(rows, ok);
    }
    if (g_capvisits < g_visits_db.n) {
        struct visit *v = realloc(g_visits, (size_t)g_visits_db.n * sizeof *v);
        if (!v) return rows_out(rows, 0);
        g_visits = v;
        g_capvisits = g_visits_db.n;
    }
    g_nvisits = 0;
    for (int i = 0; i < g_visits_db.n; i++) {
        const struct uvisit *e = &g_visits_db.e[i];
        if (!matches(filter, e->title, e->url)) continue;
        g_visits[g_nvisits++] = (struct visit){ (time_t)e->when, e->title[0] ? e->title : e->url, e->url };
    }
    qsort(g_visits, (size_t)g_nvisits, sizeof g_visits[0], newest_first);
    time_t now = time(NULL);
    char last[48] = "";
    for (int i = 0; i < g_nvisits && ok; i++) {
        char day[48];
        day_heading(g_visits[i].when, now, day, sizeof day);
        if (strcmp(day, last)) {
            ok = add_row(day, 0, 1);
            snprintf(last, sizeof last, "%s", day);
        }
        if (ok) ok = add_row(g_visits[i].title, g_visits[i].url, 0);
    }
    return rows_out(rows, ok);
}

const char *ns_marks_url(int row) { return row >= 0 && row < g_nrows ? g_url[row] : ""; }

void ns_marks_remove(int which, int row) {
    const char *url = ns_marks_url(row);
    if (!url[0]) return;
    if (which == NS_MARKS_BOOKMARKS) {
        int i = ubookmarks_find(&g_marks, url);
        if (i >= 0) {
            ubookmarks_remove(&g_marks, i);
            save_marks();
        }
    } else if (uvisits_forget(&g_visits_db, url) && g_hist_path) {
        uvisits_save(&g_visits_db, g_hist_path);
    }
}
