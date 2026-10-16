// The open tabs, kept across a restart. See ns_session.h.
//
// One line per tab, then one per entry of its list, oldest first:
//
//   tab <TAB> at <TAB> front
//   e <TAB> sx <TAB> sy <TAB> url <TAB> title
//
// An address carries no tab or newline (they are percent-encoded); a
// title's are written as spaces.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "browser/ns_session.h"

#define FILE_MAGIC "toy-os browser session 1"

static void path_of(const char *dir, char *out, size_t cap) { snprintf(out, cap, "%s/session.tsv", dir); }

static char *field(char **p) {
    char *s = *p;
    if (!s) return NULL;
    char *t = strchr(s, '\t');
    if (t) *t++ = '\0';
    *p = t;
    return s;
}

int ns_session_load(const char *dir, struct ns_session_tab *tabs, int cap, int *active) {
    char path[256];
    path_of(dir, path, sizeof path);
    *active = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char line[WEBVIEW_URL_MAX + 512];
    int n = 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, FILE_MAGIC, strlen(FILE_MAGIC))) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line, *kind = field(&p);
        if (!strcmp(kind, "tab")) {
            char *at = field(&p), *front = field(&p);
            if (!at || !front || n >= cap) break;
            memset(&tabs[n], 0, sizeof tabs[n]);
            tabs[n].at = atoi(at);
            if (atoi(front)) *active = n;
            n++;
        } else if (!strcmp(kind, "e") && n > 0) {
            struct ns_session_tab *t = &tabs[n - 1];
            char *sx = field(&p), *sy = field(&p), *url = field(&p), *title = field(&p);
            if (!url || !url[0] || t->n >= UUI_WV_HISTORY_MAX) continue;
            struct uui_wv_entry *e = &t->e[t->n++];
            e->sx = atoi(sx);
            e->sy = atoi(sy);
            e->url = strdup(url);
            e->title = strdup(title ? title : "");
        }
    }
    fclose(f);
    // A tab whose list did not survive is no tab.
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!tabs[i].n) {
            if (*active > k) (*active)--;
            continue;
        }
        if (tabs[i].at < 0 || tabs[i].at >= tabs[i].n) tabs[i].at = tabs[i].n - 1;
        if (k != i) tabs[k] = tabs[i];
        k++;
    }
    if (*active >= k) *active = k ? k - 1 : 0;
    return k;
}

void ns_session_free(struct ns_session_tab *tabs, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < tabs[i].n; j++) {
            free(tabs[i].e[j].url);
            free(tabs[i].e[j].title);
        }
}

static void put_title(FILE *f, const char *s) {
    for (; s && *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

int ns_session_save(const char *dir, struct uui_webview *const *tabs, int n, int active) {
    char path[256], tmp[264];
    path_of(dir, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", FILE_MAGIC);
    for (int i = 0; i < n; i++) {
        struct uui_webview *v = tabs[i];
        uui_webview_history_snapshot(v);
        if (!v->nhist) continue;   // a tab that never got a page
        fprintf(f, "tab\t%d\t%d\n", v->hist_at, i == active);
        for (int j = 0; j < v->nhist; j++) {
            const struct uui_wv_entry *e = &v->hist[j];
            fprintf(f, "e\t%d\t%d\t%s\t", e->sx, e->sy, e->url);
            put_title(f, e->title);
            fputc('\n', f);
        }
    }
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}
