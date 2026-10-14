// Painting on a picture -- see lib/upixel.h.
#include "lib/upixel.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void upix_rect_add(struct upix_rect *r, const struct uimg *im, int x, int y, int w, int h) {
    if (!r) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > im->w) w = im->w - x;
    if (y + h > im->h) h = im->h - y;
    if (w <= 0 || h <= 0) return;
    if (r->w <= 0 || r->h <= 0) { *r = (struct upix_rect){ x, y, w, h }; return; }
    int x1 = r->x + r->w > x + w ? r->x + r->w : x + w;
    int y1 = r->y + r->h > y + h ? r->y + r->h : y + h;
    if (x < r->x) r->x = x;
    if (y < r->y) r->y = y;
    r->w = x1 - r->x;
    r->h = y1 - r->y;
}

uint32_t upix_get(const struct uimg *im, int x, int y) {
    if (!im->px || x < 0 || y < 0 || x >= im->w || y >= im->h) return 0;
    return im->px[(size_t)y * im->w + x];
}

void upix_stamp(struct uimg *im, int cx, int cy, int size, enum upix_shape shape,
                uint32_t colour, struct upix_rect *dirty) {
    if (!im->px || size < 1) return;
    int x0 = cx - (size - 1) / 2, y0 = cy - (size - 1) / 2;
    // IN DOUBLED COORDINATES, so an even brush's centre between two
    // pixels is a whole number: pixel i's centre is 2i - (size - 1) from
    // the brush's, and the disc is everything within `size` of it.
    int r2 = size * size;
    for (int j = 0; j < size; j++) {
        int y = y0 + j;
        if (y < 0 || y >= im->h) continue;
        int dy = 2 * j - (size - 1);
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int i = 0; i < size; i++) {
            int x = x0 + i;
            if (x < 0 || x >= im->w) continue;
            int dx = 2 * i - (size - 1);
            if (shape == UPIX_ROUND && size > 2 && dx * dx + dy * dy > r2) continue;
            row[x] = colour;
        }
    }
    upix_rect_add(dirty, im, x0, y0, size, size);
}

void upix_line(struct uimg *im, int x0, int y0, int x1, int y1, int size,
               enum upix_shape shape, uint32_t colour, int from_start,
               struct upix_rect *dirty) {
    // Bresenham, all octants.
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, first = 1;
    for (;;) {
        if (!first || from_start) upix_stamp(im, x0, y0, size, shape, colour, dirty);
        first = 0;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int near(uint32_t a, uint32_t b, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

// A SCANLINE FILL over a stack of seeds: each pop fills the whole run of
// its row, then seeds the rows above and below once per run they touch.
// The bitmap is what ends it when the fill colour itself matches: with a
// tolerance, a pixel already filled can still look like the target.
int upix_fill(struct uimg *im, int x, int y, uint32_t colour, int tolerance,
              struct upix_rect *dirty) {
    if (!im->px || x < 0 || y < 0 || x >= im->w || y >= im->h) return 0;
    uint32_t target = im->px[(size_t)y * im->w + x];
    if (target == colour && tolerance == 0) return 0;
    size_t n = (size_t)im->w * im->h;
    uint8_t *seen = calloc((n + 7) / 8, 1);
    if (!seen) return -ENOMEM;
    int cap = 1024, top = 0, changed = 0;
    int *stack = malloc(sizeof(int) * 2 * (size_t)cap);
    if (!stack) { free(seen); return -ENOMEM; }
#define BIT(X, Y) ((size_t)(Y) * im->w + (size_t)(X))
#define SEEN(X, Y) (seen[BIT(X, Y) >> 3] & (1u << (BIT(X, Y) & 7)))
#define MARK(X, Y) (seen[BIT(X, Y) >> 3] |= (uint8_t)(1u << (BIT(X, Y) & 7)))
#define MATCH(X, Y) (!SEEN(X, Y) && near(im->px[BIT(X, Y)], target, tolerance))
    stack[top++] = x;
    stack[top++] = y;
    while (top) {
        int sy = stack[--top], sx = stack[--top];
        if (!MATCH(sx, sy)) continue;
        int l = sx, r = sx;
        while (l > 0 && MATCH(l - 1, sy)) l--;
        while (r + 1 < im->w && MATCH(r + 1, sy)) r++;
        uint32_t *row = im->px + (size_t)sy * im->w;
        for (int i = l; i <= r; i++) { row[i] = colour; MARK(i, sy); }
        changed += r - l + 1;
        upix_rect_add(dirty, im, l, sy, r - l + 1, 1);
        for (int dyi = -1; dyi <= 1; dyi += 2) {
            int ny = sy + dyi;
            if (ny < 0 || ny >= im->h) continue;
            int in_run = 0;
            for (int i = l; i <= r; i++) {
                int m = MATCH(i, ny);
                if (m && !in_run) {
                    if (top + 2 > 2 * cap) {
                        int *grown = realloc(stack, sizeof(int) * 4 * (size_t)cap);
                        if (!grown) { free(stack); free(seen); return -ENOMEM; }
                        stack = grown;
                        cap *= 2;
                    }
                    stack[top++] = i;
                    stack[top++] = ny;
                }
                in_run = m;
            }
        }
    }
#undef BIT
#undef SEEN
#undef MARK
#undef MATCH
    free(stack);
    free(seen);
    return changed;
}
