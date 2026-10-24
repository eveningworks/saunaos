#include "urgn.h"

#include <stdlib.h>
#include <string.h>

void urgn_init(struct urgn *g) { g->n = g->cap = 0; g->r = 0; }

void urgn_free(struct urgn *g) {
    free(g->r);
    urgn_init(g);
}

static int push(struct urgn *g, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : 8;
        struct urect *r = realloc(g->r, sizeof *r * (size_t)cap);
        if (!r) return -1;
        g->r = r;
        g->cap = cap;
    }
    g->r[g->n++] = (struct urect){ x, y, w, h };
    return 0;
}

int urgn_set_rect(struct urgn *g, int x, int y, int w, int h) {
    g->n = 0;
    if (push(g, x, y, w, h) < 0) { g->n = 0; return -1; }
    return 0;
}

int urgn_copy(struct urgn *dst, const struct urgn *src) {
    if (dst == src) return 0;
    dst->n = 0;
    for (int i = 0; i < src->n; i++)
        if (push(dst, src->r[i].x, src->r[i].y, src->r[i].w, src->r[i].h) < 0) { dst->n = 0; return -1; }
    return 0;
}

// Two rectangles sharing a whole edge become one, until none do. Keeps the set of pixels.
static void coalesce(struct urgn *g) {
    for (int merged = 1; merged;) {
        merged = 0;
        for (int i = 0; i < g->n; i++)
            for (int j = i + 1; j < g->n; j++) {
                struct urect *a = &g->r[i], *b = &g->r[j];
                int join = 0;
                if (a->x == b->x && a->w == b->w && (a->y + a->h == b->y || b->y + b->h == a->y)) {
                    if (b->y < a->y) a->y = b->y;
                    a->h += b->h;
                    join = 1;
                } else if (a->y == b->y && a->h == b->h && (a->x + a->w == b->x || b->x + b->w == a->x)) {
                    if (b->x < a->x) a->x = b->x;
                    a->w += b->w;
                    join = 1;
                }
                if (join) {
                    g->r[j] = g->r[--g->n];
                    merged = 1;
                    j--;
                }
            }
    }
}

// What is left of `r` once `c` is taken away, pushed onto out: up to four pieces.
static int cut(struct urgn *out, struct urect r, struct urect c) {
    int x0 = r.x > c.x ? r.x : c.x, y0 = r.y > c.y ? r.y : c.y;
    int x1 = r.x + r.w < c.x + c.w ? r.x + r.w : c.x + c.w, y1 = r.y + r.h < c.y + c.h ? r.y + r.h : c.y + c.h;
    if (x1 <= x0 || y1 <= y0) return push(out, r.x, r.y, r.w, r.h);
    if (push(out, r.x, r.y, r.w, y0 - r.y) < 0 ||                     // above
        push(out, r.x, y1, r.w, r.y + r.h - y1) < 0 ||                // below
        push(out, r.x, y0, x0 - r.x, y1 - y0) < 0 ||                  // left
        push(out, x1, y0, r.x + r.w - x1, y1 - y0) < 0)               // right
        return -1;
    return 0;
}

// A result built aside, then put in dst: dst may be an operand.
static int finish(struct urgn *dst, struct urgn *tmp, int ok) {
    if (ok < 0) { urgn_free(tmp); dst->n = 0; return -1; }
    coalesce(tmp);
    free(dst->r);
    *dst = *tmp;
    return 0;
}

int urgn_subtract(struct urgn *dst, const struct urgn *a, const struct urgn *b) {
    struct urgn cur, next;
    urgn_init(&cur);
    urgn_init(&next);
    int ok = urgn_copy(&cur, a);
    for (int j = 0; ok == 0 && j < b->n; j++) {
        next.n = 0;
        for (int i = 0; ok == 0 && i < cur.n; i++) ok = cut(&next, cur.r[i], b->r[j]);
        struct urgn t = cur;
        cur = next;
        next = t;
    }
    urgn_free(&next);
    return finish(dst, &cur, ok);
}

