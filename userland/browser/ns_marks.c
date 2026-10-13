// Bookmarks and history. See ns_marks.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "browser/ns_marks.h"
#include "lib/ubookmarks.h"
#include "lib/uvisits.h"
#include "ui/ulog.h"

static char g_url[NS_MARKS_MAX][1024];   // row i's address, for the last rows built
static struct ubookmarks g_marks;
static struct uvisits g_visits_db;
static char g_marks_path[256], g_hist_path[256];
static int g_ready, g_marks_dirty;

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int ns_marks_init(const char *dir) {
    mkdir("/var/lib", 0755);
    mkdir(dir, 0755);
    snprintf(g_marks_path, sizeof g_marks_path, "%s/bookmarks.tsv", dir);
    snprintf(g_hist_path, sizeof g_hist_path, "%s/history.tsv", dir);
    time_t now = time(NULL);
    // NetSurf's own files, from before the engine left this process, are
    // read the first time and then left alone.
    if (!exists(g_marks_path)) {
        char old[256];
        snprintf(old, sizeof old, "%s/bookmarks", dir);
        int n = ubookmarks_import_netsurf(&g_marks, old);
        if (n) ulogf("browser: %d bookmarks taken from NetSurf's list\n", n);
        g_marks_dirty = 1;
        g_ready = 1;
    } else {
        g_ready = ubookmarks_load(&g_marks, g_marks_path) == 0;
    }
    if (!exists(g_hist_path)) {
        char old[256];
        snprintf(old, sizeof old, "%s/history", dir);
        int n = uvisits_import_netsurf(&g_visits_db, old, now);
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
    if (g_visits_db.dirty) uvisits_save(&g_visits_db, g_hist_path);
}

void ns_marks_visited(const char *url, const char *title) {
    uvisits_visit(&g_visits_db, url, title, time(NULL));
    // Saved as it happens: a browser killed with its window loses nothing.
    uvisits_save(&g_visits_db, g_hist_path);
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

int ns_marks_rows(int which, const char *filter, struct uui_entry *rows, int cap) {
    if (cap > NS_MARKS_MAX) cap = NS_MARKS_MAX;
    int n = 0;
    if (which == NS_MARKS_BOOKMARKS) {
        for (int i = 0; i < g_marks.n && n < cap; i++) {
            const struct ubookmark *b = &g_marks.b[i];
            if (!matches(filter, b->title, b->url)) continue;
            memset(&rows[n], 0, sizeof rows[0]);
            snprintf(rows[n].title, sizeof rows[0].title, "%s", b->title);
            shown_url(rows[n].sub, sizeof rows[0].sub, b->url);
            snprintf(g_url[n], sizeof g_url[0], "%s", b->url);
            n++;
        }
        return n;
    }
    if (g_capvisits < g_visits_db.n) {
        struct visit *v = realloc(g_visits, (size_t)g_visits_db.n * sizeof *v);
        if (!v) return 0;
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
    for (int i = 0; i < g_nvisits && n < cap; i++) {
        char day[48];
        day_heading(g_visits[i].when, now, day, sizeof day);
        if (strcmp(day, last)) {
            if (n + 1 >= cap) break;
            memset(&rows[n], 0, sizeof rows[0]);
            snprintf(rows[n].title, sizeof rows[0].title, "%s", day);
            rows[n].heading = 1;
            g_url[n][0] = '\0';
            n++;
            snprintf(last, sizeof last, "%s", day);
        }
        struct uui_entry *r = &rows[n];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", g_visits[i].title);
        shown_url(r->sub, sizeof r->sub, g_visits[i].url);
        snprintf(g_url[n], sizeof g_url[0], "%s", g_visits[i].url);
        n++;
    }
    return n;
}

const char *ns_marks_url(int row) { return row >= 0 && row < NS_MARKS_MAX ? g_url[row] : ""; }

void ns_marks_remove(int which, int row) {
    const char *url = ns_marks_url(row);
    if (!url[0]) return;
    if (which == NS_MARKS_BOOKMARKS) {
        int i = ubookmarks_find(&g_marks, url);
        if (i >= 0) {
            ubookmarks_remove(&g_marks, i);
            save_marks();
        }
    } else if (uvisits_forget(&g_visits_db, url)) {
        uvisits_save(&g_visits_db, g_hist_path);
    }
}
