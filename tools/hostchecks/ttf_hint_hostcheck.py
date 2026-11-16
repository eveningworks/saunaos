#!/usr/bin/env python3
"""The light autohinter against FreeType's, and the cell it sizes, on the HOST.

`userland/libs/media/ttf_hint.c` fits a glyph to the pixel grid vertically
only, built to match FreeType's latin autohinter in LIGHT mode
(docs/decisions/gui.md, "Small text: our own light autohinter, blended
in sRGB, grayscale only"). This
compiles the tree's ttf.c and ttf_hint.c with the host gcc and checks:

**Where letters start and stop, against FreeType.** For the shipped faces
at 8-24 px and twenty letters that sit on every blue zone (x-height, cap
height, ascender, baseline, descender; flat and round), the topmost and
bottommost rows with real ink (coverage >= 48) must be FreeType light's in
at least 90% of cases at the top and 95% at the bottom. Unhinted, the
outline agrees about three times in four. The two do not share a line of
code; the misses are a zone whose fitted row rounds the other way when it
sits near half a pixel (FreeType measures its zones from outline points,
this from bounding boxes).

**That `ttf_glyph_extent()` bounds the ink.** Every printable Latin-1
character, 8-24 px, every shipped face: rendered with its baseline in a
large cell, no coverage may fall outside the rows the extent reported --
fontd's atlas and ugfx's private fonts size their cells from it, and a
glyph past it loses rows (an accent's top, hinted above the ascent).

`--positive-control` runs three broken copies and requires each to go RED:
hinting switched off (the FreeType check), the x-height left unrounded
(the FreeType check), and the extent measured on the UNHINTED outline (the
extent check).

Needs gcc; the FreeType comparison needs libfreetype's headers
(`pkg-config freetype2`) and says SKIPPED without them.
"""

# The light autohinter's rows against FreeType light's, and
# ttf_glyph_extent() against the ink, over the shipped faces at 8-24 px.
# Host gcc + libfreetype. Carries a --positive-control that must go red.
RUNNER = {"covers": "the autohinter's rows vs FreeType light; extent bounds the ink", "gate": True,
          "parallel": True}

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = toolpath.REPO
TOOL = "ttf_hint_hostcheck"
FACES = ["liberation-sans", "liberation-serif", "liberation-sans-bold", "dejavu-sans-mono"]
LETTERS = "xzoeacsHEOTpqgdbhkln"
SIZES = (8, 24)
TOP_MIN, BOTTOM_MIN = 0.90, 0.95

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"
#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#define W 160
#define H 160
#define BASE 100
static uint8_t cov[W * H];

static uint8_t *slurp(const char *path, long *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    *len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*len);
    if (!d || fread(d, 1, (size_t)*len, fp) != (size_t)*len) { fclose(fp); return 0; }
    fclose(fp);
    return d;
}

// The rows ink reaches, as heights above the baseline: `top` the topmost
// row's top edge, `bot` the bottommost row's bottom edge. 999 for none.
static void rows(int *top, int *bot) {
    int first = -1, last = -1;
    for (int y = 0; y < H; y++) {
        int m = 0;
        for (int x = 0; x < W; x++) if (cov[y * W + x] > m) m = cov[y * W + x];
        if (m >= 48) { if (first < 0) first = y; last = y; }
    }
    *top = first < 0 ? 999 : BASE - first;
    *bot = first < 0 ? 999 : BASE - last - 1;
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    long len;
    uint8_t *data = slurp(argv[2], &len);
    struct ttf_font t;
    if (!data || !ttf_open(&t, data, (uint32_t)len)) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    struct ttf_scratch *sc = malloc(sizeof *sc);
    int from = atoi(argv[3]), to = atoi(argv[4]);

    if (!strcmp(argv[1], "extent")) {
        // Every printable Latin-1 character: ink only inside its extent.
        int bad = 0, glyphs = 0;
        for (int px = from; px <= to; px++)
            for (int cp = 0x21; cp <= 0xFF; cp++) {
                if (cp >= 0x7F && cp <= 0xA0) continue;
                int gid = ttf_glyph_index(&t, (uint32_t)cp), up, down;
                if (gid <= 0 || !ttf_glyph_extent(&t, gid, px, &up, &down, sc)) continue;
                memset(cov, 0, sizeof cov);
                ttf_render_glyph(&t, gid, px, cov, W, H, 8, BASE, sc);
                glyphs++;
                for (int y = 0; y < H; y++) {
                    if (y >= BASE - up && y < BASE + down) continue;
                    int ink = 0;
                    for (int x = 0; x < W; x++) if (cov[y * W + x]) ink = 1;
                    if (ink) {
                        if (bad < 5) printf("OUT %d U+%04X row %d outside [%d, %d)\n", px, cp, BASE - y, -down, up);
                        bad++;
                        break;
                    }
                }
            }
        printf("EXTENT %d %d\n", glyphs, bad);
        return 0;
    }

#ifdef HAVE_FREETYPE
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[2], 0, &face)) return 1;
    for (int px = from; px <= to; px++) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
        for (const char *c = argv[5]; *c; c++) {
            int ours_t, ours_b, ft_t, ft_b;
            memset(cov, 0, sizeof cov);
            ttf_render_glyph(&t, ttf_glyph_index(&t, (uint8_t)*c), px, cov, W, H, 8, BASE, sc);
            rows(&ours_t, &ours_b);
            memset(cov, 0, sizeof cov);
            if (FT_Load_Char(face, (uint8_t)*c, FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_RENDER))
                continue;
            FT_Bitmap *b = &face->glyph->bitmap;
            for (unsigned y = 0; y < b->rows; y++)
                for (unsigned x = 0; x < b->width; x++) {
                    int X = 8 + face->glyph->bitmap_left + (int)x, Y = BASE - face->glyph->bitmap_top + (int)y;
                    if (X >= 0 && X < W && Y >= 0 && Y < H) cov[Y * W + X] = b->buffer[(int)y * b->pitch + (int)x];
                }
            rows(&ft_t, &ft_b);
            printf("ROWS %d %c %d %d %d %d\n", px, *c, ours_t, ours_b, ft_t, ft_b);
        }
    }
