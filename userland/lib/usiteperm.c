// Per-site permissions: see usiteperm.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/usiteperm.h"

static const char *word(int v) { return v == USITEPERM_ALLOW ? "allow" : "block"; }

static struct usiteperm_entry *grow(struct usiteperm *t) {
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        struct usiteperm_entry *e = realloc(t->e, (size_t)cap * sizeof *e);
        if (!e) return NULL;
        t->e = e;
        t->cap = cap;
    }
    struct usiteperm_entry *e = &t->e[t->n++];
    memset(e, 0, sizeof *e);
    return e;
}

int usiteperm_load(struct usiteperm *t, const char *path) {
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        struct usiteperm_entry *e = grow(t);
        if (!e) { rc = -1; break; }
        char *k = strchr(line, '\t'), *v = k ? strchr(k + 1, '\t') : NULL;
        int val = !v ? 0 : !strcmp(v + 1, "allow") ? USITEPERM_ALLOW
                         : !strcmp(v + 1, "block") ? USITEPERM_BLOCK : 0;
        if (val && (size_t)(k - line) < sizeof e->host && (size_t)(v - k - 1) < sizeof e->kind) {
            memcpy(e->host, line, (size_t)(k - line));
            memcpy(e->kind, k + 1, (size_t)(v - k - 1));
            e->value = val;
        } else if (!(e->raw = strdup(line))) {
            t->n--;
            rc = -1;
            break;
        }
    }
    fclose(f);
    return rc;
}

void usiteperm_free(struct usiteperm *t) {
    for (int i = 0; i < t->n; i++) free(t->e[i].raw);
    free(t->e);
    t->e = NULL;
    t->n = t->cap = 0;
}

static int find(const struct usiteperm *t, const char *host, const char *kind) {
    for (int i = 0; i < t->n; i++)
        if (t->e[i].value && !strcmp(t->e[i].host, host) && !strcmp(t->e[i].kind, kind)) return i;
    return -1;
}

int usiteperm_get(const struct usiteperm *t, const char *host, const char *kind) {
    int i = find(t, host, kind);
    return i < 0 ? USITEPERM_DEFAULT : t->e[i].value;
}

static int save(const struct usiteperm *t) {
    char tmp[sizeof t->path + 8];
    snprintf(tmp, sizeof tmp, "%s.new", t->path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < t->n; i++) {
        const struct usiteperm_entry *e = &t->e[i];
        if (e->value) fprintf(f, "%s\t%s\t%s\n", e->host, e->kind, word(e->value));
        else fprintf(f, "%s\n", e->raw);
    }
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, t->path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int usiteperm_set(struct usiteperm *t, const char *host, const char *kind, int value) {
    if (strlen(host) >= USITEPERM_HOST || strlen(kind) >= USITEPERM_KIND || !host[0]) return -1;
    int i = find(t, host, kind);
    if (value == USITEPERM_DEFAULT) {
        if (i < 0) return 0;
        t->e[i] = t->e[--t->n];
    } else if (i >= 0) {
        t->e[i].value = value;
    } else {
        struct usiteperm_entry *e = grow(t);
        if (!e) return -1;
        snprintf(e->host, sizeof e->host, "%s", host);
        snprintf(e->kind, sizeof e->kind, "%s", kind);
        e->value = value;
    }
    return save(t);
}

int usiteperm_list(const struct usiteperm *t, const char *kind, char *out, int cap) {
    int len = 0;
    if (cap < 1) return -1;
    for (int i = 0; i < t->n; i++) {
        const struct usiteperm_entry *e = &t->e[i];
        if (!e->value || strcmp(e->kind, kind)) continue;
        int n = snprintf(out + len, (size_t)(cap - len), "%s\t%s\n", e->host, word(e->value));
        if (n < 0 || n >= cap - len) { out[0] = '\0'; return -1; }
        len += n;
    }
    out[len] = '\0';
    return len;
}
