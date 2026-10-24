#!/usr/bin/env python3
"""Check userland/lib/urgn.c -- the growable region w16's GDI regions and
DC clips are -- with the host gcc under ASan and UBSan, against a BITMAP:
every operation is replayed on sets of pixels in Python, and each region
must cover exactly those pixels.

What it asserts, over seeded random sequences of rect sets, unions,
intersections, subtractions, xors and offsets on four registers (an
operand may also be the destination):

  - the rects are DISJOINT and non-empty, and EXACTLY the bitmap -- no
    cap, no superset (urgn.h: the opposite of lib/uregion's bound)
  - the bbox, the area, containment, overlap and equality agree with it
  - a shape added row by row (a polygon's spans) COALESCES: forty rows of
    one span are one rect, a staircase a rect per step

    python3 tools/urgn_hostcheck.py
    python3 tools/urgn_hostcheck.py --positive-control [cut|coalesce]   # must FAIL

Needs only gcc and the standard library.
"""

import argparse
import random
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

# stdin, one op per line, on registers 0-3:
#   R d x y w h  set     A d x y w h  add a rect     K d x y w h  cut a rect
#   U|I|S|X d a b  union / intersect / subtract / xor into d
#   O d dx dy  offset    P d  print "n area bx by bw bh" then the rects
#   Q a b x y w h  print "equal contains(x,y) overlaps(x,y,w,h)"
DRIVER = r"""
#include "urgn.h"
#include <stdio.h>

int main(void) {
    struct urgn g[4];
    for (int i = 0; i < 4; i++) urgn_init(&g[i]);
    char op;
    while (scanf(" %c", &op) == 1) {
        int d, a, b, x, y, w, h;
        if (op == 'P') {
            if (scanf("%d", &d) != 1) return 2;
            struct urect bb;
            urgn_bbox(&g[d], &bb);
            printf("%d %lld %d %d %d %d\n", g[d].n, urgn_area(&g[d]), bb.x, bb.y, bb.w, bb.h);
            for (int i = 0; i < g[d].n; i++) printf("%d %d %d %d\n", g[d].r[i].x, g[d].r[i].y, g[d].r[i].w, g[d].r[i].h);
        } else if (op == 'Q') {
            if (scanf("%d %d %d %d %d %d", &a, &b, &x, &y, &w, &h) != 6) return 2;
            printf("%d %d %d\n", urgn_equal(&g[a], &g[b]), urgn_contains(&g[a], x, y), urgn_overlaps(&g[a], x, y, w, h));
        } else if (op == 'O') {
            if (scanf("%d %d %d", &d, &x, &y) != 3) return 2;
            urgn_offset(&g[d], x, y);
        } else if (op == 'R' || op == 'A' || op == 'K') {
            if (scanf("%d %d %d %d %d", &d, &x, &y, &w, &h) != 5) return 2;
            int r = op == 'R' ? urgn_set_rect(&g[d], x, y, w, h) : op == 'A' ? urgn_add_rect(&g[d], x, y, w, h)
                                                                             : urgn_cut_rect(&g[d], x, y, w, h);
            if (r < 0) return 3;
        } else {
            if (scanf("%d %d %d", &d, &a, &b) != 3) return 2;
            int r = op == 'U' ? urgn_union(&g[d], &g[a], &g[b]) : op == 'I' ? urgn_intersect(&g[d], &g[a], &g[b])
                  : op == 'S' ? urgn_subtract(&g[d], &g[a], &g[b]) : urgn_xor(&g[d], &g[a], &g[b]);
            if (r < 0) return 3;
        }
        fflush(stdout);
    }
    for (int i = 0; i < 4; i++) urgn_free(&g[i]);
    return 0;
}
"""

CONTROLS = {
    # A cut loses the strip below the hole.
    "cut": [("push(out, r.x, y1, r.w, r.y + r.h - y1) < 0 ||", "0 ||")],
    # Coalescing joins rects that only share part of an edge.
    "coalesce": [("if (a->x == b->x && a->w == b->w && (a->y + a->h == b->y || b->y + b->h == a->y)) {",
                  "if (a->x == b->x && (a->y + a->h == b->y || b->y + b->h == a->y)) {")],
}

ASAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all")


def build(tmp, poison=None):
    hostcheck.stage(tmp, "userland/lib/uregion.h")
    hostcheck.stage(tmp, "userland/lib/urgn.h")
    src = hostcheck.stage(tmp, "userland/lib/urgn.c", apply=bool(poison), tool="urgn_hostcheck",
                          edits=CONTROLS.get(poison, []))
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "urgn_host", [src, drv], flags=ASAN, includes=[tmp], tool="urgn_hostcheck")


def pixels(x, y, w, h):
    return {(i, j) for i in range(x, x + w) for j in range(y, y + h)} if w > 0 and h > 0 else set()


class Model:
    """The same registers as sets of pixels."""
    def __init__(self):
        self.g = [set() for _ in range(4)]

    def apply(self, op):
        k = op[0]
        if k == "R":
            self.g[op[1]] = pixels(*op[2:])
        elif k == "A":
            self.g[op[1]] = self.g[op[1]] | pixels(*op[2:])
        elif k == "K":
            self.g[op[1]] = self.g[op[1]] - pixels(*op[2:])
        elif k == "O":
            self.g[op[1]] = {(x + op[2], y + op[3]) for x, y in self.g[op[1]]}
        elif k in "UISX":
            a, b = self.g[op[2]], self.g[op[3]]
            self.g[op[1]] = {"U": a | b, "I": a & b, "S": a - b, "X": a ^ b}[k]


