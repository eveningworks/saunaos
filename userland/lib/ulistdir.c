// ulistdir.h: a whole listing, paged.
#include <errno.h>
#include <stdlib.h>
#include "ulistdir.h"

int ulistdir(const char *path, struct sys_dirent **out) {
    *out = 0;
    int cap = SYS_LISTDIR_MAX, n = 0;
    struct sys_dirent *v = malloc(sizeof *v * (size_t)cap);
    if (!v) { errno = ENOMEM; return -1; }
    for (;;) {
        if (cap - n < SYS_LISTDIR_MAX) {
            struct sys_dirent *g = realloc(v, sizeof *v * (size_t)cap * 2);
            if (!g) { free(v); errno = ENOMEM; return -1; }
            v = g;
            cap *= 2;
        }
        int got = sys_listdir_at(path, v + n, SYS_LISTDIR_MAX, n);
        if (got < 0) { free(v); return -1; }
        n += got;
        if (got < SYS_LISTDIR_MAX) break;
    }
    *out = v;
    return n;
}
