#!/usr/bin/env python3
"""Colour emoji (COLR v0 over CPAL) against FreeType's, and the emoji table, on the HOST.

`ttf_render_color_glyph()` (userland/libs/media/ttf.c) stacks a colour
glyph's layers -- ordinary outlines, each filled with one CPAL colour --
premultiplied. This compiles the tree's ttf.c with the host gcc and draws
the shipped emoji face's glyphs both ways: ours, and FreeType's own COLR v0
renderer (FT_LOAD_COLOR, unhinted, as ours draws a face with no Latin
letters). The two share no code. Over thirty emoji at 16, 24 and 40 px, the
mean difference per channel must stay under 4 (of 255) and fewer than 2% of
pixels may differ by more than 48 -- what is left is the two rasterizers'
edge coverage, not colour or order.

It also checks `base/uemoji.h` at the cases that decide how text looks:
U+2B50 and U+1F600 are emoji, U+2605 and U+2764 text, U+FE0F makes U+2764
an emoji and U+FE0E makes U+2B50 text.

`--positive-control` reads CPAL's colours in the wrong byte order (RGBA for
BGRA) and, separately, stacks the layers in reverse; each must go RED.

Needs gcc and libfreetype's headers (`pkg-config freetype2`); says SKIPPED
without them.
"""

