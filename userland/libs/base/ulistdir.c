// ulistdir.h: listings over SYS_GETDENTS's variable-length records.
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "ulistdir.h"

#define PAGE_FIRST 8192u   // grows when one record (a long name) does not fit

int ulistdir_page(const char *path, uint32_t *start, char **buf, uint32_t *cap) {
    if (!*buf) {
        *buf = malloc(PAGE_FIRST);
        if (!*buf) { errno = ENOMEM; return -1; }
        *cap = PAGE_FIRST;
    }
    for (;;) {
        int got = sys_getdents(path, *buf, *cap, *start);
        if (got >= 0) {
            // Resume after the last record this page holds.
            int pos = 0, last = 0;
            while (pos < got) {
                last = pos;
                pos += ((const struct sys_dirent64 *)(*buf + pos))->d_reclen;
            }
            if (got > 0) *start = (uint32_t)((const struct sys_dirent64 *)(*buf + last))->d_off;
            return got;
        }
        if (errno != EINVAL) return -1;
        // Not even one record fitted: a name longer than the page.
        char *g = realloc(*buf, (size_t)*cap * 2);
        if (!g) { errno = ENOMEM; return -1; }
        *buf = g;
        *cap *= 2;
    }
}

static void too_long_once(const char *name) {
    static int said;
    if (said++) return;
    sys_eprint("ulistdir: a name longer than 255 bytes is left out of a listing: ");
    sys_eprint(name);
    sys_eprint("\n");
}

int ulistdir_next(const char *page, int len, int *pos, struct ulist_entry *e) {
    for (;;) {
        if (*pos >= len) return 0;
        const struct sys_dirent64 *r = (const struct sys_dirent64 *)(page + *pos);
        if (r->d_reclen == 0 || *pos + r->d_reclen > len) return 0;   // a malformed page ends it
        *pos += r->d_reclen;
        size_t nl = strlen(r->d_name);
        if (nl >= ULIST_NAME_MAX) { too_long_once(r->d_name); continue; }
        memcpy(e->name, r->d_name, nl + 1);
        e->size = r->d_size;
        e->is_dir = r->d_type == SYS_DT_DIR;
        e->modified = r->d_modified;
        return 1;
    }
}

int ulistdir_any(const char *path) {
    // One record's header and a short name: a longer first name answers
    // EINVAL, which says just as surely that there is one.
    char small[64];
    int got = sys_getdents(path, small, sizeof small, 0);
    if (got > 0) return 1;
    if (got == 0) return 0;
    return errno == EINVAL ? 1 : -1;
}

int ulistdir_first(const char *path, struct ulist_entry *out, int max) {
    char *page = 0;
    uint32_t start = 0, cap = 0;
    int n = 0, got = 0;
    while (n < max && (got = ulistdir_page(path, &start, &page, &cap)) > 0)
        for (int pos = 0; n < max && ulistdir_next(page, got, &pos, &out[n]); ) n++;
    free(page);
    return got < 0 ? -1 : n;
}

int ulistdir(const char *path, struct ulist_entry **out) {
    *out = 0;
    char *page = 0;
    uint32_t start = 0, cap = 0;
    struct ulist_entry *v = 0;
    int n = 0, vcap = 0;
    for (;;) {
        int got = ulistdir_page(path, &start, &page, &cap);
        if (got < 0) { free(v); free(page); return -1; }
        if (got == 0) break;
        struct ulist_entry e;
        for (int pos = 0; ulistdir_next(page, got, &pos, &e); ) {
            if (n == vcap) {
                int want = vcap ? vcap * 2 : 32;
                struct ulist_entry *g = realloc(v, sizeof *v * (size_t)want);
                if (!g) { free(v); free(page); errno = ENOMEM; return -1; }
                v = g;
                vcap = want;
            }
            v[n++] = e;
        }
    }
    free(page);
    if (!v && !(v = malloc(sizeof *v))) { errno = ENOMEM; return -1; }   // an empty listing is a valid block
    *out = v;
    return n;
}
