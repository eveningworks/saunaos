#!/usr/bin/env python3
"""Check userland/lib/upixel.c and upixundo.c -- the paint tools and their
undo -- with the host gcc, against a reference written here.

What it asserts, on seeded random pictures:

  - a STAMP covers exactly the square, or the disc of pixels whose
    centres lie within the brush's radius of its centre (computed here in
    floating point, not in the doubled-integer form the C uses), clipped
    at every edge
  - a LINE of stamps covers what stamping each Bresenham point does, and
    leaves out the first point when asked to
  - a FILL changes exactly the 4-connected region a breadth-first search
    finds, at tolerance 0 and above it, and reports how many pixels
  - every call's DIRTY RECT covers every pixel it changed
  - ONE HISTORY over TWO pictures and CUSTOM steps (a number set): each
    step undone gives back exactly the state before it, and a new step
    lets go of (drops) the custom steps in the redo list
  - SOURCE-OVER (upix_over) within one of a floating-point Porter-Duff
    on straight alpha, at full, half, near-zero and random opacity; and a
    stamp at an OFFSET (upix_over_at) clipped at every edge, leaving
    every pixel outside it untouched
  - SHAPES: a random polygon's anti-aliased fill against 16 x 16 samples
    a pixel (full is 255, empty 0, partial within a sub-row's error, the
    ink within 2% of the area), the plain fill pixel-exact against centre
    sampling; a stroke's coverage full inside, empty outside its width;
    every figure closed and inside its box
  - BRUSHES: a coverage stroke (soft round tip, any hardness, over,
    multiply or erase) within 3 of a floating-point reference at every
    pixel, after running its path forward and back again -- so crossing
    itself must not darken it; the highlighter keeps black black and
    turns white into its colour; a chisel tip covers exactly its rects;
    a spray stays inside its disc, repeats for one seed, and with enough
    dots covers all of it
  - JOINED steps (a custom step and a stroke) undo and redo as one, and
    the step before them still undoes alone
  - SELECTIONS: runs of rectangles, magic-wand picks and lassos with New,
    Add and Subtract against a set reference (the wand exactly the
    region the fill reference fills, the lasso centre-sampled even-odd),
    the box their bounds; invert the rest; the copy the selected pixels
    in their box, transparent elsewhere; paint and keep-outside touching
    only what they should
  - UNDO: after a run of strokes and fills, each recorded with begin and
    commit, undoing all of them gives back the first picture exactly,
    redoing all gives back the last, and the state number comes back to
    what it was; a step that changes the picture's size (a transpose)
    undoes too; a small byte limit drops the OLDEST steps and keeps the
    newest

    python3 tools/upixel_hostcheck.py
    python3 tools/upixel_hostcheck.py --positive-control   # must FAIL

Needs only gcc and the standard library.
"""

import argparse
import random
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

