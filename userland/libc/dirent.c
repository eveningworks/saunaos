// opendir/readdir/closedir over SYS_LISTDIR. See <dirent.h> for why a
// DIR is a snapshot rather than a cursor.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

struct _DIR {
    struct sys_dirent *ents;
    int n;
    int i;
    struct dirent cur;   // what readdir() hands back a pointer to
};

DIR *opendir(const char *path) {
    if (!path) return 0;
    DIR *d = (DIR *)malloc(sizeof *d);
    if (!d) return 0;
    d->ents = (struct sys_dirent *)malloc(sizeof(struct sys_dirent) * SYS_LISTDIR_MAX);
    if (!d->ents) { free(d); return 0; }
    int n = sys_listdir(path, d->ents, SYS_LISTDIR_MAX);
    // A missing path and an empty directory are different answers and
    // must not become the same one: sys_listdir reports -1 for the
    // first and 0 for the second, and an opendir() that succeeded on a
    // missing path would make every caller's error handling dead code.
    if (n < 0) { free(d->ents); free(d); return 0; }
    d->n = n;
    d->i = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || d->i >= d->n) return 0;
    const struct sys_dirent *e = &d->ents[d->i++];
    k_strlcpy(d->cur.d_name, e->name, sizeof d->cur.d_name);
    d->cur.d_type = e->is_dir ? DT_DIR : DT_REG;
    return &d->cur;
}

void rewinddir(DIR *d) {
    // Rewinds the SNAPSHOT, and deliberately does not re-read the
    // directory: POSIX says rewinddir resets the position, and a
    // caller that wanted the current contents would have to reopen
    // anyway (see <dirent.h>).
    if (d) d->i = 0;
}

int closedir(DIR *d) {
    if (!d) return -1;
    free(d->ents);
    free(d);
    return 0;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

// qsort() hands the comparator pointers to ELEMENTS, which here are
// `struct dirent *`; scandir's comparator wants exactly that, but
// qsort's signature is void-typed, so the caller's function rides in a
// file-scope variable for the one sort. Not thread-safe across two
// concurrent scandir() calls with different comparators -- the price of
// a qsort without a context argument.
static int (*g_compar)(const struct dirent **, const struct dirent **);
static int scandir_cmp(const void *a, const void *b) {
    return g_compar((const struct dirent **)a, (const struct dirent **)b);
}

int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    if (!namelist) { errno = EINVAL; return -1; }
    DIR *d = opendir(dir);
    if (!d) { if (!errno) errno = ENOENT; return -1; }
    struct dirent **list = malloc(sizeof *list * (size_t)(d->n ? d->n : 1));
    int n = 0;
    if (!list) { closedir(d); errno = ENOMEM; return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (filter && !filter(e)) continue;
        struct dirent *copy = malloc(sizeof *copy);
        if (!copy) {
            while (n) free(list[--n]);
            free(list);
            closedir(d);
            errno = ENOMEM;
            return -1;
        }
        *copy = *e;
        list[n++] = copy;
    }
    closedir(d);
    if (compar && n > 1) {
        g_compar = compar;
        qsort(list, (size_t)n, sizeof *list, scandir_cmp);
    }
    *namelist = list;
    return n;
}
