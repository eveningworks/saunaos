#include "base/udatadir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "base/udirs.h"
#include "rt/ulock.h"

// $<var>/`rel`, else <home>/<fallback>/`rel` -- XDG: a relative value is ignored.
static char *xdg_path(const char *var, const char *fallback, const char *rel) {
    const char *v = getenv(var), *home = udir_home();
    char *p = NULL;
    int n = v && *v == '/' ? asprintf(&p, "%s/%s", v, rel)
                           : asprintf(&p, "%s/%s/%s", strcmp(home, "/") ? home : "", fallback, rel);
    return n < 0 ? NULL : p;
}

static char *make_parents(char *p) {
    for (char *s = p ? strchr(p + 1, '/') : NULL; s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    return p;
}

char *udatadir_home(const char *rel) { return xdg_path("XDG_DATA_HOME", ".local/share", rel); }
char *udatadir_prepare(const char *rel) { return make_parents(udatadir_home(rel)); }

static char *personal(const char *var, const char *fallback, const char *rel,
                      const char *machine) {
    return geteuid() == 0 ? strdup(machine) : xdg_path(var, fallback, rel);
}

char *udatadir_state(const char *rel, const char *machine) {
    return personal("XDG_STATE_HOME", ".local/state", rel, machine);
}
char *udatadir_state_prepare(const char *rel, const char *machine) {
    return make_parents(udatadir_state(rel, machine));
}
char *udatadir_cache(const char *rel, const char *machine) {
    return personal("XDG_CACHE_HOME", ".cache", rel, machine);
}
char *udatadir_cache_prepare(const char *rel, const char *machine) {
    return make_parents(udatadir_cache(rel, machine));
}

char *udatadir_find(const char *rel) {
    char *p = udatadir_home(rel);
    if (p && access(p, F_OK) == 0) return p;
    free(p);
    // XDG: an unset or EMPTY $XDG_DATA_DIRS means the default; a relative
    // entry is ignored.
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    for (const char *s = dirs; *s; ) {
        const char *e = strchr(s, ':');
        int n = e ? (int)(e - s) : (int)strlen(s);
        if (n && *s == '/') {
            p = NULL;
            if (asprintf(&p, "%.*s/%s", n, s, rel) < 0) return NULL;
            if (access(p, F_OK) == 0) return p;
            free(p);
        }
        if (!e) break;
        s = e + 1;
    }
    return NULL;
}

static struct { char *rel, *path; } *g_files;
static int g_nfiles;
static volatile int g_files_lock;

const char *udatadir_state_file(const char *rel, const char *machine) {
    const char *found = NULL;
    ulock_take(&g_files_lock);
    for (int i = 0; i < g_nfiles && !found; i++)
        if (!strcmp(g_files[i].rel, rel)) found = g_files[i].path;
    if (!found) {
        void *grown = realloc(g_files, (size_t)(g_nfiles + 1) * sizeof *g_files);
        char *r = strdup(rel), *p = udatadir_state_prepare(rel, machine);
        if (grown && r && p) {
            g_files = grown;
            g_files[g_nfiles].rel = r;
            g_files[g_nfiles++].path = p;
            found = p;
        } else {
            if (grown) g_files = grown;
            free(r);
            free(p);
        }
    }
    ulock_drop(&g_files_lock);
    return found;
}