DRIVER = r"""
#include "lib/upixel.h"
#include "lib/upixundo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void uimg_free(struct uimg *im) { free(im->px); memset(im, 0, sizeof *im); }

static struct uimg ims[2];
#define im (*cur)
static struct uimg *cur = &ims[0];
static int g_val, g_drops;

// A custom step: a number set from one value to another.
struct num { int before, after; };
static void num_apply(void *p, int redo) { struct num *n = p; g_val = redo ? n->after : n->before; }
static void num_drop(void *p) { free(p); g_drops++; }
static const struct upixundo_custom NUM = { num_apply, num_drop };
static struct upix_rect dirty;
static struct upixundo u;
static struct upix_stroke stroke;
static struct upix_sel sel;

static void sel_dump(void) {
    printf("sel %d %d %d %d", sel.box.x, sel.box.y, sel.box.w, sel.box.h);
    for (int i = 0; i < sel.w * sel.h; i++) printf(" %d", sel.a[i] ? 1 : 0);
    printf("\n");
}

static void dump(void) {
    printf("img %d %d", im.w, im.h);
    for (int i = 0; i < im.w * im.h; i++) printf(" %x", im.px[i]);
    printf("\n");
}

int main(void) {
    char cmd[32];
    upixundo_init(&u, 64u << 20);
    while (scanf("%31s", cmd) == 1) {
        int a, b, c, d, e, f, g;
        unsigned col;
        if (!strcmp(cmd, "new")) {
            if (scanf("%d %d %x", &a, &b, &col) != 3) return 2;
            free(im.px);
            upixundo_clear(&u);   // a new picture: the old one's steps no longer fit it
            im.w = a; im.h = b;
            im.px = malloc((size_t)a * b * 4);
            for (int i = 0; i < a * b; i++) im.px[i] = col;
        } else if (!strcmp(cmd, "set")) {           // set X Y COLOUR
            if (scanf("%d %d %x", &a, &b, &col) != 3) return 2;
            im.px[b * im.w + a] = col;
        } else if (!strcmp(cmd, "limit")) {
            if (scanf("%d", &a) != 1) return 2;
            u.limit = (size_t)a;
        } else if (!strcmp(cmd, "stamp")) {
            if (scanf("%d %d %d %d %x", &a, &b, &c, &d, &col) != 5) return 2;
            upix_stamp(&im, a, b, c, (enum upix_shape)d, col, &dirty);
        } else if (!strcmp(cmd, "line")) {
            if (scanf("%d %d %d %d %d %d %x %d", &a, &b, &c, &d, &e, &f, &col, &g) != 8) return 2;
            upix_line(&im, a, b, c, d, e, (enum upix_shape)f, col, g, &dirty);
        } else if (!strcmp(cmd, "fill")) {
            if (scanf("%d %d %x %d", &a, &b, &col, &c) != 4) return 2;
            printf("changed %d\n", upix_fill(&im, a, b, col, c, &dirty));
        } else if (!strcmp(cmd, "clean")) {
            dirty = (struct upix_rect){ 0, 0, 0, 0 };
        } else if (!strcmp(cmd, "dirty")) {
            printf("dirty %d %d %d %d\n", dirty.x, dirty.y, dirty.w, dirty.h);
        } else if (!strcmp(cmd, "begin")) {
            dirty = (struct upix_rect){ 0, 0, 0, 0 };
            if (upixundo_begin(&u, &im)) return 3;
        } else if (!strcmp(cmd, "commit")) {
            if (upixundo_commit(&u, &im, &dirty)) return 3;
        } else if (!strcmp(cmd, "transpose")) {       // a whole-picture change
            uint32_t *t = malloc((size_t)im.w * im.h * 4);
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) t[x * im.h + y] = im.px[y * im.w + x];
            free(im.px); im.px = t;
            int w = im.w; im.w = im.h; im.h = w;
            if (upixundo_commit(&u, &im, 0)) return 3;
        } else if (!strcmp(cmd, "undo")) {
            printf("undo %d\n", upixundo_undo(&u, 0));
        } else if (!strcmp(cmd, "redo")) {
            printf("redo %d\n", upixundo_redo(&u, 0));
        } else if (!strcmp(cmd, "state")) {
            printf("state %u %d %d\n", upixundo_state(&u), upixundo_can_undo(&u), u.count);
        } else if (!strcmp(cmd, "on")) {             // on N: the picture the next commands paint
            if (scanf("%d", &a) != 1) return 2;
            cur = &ims[a & 1];
        } else if (!strcmp(cmd, "custom")) {         // custom V: the number set to V, as a step
            if (scanf("%d", &a) != 1) return 2;
            struct num *n = malloc(sizeof *n);
            n->before = g_val; n->after = a; g_val = a;
            if (upixundo_push(&u, &NUM, n, sizeof *n)) return 3;
        } else if (!strcmp(cmd, "join")) {          // the next step joins the one before it
            upixundo_join(&u);
        } else if (!strcmp(cmd, "val")) {
            printf("val %d %d\n", g_val, g_drops);
        } else if (!strcmp(cmd, "over")) {           // over OPACITY: picture 1 laid on picture 0
            if (scanf("%d", &a) != 1) return 2;
            struct upix_rect all = { 0, 0, ims[0].w, ims[0].h };
            upix_over(&ims[0], &ims[1], &all, a);
        } else if (!strcmp(cmd, "overat")) {         // overat X Y: picture 1 stamped on picture 0 at X, Y
            if (scanf("%d %d", &a, &b) != 2) return 2;
            upix_over_at(&ims[0], a, b, &ims[1]);
        } else if (!strcmp(cmd, "mask")) {          // mask KIND(fill|stroke) AA CLOSED WIDTH N x y ...
            char kind[8];
            float width;
            if (scanf("%7s %d %d %f %d", kind, &a, &b, &width, &c) != 5) return 2;
            struct upix_pt *pts = malloc(sizeof *pts * (size_t)(c ? c : 1));
            for (int i = 0; i < c; i++) if (scanf("%f %f", &pts[i].x, &pts[i].y) != 2) return 2;
            struct upix_mask m;
            int rc = kind[0] == 'f' ? upix_mask_fill(&m, pts, c, 64, 64, a)
                                    : upix_mask_stroke(&m, pts, c, b, width, 64, 64, a);
            printf("mask %d %d %d %d %d", rc, m.x, m.y, m.w, m.h);
            for (int i = 0; i < m.w * m.h; i++) printf(" %d", m.a[i]);
            printf("\n");
            upix_mask_free(&m);
            free(pts);
        } else if (!strcmp(cmd, "figure")) {        // figure KIND x0 y0 x1 y1
            float x0, y0, x1, y1;
            if (scanf("%d %f %f %f %f", &a, &x0, &y0, &x1, &y1) != 5) return 2;
            struct upix_pt pts[512];
            int n = upix_figure((enum upix_figure)a, x0, y0, x1, y1, pts, 512);
            printf("figure %d", n);
            for (int i = 0; i < n; i++) printf(" %.4f %.4f", pts[i].x, pts[i].y);
            printf("\n");
        } else if (!strcmp(cmd, "sbegin")) {        // sbegin COLOUR BLEND: a coverage stroke on this picture
            if (scanf("%x %d", &col, &a) != 2) return 2;
            if (upix_stroke_begin(&stroke, &im, col, (enum upix_blend)a)) return 3;
        } else if (!strcmp(cmd, "sround")) {        // sround x0 y0 x1 y1 SIZE HARDNESS
            float x0, y0, x1, y1, sz;
            if (scanf("%f %f %f %f %f %d", &x0, &y0, &x1, &y1, &sz, &a) != 6) return 2;
            upix_stroke_round(&stroke, x0, y0, x1, y1, sz, a, &dirty);
        } else if (!strcmp(cmd, "schisel")) {       // schisel x0 y0 x1 y1 W H
            if (scanf("%d %d %d %d %d %d", &a, &b, &c, &d, &e, &f) != 6) return 2;
            upix_stroke_chisel(&stroke, a, b, c, d, e, f, &dirty);
        } else if (!strcmp(cmd, "send")) {
            upix_stroke_end(&stroke);
        } else if (!strcmp(cmd, "spray")) {         // spray CX CY SIZE N COLOUR SEED
            unsigned seed;
            if (scanf("%d %d %d %d %x %u", &a, &b, &c, &d, &col, &seed) != 6) return 2;
            upix_spray(&im, a, b, c, d, col, &seed, &dirty);
        } else if (!strcmp(cmd, "sinit")) {         // a selection the picture's size
            upix_sel_free(&sel);
            if (upix_sel_init(&sel, im.w, im.h)) return 3;
        } else if (!strcmp(cmd, "srect")) {         // srect x0 y0 x1 y1 OP
            if (scanf("%d %d %d %d %d", &a, &b, &c, &d, &e) != 5) return 2;
            upix_sel_rect(&sel, a, b, c, d, (enum upix_sel_op)e);
        } else if (!strcmp(cmd, "swand")) {         // swand X Y TOL OP
            if (scanf("%d %d %d %d", &a, &b, &c, &d) != 4) return 2;
            printf("wand %d\n", upix_sel_wand(&sel, &im, a, b, c, (enum upix_sel_op)d));
        } else if (!strcmp(cmd, "spoly")) {         // spoly OP N x y ...
            if (scanf("%d %d", &a, &c) != 2) return 2;
            struct upix_pt *pts = malloc(sizeof *pts * (size_t)(c ? c : 1));
            for (int i = 0; i < c; i++) if (scanf("%f %f", &pts[i].x, &pts[i].y) != 2) return 2;
            if (upix_sel_poly(&sel, pts, c, (enum upix_sel_op)a)) return 3;
            free(pts);
        } else if (!strcmp(cmd, "sinv")) {
            upix_sel_invert(&sel);
        } else if (!strcmp(cmd, "sall")) {
            upix_sel_all(&sel);
        } else if (!strcmp(cmd, "sdump")) {
            sel_dump();
        } else if (!strcmp(cmd, "scopy")) {         // the cut-out, as an image
            struct uimg o;
            int rc = upix_sel_copy(&sel, &im, &o);
            printf("copy %d %d %d", rc, o.w, o.h);
            for (int i = 0; i < o.w * o.h; i++) printf(" %x", o.px[i]);
            printf("\n");
            free(o.px);
        } else if (!strcmp(cmd, "spaint")) {        // spaint COLOUR
            if (scanf("%x", &col) != 1) return 2;
            upix_sel_paint(&sel, &im, col, &dirty);
        } else if (!strcmp(cmd, "skeep")) {         // skeep X Y W H: picture 0 kept outside from picture 1
            if (scanf("%d %d %d %d", &a, &b, &c, &d) != 4) return 2;
            struct upix_rect r = { a, b, c, d };
            upix_sel_keep_outside(&sel, &ims[0], &ims[1], &r);
        } else if (!strcmp(cmd, "dump")) {
            dump();
        } else return 2;
        fflush(stdout);
    }
    return 0;
}
"""


