// uvisits -- see lib/uvisits.h.
#include "lib/uvisits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAGIC "# SaunaOS history 1"

void uvisits_free(struct uvisits *h) {
    for (int i = 0; i < h->n; i++) {
        free(h->e[i].url);
        free(h->e[i].title);
    }
    free(h->e);
    memset(h, 0, sizeof *h);
}

static int find(const struct uvisits *h, const char *url) {
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->e[i].url, url)) return i;
    return -1;
}

static void drop(struct uvisits *h, int i) {
    free(h->e[i].url);
    free(h->e[i].title);
    h->e[i] = h->e[--h->n];
    h->dirty = 1;
}

static char *clean_dup(const char *s) {
    char *d = strdup(s ? s : "");
    for (char *p = d; p && *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    return d;
}

// Makes or updates the entry; the oldest makes room past the limit.
static void put(struct uvisits *h, const char *url, const char *title, int64_t when, int visits) {
    if (!url || !url[0]) return;
    int i = find(h, url);
    if (i >= 0) {
        struct uvisit *e = &h->e[i];
        if (when > e->when) e->when = when;
        e->visits += visits;
        if (title && title[0]) {
            free(e->title);
            e->title = clean_dup(title);
        }
        h->dirty = 1;
        return;
    }
    if (h->n >= UVISITS_MAX) {
        int old = 0;
        for (int k = 1; k < h->n; k++) if (h->e[k].when < h->e[old].when) old = k;
        drop(h, old);
    }
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 64;
        struct uvisit *g = realloc(h->e, (size_t)cap * sizeof *g);
        if (!g) return;
        h->e = g;
        h->cap = cap;
    }
    struct uvisit *e = &h->e[h->n];
    e->url = clean_dup(url);
    e->title = clean_dup(title);
    e->when = when;
    e->visits = visits;
    if (!e->url || !e->title) {
        free(e->url);
        free(e->title);
        return;
    }
    h->n++;
    h->dirty = 1;
}

void uvisits_visit(struct uvisits *h, const char *url, const char *title, int64_t now) {
    put(h, url, title, now, 1);
}

int uvisits_forget(struct uvisits *h, const char *url) {
    int i = find(h, url);
    if (i < 0) return 0;
    drop(h, i);
    return 1;
}

int uvisits_load(struct uvisits *h, const char *path, int64_t now) {
    memset(h, 0, sizeof *h);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char line[8192 + 512];
    int ok = fgets(line, sizeof line, f) && !strncmp(line, FILE_MAGIC, strlen(FILE_MAGIC));
    int64_t oldest = now - (int64_t)UVISITS_KEEP_DAYS * 86400;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *f2 = strchr(line, '\t'), *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
        char *f4 = f3 ? strchr(f3 + 1, '\t') : NULL;
        if (!f4) continue;
        *f2 = *f3 = *f4 = '\0';
        int64_t when = strtoll(line, NULL, 10);
        if (when < oldest) continue;
        put(h, f3 + 1, f4 + 1, when, atoi(f2 + 1));
    }
    fclose(f);
    h->dirty = 0;
    return ok ? 0 : -1;
}

int uvisits_save(struct uvisits *h, const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", FILE_MAGIC);
    for (int i = 0; i < h->n; i++)
        fprintf(f, "%lld\t%d\t%s\t%s\n", (long long)h->e[i].when, h->e[i].visits, h->e[i].url,
                h->e[i].title);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    h->dirty = 0;
    return 0;
}

// One line of the file without its newline; 0 at the end.
static int getline_cut(char *buf, int cap, FILE *f) {
    if (!fgets(buf, cap, f)) return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

int uvisits_import_netsurf(struct uvisits *h, const char *path, int64_t now) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char host[512], line[8][4200];
    int version = getline_cut(host, sizeof host, f) ? atoi(host) : 0;
    int n = 0;
    int64_t oldest = now - (int64_t)UVISITS_KEEP_DAYS * 86400;
    // A host line ("host [hsts...]"), a count, then 8 lines per address:
    // scheme, port, path, visits, last visit, type, (an unused line), title.
    while (version >= 106 && version <= 107 && getline_cut(host, sizeof host, f)) {
        char count[32];
        if (!getline_cut(count, sizeof count, f)) break;
        host[strcspn(host, " ")] = '\0';
        int urls = atoi(count);
        for (int i = 0; i < urls; i++) {
            int k = 0;
            while (k < 8 && getline_cut(line[k], sizeof line[k], f)) k++;
            if (k < 8) break;
            int visits = atoi(line[3]);
            int64_t when = strtoll(line[4], NULL, 10);
            if (!host[0] || visits <= 0 || when < oldest) continue;
            static char url[4400];
            int file = !strcasecmp(host, "localhost") && !strcasecmp(line[0], "file");
            snprintf(url, sizeof url, "%s://%s%s%s%s", line[0], file ? "" : host,
                     atoi(line[1]) ? ":" : "", atoi(line[1]) ? line[1] : "", line[2]);
            put(h, url, line[7], when, visits);
            n++;
        }
    }
    fclose(f);
    return n;
}
