#!/usr/bin/env python3
"""Check userland/lib/winfnt.c against FreeType's winfnt driver, on the HOST.

w16 draws a Windows 3.x program's text in the program's own bitmap
fonts -- the .FON files a Windows 3.1 installation keeps in
WINDOWS\\SYSTEM -- through winfnt.c. FreeType reads the same format with
code that shares nothing with it. For every raster font in every file
given, and every character 32..255 FreeType has a glyph for:

  - the advance width agrees
  - the glyph's bitmap agrees, bit for bit, over FreeType's width and
    rows

The fonts are the maintainer's own (never in this repo), so the files
are arguments: the folder holding them, or the .FON files themselves.

    python3 tools/winfnt_hostcheck.py ~/WINDOWS/SYSTEM
    python3 tools/winfnt_hostcheck.py --positive-control ~/WINDOWS/SYSTEM   # must FAIL

Needs FreeType's headers on the host (pkg-config freetype2).
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

# winfnt.c's side: every font of the file, every character, as bit rows.
DRIVER = r"""
#include "lib/winfnt.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t buf[1 << 22];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int count = winfnt_count(buf, n);
    for (int i = 0; i < count; i++) {
        struct winfnt w;
        if (winfnt_get(buf, n, i, &w) < 0 || w.vector) continue;
        printf("font %d\n", i);
        for (int ch = 32; ch < 256; ch++) {
            static uint8_t cov[256 * 256];
            int adv = winfnt_glyph(&w, (unsigned)ch, cov, 256, 256);
            printf("g %d %d", ch, adv);
            for (int y = 0; adv > 0 && y < w.pix_height; y++) {
                putchar(' ');
                for (int x = 0; x < adv && x < 256; x++) putchar(cov[y * 256 + x] ? '1' : '0');
            }
            putchar('\n');
        }
    }
    return 0;
}
"""

# FreeType's side: font INDEX of the file, the same lines.
FREETYPE = r"""
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], atoi(argv[2]), &face)) return 1;
    if (face->num_fixed_sizes < 1 || FT_Select_Size(face, 0)) return 1;
    if (face->num_charmaps > 0) FT_Set_Charmap(face, face->charmaps[0]);
    for (int ch = 32; ch < 256; ch++) {
        FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)ch);
        if (!gi || FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO)) continue;
        FT_GlyphSlot s = face->glyph;
        printf("g %d %ld", ch, (long)(s->advance.x >> 6));
        for (unsigned y = 0; y < s->bitmap.rows; y++) {
            putchar(' ');
            for (unsigned x = 0; x < s->bitmap.width; x++)
                putchar(s->bitmap.buffer[y * s->bitmap.pitch + x / 8] >> (7 - x % 8) & 1 ? '1' : '0');
        }
        putchar('\n');
    }
    return 0;
}
"""

# The control: a glyph's bits read right to left.
CONTROL = ("] >> (7 - x % 8) & 1 ? 255 : 0;", "] >> (x % 8) & 1 ? 255 : 0;")


def glyphs(lines):
    """{font index: {char: (advance, rows)}} from the driver's output."""
    out, cur = {}, None
    for line in lines:
        p = line.split()
        if p and p[0] == "font":
            cur = out.setdefault(int(p[1]), {})
        elif p and p[0] == "g" and cur is not None:
            cur[int(p[1])] = (int(p[2]), p[3:])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", help="a folder of .FON files, or the files")
    ap.add_argument("--positive-control", action="store_true",
                    help="compile glyph bits read right to left; the run must fail")
    args = ap.parse_args()
    files = []
    for p in args.paths:
        files += sorted(glob.glob(os.path.join(p, "*.[Ff][Oo][Nn]"))) if os.path.isdir(p) else [p]
    if not files:
        sys.exit("winfnt_hostcheck: no .FON files given")
    ft_flags = subprocess.run(["pkg-config", "--cflags", "--libs", "freetype2"], capture_output=True, text=True)
    if ft_flags.returncode:
        sys.exit("winfnt_hostcheck: FreeType's headers are not installed (pkg-config freetype2)")

    fails, n = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.stage(tmp, "userland/lib/winfnt.h", dest="lib/winfnt.h")
        src = hostcheck.stage(tmp, "userland/lib/winfnt.c", edits=[CONTROL], apply=args.positive_control,
                              tool="winfnt_hostcheck")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "winfnt_host", [src, drv], includes=[tmp], tool="winfnt_hostcheck")
        ftc = hostcheck.write(tmp, "freetype.c", FREETYPE)
        ft = hostcheck.compile(tmp, "winfnt_freetype", [ftc, *ft_flags.stdout.split()],
                               flags=("-O2", "-Wall"), tool="winfnt_hostcheck")
        for path in files:
            mine = glyphs(subprocess.run([exe, path], capture_output=True, text=True, check=True).stdout.splitlines())
            for index, gl in mine.items():
                r = subprocess.run([ft, path, str(index)], capture_output=True, text=True)
                if r.returncode:
                    fails.append(f"{os.path.basename(path)} font {index}: FreeType could not open it")
                    continue
                for line in r.stdout.splitlines():
                    p = line.split()
                    ch, adv, rows = int(p[1]), int(p[2]), p[3:]
                    n += 1
                    got = gl.get(ch)
                    width = len(rows[0]) if rows else 0
                    if not got or got[0] != adv or [row[:width] for row in got[1]][:len(rows)] != rows:
                        why = "its bitmap differs" if got and got[0] == adv else \
                            f"advance {got and got[0]}, FreeType {adv}"
                        fails.append(f"{os.path.basename(path)} font {index} char {ch}: {why}")

    print(f"winfnt_hostcheck: {n} glyphs in {len(files)} files, {len(fails)} differ")
    for f in fails[:20]:
        print("  FAIL", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
