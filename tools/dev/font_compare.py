#!/usr/bin/env python3
"""tools/dev/font_compare.py -- small text from SaunaOS's rasterizer, beside a reference.

Renders real UI strings with the tree's own `userland/libs/media/ttf.c`
(compiled for the host from `font_compare.c`) and, when libfreetype is
installed, with FreeType's LIGHT AUTOHINTER -- the vertical-only hinting
SaunaOS's own autohinter is built to match. Each VARIANT is a renderer, a
sub-scanline count and a blend:

  today      ours, the tree's TTF_SUBSAMPLES, blended in sRGB (what ugfx does)
  sub16      ours, 16 sub-scanlines, sRGB
  hint       FreeType light autohint, sRGB
  hintdw     FreeType light autohint, gamma 1.8 + a contrast boost (DirectWrite's shape)
  hintlinc   FreeType light autohint, linear light + a contrast boost

Every renderer is laid out with ttf.c's advances and kerning, so two sheets
differ only in the glyphs' pixels. Out: `<variant>_1x.png` and
`<variant>_3x.png` (nearest-neighbour zoom) per variant, and `compare.png`
with every variant side by side.

  tools/dev/font_compare.py --out /tmp/fc
  tools/dev/font_compare.py --variants today,hint --sizes 9-12 --face dejavu-sans-mono --out /tmp/fc

THE TRAP: a comparison sheet is only fair at ACTUAL PIXELS -- judge the
1x images at 100% zoom; the 3x ones are for seeing why. And blending is
part of the look: a linear-light blend thins dark text on a light ground
however good the coverage is.

Needs gcc and Pillow; the FreeType variants need libfreetype's headers
(pkg-config freetype2) and are skipped without them.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
ROOT = toolpath.REPO

try:
    from PIL import Image
except ImportError:
    sys.exit("font_compare: needs Pillow (python3-pil)")

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(ROOT, "data", "fonts")

# variant -> (renderer, sub-scanlines or None for the tree's, blend)
VARIANTS = {
    "today":    ("ours", None, "srgb"),
    "sub16":    ("ours", 16, "srgb"),
    "hint":     ("ft", 16, "srgb"),
    "hintdw":   ("ft", 16, "gamma18"),
    "hintlinc": ("ft", 16, "linear"),
}

# Colours from utheme_default(): body text on the window ground, a title
# bar, and a dark terminal.
DARK = ((20, 20, 20), (228, 228, 228))
TITLE = ((255, 255, 255), (50, 90, 160))
TERM = ((220, 220, 220), (30, 30, 34))
LINES = [
    (DARK, "File   Edit   View   Window   Help"),
    (DARK, "The quick brown fox jumps over the lazy dog"),
    (DARK, "Åland · Ärlig · Éclair · 0123456789 · 125%"),
    (TITLE, "Display Settings — Notepad"),
]
MONO_LINE = (TERM, "$ ls -la /usr/share/fonts  # ÅÄÖ")


def build(workdir, subsamples, with_ft):
    """The renderer, against a COPY of ttf.h when the sub-scanline count changes."""
    inc = os.path.join(workdir, f"inc{subsamples or 'tree'}")
    os.makedirs(inc, exist_ok=True)
    api = os.path.join(ROOT, "kernel", "include", "api")
    # Only these two: api/ also holds a string.h that would shadow libc's.
    shutil.copy(os.path.join(api, "fixed.h"), inc)
    with open(os.path.join(api, "ttf.h")) as f:
        hdr = f.read()
    if subsamples:
        import re
        hdr, n = re.subn(r"#define TTF_SUBSAMPLES\s+\d+", f"#define TTF_SUBSAMPLES {subsamples}", hdr)
        if n != 1:
            sys.exit("font_compare: TTF_SUBSAMPLES not found in ttf.h")
    with open(os.path.join(inc, "ttf.h"), "w") as f:
        f.write(hdr)
    exe = os.path.join(workdir, f"render{subsamples or 'tree'}{'_ft' if with_ft else ''}")
    cmd = ["gcc", "-O2", "-I", inc, os.path.join(HERE, "font_compare.c"),
           os.path.join(ROOT, "userland", "libs", "media", "ttf.c"), "-o", exe]
    if with_ft:
        cflags = subprocess.run(["pkg-config", "--cflags", "freetype2"], capture_output=True, text=True).stdout.split()
        libs = subprocess.run(["pkg-config", "--libs", "freetype2"], capture_output=True, text=True).stdout.split()
        cmd[1:1] = ["-DHAVE_FREETYPE"] + cflags
        cmd += libs
    subprocess.run(cmd, check=True)
    return exe


def have_freetype():
    return shutil.which("pkg-config") and subprocess.run(
        ["pkg-config", "--exists", "freetype2"]).returncode == 0


def srgb_to_linear(c):
    c /= 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c):
    c = c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return max(0, min(255, round(c * 255)))


LIN = [srgb_to_linear(i) for i in range(256)]


def blend_px(fg, bg, a, mode):
    if mode == "srgb":
        al = a / 255
        return tuple(round(f * al + b * (1 - al)) for f, b in zip(fg, bg))
    if mode == "gamma18":
        al = (a / 255) ** (1 / 1.25)
        return tuple(max(0, min(255, round(255 * (((f / 255) ** 1.8) * al + ((b / 255) ** 1.8) * (1 - al)) ** (1 / 1.8))))
                     for f, b in zip(fg, bg))
    al = (a / 255) ** (1 / 1.45)
    return tuple(linear_to_srgb(LIN[f] * al + LIN[b] * (1 - al)) for f, b in zip(fg, bg))


def strip(exe, renderer, font, px, text, colours, mode, workdir):
    raw = os.path.join(workdir, "strip.raw")
    subprocess.run([exe, renderer, font, str(px), text, raw], check=True)
    with open(raw, "rb") as f:
        blob = f.read()
    nl = blob.index(b"\n")
    w, h, _ = map(int, blob[:nl].split())
    cov = blob[nl + 1:]
    fg, bg = colours
    im = Image.new("RGB", (w, h), bg)
    pix = im.load()
    for y in range(h):
        for x in range(w):
            a = cov[y * w + x]
            if a:
                pix[x, y] = blend_px(fg, bg, a, mode)
    return im


def parse_sizes(s):
    out = []
    for part in s.split(","):
        if "-" in part:
            a, b = map(int, part.split("-"))
            out += range(a, b + 1)
        else:
            out.append(int(part))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, help="directory for the PNGs")
    ap.add_argument("--variants", default=",".join(VARIANTS), help="comma list of: " + ", ".join(VARIANTS))
    ap.add_argument("--sizes", default="9-14", help="em sizes in px, e.g. 9-14 or 10,12")
    ap.add_argument("--face", default="liberation-sans", help="UI face under data/fonts/")
    ap.add_argument("--mono", default="dejavu-sans-mono", help="terminal face under data/fonts/ ('' for none)")
    ap.add_argument("--text", action="append", help="a line to draw instead of the built-in ones (repeatable)")
    args = ap.parse_args()

    names = [v for v in args.variants.split(",") if v]
    for v in names:
        if v not in VARIANTS:
            sys.exit(f"font_compare: unknown variant {v!r}")
    ft_ok = have_freetype()
    if not ft_ok:
        skipped = [v for v in names if VARIANTS[v][0] == "ft"]
        if skipped:
            print(f"font_compare: no libfreetype headers; skipping {', '.join(skipped)}")
        names = [v for v in names if VARIANTS[v][0] != "ft"]
    lines = [(DARK, t) for t in args.text] if args.text else LINES
    face = os.path.join(FONTS, args.face + ".ttf")
    mono = os.path.join(FONTS, args.mono + ".ttf") if args.mono else None
    sizes = parse_sizes(args.sizes)
    os.makedirs(args.out, exist_ok=True)

    sheets = []
    with tempfile.TemporaryDirectory() as work:
        exes = {}
        for v in names:
            renderer, sub, mode = VARIANTS[v]
            key = (sub, renderer == "ft")
            if key not in exes:
                exes[key] = build(work, sub, renderer == "ft")
            rows = []
            for px in sizes:
                block = [strip(exes[key], renderer, face, px, t, c, mode, work) for c, t in lines]
                if mono:
                    block.append(strip(exes[key], renderer, mono, px, MONO_LINE[1], MONO_LINE[0], mode, work))
                rows.append(block)
            width = max(im.width for block in rows for im in block)
            height = sum(im.height for block in rows for im in block) + 6 * (len(rows) - 1)
            sheet = Image.new("RGB", (width, height), (236, 236, 236))
            y = 0
            for block in rows:
                for im in block:
                    sheet.paste(im, (0, y))
                    y += im.height
                y += 6
            sheet.save(os.path.join(args.out, f"{v}_1x.png"))
            sheet.resize((width * 3, height * 3), Image.NEAREST).save(os.path.join(args.out, f"{v}_3x.png"))
            sheets.append(sheet)
            print(f"{v}: {width}x{height}")

    if sheets:
        gap = 12
        w = sum(s.width for s in sheets) + gap * (len(sheets) - 1)
        h = max(s.height for s in sheets)
        cmp_im = Image.new("RGB", (w, h), (255, 255, 255))
        x = 0
        for s in sheets:
            cmp_im.paste(s, (x, 0))
            x += s.width + gap
        cmp_im.save(os.path.join(args.out, "compare.png"))
        print(f"wrote {args.out}/compare.png ({', '.join(names)}, left to right)")


if __name__ == "__main__":
    main()