# Colour emoji: ttf.c's COLR v0 compositing against FreeType's own, and
# base/uemoji.h's presentation cases. Host gcc + libfreetype. Carries a
# --positive-control that must go red.
RUNNER = {"name": "emoji_host", "covers": "COLR v0 emoji vs FreeType's colour render; Emoji_Presentation cases", "gate": True,
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
TOOL = "emoji_hostcheck"
FONT = os.path.join(ROOT, "data", "fonts", "fallback", "twemoji.ttf")
EMOJI = [0x1F600, 0x1F602, 0x1F60D, 0x1F622, 0x1F44D, 0x1F44B, 0x1F389, 0x1F525, 0x1F4C1, 0x1F680,
         0x1F431, 0x1F436, 0x1F355, 0x1F34E, 0x1F308, 0x1F319, 0x2B50, 0x2705, 0x274C, 0x26A0,
         0x2764, 0x1F496, 0x1F3B5, 0x1F4F7, 0x1F512, 0x1F697, 0x1F30D, 0x1F9E9, 0x1F6A9, 0x1F914]
SIZES = (16, 24, 40)
MEAN_MAX, OFF_MAX = 4.0, 0.02

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"
#include "base/uemoji.h"
#include <ft2build.h>
#include FT_FREETYPE_H

#define W 96
#define H 96
#define BASE 72
static uint8_t ours[W * H * 4], theirs[W * H * 4], cov[W * H];

int main(int argc, char **argv) {
    // The presentation cases first: one line, 1 per expectation met.
    printf("UEMOJI %d %d %d %d %d %d\n", uemoji_presentation(0x2B50), uemoji_presentation(0x1F600),
           !uemoji_presentation(0x2605), !uemoji_presentation(0x2764),
           uemoji_wanted(0x2764, UEMOJI_VS_EMOJI), !uemoji_wanted(0x2B50, UEMOJI_VS_TEXT));
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, fp) != (size_t)len) return 1;
    fclose(fp);
    struct ttf_font t;
    if (!ttf_open(&t, data, (uint32_t)len)) return 1;
    struct ttf_scratch *sc = malloc(sizeof *sc);
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], 0, &face)) return 1;
    for (int a = 3; a < argc; a++) {
        unsigned cp = (unsigned)strtoul(argv[a], 0, 16);
        int px = atoi(argv[2]);
        int gid = ttf_glyph_index(&t, cp);
        memset(ours, 0, sizeof ours);
        memset(theirs, 0, sizeof theirs);
        int r = ttf_render_color_glyph(&t, gid, px, ours, cov, W, H, 16, BASE, 0, sc);
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
        if (FT_Load_Glyph(face, FT_Get_Char_Index(face, cp), FT_LOAD_COLOR | FT_LOAD_NO_HINTING) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) { printf("SKIP %X\n", cp); continue; }
        FT_Bitmap *b = &face->glyph->bitmap;
        if (b->pixel_mode != FT_PIXEL_MODE_BGRA) { printf("SKIP %X\n", cp); continue; }
        for (unsigned y = 0; y < b->rows; y++)
            for (unsigned x = 0; x < b->width; x++) {
                int X = 16 + face->glyph->bitmap_left + (int)x, Y = BASE - face->glyph->bitmap_top + (int)y;
                if (X < 0 || X >= W || Y < 0 || Y >= H) continue;
                const uint8_t *q = b->buffer + (int)y * b->pitch + (int)x * 4;  // B, G, R, A premultiplied
                uint8_t *p = theirs + (Y * W + X) * 4;
                p[0] = q[2]; p[1] = q[1]; p[2] = q[0]; p[3] = q[3];
            }
        long sum = 0, off = 0, n = 0;
        for (int k = 0; k < W * H; k++) {
            if (!ours[k * 4 + 3] && !theirs[k * 4 + 3]) continue;
            int worst = 0;
            for (int c = 0; c < 4; c++) {
                int d = abs((int)ours[k * 4 + c] - (int)theirs[k * 4 + c]);
                sum += d;
                if (d > worst) worst = d;
            }
            if (worst > 48) off++;
            n++;
        }
        printf("GLYPH %X %d %ld %ld %ld\n", cp, r, sum, off, n);
    }
    return 0;
}
"""

CONTROLS = [
    ("CPAL read as RGBA",
     "    *argb = rd_u8(f, c + 3) << 24 | rd_u8(f, c + 2) << 16 | rd_u8(f, c + 1) << 8 | rd_u8(f, c);",
     "    *argb = rd_u8(f, c + 3) << 24 | rd_u8(f, c) << 16 | rd_u8(f, c + 1) << 8 | rd_u8(f, c + 2);"),
    ("layers stacked in reverse",
     "        if (!colr_layer(f, first + i, fg, &lg, &c)) return -1;",
     "        if (!colr_layer(f, first + (n - 1 - i), fg, &lg, &c)) return -1;"),
]


def have_freetype():
    return bool(shutil.which("pkg-config")) and subprocess.run(
        ["pkg-config", "--exists", "freetype2"]).returncode == 0


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    os.makedirs(os.path.join(inc, "base"), exist_ok=True)
    for h in ("kernel/include/api/ttf.h", "kernel/include/api/fixed.h", "userland/libs/media/ttf_internal.h"):
        hostcheck.stage(inc, h, tool=TOOL)
    for h in ("userland/libs/base/uemoji.h", "userland/libs/base/uemoji_table.h"):
        hostcheck.stage(inc, h, dest=os.path.join("base", os.path.basename(h)), tool=TOOL)
    edits = [(control[1], control[2])] if control else []
    ttf = hostcheck.stage(tmp, "userland/libs/media/ttf.c", edits=edits, apply=bool(edits), tool=TOOL)
    hint = hostcheck.stage(tmp, "userland/libs/media/ttf_hint.c", tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    cflags = subprocess.run(["pkg-config", "--cflags", "freetype2"], capture_output=True, text=True).stdout.split()
    libs = subprocess.run(["pkg-config", "--libs", "freetype2"], capture_output=True, text=True).stdout.split()
    return hostcheck.compile(tmp, "emoji_check", [drv, ttf, hint, *libs],
                             flags=list(hostcheck.STRICT) + cflags, includes=[inc], tool=TOOL)


def run(exe, quiet=False):
    """Number of failures."""
    bad = 0
    for px in SIZES:
        r = subprocess.run([exe, FONT, str(px)] + [f"{c:X}" for c in EMOJI], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{TOOL}: driver failed: {r.stderr}")
        lines = r.stdout.splitlines()
        if px == SIZES[0]:
            cases = list(map(int, lines[0].split()[1:]))
            ok = all(cases)
            bad += not ok
            if not quiet:
                print(f"  {'ok  ' if ok else 'FAIL'} uemoji.h: the six presentation cases {cases}")
        glyphs = [ln.split() for ln in lines if ln.startswith("GLYPH")]
        drawn = [g for g in glyphs if g[2] == "1"]
        sums = sum(int(g[3]) for g in drawn)
        offs = sum(int(g[4]) for g in drawn)
        pixels = sum(int(g[5]) for g in drawn)
        mean = sums / max(1, pixels * 4)
        frac = offs / max(1, pixels)
        ok = len(drawn) == len(EMOJI) and mean < MEAN_MAX and frac < OFF_MAX
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {px} px: {len(drawn)}/{len(EMOJI)} drawn in colour, "
                  f"mean difference {mean:.2f}/255, {frac:.1%} of pixels past 48 "
                  f"(bars {MEAN_MAX}, {OFF_MAX:.0%})")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the colour path; must go RED")
    args = ap.parse_args()
    if not have_freetype():
        print(f"{TOOL}: SKIPPED -- no libfreetype headers on this host")
        return 0
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                bad = run(build(tmp, control), quiet=True)
            print(f"  {'RED  ' if bad else 'green'} {control[0]}: {bad} check(s) failing")
            red.append(bad > 0)
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        bad = run(build(tmp))
    print(f"{TOOL}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
