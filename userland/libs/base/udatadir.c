#include "base/udatadir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *udatadir_home(const char *rel) {
    const char *v = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char *p = NULL;
    int n = v && *v == '/' ? asprintf(&p, "%s/%s", v, rel)
                           : asprintf(&p, "%s/.local/share/%s", home && *home == '/' ? home : "/home", rel);
    return n < 0 ? NULL : p;
}

char *udatadir_prepare(const char *rel) {
    char *p = udatadir_home(rel);
    for (char *s = p ? strchr(p + 1, '/') : NULL; s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    return p;
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