def run(exe, ops, queries):
    """ops: op tuples; queries: ("P", d) or ("Q", a, b, x, y, w, h). Returns the reply lines."""
    text = "".join(" ".join(map(str, o)) + "\n" for o in ops)
    text += "".join(" ".join(map(str, q)) + "\n" for q in queries)
    r = subprocess.run([exe], input=text, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"driver exit {r.returncode}: {r.stderr.strip()[:300]}")
    return r.stdout.split("\n")


def check_region(name, want, lines, fails):
    """lines[0] the header; the rects follow. Returns how many lines were read."""
    n, area, bx, by, bw, bh = map(int, lines[0].split())
    got = set()
    for i in range(n):
        x, y, w, h = map(int, lines[1 + i].split())
        if w <= 0 or h <= 0:
            fails.append(f"{name}: an empty rect {x, y, w, h} was stored")
            return 1 + n
        p = pixels(x, y, w, h)
        if got & p:
            fails.append(f"{name}: rects overlap at {sorted(got & p)[0]}")
            return 1 + n
        got |= p
    if got != want:
        miss, extra = want - got, got - want
        fails.append(f"{name}: not exact -- {len(miss)} missing (e.g. {sorted(miss)[:1]}), "
                     f"{len(extra)} extra (e.g. {sorted(extra)[:1]})")
    if area != len(got):
        fails.append(f"{name}: area {area} != {len(got)} covered pixels")
    if got:
        xs, ys = [x for x, _ in got], [y for _, y in got]
        b = (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
        if (bx, by, bw, bh) != b:
            fails.append(f"{name}: bbox {(bx, by, bw, bh)} != {b}")
    elif (bx, by, bw, bh) != (0, 0, 0, 0):
        fails.append(f"{name}: an empty region's bbox is {(bx, by, bw, bh)}")
    return 1 + n


def random_case(rng):
    ops = []
    for d in range(4):
        ops.append(("R", d, rng.randint(-5, 20), rng.randint(-5, 20), rng.randint(0, 30), rng.randint(0, 25)))
    for _ in range(rng.randint(1, 16)):
        k = rng.choice("RAKUISXO")
        d = rng.randrange(4)
        if k in "RAK":
            ops.append((k, d, rng.randint(-10, 40), rng.randint(-10, 35), rng.randint(-2, 25), rng.randint(-2, 20)))
        elif k == "O":
            ops.append(("O", d, rng.randint(-6, 6), rng.randint(-6, 6)))
        else:
            ops.append((k, d, rng.randrange(4), rng.randrange(4)))   # an operand may be d itself
    return ops


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", nargs="?", const="cut", choices=sorted(CONTROLS))
    args = ap.parse_args()
    fails, cases = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)
        rng = random.Random(7)
        for c in range(400):
            ops = random_case(rng)
            m = Model()
            for o in ops:
                m.apply(o)
            qs = [("P", d) for d in range(4)]
            probes = [(rng.randrange(4), rng.randrange(4), rng.randint(-10, 40), rng.randint(-10, 35),
                       rng.randint(1, 12), rng.randint(1, 12)) for _ in range(4)]
            qs += [("Q",) + p for p in probes]
            try:
                lines = run(exe, ops, qs)
            except RuntimeError as e:
                fails.append(f"random #{c}: {e}")
                continue
            at = 0
            for d in range(4):
                at += check_region(f"random #{c} r{d}", m.g[d], lines[at:], fails)
            for a, b, x, y, w, h in probes:
                eq, has, over = map(int, lines[at].split())
                at += 1
                want = (int(m.g[a] == m.g[b]), int((x, y) in m.g[a]), int(bool(m.g[a] & pixels(x, y, w, h))))
                if (eq, has, over) != want:
                    fails.append(f"random #{c}: equal/contains/overlaps {(eq, has, over)} != {want} "
                                 f"for r{a}, r{b} at {(x, y, w, h)}")
            cases += 1
        # Coalescing: forty one-row spans of one width are ONE rect; a staircase one per step.
        rows = [("R", 0, 0, 0, 0, 0)] + [("A", 0, 5, y, 20, 1) for y in range(40)]
        lines = run(exe, rows, [("P", 0)])
        check_region("forty rows", pixels(5, 0, 20, 40), lines, fails)
        if int(lines[0].split()[0]) != 1:
            fails.append(f"forty rows of one span are {lines[0].split()[0]} rects, not 1")
        stairs = [("R", 0, 0, 0, 0, 0)] + [("A", 0, s * 4, y, 10, 1) for s in range(5) for y in range(s * 6, s * 6 + 6)]
        lines = run(exe, stairs, [("P", 0)])
        want = set()
        for s in range(5):
            want |= pixels(s * 4, s * 6, 10, 6)
        check_region("staircase", want, lines, fails)
        if int(lines[0].split()[0]) > 5:
            fails.append(f"a five-step staircase is {lines[0].split()[0]} rects, more than 5")
        cases += 2

    if args.positive_control:
        if fails:
            print(f"urgn_hostcheck: positive control FAILED as it must ({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("urgn_hostcheck: positive control passed -- the harness cannot see the break", file=sys.stderr)
        return 1
    for f in fails[:20]:
        print("  FAIL  " + f)
    print(f"urgn_hostcheck: {cases} cases, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
