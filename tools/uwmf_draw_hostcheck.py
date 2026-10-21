#!/usr/bin/env python3
"""Check userland/ui/uwmf_draw.c (a metafile played into a box) on the HOST, against stub drawing calls.

WHAT IT CHECKS. Small metafiles written here, each played into the 100 x 50 box at (10, 20) by the real
uwmf_draw.c (and the real lib/uwmf.c under it), against stubs of ugfx and lib/uimg that PRINT what they
are asked to draw. Each case says the calls it must cause, worked out here from MS-WMF's rules, not from
the C:

  - the window onto the box: SetWindowOrg/Ext (a non-zero origin), a placeable header's bounds when no
    record sets the window, SaveDC / RestoreDC(-1) putting a changed extent back;
  - pens, brushes and fonts made into object-table slots, selected, deleted: a null pen draws no outline,
    a solid brush fills, colours come out RGB from a COLORREF;
  - text placed by its alignment (TA_BASELINE lifts it by the ascent), in a font asked for at the size the
    LOGFONT's height maps to;
  - a DIB's picture: the BMP file made of it must put the pixel data where its header and palette say (a
    24-bit DIB with no palette, an 8-bit one whose palette has a clrUsed of 4) -- the stub decodes only
    the first pixel and checks the offset, which is all this file computes;
  - nothing drawn outside the box: the play intersects the clip with it, and restores the caller's.

  python3 tools/uwmf_draw_hostcheck.py
  python3 tools/uwmf_draw_hostcheck.py --positive-control

**A CLEAN RUN PROVES NOTHING UNTIL --positive-control HAS GONE RED**: a copy that ignores the window's
origin, and one that forgets the palette when it places a DIB's bits, must each fail a case. Needs gcc.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

import hostcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAGS = ("-O1", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")

# Stubs: every drawing call prints itself. The font stub is the session face; uimg_decode reads the
# BMP header uwmf_draw built and decodes ONE pixel, the first of the bits -- where the offset points.
STUBS = r"""
#include "ui/ugfx.h"
#include "lib/uimg.h"
#include "ui/uwmf_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ugfx_font g_session;
static const struct ugfx_font *g_cur = &g_session;
const struct ugfx_font *ugfx_font_current(void) { return g_cur; }
const struct ugfx_font *ugfx_set_font(const struct ugfx_font *f) { const struct ugfx_font *was = g_cur; g_cur = f; return was; }
int ugfx_char_h(void) { return 20; }
int ugfx_text_width(const char *s) { return 10 * (int)strlen(s); }
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) { (void)s; printf("fill %d %d %d %d %08x\n", x, y, w, h, c); }
void ugfx_draw_line(struct ugfx_surface *s, int x0, int y0, int x1, int y1, uint32_t c, enum geom_aa aa) {
    (void)s; (void)aa; printf("line %d %d %d %d %08x\n", x0, y0, x1, y1, c);
}
void ugfx_draw_polyline(struct ugfx_surface *s, const int *xs, const int *ys, int n, int closed, uint32_t c, enum geom_aa aa) {
    (void)s; (void)aa; printf("poly %d %d", n, closed);
    for (int i = 0; i < n; i++) printf(" %d,%d", xs[i], ys[i]);
    printf(" %08x\n", c);
}
void ugfx_fill_polygon(struct ugfx_surface *s, const int *xs, const int *ys, int n, uint32_t c) {
    (void)s; printf("fillpoly %d", n);
    for (int i = 0; i < n; i++) printf(" %d,%d", xs[i], ys[i]);
    printf(" %08x\n", c);
}
void ugfx_draw_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry, uint32_t c, enum geom_aa aa) {
    (void)s; (void)aa; printf("ellipse %d %d %d %d %08x\n", cx, cy, rx, ry, c);
}
void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry, uint32_t c) {
    (void)s; printf("fillellipse %d %d %d %d %08x\n", cx, cy, rx, ry, c);
}
void ugfx_put_pixel(struct ugfx_surface *s, int x, int y, uint32_t c) { (void)s; printf("pixel %d %d %08x\n", x, y, c); }
void ugfx_blit(struct ugfx_surface *s, int x, int y, int w, int h, const uint32_t *src, int pitch) {
    (void)s; (void)pitch; printf("blit %d %d %d %d %08x\n", x, y, w, h, src[0] & 0xFFFFFF);
}
void ugfx_draw_string(struct ugfx_surface *s, int x, int y, const char *str, uint32_t c, uint32_t bg) {
    (void)s; printf("text %d %d [%s] %08x %s\n", x, y, str, c, bg == UGFX_TRANSPARENT ? "clear" : "opaque");
}
void ugfx_clip_save(const struct ugfx_surface *s, struct ugfx_clip *out) { (void)s; memset(out, 0, sizeof *out); }
void ugfx_clip_restore(struct ugfx_surface *s, const struct ugfx_clip *c) { (void)s; (void)c; printf("clip restored\n"); }
void ugfx_clip_intersect(struct ugfx_surface *s, int x, int y, int w, int h) { (void)s; printf("clip %d %d %d %d\n", x, y, w, h); }

