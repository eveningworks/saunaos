// See upath.h.
#include "base/upath.h"
#include "rt/sys.h"
#include "kpath.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// The same order and spirit as the kernel shell's (kernel/shell/shell_path.c):
// /bin, then /usr/bin, then /tests.
static const char *const PATH_DIRS[] = { "/bin", "/usr/bin", "/tests" };
#define PATH_DIR_COUNT (int)(sizeof(PATH_DIRS) / sizeof(PATH_DIRS[0]))

char *upath_join(const char *dir, const char *name) {
    size_t dn = dir ? strlen(dir) : 0, nn = strlen(name);
    int slash = dn > 0 && dir[dn - 1] != '/';
    char *p = malloc(dn + (size_t)slash + nn + 1);
    if (!p) { errno = ENOMEM; return 0; }
    if (dn) memcpy(p, dir, dn);
    if (slash) p[dn] = '/';
    memcpy(p + dn + slash, name, nn + 1);
    return p;
}

char *upath_dirname(const char *path) {
    size_t cap = strlen(path) + 2;   // "/" for a bare name
    char *p = malloc(cap);
    if (!p) { errno = ENOMEM; return 0; }
    k_path_dirname(path, p, cap);
    return p;
}

int upath_set(char **field, const char *s) {
    char *c = s ? strdup(s) : 0;
    if (s && !c) { errno = ENOMEM; return 0; }
    free(*field);
    *field = c;
    return 1;
}

char *upath_resolve(const char *base, const char *input) {
    size_t bn = base ? strlen(base) : 0, in = input ? strlen(input) : 0;
    size_t n = bn > in ? bn : in;
    struct kpath_scratch sc = { malloc(KPATH_SCRATCH_FOR(n)), KPATH_SCRATCH_FOR(n) };
    char *p = sc.buf ? malloc(bn + in + 3) : 0;
    if (!p) { free(sc.buf); errno = ENOMEM; return 0; }
    int ok = k_path_resolve(base, input, p, bn + in + 3, &sc);
    free(sc.buf);
    if (!ok) { free(p); errno = ENAMETOOLONG; return 0; }
    return p;
}

int upath_find_program(const char *name, char **out) {
    *out = 0;
    if (!name || !*name) return 0;
    if (strchr(name, '/')) {
        *out = strdup(name);
        if (!*out) { errno = ENOMEM; return -1; }
        return 1;
    }
    for (int i = 0; i < PATH_DIR_COUNT; i++) {
        char *cand = upath_join(PATH_DIRS[i], name);
        if (!cand) return -1;
        // Probing by opening: a directory would open too, but a PATH
        // entry holding a directory named like a command is not a case
        // worth carrying code for.
        int fd = sys_open(cand, 0);
        if (fd >= 0) { sys_close(fd); *out = cand; return 1; }
        int why = sys_errno();
        free(cand);
        // ENOENT is the ordinary answer -- keep looking. Anything else
        // is about this PROCESS, not this candidate: EMFILE means the
        // next probe cannot succeed either.
        if (why != ENOENT) { errno = why; return -1; }
    }
    return 0;
}

int upath_dir_count(void) {
    return PATH_DIR_COUNT;
}

const char *upath_dir(int index) {
    if (index < 0 || index >= upath_dir_count()) return 0;
    return PATH_DIRS[index];
}
