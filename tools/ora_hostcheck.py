#!/usr/bin/env python3
"""Check lib/uora (OpenRaster) against Python's zipfile, ElementTree and Pillow, on the HOST.

BOTH DIRECTIONS, against code that shares nothing with userland/lib/ --
archive_hostcheck.py's rule. A layered-picture writer read back only by
its own reader passes when the two share a mistake (layers stacked the
wrong way round, an opacity scaled twice, a name escaped once and
unescaped twice), and such a file opens here and in no other painter.

  WRITE: the C writer saves pictures of 1 to 5 layers with random pixels
         (alpha too), random opacities, hidden ones, and names carrying
         XML's specials and Latin-1, and toyos: attributes holding
         newlines, tabs, specials, Latin-1 and 3000 bytes. Python must find: "mimetype" FIRST,
         STORED, no extra field, "image/openraster"; no bad CRC; a
         stack.xml that parses, the right size, the layers TOP FIRST with
         their names, opacities (within 0.002) and visibility; each
         layer's PNG equal pixel for pixel; mergedimage.png equal to the
         merged picture; a thumbnail at most 256 on its long side.
  READ:  Python writes what Krita does -- a layer stored CROPPED with an
         x/y offset, a nested <stack> group, opacity "0.5", entities and
         a character reference in a name, a deflated stack.xml -- and
         the C reader must give the layers bottom first, each on the full
         canvas with the cropped one placed at its offset, at the right
         opacity and visibility, the names decoded. And it must REFUSE a
         wrong mimetype and a layer whose PNG is missing.

    python3 tools/ora_hostcheck.py
    python3 tools/ora_hostcheck.py --positive-control   # must FAIL

The positive control drops the turn from top-first to bottom-first and
the escaping of '&'. Needs gcc and Pillow.
"""

import argparse
import io
import os
import random
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include "lib/uora.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct uimg_codec uimg_codec_png;

// lib/uimg.c's dispatch, cut to the one codec .ora uses.
int uimg_encode(const struct uimg *im, const char *format, uint8_t **out, size_t *out_len) {
    (void)format;
    return uimg_codec_png.encode(im, out, out_len);
}
int uimg_decode(const void *d, size_t n, struct uimg *out) { return uimg_codec_png.decode(d, n, out); }
void uimg_free(struct uimg *im) { free(im->px); memset(im, 0, sizeof *im); }
void uimg_set_error(const char *m) { (void)m; }
int uimg_scale(const struct uimg *src, int dw, int dh, struct uimg *out) {
    out->px = malloc((size_t)dw * dh * 4);
    if (!out->px) return -1;
    out->w = dw; out->h = dh; out->has_alpha = 1;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            out->px[y * dw + x] = src->px[(size_t)(y * src->h / dh) * src->w + x * src->w / dw];
    return 0;
}

static unsigned rd(FILE *f) { unsigned v = 0; if (fscanf(f, "%x", &v) != 1) exit(2); return v; }