static unsigned u32(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
int uimg_decode(const void *data, size_t n, struct uimg *out) {
    const uint8_t *p = data;
    if (n < 54 || p[0] != 'B' || p[1] != 'M' || u32(p + 2) != n) { printf("decode refused\n"); return -1; }
    unsigned off = u32(p + 10), hs = u32(p + 14), bits = p[28] | p[29] << 8;
    int w = (int)u32(p + 18), h = (int)u32(p + 22);
    uint32_t px;
    if (off >= n) { printf("decode offset %u past %zu\n", off, n); return -1; }
    if (bits == 24) px = (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 1] << 8 | p[off];
    else { const uint8_t *e = p + 14 + hs + 4 * p[off]; px = (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0]; }
    out->w = w; out->h = h; out->has_alpha = 0;
    out->px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) out->px[i] = px;
    return 0;
}
int uimg_scale(const struct uimg *src, int dw, int dh, struct uimg *out) {
    out->w = dw; out->h = dh; out->has_alpha = 0;
    out->px = malloc((size_t)dw * dh * 4);
    for (int i = 0; i < dw * dh; i++) out->px[i] = src->px[0];
    return 0;
}
void uimg_free(struct uimg *im) { free(im->px); im->px = 0; }

static const struct ugfx_font *font(void *ctx, const char *face, int px, int bold, int italic) {
    (void)ctx; printf("font [%s] %d %d %d\n", face, px, bold, italic); return &g_session;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int played = uwmf_draw(0, buf, n, 10, 20, 100, 50, font, 0);
    printf("played %d font %s\n", played, g_cur == &g_session ? "restored" : "LEFT CHANGED");
    return 0;
}
"""


def rec(func, *args, raw=b""):
    """A record, its arguments in GDI's order (stored last first, as MS-WMF stores them)."""
    words = struct.pack("<" + "h" * len(args), *reversed(args)) if args else b""
    body = raw + words if raw and func in (0x0F43, 0x0B41, 0x0940, 0x061D) else words + raw
    return struct.pack("<IH", 3 + len(body) // 2, func) + body


def colorref(rgb):
    return struct.pack("<I", (rgb >> 16 & 0xFF) | (rgb & 0xFF00) | (rgb & 0xFF) << 16)


def metafile(records, nobjects=4, placeable=None):
    body = b"".join(records) + struct.pack("<IH", 3, 0)
    words = (18 + len(body)) // 2
    out = struct.pack("<HHHIHIH", 1, 9, 0x300, words, nobjects, 64, 0) + body
    if placeable:
        l, t, r, b = placeable
        ph = struct.pack("<IHhhhhHI", 0x9AC6CDD7, 0, l, t, r, b, 1440, 0)
        check = 0
        for (word,) in struct.iter_unpack("<H", ph):
            check ^= word
        out = ph + struct.pack("<H", check) + out
    return out


def brush(rgb, style=0):
    return struct.pack("<IH", 7, 0x02FC) + struct.pack("<H", style) + colorref(rgb) + struct.pack("<H", 0)


def pen(rgb, style=0, width=1):
    return struct.pack("<IH", 8, 0x02FA) + struct.pack("<HhH", style, width, 0) + colorref(rgb)


def polyline(*pts):
    body = struct.pack("<h", len(pts)) + b"".join(struct.pack("<hh", x, y) for x, y in pts)
    return struct.pack("<IH", 3 + len(body) // 2, 0x0325) + body


def select(slot):
    return rec(0x012D, slot)


def font(height, weight=400, face=b"Arial"):
    lf = struct.pack("<hhhhh", height, 0, 0, 0, weight) + bytes(8) + face.ljust(32, b"\0")[:32]
    return struct.pack("<IH", 3 + len(lf) // 2, 0x02FB) + lf


def dib24(rgb):
    hdr = struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, 16, 0, 0, 0, 0)
    px = bytes([rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16 & 0xFF])
    return hdr + (px * 2 + b"\0\0") * 2


def dib8(rgb):
    # four palette entries (clrUsed 4), the wanted colour at index 2
    hdr = struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 8, 0, 8, 0, 0, 4, 0)
    pal = b"\0\0\0\0" * 2 + bytes([rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16 & 0xFF, 0]) + b"\xff\xff\xff\0"
    return hdr + pal + bytes([2, 2, 0, 0]) * 2


def stretchdib(dib, xd, yd, wd, hd, xs, ys, ws, hs):
    args = (xd, yd, wd, hd, xs, ys, ws, hs, 0)            # usage DIB_RGB_COLORS
    words = struct.pack("<" + "h" * 9, *reversed(args))
    body = struct.pack("<I", 0x00CC0020) + words + dib
    return struct.pack("<IH", 3 + len(body) // 2, 0x0F43) + body


RED, BLUE, GREEN = 0xFF0000, 0x0000FF, 0x00AA00

# (name, metafile, lines that must appear in order). The box is 100 x 50 at (10, 20).
CASES = [
    ("a filled rectangle through a window with an origin",
     metafile([rec(0x020B, 100, 50), rec(0x020C, 200, 100), brush(RED), select(0), pen(0, style=5), select(1),
               rec(0x041B, 100, 50, 300, 150)]),
     ["clip 10 20 100 50", "fill 10 20 100 50 ffff0000", "clip restored", "played"]),
    ("the window's extent maps the box (a 2:1 shrink)",
     metafile([rec(0x020C, 200, 100), brush(RED), select(0), pen(0, style=5), select(1), rec(0x041B, 20, 10, 120, 60)]),
     ["fill 20 25 50 25 ffff0000"]),
    ("a solid pen outlines a rectangle and a polyline, in RGB",
     metafile([rec(0x020C, 100, 50), pen(BLUE), select(0), brush(0, style=1), select(1),
               rec(0x041B, 0, 0, 10, 10), polyline((0, 0), (50, 0), (50, 25))]),
     ["poly 4 1 10,20 19,20 19,29 10,29 ff0000ff", "poly 3 0 10,20 60,20 60,45 ff0000ff"]),
    ("SaveDC and RestoreDC(-1) put the window back",
     metafile([rec(0x020C, 100, 50), rec(0x001E), rec(0x020C, 10, 10), rec(0x0127, -1),
               brush(GREEN), select(0), pen(0, style=5), select(1), rec(0x041B, 0, 0, 50, 25)]),
     ["fill 10 20 50 25 ff00aa00"]),
    ("a placeable header's bounds are the window",
     metafile([brush(RED), select(0), pen(0, style=5), select(1), rec(0x041B, 0, 0, 200, 100)], placeable=(0, 0, 400, 200)),
     ["fill 10 20 50 25 ffff0000"]),
    ("text on its baseline, in the font's mapped size, then the font put back",
     metafile([rec(0x020C, 200, 100), rec(0x012E, 24), font(-40, 700), select(0), rec(0x0102, 1),
               rec(0x0209, raw=colorref(BLUE)), rec(0x0521, 2, raw=b"Hi"), ]),
     []),   # filled in below: TextOut's layout is count, string, y, x
    ("a 24-bit DIB stretched into the box, its bits where the header says",
     metafile([rec(0x020C, 100, 50), stretchdib(dib24(GREEN), 0, 0, 100, 50, 0, 0, 2, 2)]),
     ["blit 10 20 100 50 0000aa00"]),
    ("an 8-bit DIB with a short palette (clrUsed 4): the bits after four entries",
     metafile([rec(0x020C, 100, 50), stretchdib(dib8(BLUE), 0, 0, 50, 25, 0, 0, 2, 2)]),
     ["blit 10 20 50 25 000000ff"]),
]


def textout(text, x, y):
    s = text.encode("latin-1")
    pad = s + (b"\0" if len(s) % 2 else b"")
    body = struct.pack("<h", len(s)) + pad + struct.pack("<hh", y, x)
    return struct.pack("<IH", 3 + len(body) // 2, 0x0521) + body


# The text case: TextOut(100, 80, "Hi") in a 200 x 100 window -> (60, 60); TA_BASELINE lifts it by the
# stub's ascent (20 * 4 / 5 = 16); the -40 height maps to 20 px; weight 700 is bold.
name, _, _ = CASES[5]
CASES[5] = (name, metafile([rec(0x020C, 200, 100), rec(0x012E, 24), font(-40, 700), select(0), rec(0x0102, 1),
                            rec(0x0209, raw=colorref(BLUE)), textout("Hi", 100, 80)]),
            ["font [Arial] 20 1 0", "text 60 44 [Hi] ff0000ff clear", "played"])


def build(tmp, edits=()):
    drv = hostcheck.write(tmp, "uwmf_draw_host.c", STUBS)
    draw = hostcheck.stage(tmp, "userland/ui/uwmf_draw.c", dest="ui/uwmf_draw.c", edits=edits, apply=bool(edits),
                           tool="uwmf_draw_hostcheck")
    return hostcheck.compile(tmp, "uwmf_draw_host", [drv, draw, os.path.join(ROOT, "userland", "lib", "uwmf.c")],
                             # the kernel's headers AFTER the host's: kernel/include/api has a string.h of its own
                             flags=FLAGS + ("-idirafter", os.path.join(ROOT, "kernel", "include", "api"),
                                            "-idirafter", os.path.join(ROOT, "kernel", "include", "abi")),
                             includes=(os.path.join(ROOT, "userland"),),
                             tool="uwmf_draw_hostcheck")


def check(exe, tmp):
    problems = []
    for i, (name, data, want) in enumerate(CASES):
        path = os.path.join(tmp, f"case{i}.wmf")
        open(path, "wb").write(data)
        r = subprocess.run([exe, path], capture_output=True, text=True)
        got = r.stdout.splitlines()
        if r.returncode != 0 or r.stderr.strip():
            problems.append(f"{name}: exit {r.returncode} {r.stderr.strip()[:300]}")
            continue
        at = 0
        for line in want:
            while at < len(got) and not got[at].startswith(line):
                at += 1
            if at == len(got):
                problems.append(f"{name}: no `{line}` (in order) in {got}")
                break
            at += 1
        if not any(ln.endswith("font restored") for ln in got):
            problems.append(f"{name}: the caller's font was not put back: {got[-1:]}")
    return problems


CONTROLS = [
    ("the window's origin ignored", [("(lx - p->dc.ox)", "(lx)")]),
    ("a DIB's palette forgotten when its bits are placed", [("size_t off = 14 + hs + (size_t)colours * entry;",
                                                            "size_t off = 14 + hs;")]),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    if args.positive_control:
        rc = 0
        for name, edits in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                probs = check(build(tmp, edits), tmp)
            if probs:
                print(f"control {name:52} RED (good): {probs[0][:110]}")
            else:
                print(f"control {name:52} STAYED GREEN -- this check cannot see that break")
                rc = 1
        return rc
    with tempfile.TemporaryDirectory() as tmp:
        probs = check(build(tmp), tmp)
    for p in probs:
        print("FAIL:", p)
    print(f"uwmf_draw_hostcheck: {'ok' if not probs else f'{len(probs)} problems'} ({len(CASES)} metafiles)")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main())
