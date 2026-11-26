// Back and forward through visited folders -- see base/unavhist.h.
#include "base/unavhist.h"
#include "base/ulog.h"
#include <stdlib.h>
#include <string.h>

void unavhist_init(struct unavhist *h) {
    for (int i = 0; i < h->count; i++) free(h->dir[i]);
    memset(h->dir, 0, sizeof h->dir);
    h->count = 0;
    h->at = -1;
}

void unavhist_record(struct unavhist *h, const char *dir) {
    if (!dir || !dir[0]) return;
    if (h->at >= 0 && strcmp(h->dir[h->at], dir) == 0) return;   // the same place: not a step
    char *copy = strdup(dir);
    if (!copy) { ulogf("unavhist: out of memory recording %s\n", dir); return; }
    for (int i = h->at + 1; i < h->count; i++) { free(h->dir[i]); h->dir[i] = 0; }   // the forward list goes
    if (h->at + 1 < UNAVHIST_MAX) {
        h->at++;
    } else {
        // Full: the far end of Back quietly gets shorter.
        free(h->dir[0]);
        memmove(&h->dir[0], &h->dir[1], sizeof h->dir[0] * (UNAVHIST_MAX - 1));
    }
    h->dir[h->at] = copy;
    h->count = h->at + 1;   // anything ahead is now unreachable
}

int unavhist_copy(struct unavhist *dst, const struct unavhist *src) {
    unavhist_init(dst);
    for (int i = 0; i < src->count; i++) {
        if (!(dst->dir[i] = strdup(src->dir[i]))) {
            ulogf("unavhist: out of memory copying %d places\n", src->count);
            unavhist_init(dst);
            return 0;
        }
        dst->count = i + 1;
    }
    dst->at = src->at;
    return 1;
}

int unavhist_can(const struct unavhist *h, int delta) {
    return delta < 0 ? h->at > 0 : h->at + 1 < h->count;
}

const char *unavhist_step(struct unavhist *h, int delta,
                          int (*ok)(void *ctx, const char *dir), void *ctx,
                          const char **skipped) {
    if (skipped) *skipped = 0;
    for (int want = h->at + delta; want >= 0 && want < h->count; want += delta) {
        if (ok && !ok(ctx, h->dir[want])) {
            if (skipped && !*skipped) *skipped = h->dir[want];
            continue;
        }
        h->at = want;
        return h->dir[want];
    }
    return 0;
}