def build(tmp, poison=False):
    hostcheck.stage(tmp, "userland/lib/uimg.h", dest="lib/uimg.h")
    hostcheck.stage(tmp, "userland/lib/upixel.h", dest="lib/upixel.h")
    hostcheck.stage(tmp, "userland/lib/upixundo.h", dest="lib/upixundo.h")
    # The control: a fill that looks at 8 neighbours' rows but forgets to
    # seed the row BELOW -- a region reached only by going down stays.
    # The second: source-over that forgets to scale the picture below by
    # what the one on top leaves of it. The third: a stroke that blends
    # onto what it has already painted, so crossing itself darkens. The
    # fourth: a Subtract that adds.
    src = hostcheck.stage(tmp, "userland/lib/upixel.c", apply=poison, tool="upixel_hostcheck",
                          edits=[("for (int dyi = -1; dyi <= 1; dyi += 2) {",
                                  "for (int dyi = -1; dyi <= -1; dyi += 2) {"),
                                 ("unsigned da = ((dp >> 24) * (255 - sa) + 127) / 255, oa = sa + da;",
                                  "unsigned da = dp >> 24, oa = sa + da > 255 ? 255 : sa + da;"),
                                 ("s->im->px[i] = blend_px(s, s->before[i], c);",
                                  "s->im->px[i] = blend_px(s, s->im->px[i], c);"),
                                 ("return op == UPIX_SEL_SUB ? 0 : 255;", "return op == UPIX_SEL_SUB ? 255 : 255;")])
    und = hostcheck.stage(tmp, "userland/lib/upixundo.c")
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "upixel_host", [src, und, drv, "-lm"], includes=[tmp],
                             tool="upixel_hostcheck")


# --- the reference ------------------------------------------------------

class Pic:
    def __init__(self, w, h, c):
        self.w, self.h = w, h
        self.px = [c] * (w * h)

    def copy(self):
        p = Pic(self.w, self.h, 0)
        p.px = list(self.px)
        return p


def ref_stamp(p, cx, cy, size, shape, c):
    x0, y0 = cx - (size - 1) // 2, cy - (size - 1) // 2
    centre_x, centre_y = x0 + size / 2.0, y0 + size / 2.0
    r = size / 2.0
    for j in range(size):
        for i in range(size):
            x, y = x0 + i, y0 + j
            if not (0 <= x < p.w and 0 <= y < p.h):
                continue
            if shape == 0 and size > 2:
                dx, dy = x + 0.5 - centre_x, y + 0.5 - centre_y
                if dx * dx + dy * dy > r * r + 1e-9:
                    continue
            p.px[y * p.w + x] = c


def bresenham(x0, y0, x1, y1):
    pts = []
    dx, sx = abs(x1 - x0), 1 if x0 < x1 else -1
    dy, sy = -abs(y1 - y0), 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        pts.append((x0, y0))
        if x0 == x1 and y0 == y1:
            return pts
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def ref_line(p, x0, y0, x1, y1, size, shape, c, from_start):
    pts = bresenham(x0, y0, x1, y1)
    for k, (x, y) in enumerate(pts):
        if k == 0 and not from_start:
            continue
        ref_stamp(p, x, y, size, shape, c)


def near(a, b, tol):
    return all(abs(((a >> s) & 0xFF) - ((b >> s) & 0xFF)) <= tol for s in (0, 8, 16, 24))


def ref_fill(p, x, y, c, tol):
    target = p.px[y * p.w + x]
    if target == c and tol == 0:
        return 0
    seen, todo, region = {(x, y)}, [(x, y)], []
    while todo:
        cx, cy = todo.pop()
        if not near(p.px[cy * p.w + cx], target, tol):
            continue
        region.append((cx, cy))
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if 0 <= nx < p.w and 0 <= ny < p.h and (nx, ny) not in seen:
                seen.add((nx, ny))
                todo.append((nx, ny))
    for cx, cy in region:
        p.px[cy * p.w + cx] = c
    return len(region)


# --- driving ------------------------------------------------------------

class Driver:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def ask(self, line):
        self.send(line)
        return self.p.stdout.readline().split()

    def dump(self):
        r = self.ask("dump")
        w, h = int(r[1]), int(r[2])
        return w, h, [int(v, 16) for v in r[3:]]

    def close(self):
        self.p.stdin.close()
        self.p.wait()


PALETTE = [0xFFFFFFFF, 0xFF000000, 0xFFFF0000, 0xFF00C000, 0xFF0000FF, 0xFFFFE000, 0x80808080]


