// The directory walk -- see uwalk.h.
#include "base/uwalk.h"
#include <stdlib.h>
#include <string.h>

// Room for `need` bytes of path.
static int path_room(struct uwalk *w, size_t need) {
    if (need <= w->path_cap) return 1;
    size_t cap = w->path_cap ? w->path_cap : 256;
    while (cap < need) cap *= 2;
    char *g = realloc(w->path, cap);
    if (!g) return 0;
    w->path = g;
    w->path_cap = cap;
    return 1;
}

void uwalk_end(struct uwalk *w) {
    free(w->path);
    free(w->page);
    w->path = w->page = 0;
    w->path_cap = 0;
    w->page_cap = 0;
    w->depth = 0;
}

// Leaves the current directory; the walk frees itself when it ends.
static int pop(struct uwalk *w) {
    w->depth--;
    if (w->depth == 0) { uwalk_end(w); return 0; }
    w->path[w->frame[w->depth - 1].len] = '\0';
    return 1;
}

int uwalk_begin(struct uwalk *w, const char *root) {
    struct sys_stat st;
    memset(w, 0, sizeof *w);
    if (sys_stat(root, &st) != 0 || !st.is_dir) return 0;
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') len--;   // "/a/" walks as "/a"
    if (!path_room(w, len + 1)) return 0;
    memcpy(w->path, root, len);
    w->path[len] = '\0';
    w->frame[0].len = (int)len;
    w->frame[0].offset = 0;
    w->depth = 1;
    w->dirs = 1;
    return 1;
}

int uwalk_step(struct uwalk *w, uwalk_fn fn, void *ctx) {
    if (w->stopped || w->depth == 0) return 0;
    int top = w->depth - 1;
    size_t dl = (size_t)w->frame[top].len;
    w->path[dl] = '\0';
    uint32_t start = w->frame[top].offset;
    int n = ulistdir_page(w->path, &start, &w->page, &w->page_cap);
    if (n < 0) w->unreadable++;
    if (n <= 0) return pop(w);

    struct ulist_entry e;
    for (int pos = 0; pos < n; ) {
        const struct sys_dirent64 *rec = (const struct sys_dirent64 *)(w->page + pos);
        if (rec->d_reclen == 0 || pos + rec->d_reclen > n) {
            // A malformed page: never re-read it forever.
            if (pos == 0) { w->unreadable++; return pop(w); }
            break;
        }
        pos += rec->d_reclen;
        // Where this frame resumes after the record, whatever is done with it.
        w->frame[top].offset = (uint32_t)rec->d_off;
        w->entries++;
        size_t nl = strlen(rec->d_name);
        if (nl >= sizeof e.name) continue;   // ulistdir.h: left out, never cut
        memcpy(e.name, rec->d_name, nl + 1);
        e.size = rec->d_size;
        e.is_dir = rec->d_type == SYS_DT_DIR;
        e.modified = rec->d_modified;

        // "<dir>/<name>", in place after the directory's own path.
        int slash = !(dl == 1 && w->path[0] == '/');
        if (!path_room(w, dl + (size_t)slash + nl + 1)) { w->stopped = 1; return 0; }
        if (slash) w->path[dl] = '/';
        memcpy(w->path + dl + slash, e.name, nl + 1);

        int r = fn(ctx, w->path, &e);
        if (r < 0) { w->stopped = 1; return 0; }
        if (e.is_dir && r > 0) {
            // DESCEND NOW, and come back to the rest of this page: the
            // parent's frame remembers how far it got.
            if (w->depth < UWALK_DEPTH) {
                w->frame[w->depth].len = (int)(dl + (size_t)slash + nl);
                w->frame[w->depth].offset = 0;
                w->depth++;
                w->dirs++;
                return 1;
            }
            w->too_deep++;
        }
        w->path[dl] = '\0';
    }
    return 1;
}
