// realpath() over kernel/lib's k_path_resolve() -- the one path
// normaliser, already in libc.a (Makefile, LIBC_OBJS) -- plus stat().
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kpath.h"

char *realpath(const char *path, char *resolved) {
    if (!path) { errno = EINVAL; return 0; }
    if (!*path) { errno = ENOENT; return 0; }
    char *cwd = malloc(PATH_MAX);
    char *scratch = malloc(KPATH_SCRATCH_FOR(PATH_MAX));
    char *out = resolved ? resolved : malloc(PATH_MAX);
    char *ret = 0;
    if (!cwd || !scratch || !out) { errno = ENOMEM; goto done; }
    if (!getcwd(cwd, PATH_MAX)) goto done;               // getcwd set errno
    struct kpath_scratch sc = { scratch, KPATH_SCRATCH_FOR(PATH_MAX) };
    if (!k_path_resolve(cwd, path, out, PATH_MAX, &sc)) { errno = ENAMETOOLONG; goto done; }
    struct stat st;
    if (stat(out, &st) != 0) goto done;                  // stat set errno
    ret = out;
done:
    free(cwd);
    free(scratch);
    if (!ret && !resolved) free(out);
    return ret;
}