def random_picture(rng, d, w, h):
    """Blobs of a few colours, so a fill has regions with holes, corridors
    and diagonals to get wrong."""
    p = Pic(w, h, PALETTE[0])
    d.send(f"new {w} {h} {PALETTE[0]:x}")
    for _ in range(w * h // 3):
        x, y = rng.randrange(w), rng.randrange(h)
        c = rng.choice(PALETTE[:4])
        p.px[y * w + x] = c
        d.send(f"set {x} {y} {c:x}")
    return p


def diff(a, b):
    return [i for i, (u, v) in enumerate(zip(a, b)) if u != v]


def covers(rect, w, idxs):
    x, y, rw, rh = rect
    return all(x <= i % w < x + rw and y <= i // w < y + rh for i in idxs)


# --- brushes: the coverage stroke and the spray -------------------------------

BRUSH_CHECKS = [0]


def seg_dist(px, py, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = dx * dx + dy * dy
    t = ((px - a[0]) * dx + (py - a[1]) * dy) / l2 if l2 > 0 else 0
    t = min(1.0, max(0.0, t))
    ex, ey = a[0] + t * dx - px, a[1] + t * dy - py
    return (ex * ex + ey * ey) ** 0.5


def round_cov(d, size, hard):
    r = size / 2
    soft = max(1.0, r * (100 - hard) / 100)
    inner = r - soft
    if d >= r:
        return 0
    if d <= inner:
        return 255
    t = (d - inner) / soft
    return int((1 - t * t * (3 - 2 * t)) * 255 + 0.5)


def chan(c, sh):
    return (c >> sh) & 0xFF


def ref_blend(d, col, c, blend):
    """Floating point: the pixel `d` at coverage c/255 of `col`."""
    k = c / 255
    da = (d >> 24) / 255
    if blend == 2:   # erase
        a = da * (1 - k)
        return [chan(d, 16), chan(d, 8), chan(d, 0), a * 255]
    if blend == 1:   # multiply, over half the colour where there is nothing
        m = [chan(d, sh) - (chan(d, sh) - chan(d, sh) * chan(col, sh) / 255) * k for sh in (16, 8, 0)]
        ua = 0.5 * k
        oa = da + ua * (1 - da)
        if oa <= 0:
            return [0, 0, 0, 0]
        rgb = [(m[i] * da + chan(col, sh) * ua * (1 - da)) / oa for i, sh in enumerate((16, 8, 0))]
        return rgb + [oa * 255]
    sa = (col >> 24) / 255 * k   # over
    oa = sa + da * (1 - sa)
    if oa <= 0:
        return [0, 0, 0, 0]
    rgb = [(chan(col, sh) * sa + chan(d, sh) * da * (1 - sa)) / oa for sh in (16, 8, 0)]
    return rgb + [oa * 255]


def near_px(got, want, tol):
    g = [chan(got, 16), chan(got, 8), chan(got, 0), got >> 24]
    if want[3] < 0.5 and g[3] == 0:
        return True
    if abs(g[3] - want[3]) > tol:
        return False
    # Colour means little where alpha is nearly nothing.
    t = tol if want[3] >= 8 else 255
    return all(abs(g[i] - want[i]) <= t for i in range(3))


def brush_checks(rng, d, fails):
    n = 0
    colours = [0xFFFFFFFF, 0xFF000000, 0xFF2040C0, 0x80FF8000, 0x00000000, 0xFFFFF200]
    for case in range(40):
        w, h = rng.randrange(12, 40), rng.randrange(12, 40)
        pic = [rng.choice(colours) for _ in range(w * h)]
        d.send(f"new {w} {h} 0")
        for i, c in enumerate(pic):
            d.send(f"set {i % w} {i // w} {c:x}")
        blend = case % 3
        col = rng.choice([0xFFFFF200, 0xFF3F48CC, 0xC0ED1C24])
        size, hard = rng.uniform(1, 14), rng.choice([0, 30, 50, 100, rng.randrange(101)])
        pts = [(rng.uniform(-3, w + 3), rng.uniform(-3, h + 3)) for _ in range(rng.randrange(2, 5))]
        # Crossing back over itself: the same path forward, then backward.
        path = pts + pts[-2::-1]
        d.send("clean")
        d.send(f"sbegin {col:x} {blend}")
        for a, b in zip(path, path[1:]):
            d.send(f"sround {a[0]} {a[1]} {b[0]} {b[1]} {size} {hard}")
        d.send("send")
        _, _, got = d.dump()
        rect = [int(v) for v in d.ask("dirty")[1:]]
        n += 2
        what = f"stroke case {case} blend {blend} size {size:.2f} hardness {hard}"
        bad = None
        for i in range(w * h):
            X, Y = i % w + 0.5, i // w + 0.5
            dm = min(seg_dist(X, Y, a, b) for a, b in zip(pts, pts[1:]))
            c = max(round_cov(dm - 1e-4, size, hard), 0), round_cov(dm + 1e-4, size, hard)
            ok = any(near_px(got[i], ref_blend(pic[i], col, cv, blend), 3) if cv else got[i] == pic[i]
                     for cv in set(c))
            if not ok:
                bad = (i % w, i // w, dm, got[i], pic[i])
                break
        if bad:
            fails.append(f"{what}: pixel {bad[0]},{bad[1]} at distance {bad[2]:.2f} is {bad[3]:08x} "
                         f"(was {bad[4]:08x})")
        if not covers(rect, w, diff(pic, got)):
            fails.append(f"{what}: dirty {rect} misses a changed pixel")

    # THE HIGHLIGHTER'S PROMISES: black stays black, white takes the full
    # colour, an empty pixel takes the colour at half strength.
    d.send("new 3 1 0")
    d.send("set 0 0 ff000000")
    d.send("set 1 0 ffffffff")
    d.send("sbegin fffff200 1")
    d.send("sround -5 0.5 9 0.5 6 100")
    d.send("send")
    _, _, got = d.dump()
    n += 1
    if got[0] != 0xFF000000 or got[1] != 0xFFFFF200 or (got[2] >> 24) not in (127, 128) \
            or got[2] & 0xFFFFFF != 0xFFF200:
        fails.append("highlighter over black, white, nothing: " + " ".join(f"{v:08x}" for v in got))

    # The chisel covers exactly its rects along the Bresenham points.
    for case in range(20):
        w, h = rng.randrange(10, 40), rng.randrange(10, 40)
        d.send(f"new {w} {h} ffffffff")
        tw, th = rng.randrange(1, 6), rng.randrange(1, 12)
        x0, y0, x1, y1 = (rng.randrange(-4, w + 4), rng.randrange(-4, h + 4),
                          rng.randrange(-4, w + 4), rng.randrange(-4, h + 4))
        d.send("sbegin ff22b14c 0")
        d.send(f"schisel {x0} {y0} {x1} {y1} {tw} {th}")
        d.send("send")
        _, _, got = d.dump()
        want = set()
        for px, py in bresenham(x0, y0, x1, y1):
            for j in range(th):
                for i in range(tw):
                    x, y = px - (tw - 1) // 2 + i, py - (th - 1) // 2 + j
                    if 0 <= x < w and 0 <= y < h:
                        want.add(y * w + x)
        have = {i for i, v in enumerate(got) if v == 0xFF22B14C}
        n += 1
        if have != want or any(v not in (0xFF22B14C, 0xFFFFFFFF) for v in got):
            fails.append(f"chisel case {case} ({x0},{y0})-({x1},{y1}) {tw}x{th}: "
                         f"{len(have ^ want)} pixel(s) differ")

    # Spray: only inside the brush's disc, at most N, the same for the same seed.
    for case in range(20):
        w, h = rng.randrange(10, 40), rng.randrange(10, 40)
        size, cnt, seed = rng.randrange(1, 16), rng.randrange(1, 200), rng.randrange(1, 1 << 31)
        cx, cy = rng.randrange(-3, w + 3), rng.randrange(-3, h + 3)
        disc = Pic(w, h, 0)
        ref_stamp(disc, cx, cy, size, 0, 1)
        runs = []
        for _ in range(2):
            d.send(f"new {w} {h} ffffffff")
            d.send(f"spray {cx} {cy} {size} {cnt} ff00a2e8 {seed}")
            runs.append(d.dump()[2])
        hit = [i for i, v in enumerate(runs[0]) if v != 0xFFFFFFFF]
        n += 2
        if any(not disc.px[i] for i in hit) or len(hit) > cnt:
            fails.append(f"spray case {case}: {len(hit)} pixel(s), some outside the {size}-px disc or over {cnt}")
        if runs[0] != runs[1]:
            fails.append(f"spray case {case}: the same seed sprayed differently")
    # Enough dots fill the disc: a test that the offsets reach every cell.
    d.send("new 20 20 ffffffff")
    d.send("spray 10 10 9 4000 ff000000 77")
    got = d.dump()[2]
    disc = Pic(20, 20, 0)
    ref_stamp(disc, 10, 10, 9, 0, 1)
    n += 1
    if [v == 0xFF000000 for v in got] != [bool(v) for v in disc.px]:
        fails.append("4000 dots of a 9-px spray do not cover exactly its disc")
    BRUSH_CHECKS[0] = n


# --- selections ------------------------------------------------------------------

def ask_sel(d, w, h):
    r = d.ask("sdump")
    box = [int(v) for v in r[1:5]]
    return box, {i for i, v in enumerate(r[5:]) if v == "1"}


def box_of(cells, w):
    if not cells:
        return [0, 0, 0, 0]
    xs, ys = [i % w for i in cells], [i // w for i in cells]
    return [min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1]


def inside_poly(px, py, pts):
    # Even-odd, at the pixel's centre.
    n, c = len(pts), False
    for i in range(n):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
        if (y0 > py) != (y1 > py) and px < x0 + (py - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def combine(have, new, op):
    return new if op == 0 else have | new if op == 1 else have - new


def selection_checks(rng, d, fails):
    n = 0
    for case in range(30):
        w, h = rng.randrange(8, 30), rng.randrange(8, 30)
        pic = random_picture(rng, d, w, h)
        d.send("sinit")
        want = set()
        for step in range(rng.randrange(1, 5)):
            op = 0 if step == 0 else rng.randrange(3)
            kind = rng.randrange(3)
            if kind == 0:
                x0, y0, x1, y1 = (rng.randrange(-3, w + 3), rng.randrange(-3, h + 3),
                                  rng.randrange(-3, w + 3), rng.randrange(-3, h + 3))
                d.send(f"srect {x0} {y0} {x1} {y1} {op}")
                lx, hx, ly, hy = min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1)
                new = {y * w + x for y in range(max(0, ly), min(h, hy)) for x in range(max(0, lx), min(w, hx))}
                what = f"rect ({x0},{y0})-({x1},{y1})"
            elif kind == 1:
                x, y, tol = rng.randrange(w), rng.randrange(h), rng.choice([0, 0, 40, 255])
                got_n = int(d.ask(f"swand {x} {y} {tol} {op}")[1])
                q = pic.copy()
                ref_fill(q, x, y, 0x12345678, tol)
                new = {i for i in range(w * h) if q.px[i] != pic.px[i]}
                n += 1
                if got_n != len(new):
                    fails.append(f"selection case {case}: the wand at {x},{y} tol {tol} took {got_n}, the fill {len(new)}")
                what = f"wand ({x},{y}) tol {tol}"
            else:
                pts = [(rng.uniform(-2, w + 2), rng.uniform(-2, h + 2)) for _ in range(rng.randrange(3, 7))]
                d.send(f"spoly {op} {len(pts)} " + " ".join(f"{x} {y}" for x, y in pts))
                new = {y * w + x for y in range(h) for x in range(w) if inside_poly(x + 0.5, y + 0.5, pts)}
                what = f"lasso of {len(pts)}"
            want = combine(want, new, op)
            box, got = ask_sel(d, w, h)
            n += 2
            if got != want:
                fails.append(f"selection case {case} step {step} {what} op {op}: {len(got ^ want)} pixel(s) differ")
                break
            if box != box_of(want, w):
                fails.append(f"selection case {case} step {step}: box {box}, the pixels' {box_of(want, w)}")
        d.send("sinv")
        box, got = ask_sel(d, w, h)
        n += 1
        if got != set(range(w * h)) - want or box != box_of(set(range(w * h)) - want, w):
            fails.append(f"selection case {case}: invert is not the rest")
        d.send("sinv")
        # The cut-out: the box's size, the pixels where selected, nothing elsewhere.
        r = d.ask("scopy")
        n += 1
        if want:
            bx, by, bw, bh = box_of(want, w)
            px = [int(v, 16) for v in r[4:]]
            ok = int(r[1]) == 0 and [int(r[2]), int(r[3])] == [bw, bh] and all(
                px[j * bw + i] == (pic.px[(by + j) * w + bx + i] if (by + j) * w + bx + i in want else 0)
                for j in range(bh) for i in range(bw))
            if not ok:
                fails.append(f"selection case {case}: the copy is not the selected pixels in their box")
        elif int(r[1]) == 0:
            fails.append(f"selection case {case}: a copy of nothing succeeded")
        # Delete paints the selected pixels only; keep_outside puts back the rest.
        d.send("spaint ff00ff00")
        got = d.dump()[2]
        n += 1
        if any(got[i] != (0xFF00FF00 if i in want else pic.px[i]) for i in range(w * h)):
            fails.append(f"selection case {case}: paint touched a pixel outside, or missed one inside")
    # KEEP OUTSIDE: picture 0 painted all over, picture 1 the before.
    d.send("on 1")
    d.send("new 10 10 ffffffff")
    d.send("on 0")
    d.send("new 10 10 ffffffff")
    d.send("sinit")
    d.send("srect 2 2 6 6 0")
    d.send("stamp 5 5 10 1 ff000000")
    d.send("skeep 0 0 10 10")
    got = d.dump()[2]
    n += 1
    inside = {y * 10 + x for y in range(2, 6) for x in range(2, 6)}
    if any(got[i] != (0xFF000000 if i in inside else 0xFFFFFFFF) for i in range(100)):
        fails.append("keep_outside left a pixel painted outside the selection, or undid one inside")
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile a fill that never goes down; the run must then fail")
    ap.add_argument("--seed", type=int, default=1014)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    fails, checks = [], 0

    with tempfile.TemporaryDirectory() as tmp:
        d = Driver(build(tmp, poison=args.positive_control))

        # Stamps and lines, every size up to 9 and both shapes, at the
        # edges too.
        for case in range(60):
            w, h = rng.randrange(6, 30), rng.randrange(6, 30)
            p = Pic(w, h, PALETTE[0])
            d.send(f"new {w} {h} {PALETTE[0]:x}")
            d.send("clean")
            before = list(p.px)
            size, shape = rng.randrange(1, 10), rng.randrange(2)
            if case % 2:
                x0, y0 = rng.randrange(-4, w + 4), rng.randrange(-4, h + 4)
                x1, y1 = rng.randrange(-4, w + 4), rng.randrange(-4, h + 4)
                fs = rng.randrange(2)
                ref_line(p, x0, y0, x1, y1, size, shape, PALETTE[2], fs)
                d.send(f"line {x0} {y0} {x1} {y1} {size} {shape} {PALETTE[2]:x} {fs}")
                what = f"line ({x0},{y0})-({x1},{y1}) size {size} shape {shape} from_start {fs}"
            else:
                cx, cy = rng.randrange(-3, w + 3), rng.randrange(-3, h + 3)
                ref_stamp(p, cx, cy, size, shape, PALETTE[2])
                d.send(f"stamp {cx} {cy} {size} {shape} {PALETTE[2]:x}")
                what = f"stamp ({cx},{cy}) size {size} shape {shape} in {w}x{h}"
            _, _, got = d.dump()
            rect = [int(v) for v in d.ask("dirty")[1:]]
            checks += 2
            bad = diff(got, p.px)
            if bad:
                fails.append(f"{what}: {len(bad)} pixel(s) differ, first at {bad[0] % w},{bad[0] // w}")
            if not covers(rect, w, diff(before, got)):
                fails.append(f"{what}: dirty {rect} misses a changed pixel")

        # Fills, at tolerance 0 and above it.
        for case in range(40):
            w, h = rng.randrange(8, 40), rng.randrange(8, 40)
            p = random_picture(rng, d, w, h)
            d.send("clean")
            before = list(p.px)
            x, y, tol = rng.randrange(w), rng.randrange(h), rng.choice([0, 0, 0, 40, 255])
            want = ref_fill(p, x, y, PALETTE[4], tol)
            got_n = int(d.ask(f"fill {x} {y} {PALETTE[4]:x} {tol}")[1])
            _, _, got = d.dump()
            rect = [int(v) for v in d.ask("dirty")[1:]]
            checks += 3
            what = f"fill ({x},{y}) tol {tol} in {w}x{h} seed {args.seed} case {case}"
            if got_n != want:
                fails.append(f"{what}: reported {got_n} changed, the reference {want}")
            bad = diff(got, p.px)
            if bad:
                fails.append(f"{what}: {len(bad)} pixel(s) differ, first at {bad[0] % w},{bad[0] // w}")
            if not covers(rect, w, diff(before, got)):
                fails.append(f"{what}: dirty {rect} misses a changed pixel")

        # Undo and redo through a history of strokes, fills and a resize.
        for case in range(10):
            w, h = rng.randrange(10, 40), rng.randrange(10, 40)
            random_picture(rng, d, w, h)
            first = d.dump()
            d.ask("state")
            steps = rng.randrange(3, 12)
            for k in range(steps):
                d.send("begin")
                if k == steps // 2:
                    d.send("transpose")
                    continue
                cw, ch = d.dump()[:2]
                if rng.randrange(2):
                    d.ask(f"fill {rng.randrange(cw)} {rng.randrange(ch)} {rng.choice(PALETTE):x} 0")
                else:
                    d.send(f"line {rng.randrange(cw)} {rng.randrange(ch)} {rng.randrange(cw)} "
                           f"{rng.randrange(ch)} {rng.randrange(1, 7)} 0 {rng.choice(PALETTE):x} 1")
                d.send("commit")
            last = d.dump()
            end_state = d.ask("state")[1]
            undone = 0
            while d.ask("undo")[1] == "1":
                undone += 1
            checks += 3
            if d.dump() != first:
                fails.append(f"undo case {case}: undoing all {undone} step(s) is not the first picture")
            while d.ask("redo")[1] == "1":
                pass
            if d.dump() != last:
                fails.append(f"undo case {case}: redoing all is not the last picture")
            if d.ask("state")[1] != end_state:
                fails.append(f"undo case {case}: the state number did not come back")

        # ONE HISTORY OVER TWO PICTURES AND A CUSTOM STEP: each step undone
        # puts back exactly the state before it -- both pictures and the
        # number -- and the right picture is the one that changes.
        for case in range(20):
            d.send("on 0")
            random_picture(rng, d, 9, 9)
            d.send("on 1")
            random_picture(rng, d, 9, 9)
            d.send("custom 0")
            d.ask("undo")   # the number starts at 0, with nothing to redo past it
            states = []

            def snap():
                d.send("on 0"); a = d.dump()
                d.send("on 1"); b = d.dump()
                return a, b, d.ask("val")[1]
            states.append(snap())
            for step in range(rng.randrange(3, 10)):
                if rng.randrange(3) == 0:
                    d.send(f"custom {rng.randrange(1, 1000)}")
                else:
                    d.send(f"on {rng.randrange(2)}")
                    d.send("begin")
                    d.send(f"stamp {rng.randrange(9)} {rng.randrange(9)} {rng.randrange(1, 5)} 1 "
                           f"{rng.choice(PALETTE):x}")
                    d.send("commit")
                states.append(snap())
            checks += 1
            for back in range(len(states) - 1, 0, -1):
                d.ask("undo")
                if snap() != states[back - 1]:
                    fails.append(f"layered undo case {case}: undoing to step {back - 1} is not that state")
                    break
            drops = int(d.ask("val")[2])
            d.send("on 0"); d.send("begin"); d.send("stamp 1 1 1 1 ff123456"); d.send("commit")
            checks += 1
            customs = sum(1 for i in range(1, len(states)) if states[i][2] != states[i - 1][2])
            if int(d.ask("val")[2]) - drops < customs:
                fails.append(f"layered undo case {case}: a new step let go of fewer custom steps "
                             f"than the redo list held ({customs})")

        # SOURCE-OVER against floating point: every channel within one.
        def over_ref(dp, sp, op):
            sa = (sp >> 24) / 255 * op / 255
            da = (dp >> 24) / 255
            oa = sa + da * (1 - sa)
            if oa == 0:
                return dp
            out = round(oa * 255) << 24
            for sh in (0, 8, 16):
                cs, cd = (sp >> sh) & 255, (dp >> sh) & 255
                out |= round((cs * sa + cd * da * (1 - sa)) / oa) << sh
            return out
        for case in range(30):
            w, h = 8, 6
            pics = []
            for k in (0, 1):
                px = [rng.randrange(256) << 24 | rng.randrange(1 << 24) for _ in range(w * h)]
                for i in rng.sample(range(w * h), 6):
                    px[i] = rng.choice((0, 255)) << 24 | (px[i] & 0xFFFFFF)
                d.send(f"on {k}")
                d.send(f"new {w} {h} 0")
                for i, v in enumerate(px):
                    d.send(f"set {i % w} {i // w} {v:x}")
                pics.append(px)
            op = rng.choice((255, 128, 1, rng.randrange(256)))
            d.send(f"over {op}")
            d.send("on 0")
            got = d.dump()[2]
            checks += 1
            for i in range(w * h):
                want = over_ref(pics[0][i], pics[1][i], op)
                if (want >> 24) == 0 and (pics[1][i] >> 24) * op < 128:
                    want = pics[0][i]   # nothing laid on: the pixel below, untouched
                # ALPHA IS 8 BITS: each alpha's half-unit rounding moves a
                # colour by up to |above - below| / out-alpha -- a lot when
                # the result is nearly transparent, nothing when opaque.
                oa = max(want >> 24, 1)

                def tol(sh):
                    return 1 if sh == 24 else 1 + -(-abs(((pics[1][i] >> sh) & 255) - ((pics[0][i] >> sh) & 255)) // oa)
                if any(abs(((got[i] >> sh) & 255) - ((want >> sh) & 255)) > tol(sh) for sh in (0, 8, 16, 24)) \
                        and not ((got[i] >> 24) == 0 and (want >> 24) == 0):
                    fails.append(f"over case {case}: pixel {i} {pics[1][i]:08x} on {pics[0][i]:08x} "
                                 f"at {op} is {got[i]:08x}, want {want:08x}")
                    break

        # A STAMP AT AN OFFSET, clipped: inside it the same source-over,
        # outside it the picture below untouched -- every pixel checked.
        for case in range(20):
            dw, dh, sw, sh = 10, 8, rng.randrange(1, 7), rng.randrange(1, 6)
            ox, oy = rng.randrange(-4, 12), rng.randrange(-4, 10)
            base = [rng.randrange(1 << 32) for _ in range(dw * dh)]
            top = [rng.randrange(256) << 24 | rng.randrange(1 << 24) for _ in range(sw * sh)]
            for k, (w, h, px) in enumerate(((dw, dh, base), (sw, sh, top))):
                d.send(f"on {k}")
                d.send(f"new {w} {h} 0")
                for i, v in enumerate(px):
                    d.send(f"set {i % w} {i // w} {v:x}")
            d.send(f"overat {ox} {oy}")
            d.send("on 0")
            got = d.dump()[2]
            checks += 1
            for i in range(dw * dh):
                x, y = i % dw, i // dw
                sx, sy = x - ox, y - oy
                if not (0 <= sx < sw and 0 <= sy < sh):
                    if got[i] != base[i]:
                        fails.append(f"overat case {case}: pixel {x},{y} outside the stamp changed")
                        break
                    continue
                s = top[sy * sw + sx]
                want = base[i] if (s >> 24) == 0 else over_ref(base[i], s, 255)
                oa = max(want >> 24, 1)
                if any(abs(((got[i] >> b) & 255) - ((want >> b) & 255)) >
                       (1 if b == 24 else 1 + -(-abs(((s >> b) & 255) - ((base[i] >> b) & 255)) // oa))
                       for b in (0, 8, 16, 24)):
                    fails.append(f"overat case {case}: pixel {x},{y} is {got[i]:08x}, want {want:08x}")
                    break

        # SHAPES: a polygon's fill and a path's stroke against 16 x 16
        # samples per pixel, computed here. Anti-aliased: a pixel every
        # sample covers is 255, one none covers is 0, a partial one within
        # what four sub-rows can miss, and the whole mask's ink within 2%
        # of the area. Without: a pixel is in exactly when its centre is.
        import math

        def inside(px, py, pts):
            c = False
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                if (ay <= py) != (by <= py) and px < ax + (py - ay) * (bx - ax) / (by - ay):
                    c = not c
            return c

        def segd(px, py, a, b):
            dx, dy = b[0] - a[0], b[1] - a[1]
            l2 = dx * dx + dy * dy
            t = 0 if l2 == 0 else max(0, min(1, ((px - a[0]) * dx + (py - a[1]) * dy) / l2))
            return math.hypot(a[0] + t * dx - px, a[1] + t * dy - py)

        def ask_mask(kind, aa, closed, width, pts):
            r = d.ask(f"mask {kind} {aa} {closed} {width} {len(pts)} " + " ".join(f"{x} {y}" for x, y in pts))
            x, y, w, h = map(int, r[2:6])
            return x, y, w, h, list(map(int, r[6:]))

        def area(pts):
            return abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
                           for i in range(len(pts)))) / 2
        S = 16
        for case in range(12):
            n = rng.randrange(3, 8)
            cx, cy, rad = rng.uniform(20, 44), rng.uniform(20, 44), rng.uniform(6, 18)
            pts = [(cx + rad * rng.uniform(0.5, 1) * math.cos(2 * math.pi * k / n + rng.uniform(0, 0.4)),
                    cy + rad * rng.uniform(0.5, 1) * math.sin(2 * math.pi * k / n + rng.uniform(0, 0.4)))
                   for k in range(n)]
            for aa in (1, 0):
                mx, my, mw, mh, cov = ask_mask("fill", aa, 1, 0, pts)
                checks += 1
                bad = None
                for j in range(mh):
                    for i in range(mw):
                        X, Y = mx + i, my + j
                        v = cov[j * mw + i]
                        if not aa:
                            want = 255 if inside(X + 0.5, Y + 0.5, pts) else 0
                            if v != want:
                                bad = f"pixel {X},{Y} is {v}, its centre says {want}"
                            continue
                        k = sum(inside(X + (a + 0.5) / S, Y + (b + 0.5) / S, pts) for a in range(S) for b in range(S))
                        # A sliver every sample misses is still a sliver: 16 either end.
                        if (k == S * S and v < 239) or (k == 0 and v > 16) or abs(v - k * 255 / (S * S)) > 72:
                            bad = f"pixel {X},{Y} is {v}, the samples say {k * 255 // (S * S)}"
                    if bad:
                        break
                if bad:
                    fails.append(f"fill case {case} aa {aa}: {bad}")
                elif aa and abs(sum(cov) / 255 - area(pts)) > 0.02 * area(pts) + 1:
                    fails.append(f"fill case {case}: {sum(cov) / 255:.1f} px of ink for an area of {area(pts):.1f}")
        for case in range(10):
            pts = [(rng.uniform(10, 54), rng.uniform(10, 54)) for _ in range(rng.randrange(1, 5))]
            width, closed = rng.choice((1.0, 2.5, 5.0, rng.uniform(1, 9))), rng.randrange(2)
            segs = [(pts[0], pts[0])] if len(pts) == 1 else \
                [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts) if closed else len(pts) - 1)]
            for aa in (1, 0):
                mx, my, mw, mh, cov = ask_mask("stroke", aa, closed, width, pts)
                checks += 1
                hw = width / 2
                for j in range(mh):
                    for i in range(mw):
                        X, Y = mx + i + 0.5, my + j + 0.5
                        dmin = min(segd(X, Y, a, b) for a, b in segs)
                        v = cov[j * mw + i]
                        if not aa:
                            if dmin < hw - 1e-3 and v != 255 or dmin > hw + 1e-3 and v != 0:
                                fails.append(f"stroke case {case}: pixel at distance {dmin:.2f} of {hw} is {v}")
                                break
                        elif (dmin + 0.75 <= hw and v != 255) or (dmin - 0.75 >= hw and v != 0):
                            fails.append(f"stroke case {case}: pixel at distance {dmin:.2f} of {hw} is {v}")
                            break
                    else:
                        continue
                    break
        # THE FIGURES stay in their box and are closed polygons that the fill accepts.
        for f in range(6):
            for case in range(4):
                x0, y0 = rng.uniform(0, 30), rng.uniform(0, 30)
                x1, y1 = x0 + rng.uniform(4, 30), y0 + rng.uniform(4, 30)
                r = d.ask(f"figure {f} {x0} {y0} {x1} {y1}")
                n = int(r[1])
                xy = list(map(float, r[2:]))
                checks += 1
                if n < 3 or any(not (x0 - 0.01 <= xy[k] <= x1 + 0.01) for k in range(0, 2 * n, 2)) or \
                        any(not (y0 - 0.01 <= xy[k] <= y1 + 0.01) for k in range(1, 2 * n, 2)):
                    fails.append(f"figure {f} case {case}: {n} points, not all inside the box")

        brush_checks(rng, d, fails)
        checks += BRUSH_CHECKS[0]
        checks += selection_checks(rng, d, fails)

        # JOINED STEPS: a custom step then a stroke joined to it undo as one
        # and redo as one; the step before them is untouched by either.
        d.send("new 16 16 ffffffff")
        d.send("begin")
        d.send("stamp 3 3 3 1 ff0000ff")
        d.send("commit")
        after_a = d.dump()
        val0 = d.ask("val")[1]
        d.send("custom 41")
        d.send("join")
        d.send("begin")
        d.send("stamp 10 10 3 1 ff00ff00")
        d.send("commit")
        both = d.dump()
        d.ask("undo")
        checks += 3
        if d.dump() != after_a or d.ask("val")[1] != val0:
            fails.append("one undo of two joined steps did not take back both")
        d.ask("redo")
        if d.dump() != both or d.ask("val")[1] != "41":
            fails.append("one redo of two joined steps did not put back both")
        d.ask("undo")
        d.ask("undo")
        if d.dump()[2] != [0xFFFFFFFF] * 256:
            fails.append("the step before a joined pair did not undo on its own")

        # A small byte limit keeps the newest steps and drops the oldest.
        d.send("new 20 20 ffffffff")
        d.send("limit 500")   # a step here is 72 bytes: about six fit
        for k in range(30):
            d.send("begin")
            d.send(f"stamp {k % 20} {k % 20} 3 1 ff00{k:02x}00")
            d.send("commit")
        mid = d.dump()
        st = d.ask("state")
        kept = int(st[3])
        d.ask("undo")
        d.ask("redo")
        checks += 2
        if not (0 < kept < 30):
            fails.append(f"a 500-byte limit kept {kept} of 30 steps")
        if d.dump() != mid:
            fails.append("after the limit dropped steps, undo then redo is not the same picture")
        d.close()

    if args.positive_control:
        if fails:
            print(f"upixel_hostcheck: positive control FAILED as it must "
                  f"({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("upixel_hostcheck: positive control passed -- the harness "
              "cannot see a fill that never goes down", file=sys.stderr)
        return 1
    for f in fails[:20]:
        print("  FAIL  " + f)
    print(f"upixel_hostcheck: {checks - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
