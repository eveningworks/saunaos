#!/usr/bin/env python3
"""Check userland/libs/base/uwallpaper.c -- which of a wallpaper's sizes a
screen gets -- on the HOST.

Two halves, each against answers written out by hand here:

  - uwallpaper_pick() on the stock size set (tools/gen/gen_imgdata.py's
    WALLPAPER_SIZES) for screens from 640x480 to 5120x2880: the closest
    shape first, then the smallest that covers, else the largest; an
    entry with no positive size is never picked
  - uwallpaper_path() on a real folder: names that are not
    `<W>x<H>.<ext>` ignored ("1920x1080 (copy).jpg", "0x0.jpg",
    "02560x1440.jpg", a directory named like a size), no folder or
    nothing in it answers 0

    python3 tools/hostchecks/uwallpaper_hostcheck.py
    python3 tools/hostchecks/uwallpaper_hostcheck.py --positive-control   # must FAIL
"""

RUNNER = {
    "name": "uwallpaper_host",
    "covers": "a wallpaper's size picked for a screen, and its folder read",
    "parallel": True,
}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

DRIVER = r"""
#include "base/uwallpaper.h"
#include <stdio.h>

int main(void) {
    char cmd[8];
    while (scanf("%7s", cmd) == 1) {
        if (cmd[0] == 'k') {            // k SW SH N W H ...: the pick's index
            int sw, sh, n;
            struct uwallpaper_size s[64];
            if (scanf("%d %d %d", &sw, &sh, &n) != 3 || n > 64) return 2;
            for (int i = 0; i < n; i++)
                if (scanf("%d %d", &s[i].w, &s[i].h) != 2) return 2;
            printf("%d\n", uwallpaper_pick(s, n, sw, sh));
        } else if (cmd[0] == 'p') {     // p DIR STEM SW SH: the path, or -
            char dir[512], stem[128], out[640];
            int sw, sh;
            if (scanf("%511s %127s %d %d", dir, stem, &sw, &sh) != 4) return 2;
            printf("%s\n", uwallpaper_path(dir, stem, sw, sh, out, sizeof out) ? out : "-");
        } else return 2;
        fflush(stdout);
    }
    return 0;
}
"""

STOCK = [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160),
         (1920, 1200), (2560, 1600), (1280, 1024), (1024, 768)]

# (screen, the size it must get) -- each worked out by the rule, not run.
PICKS = [
    ((1280, 720), (1280, 720)),     # exact
    ((1920, 1080), (1920, 1080)),
    ((1366, 768), (1920, 1080)),    # 16:9 within 1%; 1280x720 does not cover
    ((1600, 900), (1920, 1080)),
    ((3840, 2160), (3840, 2160)),
    ((5120, 2880), (3840, 2160)),   # nothing covers: the largest of the shape
    ((1440, 900), (1920, 1200)),    # 16:10
    ((1280, 800), (1920, 1200)),
    ((2880, 1800), (2560, 1600)),
    ((1280, 1024), (1280, 1024)),   # 5:4, not 4:3
    ((1024, 768), (1024, 768)),
    ((800, 600), (1024, 768)),
    ((640, 480), (1024, 768)),
    ((1600, 1200), (1024, 768)),    # 4:3, the only one: scaled up
    ((2560, 1080), (2560, 1440)),   # 21:9: 16:9 is nearest
    ((1080, 1920), (1280, 1024)),   # portrait: the tallest shape there is
]

# The folder half: files in a temp folder, then (screen, expected name or None).
FILES = ["1280x720.jpg", "1920x1080.png", "800x600.qoi", "1920x1080 (copy).jpg",
         "0x0.jpg", "2560x1440.bmp", "x9.jpg", "12x.jpg", "readme.txt", "02560x1440.jpg"]
DIRS = ["3840x2160.jpg"]
PATHS = [
    ((1920, 1080), "1920x1080.png"),
    ((1280, 720), "1280x720.jpg"),
    ((3840, 2160), "1920x1080.png"),   # the bmp, 02560x1440 and the directory are not sizes
    ((800, 600), "800x600.qoi"),
]

# The control: the shape no longer counts, so 1366x768 can get 1024x768.
CONTROL = ("> best + 10) continue;", "> best + 100000) continue;")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile a pick that ignores the shape; the run must fail")
    args = ap.parse_args()
    fails, n = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.stage(tmp, "userland/libs/base/uwallpaper.h", dest="base/uwallpaper.h")
        src = hostcheck.stage(tmp, "userland/libs/base/uwallpaper.c", edits=[CONTROL],
                              apply=args.positive_control, tool="uwallpaper_hostcheck")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "uwallpaper_host", [src, drv], includes=[tmp],
                                tool="uwallpaper_hostcheck")
        pics = os.path.join(tmp, "pics")
        os.makedirs(os.path.join(pics, "sea"))
        for f in FILES:
            open(os.path.join(pics, "sea", f), "w").close()
        for d in DIRS:
            os.makedirs(os.path.join(pics, "sea", d))
        os.makedirs(os.path.join(pics, "empty"))
        open(os.path.join(pics, "empty", "photo.jpg"), "w").close()

        flat = " ".join(f"{w} {h}" for w, h in STOCK)
        feed = "".join(f"k {sw} {sh} {len(STOCK)} {flat}\n" for (sw, sh), _ in PICKS)
        feed += "k 1920 1080 0\nk 1920 1080 3 0 0 1920 1080 -4 3\nk 1920 1080 2 0 0 7 0\n"
        feed += "".join(f"p {pics} sea {sw} {sh}\n" for (sw, sh), _ in PATHS)
        feed += f"p {pics} empty 1920 1080\np {pics} nofolder 1920 1080\np {pics} sea/.. 1920 1080\n"
        out = subprocess.run([exe], input=feed, capture_output=True, text=True,
                             check=True).stdout.split("\n")

    for ((sw, sh), want), line in zip(PICKS, out):
        n += 1
        got = STOCK[int(line)] if line.strip() not in ("", "-1") else None
        if got != want:
            fails.append(f"pick for {sw}x{sh}: {got}, want {want}")
    rest = out[len(PICKS):]
    for what, want, line in (("no sizes", "-1", rest[0]), ("a zero and a negative entry", "1", rest[1]),
                             ("only entries with no size", "-1", rest[2])):
        n += 1
        if line != want:
            fails.append(f"pick from {what}: {line}, want {want}")
    rest = rest[2:]
    for ((sw, sh), want), line in zip(PATHS, rest[1:]):
        n += 1
        if line != os.path.join(pics, "sea", want):
            fails.append(f"path for {sw}x{sh}: {line}, want sea/{want}")
    for what, line in zip(("a folder with no sizes", "no folder", "a stem with a slash"),
                          rest[1 + len(PATHS):]):
        n += 1
        if line != "-":
            fails.append(f"{what}: {line}, want none")

    if args.positive_control:
        if fails:
            print(f"uwallpaper_hostcheck: positive control FAILED as it must "
                  f"({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("uwallpaper_hostcheck: positive control passed -- the check cannot see the shape ignored",
              file=sys.stderr)
        return 1
    for f in fails:
        print("  FAIL  " + f)
    print(f"uwallpaper_hostcheck: {n - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
