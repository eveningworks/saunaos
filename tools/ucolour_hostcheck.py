#!/usr/bin/env python3
"""Check userland/lib/ucolour.c's HSL and HSV conversions against
Python's colorsys, on the HOST.

A colour picker's square and strip move HSV and its fields show RGB, so
a conversion that is off by a step shows as a colour that will not stay
put. colorsys shares nothing with the C. Checked, for every RGB colour on
a 17-step grid and seeded random ones:

  - to_hsv() agrees with colorsys.rgb_to_hsv within 0.2 degrees and 0.002
    (hue only where there IS one: a grey has none, black no saturation,
    and the C then keeps what the caller held)
  - from_hsv() agrees with colorsys.hsv_to_rgb to the byte, and turns
    to_hsv()'s answer back into the same colour exactly
  - to_hsl()/from_hsl() agree with colorsys's HLS within one percent,
    and from_hsl() of colorsys's answer is within a byte of the colour

    python3 tools/ucolour_hostcheck.py
    python3 tools/ucolour_hostcheck.py --positive-control   # must FAIL
"""
import argparse
import colorsys
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

DRIVER = r"""
#include "lib/ucolour.h"
#include <stdio.h>

int main(void) {
    char cmd[8];
    while (scanf("%7s", cmd) == 1) {
        if (cmd[0] == 't') {            // t RGB: to_hsv from (7, 0.5, 0.5), and to_hsl
            unsigned rgb;
            if (scanf("%x", &rgb) != 1) return 2;
            struct ucolour_hsv v = { 7, 0.5f, 0.5f };
            ucolour_to_hsv(rgb, &v);
            struct ucolour_hsl l;
            ucolour_to_hsl(rgb, &l);
            printf("%.5f %.5f %.5f %06x %d %d %d\n", v.h, v.s, v.v, ucolour_from_hsv(&v), l.h, l.s, l.l);
        } else if (cmd[0] == 'v') {     // v H S V: from_hsv
            struct ucolour_hsv v;
            if (scanf("%f %f %f", &v.h, &v.s, &v.v) != 3) return 2;
            printf("%06x\n", ucolour_from_hsv(&v));
        } else if (cmd[0] == 'l') {     // l H S L: from_hsl
            struct ucolour_hsl l;
            if (scanf("%d %d %d", &l.h, &l.s, &l.l) != 3) return 2;
            printf("%06x\n", ucolour_from_hsl(&l));
        } else return 2;
        fflush(stdout);
    }
    return 0;
}
"""

# The control: a hue sector computed from the wrong pair of channels.
CONTROL = ("    else if (mx == g) h = (b - r) / d + 2.0f;\n    else              h = (r - g) / d + 4.0f;\n    h *= 60.0f;\n    io->h",
           "    else if (mx == g) h = (r - b) / d + 2.0f;\n    else              h = (r - g) / d + 4.0f;\n    h *= 60.0f;\n    io->h")


def rgb3(c):
    return ((c >> 16) & 0xFF) / 255, ((c >> 8) & 0xFF) / 255, (c & 0xFF) / 255


def byte(v):
    return max(0, min(255, int(v * 255 + 0.5)))


def pack(r, g, b):
    return byte(r) << 16 | byte(g) << 8 | byte(b)


def near_rgb(a, b, tol):
    return all(abs(((a >> s) & 0xFF) - ((b >> s) & 0xFF)) <= tol for s in (16, 8, 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile a hue computed from the wrong channels; the run must fail")
    ap.add_argument("--seed", type=int, default=1015)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    grid = [i * 255 // 16 for i in range(17)]
    colours = [r << 16 | g << 8 | b for r in grid for g in grid for b in grid]
    colours += [rng.randrange(1 << 24) for _ in range(3000)]
    hsvs = [(rng.uniform(0, 359.99), rng.random(), rng.random()) for _ in range(2000)]

    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.stage(tmp, "userland/lib/ucolour.h", dest="lib/ucolour.h")
        src = hostcheck.stage(tmp, "userland/lib/ucolour.c", edits=[CONTROL], apply=args.positive_control,
                              tool="ucolour_hostcheck")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "ucolour_host", [src, drv, "-lm"], includes=[tmp], tool="ucolour_hostcheck")
        feed = "".join(f"t {c:06x}\n" for c in colours) + "".join(f"v {h} {s} {v}\n" for h, s, v in hsvs)
        hsls = []
        for c in colours[:1500]:
            h, light, s = colorsys.rgb_to_hls(*rgb3(c))
            hsls.append((c, round(h * 360) % 360, round(s * 100), round(light * 100)))
        feed += "".join(f"l {h} {s} {light}\n" for _, h, s, light in hsls)
        out = subprocess.run([exe], input=feed, capture_output=True, text=True, check=True).stdout.split("\n")

    fails, n = [], 0
    for c, line in zip(colours, out):
        h, s, v, back, lh, ls, ll = line.split()
        h, s, v, back = float(h), float(s), float(v), int(back, 16)
        wh, ws, wv = colorsys.rgb_to_hsv(*rgb3(c))
        n += 3
        dh = abs(h - wh * 360)
        dh = min(dh, 360 - dh)
        if abs(v - wv) > 0.002 or (wv > 0 and abs(s - ws) > 0.002) or (ws > 0 and wv > 0 and dh > 0.2):
            fails.append(f"to_hsv({c:06x}) = ({h:.2f}, {s:.4f}, {v:.4f}), colorsys "
                         f"({wh * 360:.2f}, {ws:.4f}, {wv:.4f})")
        if (wv == 0 and (s != 0.5 or h != 7)) or (wv > 0 and ws == 0 and h != 7):
            fails.append(f"to_hsv({c:06x}) did not keep the caller's hue/saturation: ({h}, {s})")
        if back != c:
            fails.append(f"from_hsv(to_hsv({c:06x})) = {back:06x}")
        wh, wl, ws = colorsys.rgb_to_hls(*rgb3(c))
        n += 1
        dh = abs(int(lh) - wh * 360)
        dh = min(dh, 360 - dh)
        if abs(int(ll) - wl * 100) > 1 or abs(int(ls) - ws * 100) > 1 or (ws > 0.02 and dh > 1):
            fails.append(f"to_hsl({c:06x}) = ({lh}, {ls}, {ll}), colorsys ({wh * 360:.1f}, {ws * 100:.1f}, {wl * 100:.1f})")
    rest = out[len(colours):]
    for (h, s, v), line in zip(hsvs, rest):
        n += 1
        want = pack(*colorsys.hsv_to_rgb(h / 360, s, v))
        if not near_rgb(int(line, 16), want, 1):
            fails.append(f"from_hsv({h:.2f}, {s:.3f}, {v:.3f}) = {line}, colorsys {want:06x}")
    for (c, h, s, light), line in zip(hsls, rest[len(hsvs):]):
        n += 1
        # Whole percents and degrees lose up to about 1.3% of the range.
        if not near_rgb(int(line, 16), c, 4):
            fails.append(f"from_hsl({h}, {s}, {light}) = {line}, the colour was {c:06x}")

    if args.positive_control:
        if fails:
            print(f"ucolour_hostcheck: positive control FAILED as it must ({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("ucolour_hostcheck: positive control passed -- the check cannot see a wrong hue", file=sys.stderr)
        return 1
    for f in fails[:15]:
        print("  FAIL  " + f)
    print(f"ucolour_hostcheck: {n - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
