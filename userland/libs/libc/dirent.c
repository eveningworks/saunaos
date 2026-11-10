// opendir/readdir/closedir over SYS_GETDENTS. See <dirent.h> for why a
// DIR is a snapshot rather than a cursor.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "rt/sys.h"

struct _DIR {
    char *recs;          // every SYS_GETDENTS record of the directory, packed
    size_t len;          // bytes of them
    size_t pos;          // the next record readdir() decodes
    int n;               // how many records -- scandir() sizes its array by it
    struct dirent *cur;  // what readdir() hands back; grown for a long name
    size_t cur_cap;      // its bytes
};

DIR *opendir(const char *path) {
    if (!path) return 0;
    DIR *d = (DIR *)calloc(1, sizeof *d);
    if (!d) return 0;
    // THE WHOLE DIRECTORY, page by page, into one buffer: a page that
    // cannot hold even the next record (EINVAL) is a name longer than
    // it, so the page grows, as Linux's getdents64 callers do.
    size_t cap = 0;
    uint32_t page = 8192, start = 0;
    for (;;) {
        if (cap - d->len < page) {
            size_t want = cap ? cap * 2 : 16384;
            while (want - d->len < page) want *= 2;
            char *g = (char *)realloc(d->recs, want);
            if (!g) { free(d->recs); free(d); errno = ENOMEM; return 0; }
            d->recs = g;
            cap = want;
        }
        int got = sys_getdents(path, d->recs + d->len, page, start);
        if (got < 0 && errno == EINVAL) { page *= 2; continue; }
        // A missing path and an empty directory are different answers and
        // must not become the same one: the listing reports -1 for the
        // first and 0 for the second, and an opendir() that succeeded on a
        // missing path would make every caller's error handling dead code.
        if (got < 0) { free(d->recs); free(d); return 0; }
        if (got == 0) break;
        size_t p = d->len, last = p;
        while (p < d->len + (size_t)got) {
            last = p;
            p += ((const struct sys_dirent64 *)(d->recs + p))->d_reclen;
            d->n++;
        }
        start = (uint32_t)((const struct sys_dirent64 *)(d->recs + last))->d_off;
        d->len += (size_t)got;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || d->pos >= d->len) return 0;
    const struct sys_dirent64 *r = (const struct sys_dirent64 *)(d->recs + d->pos);
    if (r->d_reclen == 0 || d->pos + r->d_reclen > d->len) return 0;
    d->pos += r->d_reclen;
    // d_name[256] holds every name a filesystem here keeps to NAME_MAX;
    // a longer one (exFAT, SMB) is returned WHOLE, in a struct grown past
    // its declared size -- glibc's readdir() does the same, and POSIX
    // leaves d_name's size open for it.
    size_t nl = strlen(r->d_name);
    size_t need = offsetof(struct dirent, d_name) + nl + 1;
    if (need < sizeof(struct dirent)) need = sizeof(struct dirent);
    if (need > d->cur_cap) {
        struct dirent *g = (struct dirent *)realloc(d->cur, need);
        if (!g) { errno = ENOMEM; return 0; }
        d->cur = g;
        d->cur_cap = need;
    }
    d->cur->d_type = r->d_type == SYS_DT_DIR ? DT_DIR : DT_REG;
    memcpy(d->cur->d_name, r->d_name, nl + 1);
    return d->cur;
}

void rewinddir(DIR *d) {
    // Rewinds the SNAPSHOT, and deliberately does not re-read the
    // directory: POSIX says rewinddir resets the position, and a
    // caller that wanted the current contents would have to reopen
    // anyway (see <dirent.h>).
    if (d) d->pos = 0;
}

int closedir(DIR *d) {
    if (!d) return -1;
    free(d->recs);
    free(d->cur);
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
        // Its own size: a long name's entry is bigger than the struct.
        size_t sz = offsetof(struct dirent, d_name) + strlen(e->d_name) + 1;
        if (sz < sizeof *e) sz = sizeof *e;
        struct dirent *copy = malloc(sz);
        if (!copy) {
            while (n) free(list[--n]);
            free(list);
            closedir(d);
            errno = ENOMEM;
            return -1;
        }
        memcpy(copy, e, sz);
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