int main(int argc, char **argv) {
    char err[200] = "";
    if (argc == 4 && !strcmp(argv[1], "write")) {
        // spec: N W H, then per layer bottom first: opacity hidden namehex, W*H pixels; then merged pixels
        FILE *f = fopen(argv[2], "r");
        int n, w, h;
        if (!f || fscanf(f, "%d %d %d", &n, &w, &h) != 3) return 2;
        struct uora_layer *L = calloc((size_t)n, sizeof *L);
        for (int i = 0; i < n; i++) {
            char hex[300];
            if (fscanf(f, "%d %d %299s %d", &L[i].opacity, &L[i].hidden, hex, &L[i].nattrs) != 4) return 2;
            for (int k = 0; hex[2 * k] && hex[2 * k] != '-' && k < UORA_NAME_MAX - 1; k++) {
                unsigned b; sscanf(hex + 2 * k, "%2x", &b); L[i].name[k] = (char)b;
            }
            for (int a = 0; a < L[i].nattrs; a++) {
                static char vhex[20000];
                if (fscanf(f, "%23s %19999s", L[i].attrs[a].name, vhex) != 2) return 2;
                char *v = calloc(strlen(vhex) / 2 + 1, 1);
                for (int k = 0; vhex[2 * k] && vhex[2 * k] != '-'; k++) {
                    unsigned b; sscanf(vhex + 2 * k, "%2x", &b); v[k] = (char)b;
                }
                L[i].attrs[a].value = v;
            }
            L[i].img.w = w; L[i].img.h = h; L[i].img.has_alpha = 1;
            L[i].img.px = malloc((size_t)w * h * 4);
            for (int p = 0; p < w * h; p++) L[i].img.px[p] = rd(f);
        }
        struct uimg m = { 0 };
        m.w = w; m.h = h; m.has_alpha = 1;
        m.px = malloc((size_t)w * h * 4);
        for (int p = 0; p < w * h; p++) m.px[p] = rd(f);
        int rc = uora_save(argv[3], L, n, &m, err, sizeof err);
        printf("save %d %s\n", rc, err);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "read")) {
        struct uora_layer *L;
        int n, w, h;
        int rc = uora_load(argv[2], &L, &n, &w, &h, err, sizeof err);
        printf("load %d %s\n", rc, rc ? err : "-");
        if (rc) return 0;
        printf("size %d %d %d\n", w, h, n);
        for (int i = 0; i < n; i++) {
            printf("layer %d %d ", L[i].opacity, L[i].hidden);
            for (const char *c = L[i].name; *c; c++) printf("%02x", (unsigned char)*c);
            printf("- %d", L[i].nattrs);
            for (int a = 0; a < L[i].nattrs; a++) {
                printf(" %s=", L[i].attrs[a].name);
                for (const char *c = L[i].attrs[a].value; *c; c++) printf("%02x", (unsigned char)*c);
                printf("-");
            }
            printf("\n");
            for (int p = 0; p < w * h; p++) printf("%x ", L[i].img.px[p]);
            printf("\n");
        }
        uora_free(L, n);
        return 0;
    }
    return 2;
}
"""

SABOTAGE = [
    ("userland/lib/uora.c",
     "    for (int i = 0, j = count - 1; i < j; i++, j--) { struct uora_layer t = L[i]; L[i] = L[j]; L[j] = t; }",
     "    (void)0;"),
    ("userland/lib/uora.c",
     """const char *rep = u >= 0x80 ? num : *s == '&' ? "&amp;" :""",
     """const char *rep = u >= 0x80 ? num : *s == '&' ? "&" :"""),
]

SYS_SHIM = r'''
#ifndef SHIM_RT_SYS_H
#define SHIM_RT_SYS_H
#include <stdint.h>
#include <sys/stat.h>
#include "rtctime.h"
struct sys_stat { uint64_t size; uint32_t is_dir; struct rtc_time modified; };
static inline int sys_stat(const char *p, struct sys_stat *o) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    o->size = (uint64_t)st.st_size;
    o->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    o->modified = (struct rtc_time){ 0 };
    return 0;
}
#endif
'''


def build(tmp, sabotage):
    shim = os.path.join(tmp, "shim")
    os.makedirs(os.path.join(shim, "rt"), exist_ok=True)
    for name in ("kcrc.h",):
        with open(os.path.join(shim, name), "w") as f:
            f.write('#include "%s"\n' % os.path.join(ROOT, "kernel", "include", "api", name))
    with open(os.path.join(shim, "rt", "sys.h"), "w") as f:
        f.write(SYS_SHIM)
    srcs = {p: os.path.join(ROOT, p) for p in
            ("userland/lib/uora.c", "userland/lib/uzip.c", "userland/lib/uinflate.c",
             "userland/lib/uimg_png.c")}
    if sabotage:
        text = open(srcs["userland/lib/uora.c"]).read()
        for path, old, new in SABOTAGE:
            if text.count(old) != 1:
                sys.exit(f"positive control: {path} no longer has the line it breaks: {old[:50]}")
            text = text.replace(old, new)
        broken = os.path.join(tmp, "uora.c")
        with open(broken, "w") as f:
            f.write(text)
        srcs["userland/lib/uora.c"] = broken
    drv = os.path.join(tmp, "driver.c")
    with open(drv, "w") as f:
        f.write(DRIVER)
    exe = os.path.join(tmp, "ora")
    cmd = ["gcc", "-O1", "-Wall", "-Wno-unused-parameter", "-o", exe, drv, *srcs.values(),
           os.path.join(ROOT, "kernel", "lib", "kcrc.c"),
           "-I" + shim, "-I" + os.path.join(ROOT, "userland"),
           "-idirafter", os.path.join(ROOT, "kernel", "include", "abi")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stderr)
    return exe


def run(exe, *args):
    return subprocess.run([exe, *args], capture_output=True, text=True, check=True).stdout.splitlines()


def rgba(im):
    """Every pixel of an RGBA image as (r, g, b, a), in reading order."""
    b = im.tobytes()
    return [tuple(b[i:i + 4]) for i in range(0, len(b), 4)]


def argb_to_rgba(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24)


NS = "{https://github.com/eveningworks/toy-os/ns/openraster}"
VALUES = ["Hello,\nworld", 'a & b <c> "d" \'e\'', "Caf\xe9\ttab", "", "x" * 3000]
NAMES = ["Background", "Ink & wash", "<sketch>", 'say "hi"', "it's", "Caf\xe9 cr\xe8me", "Layer 2"]


def check_write(exe, tmp, rng, fails):
    checks = 0
    for case in range(12):
        n, w, h = rng.randrange(1, 6), rng.randrange(1, 40), rng.randrange(1, 30)
        layers = []
        for i in range(n):
            px = [rng.randrange(256) << 24 | rng.randrange(1 << 24) for _ in range(w * h)]
            attrs = [(f"k{j}", rng.choice(VALUES)) for j in range(rng.randrange(3))]
            layers.append((rng.choice((255, 128, 1, 0, rng.randrange(256))), rng.randrange(2),
                           rng.choice(NAMES), px, attrs))
        merged = [rng.randrange(1 << 32) | 0xFF000000 for _ in range(w * h)]
        spec = os.path.join(tmp, "spec.txt")
        with open(spec, "w") as f:
            f.write(f"{n} {w} {h}\n")
            for op, hid, name, px, attrs in layers:
                f.write(f"{op} {hid} {name.encode('latin-1').hex()}- {len(attrs)}\n")
                for k, v in attrs:
                    f.write(f"{k} {v.encode('latin-1').hex()}-\n")
                f.write(" ".join(f"{v:x}" for v in px) + "\n")
            f.write(" ".join(f"{v:x}" for v in merged) + "\n")
        out = os.path.join(tmp, f"w{case}.ora")
        line = run(exe, "write", spec, out)[0]
        checks += 1
        if not line.startswith("save 0"):
            fails.append(f"write case {case}: {line}")
            continue
        z = zipfile.ZipFile(out)
        first = z.infolist()[0]
        if first.filename != "mimetype" or first.compress_type != zipfile.ZIP_STORED or first.extra \
                or z.read("mimetype") != b"image/openraster":
            fails.append(f"write case {case}: the first member is not a stored mimetype "
                         f"({first.filename}, method {first.compress_type})")
        if z.testzip() is not None:
            fails.append(f"write case {case}: zipfile finds a bad CRC in {z.testzip()}")
        try:
            root = ET.fromstring(z.read("stack.xml"))
        except ET.ParseError as e:
            fails.append(f"write case {case}: stack.xml does not parse: {e}")
            continue
        if (int(root.get("w")), int(root.get("h"))) != (w, h):
            fails.append(f"write case {case}: stack.xml says {root.get('w')}x{root.get('h')}")
        els = root.findall("./stack/layer")
        if len(els) != n:
            fails.append(f"write case {case}: {len(els)} layers in stack.xml, want {n}")
            continue
        for k, el in enumerate(els):        # top first
            op, hid, name, px, attrs = layers[n - 1 - k]
            for ak, av in attrs:
                if el.get(NS + ak) != av:
                    fails.append(f"write case {case}: layer {k}'s toyos:{ak} reads {el.get(NS + ak)!r:.60}, "
                                 f"want {av!r:.60}")
            if el.get("name") != name:
                fails.append(f"write case {case}: layer {k} from the top is named {el.get('name')!r}, want {name!r}")
            if abs(float(el.get("opacity")) - op / 255) > 0.002:
                fails.append(f"write case {case}: layer {k} opacity {el.get('opacity')}, want {op / 255:.3f}")
            if el.get("visibility") != ("hidden" if hid else "visible"):
                fails.append(f"write case {case}: layer {k} visibility {el.get('visibility')}")
            im = Image.open(io.BytesIO(z.read(el.get("src")))).convert("RGBA")
            if im.size != (w, h) or rgba(im) != [argb_to_rgba(v) for v in px]:
                fails.append(f"write case {case}: layer {k}'s PNG is not its pixels")
        mi = Image.open(io.BytesIO(z.read("mergedimage.png"))).convert("RGBA")
        if rgba(mi) != [argb_to_rgba(v) for v in merged]:
            fails.append(f"write case {case}: mergedimage.png is not the merged picture")
        th = Image.open(io.BytesIO(z.read("Thumbnails/thumbnail.png")))
        if max(th.size) > 256:
            fails.append(f"write case {case}: a {th.size} thumbnail")
    # The thumbnail bound on a picture bigger than it.
    w, h = 600, 300
    spec = os.path.join(tmp, "spec.txt")
    with open(spec, "w") as f:
        f.write(f"1 {w} {h}\n255 0 {b'Big'.hex()}- 0\n" + " ".join(["ff336699"] * (w * h)) + "\n"
                + " ".join(["ff336699"] * (w * h)) + "\n")
    run(exe, "write", spec, os.path.join(tmp, "big.ora"))
    checks += 1
    th = Image.open(io.BytesIO(zipfile.ZipFile(os.path.join(tmp, "big.ora")).read("Thumbnails/thumbnail.png")))
    if th.size != (256, 128):
        fails.append(f"a 600x300 picture's thumbnail is {th.size}, want (256, 128)")
    return checks


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def krita_ora(path, layers, w, h, mimetype=b"image/openraster", drop_src=None):
    """layers: top first, (name_xml, opacity_str, visibility, x, y, PIL image, group)."""
    xml = [f"<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"{w}\" h=\"{h}\" "
           f"xmlns:toyos=\"{NS[1:-1]}\" xmlns:krita=\"http://krita.org/ns\">\n<stack>\n"]
    open_group = False
    for i, (name, op, vis, x, y, im, group) in enumerate(layers):
        if group and not open_group:
            xml.append('<stack name="Group" opacity="1.0" visibility="visible">\n')
            open_group = True
        if not group and open_group:
            xml.append("</stack>\n")
            open_group = False
        extra = ' toyos:text="two&#10;lines &amp; more" krita:ignored="1" toyos:px="40"' if i == 0 else ""
        xml.append(f'<layer name="{name}" src="data/l{i}.png" x="{x}" y="{y}" opacity="{op}" visibility="{vis}"{extra}/>\n')
    if open_group:
        xml.append("</stack>\n")
    xml.append("</stack>\n</image>\n")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mimetype, compress_type=zipfile.ZIP_STORED)
        z.writestr("stack.xml", "".join(xml), compress_type=zipfile.ZIP_DEFLATED)
        for i, (_, _, _, _, _, im, _) in enumerate(layers):
            if i != drop_src:
                z.writestr(f"data/l{i}.png", png_bytes(im), compress_type=zipfile.ZIP_DEFLATED)


def check_read(exe, tmp, rng, fails):
    checks = 0
    w, h = 24, 16
    full = Image.new("RGBA", (w, h))
    full.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256), 255) for _ in range(w * h)])
    crop = Image.new("RGBA", (6, 5))
    crop.putdata([(rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256))
                  for _ in range(30)])
    mid = Image.new("RGBA", (w, h))
    mid.putdata([(10, 20, 30, rng.randrange(256)) for _ in range(w * h)])
    top_first = [
        ("Ink &amp; wash", "0.5", "visible", 7, 9, crop, False),
        ("Caf&#233;", "1.000", "hidden", 0, 0, mid, True),
        ("Background", "1", "visible", 0, 0, full, False),
    ]
    path = os.path.join(tmp, "krita.ora")
    krita_ora(path, top_first, w, h)
    out = run(exe, "read", path)
    checks += 1
    if not out or out[0] != "load 0 -":
        fails.append(f"read: a Krita-shaped .ora is refused: {out[:1]}")
        return checks
    _, cw, ch, n = out[1].split()
    if (int(cw), int(ch), int(n)) != (w, h, 3):
        fails.append(f"read: size line {out[1]!r}")
        return checks
    want = list(reversed(top_first))   # bottom first
    for k in range(3):
        head = out[2 + 2 * k].split()
        px = [int(v, 16) for v in out[3 + 2 * k].split()]
        name, op, vis, x, y, im, _ = want[k]
        dec = bytes.fromhex(head[3].rstrip("-")).decode("latin-1")
        got_attrs = {}
        for item in head[5:]:
            key, _, val = item.partition("=")
            got_attrs[key] = bytes.fromhex(val.rstrip("-")).decode("latin-1")
        want_attrs = {"text": "two\nlines & more", "px": "40"} if k == 2 else {}
        if got_attrs != want_attrs:
            fails.append(f"read: layer {k}'s toyos attributes {got_attrs}, want {want_attrs}")
        wname = name.replace("&amp;", "&").replace("&#233;", "\xe9")
        if dec != wname:
            fails.append(f"read: layer {k} (bottom first) named {dec!r}, want {wname!r}")
        wop = round(float(op) * 255)
        if abs(int(head[1]) - wop) > 1 or int(head[2]) != (vis == "hidden"):
            fails.append(f"read: layer {k} opacity/hidden {head[1]}/{head[2]}, want {wop}/{vis}")
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(im, (x, y))
        wantpx = [a << 24 | r << 16 | g << 8 | b for r, g, b, a in rgba(canvas)]
        # A fully transparent pixel's colour is not the picture's.
        got = [v if v >> 24 else 0 for v in px]
        wantpx = [v if v >> 24 else 0 for v in wantpx]
        if got != wantpx:
            bad = next(i for i in range(w * h) if got[i] != wantpx[i])
            fails.append(f"read: layer {k}'s pixels differ first at {bad % w},{bad // w}: "
                         f"{got[bad]:08x}, want {wantpx[bad]:08x}")
    # Refusals.
    bad = os.path.join(tmp, "bad.ora")
    krita_ora(bad, top_first, w, h, mimetype=b"image/png")
    checks += 1
    if run(exe, "read", bad)[0].startswith("load 0"):
        fails.append("read: a wrong mimetype is accepted")
    krita_ora(bad, top_first, w, h, drop_src=1)
    checks += 1
    if run(exe, "read", bad)[0].startswith("load 0"):
        fails.append("read: a layer whose PNG is missing is accepted")
    # And our own file, read back by ours (the round trip Paint does).
    checks += 1
    rt = run(exe, "read", os.path.join(tmp, "w0.ora"))
    if not rt[0].startswith("load 0"):
        fails.append(f"read: its own file is refused: {rt[0]}")
    return checks


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="build with the stack left top first and '&' unescaped; the run must fail")
    ap.add_argument("--seed", type=int, default=2026)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, args.positive_control)
        checks = check_write(exe, tmp, rng, fails) + check_read(exe, tmp, rng, fails)
    if args.positive_control:
        if fails:
            print(f"ora_hostcheck: positive control FAILED as it must ({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("ora_hostcheck: positive control passed -- the harness cannot see the breaks", file=sys.stderr)
        return 1
    for f in fails[:20]:
        print("  FAIL  " + f)
    print(f"ora_hostcheck: {checks} checks, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
