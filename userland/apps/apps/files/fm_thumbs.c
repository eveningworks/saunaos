// Thumbnails for the icons view: the File Manager's side of media/uthumb.h,
// which owns the cache, the worker and the copy on disk -- shared with the
// Image Viewer's filmstrip.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "kpath.h"
#include "media/uthumb.h"

#include <string.h>

static int wake(void *ctx) {
    (void)ctx;
    return uapp_post(g_app, POST_THUMB, 0);
}

static void init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    // "files" is what tools/tests/gui/thumbcache_test.py waits on.
    static const struct uthumb_config cfg = {
        .wake = wake, .log_prefix = "files",
    };
    uthumb_init(&cfg);
}

// The fileview's callback: a lookup that may ENQUEUE, never a decode.
const struct uimg *pane_thumb(void *ctx, const char *dir,
                              const struct ulist_entry *e, int px) {
    (void)ctx;
    if (!ufolderopts_thumb_ok(&g_opt.folder, e->size)) return 0;   // too big: its type's icon
    char *path = upath_join(dir, e->name);
    if (!path) return 0;
    init_once();
    const struct uimg *t = uthumb_get(path, &e->modified, e->size, px);
    free(path);
    return t;
}

int pane_badge(void *ctx, const char *dir, const struct ulist_entry *e, int px, char *out, int cap) {
    (void)ctx;
    char *path = upath_join(dir, e->name);
    const char *n = path ? uthumb_note(path, px) : 0;
    free(path);
    if (!n) return 0;
    strlcpy(out, n, (size_t)cap);
    return 1;
}

int thumb_posted(void) {
    init_once();
    return uthumb_posted();
}

int thumb_tick(void) {
    return uthumb_tick();
}