int urgn_intersect(struct urgn *dst, const struct urgn *a, const struct urgn *b) {
    struct urgn out;
    urgn_init(&out);
    int ok = 0;
    for (int i = 0; ok == 0 && i < a->n; i++)
        for (int j = 0; ok == 0 && j < b->n; j++) {
            const struct urect *p = &a->r[i], *q = &b->r[j];
            int x0 = p->x > q->x ? p->x : q->x, y0 = p->y > q->y ? p->y : q->y;
            int x1 = p->x + p->w < q->x + q->w ? p->x + p->w : q->x + q->w;
            int y1 = p->y + p->h < q->y + q->h ? p->y + p->h : q->y + q->h;
            ok = push(&out, x0, y0, x1 - x0, y1 - y0);
        }
    return finish(dst, &out, ok);
}

int urgn_union(struct urgn *dst, const struct urgn *a, const struct urgn *b) {
    struct urgn out;
    urgn_init(&out);
    int ok = urgn_subtract(&out, b, a);          // b's pixels a lacks: disjoint from a
    for (int i = 0; ok == 0 && i < a->n; i++) ok = push(&out, a->r[i].x, a->r[i].y, a->r[i].w, a->r[i].h);
    return finish(dst, &out, ok);
}

int urgn_xor(struct urgn *dst, const struct urgn *a, const struct urgn *b) {
    struct urgn ab, ba;
    urgn_init(&ab);
    urgn_init(&ba);
    int ok = urgn_subtract(&ab, a, b);
    if (ok == 0) ok = urgn_subtract(&ba, b, a);
    for (int i = 0; ok == 0 && i < ba.n; i++) ok = push(&ab, ba.r[i].x, ba.r[i].y, ba.r[i].w, ba.r[i].h);
    urgn_free(&ba);
    return finish(dst, &ab, ok);
}

int urgn_add_rect(struct urgn *g, int x, int y, int w, int h) {
    struct urgn r;
    urgn_init(&r);
    if (urgn_set_rect(&r, x, y, w, h) < 0) return -1;
    int ok = urgn_union(g, g, &r);
    urgn_free(&r);
    return ok;
}

int urgn_cut_rect(struct urgn *g, int x, int y, int w, int h) {
    struct urgn r;
    urgn_init(&r);
    if (urgn_set_rect(&r, x, y, w, h) < 0) return -1;
    int ok = urgn_subtract(g, g, &r);
    urgn_free(&r);
    return ok;
}

int urgn_append(struct urgn *g, int x, int y, int w, int h) { return push(g, x, y, w, h); }

void urgn_tidy(struct urgn *g) { coalesce(g); }

void urgn_offset(struct urgn *g, int dx, int dy) {
    for (int i = 0; i < g->n; i++) {
        g->r[i].x += dx;
        g->r[i].y += dy;
    }
}

int urgn_is_empty(const struct urgn *g) { return g->n == 0; }

int urgn_bbox(const struct urgn *g, struct urect *out) {
    *out = (struct urect){ 0, 0, 0, 0 };
    if (!g->n) return 0;
    int x0 = g->r[0].x, y0 = g->r[0].y, x1 = x0 + g->r[0].w, y1 = y0 + g->r[0].h;
    for (int i = 1; i < g->n; i++) {
        const struct urect *r = &g->r[i];
        if (r->x < x0) x0 = r->x;
        if (r->y < y0) y0 = r->y;
        if (r->x + r->w > x1) x1 = r->x + r->w;
        if (r->y + r->h > y1) y1 = r->y + r->h;
    }
    *out = (struct urect){ x0, y0, x1 - x0, y1 - y0 };
    return 1;
}

long long urgn_area(const struct urgn *g) {
    long long a = 0;
    for (int i = 0; i < g->n; i++) a += (long long)g->r[i].w * g->r[i].h;
    return a;
}

int urgn_contains(const struct urgn *g, int x, int y) {
    for (int i = 0; i < g->n; i++) {
        const struct urect *r = &g->r[i];
        if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h) return 1;
    }
    return 0;
}

int urgn_overlaps(const struct urgn *g, int x, int y, int w, int h) {
    for (int i = 0; i < g->n; i++) {
        const struct urect *r = &g->r[i];
        if (x < r->x + r->w && r->x < x + w && y < r->y + r->h && r->y < y + h) return 1;
    }
    return 0;
}

int urgn_equal(const struct urgn *a, const struct urgn *b) {
    if (urgn_area(a) != urgn_area(b)) return 0;
    struct urgn d;
    urgn_init(&d);
    if (urgn_xor(&d, a, b) < 0) return 0;
    int same = d.n == 0;
    urgn_free(&d);
    return same;
}
