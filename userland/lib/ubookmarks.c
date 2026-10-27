// ubookmarks -- see lib/ubookmarks.h.
#include "lib/ubookmarks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAGIC "# SaunaOS bookmarks 1"

void ubookmarks_free(struct ubookmarks *bm) {
    for (int i = 0; i < bm->n; i++) {
        free(bm->b[i].url);
        free(bm->b[i].title);
    }
    free(bm->b);
    memset(bm, 0, sizeof *bm);
}

int ubookmarks_find(const struct ubookmarks *bm, const char *url) {
    for (int i = 0; i < bm->n; i++)
        if (!strcmp(bm->b[i].url, url)) return i;
    return -1;
}

// A tab or a line break would end the field it is in.
static char *clean_dup(const char *s) {
    char *d = strdup(s ? s : "");
    for (char *p = d; p && *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    return d;
}

int ubookmarks_add(struct ubookmarks *bm, const char *url, const char *title) {
    if (!url || !url[0]) return -1;
    if (ubookmarks_find(bm, url) >= 0) return 0;
    if (bm->n == bm->cap) {
        int cap = bm->cap ? bm->cap * 2 : 32;
        struct ubookmark *g = realloc(bm->b, (size_t)cap * sizeof *g);
        if (!g) return -1;
        bm->b = g;
        bm->cap = cap;
    }
    struct ubookmark *b = &bm->b[bm->n];
    b->url = clean_dup(url);
    b->title = clean_dup(title && title[0] ? title : url);
    if (!b->url || !b->title) {
        free(b->url);
        free(b->title);
        return -1;
    }
    bm->n++;
    return 0;
}

void ubookmarks_remove(struct ubookmarks *bm, int i) {
    if (i < 0 || i >= bm->n) return;
    free(bm->b[i].url);
    free(bm->b[i].title);
    memmove(&bm->b[i], &bm->b[i + 1], (size_t)(bm->n - i - 1) * sizeof bm->b[0]);
    bm->n--;
}

int ubookmarks_load(struct ubookmarks *bm, const char *path) {
    memset(bm, 0, sizeof *bm);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char line[8192 + 512];
    int ok = fgets(line, sizeof line, f) && !strncmp(line, FILE_MAGIC, strlen(FILE_MAGIC));
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        ubookmarks_add(bm, line, tab + 1);
    }
    fclose(f);
    return ok ? 0 : -1;
}

int ubookmarks_save(const struct ubookmarks *bm, const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", FILE_MAGIC);
    for (int i = 0; i < bm->n; i++) fprintf(f, "%s\t%s\n", bm->b[i].url, bm->b[i].title);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

// The few entities a title in NetSurf's file can hold, decoded in place.
static void unescape(char *s) {
    static const struct { const char *e; char c; } E[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' },
    };
    char *w = s;
    for (char *r = s; *r;) {
        size_t i = 0;
        for (; i < sizeof E / sizeof E[0]; i++)
            if (!strncmp(r, E[i].e, strlen(E[i].e))) break;
        if (i < sizeof E / sizeof E[0]) {
            *w++ = E[i].c;
            r += strlen(E[i].e);
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int ubookmarks_import_netsurf(struct ubookmarks *bm, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t cap = 65536, n = 0;
    char *text = malloc(cap);
    for (size_t got; text && (got = fread(text + n, 1, cap - n - 1, f)) > 0;) {
        n += got;
        if (n + 1 == cap) {
            char *g = realloc(text, cap * 2);
            if (!g) break;
            text = g;
            cap *= 2;
        }
    }
    fclose(f);
    if (!text) return 0;
    text[n] = '\0';
    int added = 0;
    for (char *p = text; (p = strstr(p, "<a href=\"")) != NULL;) {
        p += 9;
        char *q = strchr(p, '"');
        char *gt = q ? strchr(q, '>') : NULL;
        char *end = gt ? strstr(gt, "</a>") : NULL;
        if (!end) break;
        *q = '\0';
        *end = '\0';
        unescape(p);
        unescape(gt + 1);
        if (ubookmarks_add(bm, p, gt + 1) == 0) added++;
        p = end + 4;
    }
    free(text);
    return added;
}
