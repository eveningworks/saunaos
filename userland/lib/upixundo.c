// Undo for a picture being painted -- see lib/upixundo.h.
#include "lib/upixundo.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void upixundo_init(struct upixundo *u, size_t limit_bytes) {
    memset(u, 0, sizeof *u);
    u->limit = limit_bytes;
    u->next_id = 1;
}

static void step_free(struct upixundo *u, struct upixundo_step *s) {
    if (s->custom) {
        u->bytes -= s->custom_bytes;
        if (s->custom->drop) s->custom->drop(s->payload);
        s->custom = 0;
        return;
    }
    size_t px = s->whole ? (size_t)s->bw * s->bh + (size_t)s->aw * s->ah : 2 * (size_t)s->w * s->h;
    u->bytes -= px * 4;
    free(s->before);
    free(s->after);
    s->before = s->after = 0;
}

// The redo list, gone: a new step was taken.
static void drop_after(struct upixundo *u) {
    for (int i = u->at; i < u->count; i++) step_free(u, &u->steps[i]);
    u->count = u->at;
}

void upixundo_clear(struct upixundo *u) {
    u->at = 0;
    drop_after(u);
}

void upixundo_free(struct upixundo *u) {
    upixundo_clear(u);
    free(u->steps);
    uimg_free(&u->snap);
    size_t limit = u->limit;
    upixundo_init(u, limit);
}

int upixundo_begin(struct upixundo *u, const struct uimg *im) {
    u->snapped = 0;
    size_t n = (size_t)im->w * im->h;
    if (!im->px || !n) return -ENOMEM;
    if ((size_t)u->snap.w * u->snap.h != n || !u->snap.px) {
        uimg_free(&u->snap);
        u->snap.px = malloc(n * 4);
        if (!u->snap.px) return -ENOMEM;
    }
    u->snap.w = im->w;
    u->snap.h = im->h;
    memcpy(u->snap.px, im->px, n * 4);
    u->snapped = 1;
    return 0;
}

void upixundo_cancel(struct upixundo *u) { u->snapped = 0; }

static uint32_t *cut(const uint32_t *px, int stride, int x, int y, int w, int h) {
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) return 0;
    for (int j = 0; j < h; j++)
        memcpy(out + (size_t)j * w, px + (size_t)(y + j) * stride + x, (size_t)w * 4);
    return out;
}

static uint32_t *dup_px(const uint32_t *px, size_t n) {
    uint32_t *out = malloc(n * 4);
    if (out) memcpy(out, px, n * 4);
    return out;
}

// Oldest first, never the newest.
static void trim(struct upixundo *u) {
    int drop = 0;
    while (u->bytes > u->limit && drop < u->at - 1) step_free(u, &u->steps[drop++]);
    if (!drop) return;
    memmove(u->steps, u->steps + drop, sizeof u->steps[0] * (size_t)(u->count - drop));
    u->count -= drop;
    u->at -= drop;
}

// Room for one more step, after the redo list is let go.
static int make_room(struct upixundo *u) {
    drop_after(u);
    if (u->count < u->cap) return 0;
    int cap = u->cap ? u->cap * 2 : 32;
    struct upixundo_step *grown = realloc(u->steps, sizeof *grown * (size_t)cap);
    if (!grown) return -ENOMEM;
    u->steps = grown;
    u->cap = cap;
    return 0;
}

int upixundo_push(struct upixundo *u, const struct upixundo_custom *ops, void *payload, size_t bytes) {
    u->snapped = 0;
    if (make_room(u) != 0) { if (ops->drop) ops->drop(payload); return -ENOMEM; }
    struct upixundo_step s = { 0 };
    s.custom = ops;
    s.payload = payload;
    s.custom_bytes = bytes;
    s.id = u->next_id++;
    u->bytes += bytes;
    u->steps[u->count++] = s;
    u->at = u->count;
    trim(u);
    return 0;
}

int upixundo_commit(struct upixundo *u, struct uimg *im, const struct upix_rect *dirty) {
    if (!u->snapped) return 0;
    u->snapped = 0;
    int whole = !dirty || im->w != u->snap.w || im->h != u->snap.h;
    if (!whole && (dirty->w <= 0 || dirty->h <= 0)) return 0;
    if (make_room(u) != 0) return -ENOMEM;
    struct upixundo_step s = { 0 };
    s.im = im;
    s.whole = whole;
    s.id = u->next_id++;
    if (whole) {
        s.bw = u->snap.w; s.bh = u->snap.h; s.aw = im->w; s.ah = im->h;
        s.before = dup_px(u->snap.px, (size_t)s.bw * s.bh);
        s.after = dup_px(im->px, (size_t)s.aw * s.ah);
        u->bytes += ((size_t)s.bw * s.bh + (size_t)s.aw * s.ah) * 4;
    } else {
        s.x = dirty->x; s.y = dirty->y; s.w = dirty->w; s.h = dirty->h;
        s.before = cut(u->snap.px, u->snap.w, s.x, s.y, s.w, s.h);
        s.after = cut(im->px, im->w, s.x, s.y, s.w, s.h);
        u->bytes += 2 * (size_t)s.w * s.h * 4;
    }
    if (!s.before || !s.after) { step_free(u, &s); return -ENOMEM; }
    u->steps[u->count++] = s;
    u->at = u->count;
    trim(u);
    return 0;
}

int upixundo_can_undo(const struct upixundo *u) { return u->at > 0; }
int upixundo_can_redo(const struct upixundo *u) { return u->at < u->count; }

// Puts one side of a step into its picture, or runs its custom ops.
static int apply(const struct upixundo_step *s, int after, struct upix_rect *changed) {
    if (s->custom) {
        s->custom->apply(s->payload, after);
        if (changed) *changed = (struct upix_rect){ 0, 0, 0, 0 };
        return 1;
    }
    struct uimg *im = s->im;
    const uint32_t *src = after ? s->after : s->before;
    if (s->whole) {
        int w = after ? s->aw : s->bw, h = after ? s->ah : s->bh;
        uint32_t *px = dup_px(src, (size_t)w * h);
        if (!px) return 0;
        free(im->px);
        im->px = px;
        im->w = w;
        im->h = h;
        if (changed) *changed = (struct upix_rect){ 0, 0, 0, 0 };
        return 1;
    }
    for (int j = 0; j < s->h; j++)
        memcpy(im->px + (size_t)(s->y + j) * im->w + s->x, src + (size_t)j * s->w, (size_t)s->w * 4);
    if (changed) *changed = (struct upix_rect){ s->x, s->y, s->w, s->h };
    return 1;
}

int upixundo_undo(struct upixundo *u, struct upix_rect *changed) {
    if (!u->at || !apply(&u->steps[u->at - 1], 0, changed)) return 0;
    u->at--;
    return 1;
}

int upixundo_redo(struct upixundo *u, struct upix_rect *changed) {
    if (u->at >= u->count || !apply(&u->steps[u->at], 1, changed)) return 0;
    u->at++;
    return 1;
}

unsigned upixundo_state(const struct upixundo *u) {
    return u->at ? u->steps[u->at - 1].id : 0;
}
