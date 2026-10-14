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

static struct uimg im;
static struct upix_rect dirty;
static struct upixundo u;

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
            printf("undo %d\n", upixundo_undo(&u, &im, 0));
        } else if (!strcmp(cmd, "redo")) {
            printf("redo %d\n", upixundo_redo(&u, &im, 0));
        } else if (!strcmp(cmd, "state")) {
            printf("state %u %d %d\n", upixundo_state(&u), upixundo_can_undo(&u), u.count);
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
    src = hostcheck.stage(tmp, "userland/lib/upixel.c", apply=poison, tool="upixel_hostcheck",
                          edits=[("for (int dyi = -1; dyi <= 1; dyi += 2) {",
                                  "for (int dyi = -1; dyi <= -1; dyi += 2) {")])
    und = hostcheck.stage(tmp, "userland/lib/upixundo.c")
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "upixel_host", [src, und, drv], includes=[tmp],
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