#endif
    return 0;
}
"""

CONTROLS = [
    ("hinting off",
     "userland/libs/media/ttf.c",
     "    if (sc->npoints == 0) return 1; // a space: nothing to fill\n    if (!f->no_hint) ttf_hint_outline(",
     "    if (sc->npoints == 0) return 1; // a space: nothing to fill\n    if (0) ttf_hint_outline(",
     "rows"),
    ("x-height not rounded",
     "userland/libs/media/ttf_hint.c",
     "    fx_t k = fx_div(xh_fit, xh);",
     "    fx_t k = FX_ONE; (void)xh_fit;",
     "rows"),
    ("extent of the unhinted outline",
     "userland/libs/media/ttf.c",
     "    if (!f->no_hint) ttf_hint_outline(f, scale, 0, sc);",
     "",
     "extent"),
]


def have_freetype():
    return bool(shutil.which("pkg-config")) and subprocess.run(
        ["pkg-config", "--exists", "freetype2"]).returncode == 0


def build(tmp, control=None):
    """The driver over a copy of ttf.c and ttf_hint.c, one control's edit applied."""
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    for h in ("kernel/include/api/ttf.h", "kernel/include/api/fixed.h", "userland/libs/media/ttf_internal.h"):
        hostcheck.stage(inc, h, tool=TOOL)
    srcs = []
    for rel in ("userland/libs/media/ttf.c", "userland/libs/media/ttf_hint.c"):
        edits = [(control[2], control[3])] if control and control[1] == rel else []
        srcs.append(hostcheck.stage(tmp, rel, edits=edits, apply=bool(edits), tool=TOOL))
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    flags = list(hostcheck.STRICT)
    libs = []
    if have_freetype():
        flags += ["-DHAVE_FREETYPE"] + subprocess.run(
            ["pkg-config", "--cflags", "freetype2"], capture_output=True, text=True).stdout.split()
        libs = subprocess.run(["pkg-config", "--libs", "freetype2"], capture_output=True, text=True).stdout.split()
    return hostcheck.compile(tmp, "ttf_hint_check", [drv, *srcs, *libs], flags=flags,
                             includes=[inc], tool=TOOL)


def run(exe, mode, face):
    font = os.path.join(ROOT, "data", "fonts", face + ".ttf")
    r = subprocess.run([exe, mode, font, str(SIZES[0]), str(SIZES[1]), LETTERS],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{TOOL}: driver failed on {face}: {r.stderr}")
    return r.stdout.splitlines()


def check_rows(exe, quiet=False):
    """1 per face whose agreement with FreeType is under the bar."""
    bad = 0
    for face in FACES:
        rows = [line.split() for line in run(exe, "rows", face) if line.startswith("ROWS")]
        if not rows:
            continue
        n = len(rows)
        top = sum(r[3] == r[5] for r in rows) / n
        bot = sum(r[4] == r[6] for r in rows) / n
        ok = top >= TOP_MIN and bot >= BOTTOM_MIN
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {face}: top row as FreeType's {top:.0%}, "
                  f"bottom {bot:.0%} ({n} glyph sizes; bar {TOP_MIN:.0%}/{BOTTOM_MIN:.0%})")
    return bad


def check_extent(exe, quiet=False):
    bad = 0
    for face in FACES:
        out = run(exe, "extent", face)
        glyphs, outside = map(int, out[-1].split()[1:])
        bad += outside > 0
        if not quiet:
            print(f"  {'ok  ' if not outside else 'FAIL'} {face}: {outside} of {glyphs} glyph sizes "
                  "ink outside ttf_glyph_extent()")
            for line in out[:-1]:
                print("       " + line)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the hinter; each check must go RED")
    args = ap.parse_args()
    ft = have_freetype()

    if args.positive_control:
        red = []
        for control in CONTROLS:
            name, mode = control[0], control[4]
            if mode == "rows" and not ft:
                print(f"  skip  {name}: no libfreetype")
                continue
            with tempfile.TemporaryDirectory() as tmp:
                exe = build(tmp, control)
                bad = check_rows(exe, quiet=True) if mode == "rows" else check_extent(exe, quiet=True)
            print(f"  {'RED  ' if bad else 'green'} {name}: {bad} face(s) failing")
            red.append(bad > 0)
        ok = all(red) and red
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        if ft:
            print("rows against FreeType light:")
            bad = check_rows(exe)
        else:
            print(f"{TOOL}: SKIPPED the FreeType comparison -- no libfreetype headers")
            bad = 0
        print("ttf_glyph_extent() bounds the ink:")
        bad += check_extent(exe)
    print(f"{TOOL}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
