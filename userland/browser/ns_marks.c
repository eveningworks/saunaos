// Bookmarks and history. See ns_marks.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "utils/errors.h"
#include "browser/ns_marks.h"

#include "content/urldb.h"
#include "desktop/hotlist.h"
#include "netsurf/url_db.h"
#include "utils/nsurl.h"

static char g_url[NS_MARKS_MAX][1024];   // row i's address, for the last rows built
static int g_ready;

int ns_marks_init(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        // An empty list NetSurf can load: without a file it would make
        // its own default links (hotlist_generate()).
        FILE *f = fopen(path, "w");
        if (f) {
            fputs("<!DOCTYPE html>\n<html>\n<head>\n<title>Bookmarks</title>\n</head>\n"
                  "<body>\n<ul>\n</ul>\n</body>\n</html>\n", f);
            fclose(f);
        }
    }
    g_ready = hotlist_init(path, path) == NSERROR_OK;
    return g_ready;
}

void ns_marks_fini(void) {
    if (g_ready) hotlist_fini();
    g_ready = 0;
}

static struct nsurl *url_of(const char *s) {
    struct nsurl *u = NULL;
    return s && s[0] && nsurl_create(s, &u) == NSERROR_OK ? u : NULL;
}

int ns_bookmarked(const char *url) {
    struct nsurl *u = url_of(url);
    int on = g_ready && u && hotlist_has_url(u);
    if (u) nsurl_unref(u);
    return on;
}

int ns_bookmark_toggle(const char *url, const char *title) {
    struct nsurl *u = url_of(url);
    if (!u || !g_ready) {
        if (u) nsurl_unref(u);
        return 0;
    }
    int on = hotlist_has_url(u);
    if (on) hotlist_remove_url(u);
    else hotlist_add_entry(u, title && title[0] ? title : url, false, 0);
    nsurl_unref(u);
    return !on;
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

// --- one walk's state: neither iterator takes a context for us ----------------

static struct uui_entry *g_rows;
static int g_cap, g_n;
static const char *g_filter;

static nserror bm_address(void *ctx, struct nsurl *url, const char *title) {
    (void)ctx;
    const char *s = nsurl_access(url);
    if (g_n >= g_cap || !matches(g_filter, title, s)) return NSERROR_OK;
    struct uui_entry *r = &g_rows[g_n];
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "%s", title && title[0] ? title : s);
    shown_url(r->sub, sizeof r->sub, s);
    snprintf(g_url[g_n], sizeof g_url[0], "%s", s);
    g_n++;
    return NSERROR_OK;
}

struct visit { time_t when; char title[96]; char url[1024]; };
static struct visit *g_visits;
static int g_nvisits, g_capvisits;

static bool hist_entry(struct nsurl *url, const struct url_data *d) {
    if (!d || !d->visits || !d->last_visit) return true;   // fetched, never browsed to
    const char *s = nsurl_access(url);
    if (!matches(g_filter, d->title, s)) return true;
    if (g_nvisits == g_capvisits) {
        int cap = g_capvisits ? g_capvisits * 2 : 64;
        if (cap > 4096) return false;
        struct visit *v = realloc(g_visits, (size_t)cap * sizeof *v);
        if (!v) return false;
        g_visits = v;
        g_capvisits = cap;
    }
    struct visit *v = &g_visits[g_nvisits++];
    v->when = d->last_visit;
    snprintf(v->title, sizeof v->title, "%s", d->title && d->title[0] ? d->title : s);
    snprintf(v->url, sizeof v->url, "%s", s);
    return true;
}

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
    g_rows = rows;
    g_cap = cap;
    g_n = 0;
    g_filter = filter;
    if (which == NS_MARKS_BOOKMARKS) {
        if (g_ready) hotlist_iterate(NULL, NULL, bm_address, NULL);
        return g_n;
    }
    g_nvisits = 0;
    urldb_iterate_entries(hist_entry);
    qsort(g_visits, (size_t)g_nvisits, sizeof g_visits[0], newest_first);
    time_t now = time(NULL);
    char last[48] = "";
    for (int i = 0; i < g_nvisits && g_n < cap; i++) {
        char day[48];
        day_heading(g_visits[i].when, now, day, sizeof day);
        if (strcmp(day, last)) {
            if (g_n + 1 >= cap) break;
            memset(&rows[g_n], 0, sizeof rows[0]);
            snprintf(rows[g_n].title, sizeof rows[0].title, "%s", day);
            rows[g_n].heading = 1;
            g_url[g_n][0] = '\0';
            g_n++;
            snprintf(last, sizeof last, "%s", day);
        }
        struct uui_entry *r = &rows[g_n];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", g_visits[i].title);
        shown_url(r->sub, sizeof r->sub, g_visits[i].url);
        snprintf(g_url[g_n], sizeof g_url[0], "%s", g_visits[i].url);
        g_n++;
    }
    return g_n;
}

const char *ns_marks_url(int row) { return row >= 0 && row < NS_MARKS_MAX ? g_url[row] : ""; }

void ns_marks_remove(int which, int row) {
    struct nsurl *u = url_of(ns_marks_url(row));
    if (!u) return;
    if (which == NS_MARKS_BOOKMARKS) {
        if (g_ready) hotlist_remove_url(u);
    } else {
        urldb_reset_url_visit_data(u);
    }
    nsurl_unref(u);
}
