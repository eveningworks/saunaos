// Back and forward through visited folders -- see lib/unavhist.h.
#include "lib/unavhist.h"
#include <stdio.h>
#include <string.h>

void unavhist_init(struct unavhist *h) {
    h->count = 0;
    h->at = -1;
}

void unavhist_record(struct unavhist *h, const char *dir) {
    if (!dir || !dir[0]) return;
    if (h->at >= 0 && strcmp(h->dir[h->at], dir) == 0) return;   // the same place: not a step
    if (h->at + 1 < UNAVHIST_MAX) {
        h->at++;
    } else {
        // Full: the far end of Back quietly gets shorter.
        memmove(h->dir[0], h->dir[1], sizeof h->dir[0] * (UNAVHIST_MAX - 1));
    }
    snprintf(h->dir[h->at], UNAVHIST_PATH, "%s", dir);
    h->count = h->at + 1;   // anything ahead is now unreachable
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
