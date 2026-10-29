#!/usr/bin/env python3
"""Generate the image data this repo ships: the test vectors and the wallpapers.

TWO OUTPUTS, ONE TOOL, because both are "a JPEG produced by libjpeg for
saunaos to read back" and splitting them would mean two scripts with the
same encoder settings to keep in step.

  --vectors     userland/tests/uimg_vectors.h -- small JPEGs and QOIs as
                byte arrays, each with the RGBA **Pillow** decodes them
                to. That is the whole point of the file: /tests/uimg_test
                compares userland/lib/uimg_*.c's output against a
                different implementation's, not against its own. A
                decoder tested only against itself is the AES-with-a-
                wrong-round-key problem docs/roadmap-details.md warns
                about -- self-consistent and worthless.

                THE QOI VECTORS ARE ENCODED BY PILLOW TOO, which matters
                more than it does for JPEG: QOI is simple enough that
                this repo could have written its own encoder, and then a
                misread chunk type would round-trip perfectly through
                the matching bug. A foreign encoder cannot do that.
                Their tolerance is 0 -- QOI is lossless, so "close
                enough" is not a thing that exists.

                BMP and GIF the same way, Pillow writing and reading,
                tolerance 0; and one ANIMATION, every frame as Pillow
                composites it, for the frame iterator. The breadth for
                both lives in tools/uimg_codec_hostcheck.py; these few
                are what proves the codecs in ring 3.

  --wallpapers  data/wallpapers/*.jpg and animated/rain.gif -- the backgrounds, drawn
                here rather than committed as somebody's photograph so
                the repo carries no image it does not own.

  --pictures    data/usr/share/pictures/ -- sample BMPs and GIFs, so a
                machine can open each variant the decoders read with
                no file brought from elsewhere: 24-bit, 8-bit palette,
                RLE8 and a 32-bit alpha BMP; a still GIF and two
                animated ones, one over transparency. The RLE8 and alpha
                BMPs are packed HERE, since Pillow writes neither.

(The app icons have their own generator, tools/gen_icons.py, because
what they need is drawing code rather than encoder settings.)

Both outputs are COMMITTED, like kernel/drivers/font_ttf.c and the
cursor themes: a checkout without Pillow still builds. Re-run this only
when the vectors or the wallpapers should actually change, and say so in
the commit message when you do.

Needs Pillow (see docs/tools.md's host-tools section).
"""
import argparse
import io
import math
import random
import os
import struct
import sys

try:
    from PIL import Image, ImageOps, ImageDraw, ImageFilter
except ImportError:
    sys.exit("gen_imgdata.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VECTORS_H = os.path.join(ROOT, "userland", "tests", "uimg_vectors.h")
WALLPAPER_DIR = os.path.join(ROOT, "data", "wallpapers")

# errno values from kernel/include/abi/errno.h, as POSITIVE numbers; the
# test compares them against a negative return.
EINVAL = 22
ENOTSUP = 95
# ENOTSUP (95) had a vector here until progressive JPEG stopped being a
# refusal; the remaining -ENOTSUP paths (arithmetic coding, 12-bit, CMYK)
# have no encoder here to produce a file with.


def gradient(w, h):
    """A smooth two-axis gradient with a hard edge in it.

    Both halves matter. The gradient exercises the DC path and the
    chroma resampler; the hard edge is what produces large AC
    coefficients, so an IDCT with a wrong constant shows up there and
    nowhere else -- a decoder that only ever sees flat colour passes
    with the entire butterfly deleted.
    """
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = 255 * x // max(1, w - 1)
            g = 255 * y // max(1, h - 1)
            b = 128 + 127 * int(math.sin(6.0 * x / max(1, w)) >= 0)
            if x > w // 2 and y > h // 2:
                r, g, b = 250, 12, 30       # the hard edge
            px[x, y] = (r, g, b)
    return im


def noise(w, h):
    """Deterministic per-pixel noise -- no two neighbours alike.

    For QOI this is the anti-run image: almost every pixel is a literal
    or an index hit, so the 64-entry hash table is actually exercised.
    """
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            s = (x * 1103515245 + y * 12345 + 7) & 0x7FFFFFFF
            px[x, y] = ((s >> 7) & 255, (s >> 15) & 255, (s >> 3) & 255)
    return im


def alpha_disc(w, h):
    """A soft-edged disc on transparency -- an icon's shape, in miniature.

    The point is the EDGE: alpha steps through every value between 0 and
    255 there, so a decoder that dropped the alpha channel, or one that
    read RGBA chunks as RGB, is wrong in a way an all-or-nothing mask
    would hide.
    """
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    cx, cy, r = (w - 1) / 2.0, (h - 1) / 2.0, min(w, h) * 0.42
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            a = 255 if d <= r - 1 else (0 if d >= r + 1 else int(255 * (r + 1 - d) / 2))
            if a:
                px[x, y] = (240, 90 + int(120 * x / w), 40, a)
    return im


def encode(im, **kw):
    buf = io.BytesIO()
    im.save(buf, format="JPEG", **kw)
    return buf.getvalue()


def c_bytes(name, data):
    out = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 16):
        row = ", ".join("0x%02x" % b for b in data[i:i + 16])
        out.append("    %s," % row)
    out.append("};")
    return "\n".join(out)


def build_vectors():
    vectors = []

    def add(name, data, tol, exif=False):
        """One decodable vector: the bytes, and what PILLOW decodes them to.

        `exif` turns the reference the way Pillow's own
        ImageOps.exif_transpose() does. The decoder applies the
        orientation tag (docs/decisions/gui.md), so a reference that did
        not would fail every rotated file and pass a decoder that
        ignored the tag -- the assertion backwards.
        """
        ref = Image.open(io.BytesIO(data))
        if exif:
            ref = ImageOps.exif_transpose(ref)
        ref = ref.convert("RGBA")
        vectors.append({
            "name": name, "jpeg": data, "w": ref.width, "h": ref.height,
            "rgb": ref.tobytes(), "err": 0, "tol": tol,
        })

    def ok(name, im, **kw):
        # JPEG: lossy, so the tolerance is the level two conforming IDCTs
        # are allowed to differ by. Alpha is still compared EXACTLY --
        # the decoder must fill 0xFF, and a vector that ignored alpha
        # would not notice if it filled 0.
        add(name, encode(im, **kw), 3)

    def qoi(name, im):
        buf = io.BytesIO()
        im.save(buf, format="QOI")
        add(name, buf.getvalue(), 0)   # lossless: exact, or it is wrong

    def bad(name, data, err):
        vectors.append({"name": name, "jpeg": data, "w": 0, "h": 0,
                        "rgb": None, "err": err, "tol": 0})

    ok("rgb 4:4:4 q95", gradient(16, 16), quality=95, subsampling=0)
    ok("rgb 4:2:2 q90", gradient(24, 16), quality=90, subsampling=1)
    ok("rgb 4:2:0 q80", gradient(32, 24), quality=80, subsampling=2)
    # 17x9 is deliberately not a multiple of an MCU in either axis: the
    # decoder pads its planes out to whole MCUs and must crop back, and
    # an off-by-one there produces a picture with a garbage right/bottom
    # edge that a square test image cannot show.
    ok("partial MCU 17x9", gradient(17, 9), quality=85, subsampling=2)
    ok("grayscale", gradient(16, 16).convert("L"), quality=90)
    # Restart markers reset the DC predictor mid-scan. A decoder that
    # ignores them decodes the first interval correctly and then drifts,
    # so this vector fails in bands rather than everywhere.
    ok("restart markers", gradient(32, 32), quality=85, subsampling=2,
       restart_marker_rows=1)

    # --- QOI, whose chunk types are what there is to get wrong --------
    #
    # Each image is shaped to force a different chunk: flat bands are
    # RUNs, a gradient is DIFF/LUMA, noise is literal RGB plus INDEX
    # hits off the 64-entry hash, and the alpha one is RGBA chunks. An
    # image that exercised only runs would pass with the hash function
    # wrong, which is this format's subtle failure.
    qoi("qoi flat runs", Image.new("RGB", (24, 16), (30, 90, 160)))
    qoi("qoi gradient", gradient(24, 16))
    qoi("qoi noise", noise(20, 12))
    qoi("qoi rgb, no alpha", gradient(16, 16).convert("RGB"))
    qoi("qoi with alpha", alpha_disc(24, 24))

    # PROGRESSIVE DECODES NOW. It is here as a vector rather than as a
    # refusal because it used to be the refusal, and a build that
    # regressed to one would otherwise look like a passing suite with a
    # missing feature. Noise rather than a gradient on purpose: a smooth
    # ramp has almost no AC coefficients, so the refinement scans this
    # is meant to cover carry nothing.
    ok("progressive 4:2:0", noise(32, 24), quality=85, subsampling=2,
       progressive=True)
    ok("progressive 4:4:4", noise(17, 9), quality=92, subsampling=0,
       progressive=True)

    # EXIF ORIENTATION 6 -- a quarter turn, so the DIMENSIONS swap. A
    # decoder ignoring the tag fails on the size before it gets to a
    # pixel, which is the loudest way for this to break.
    ex = Image.Exif()
    ex[0x0112] = 6
    add("exif orientation 6", encode(noise(24, 16), quality=92,
                                     exif=ex.tobytes()), 3, exif=True)
    # --- BMP and GIF -------------------------------------------------
    #
    # 13 wide on purpose: a BMP row pads to four bytes, and a width that
    # needs no padding cannot show a decoder that forgot it.
    def saved(im, fmt, **kw):
        buf = io.BytesIO()
        im.save(buf, format=fmt, **kw)
        return buf.getvalue()

    add("bmp 24-bit", saved(gradient(13, 7), "BMP"), 0)
    add("bmp 8-bit palette", saved(noise(13, 7).quantize(37), "BMP"), 0)
    add("bmp 1-bit", saved(gradient(13, 7).convert("1"), "BMP"), 0)
    add("gif 256 colours", saved(noise(20, 12).quantize(256), "GIF"), 0)
    add("gif interlaced", saved(gradient(24, 17).quantize(60), "GIF", interlace=True), 0)

    truncated = vectors[2]["jpeg"][:len(vectors[2]["jpeg"]) * 6 // 10]
    bad("truncated is refused", truncated, EINVAL)
    bad("not an image", b"this is not a JPEG, not even slightly\n", EINVAL)

    # A QOI truncated mid-chunk, and one whose header lies about its
    # channel count. Byte-level corruption rather than a short file,
    # because a decoder that trusts the header is the one that walks off
    # the end of the buffer.
    buf = io.BytesIO()
    alpha_disc(24, 24).save(buf, format="QOI")
    whole = buf.getvalue()
    bad("qoi truncated is refused", whole[:len(whole) * 6 // 10], EINVAL)
    bogus = bytearray(whole)
    bogus[12] = 7          # channels: legal values are 3 and 4
    bad("qoi with an impossible channel count", bytes(bogus), EINVAL)

    whole = saved(gradient(13, 7), "BMP")
    bad("bmp truncated is refused", whole[:len(whole) - 20], EINVAL)
    # A BMP that wraps a JPEG (BI_JPEG, a printer format): fine, refused.
    jpg = bytearray(whole[:54])
    jpg[30:34] = struct.pack("<I", 4)
    bad("bmp wrapping a JPEG is unsupported", bytes(jpg) + b"\xff\xd8\xff\xd9", ENOTSUP)
    gif = saved(noise(20, 12).quantize(256), "GIF")
    bad("gif truncated is refused", gif[:len(gif) // 2], EINVAL)

    return vectors


def build_anim_vectors():
    """One animation: three frames, disposals 2, 3 and 1 over a
    transparent index, and a delay of 0 that uimg_anim_next() must turn
    into 100 ms. The reference is each frame as PILLOW composites it,
    with its fully transparent pixels written as 0 -- Pillow keeps the
    palette's colour under alpha 0, the decoder writes nothing there."""
    pal = [255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9] + [0] * 756
    frames = []
    for k in range(3):
        f = Image.new("P", (12, 9))
        f.putpalette(pal)
        for y in range(9):
            for x in range(12):
                f.putpixel((x, y), 3 if (k == 1 and x < 5) else (x + y + k) % 3)
        frames.append(f)
    buf = io.BytesIO()
    frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                   duration=[50, 0, 90], loop=0, disposal=[2, 3, 1],
                   transparency=3, optimize=False)
    data = buf.getvalue()
    ref = Image.open(io.BytesIO(data))
    rgba = bytearray()
    for k in range(ref.n_frames):
        ref.seek(k)
        raw = ref.convert("RGBA").tobytes()
        for i in range(0, len(raw), 4):
            rgba += raw[i:i + 4] if raw[i + 3] else bytes(4)
    return [{"name": "gif three frames, disposals 2 3 1", "data": data,
             "w": ref.width, "h": ref.height, "frames": ref.n_frames, "loops": 0,
             "delays": [50, 100, 90], "rgba": bytes(rgba)}]


def write_vectors(vectors, anims):
    out = ['// GENERATED by tools/gen_imgdata.py -- do not edit by hand.',
           '//',
           '// Each vector is a file and the RGBA **Pillow** decodes it to --',
           '// libjpeg for the JPEGs, its own QOI plugin for the QOIs, and it',
           '// ENCODED the QOIs too. /tests/uimg_test decodes the same bytes',
           '// with userland/lib/uimg_*.c and compares, which is the only way',
           '// to tell a correct decoder from a self-consistent one.',
           '#ifndef UIMG_VECTORS_H',
           '#define UIMG_VECTORS_H',
           '',
           'struct uimg_vector {',
           '    const char *name;',
           '    const unsigned char *data;   // the FILE, whatever format it is',
           '    unsigned len;',
           '    int w, h;                  // expected size; 0 when err != 0',
           '    int err;                   // 0 = must decode; else the POSITIVE',
           '                               // errno the decoder must return',
           '    int tol;                   // largest per-channel difference to',
           '                               // accept: 3 for JPEG (two conforming',
           '                               // IDCTs may differ), 0 for QOI',
           '    const unsigned char *rgba; // w*h*4 reference pixels, or 0',
           '};',
           '']
    for i, v in enumerate(vectors):
        out.append(c_bytes("uimg_vec%d_file" % i, v["jpeg"]))
        if v["rgb"] is not None:
            out.append(c_bytes("uimg_vec%d_rgba" % i, v["rgb"]))
        out.append("")
    out.append("static const struct uimg_vector uimg_vectors[] = {")
    for i, v in enumerate(vectors):
        rgb = ("uimg_vec%d_rgba" % i) if v["rgb"] is not None else "0"
        out.append('    { "%s", uimg_vec%d_file, sizeof uimg_vec%d_file, '
                   '%d, %d, %d, %d, %s },' %
                   (v["name"], i, i, v["w"], v["h"], v["err"], v["tol"], rgb))
    out.append("};")
    out.append("")
    out.append("#define UIMG_VECTOR_COUNT "
               "((int)(sizeof uimg_vectors / sizeof uimg_vectors[0]))")
    out.append("")
    out += ['// An animation, stepped through uimg_anim_next(): every frame as',
            '// Pillow composites it, and each delay AFTER that call\'s clamp.',
            'struct uimg_anim_vector {',
            '    const char *name;',
            '    const unsigned char *data;',
            '    unsigned len;',
            '    int w, h, frames, loops;',
            '    const int *delays;',
            '    const unsigned char *rgba; // frames * w * h * 4',
            '};',
            '']
    for i, a in enumerate(anims):
        out.append(c_bytes("uimg_anim%d_file" % i, a["data"]))
        out.append(c_bytes("uimg_anim%d_rgba" % i, a["rgba"]))
        out.append("static const int uimg_anim%d_delays[] = { %s };"
                   % (i, ", ".join(str(d) for d in a["delays"])))
        out.append("")
    out.append("static const struct uimg_anim_vector uimg_anim_vectors[] = {")
    for i, a in enumerate(anims):
        out.append('    { "%s", uimg_anim%d_file, sizeof uimg_anim%d_file, %d, %d, %d, %d, '
                   'uimg_anim%d_delays, uimg_anim%d_rgba },'
                   % (a["name"], i, i, a["w"], a["h"], a["frames"], a["loops"], i, i))
    out.append("};")
    out.append("")
    out.append("#define UIMG_ANIM_VECTOR_COUNT "
               "((int)(sizeof uimg_anim_vectors / sizeof uimg_anim_vectors[0]))")
    out.append("")
    out.append("#endif")
    out.append("")
    with open(VECTORS_H, "w") as f:
        f.write("\n".join(out))
    total = sum(len(v["jpeg"]) for v in vectors)
    print("wrote %s: %d vectors, %d bytes of JPEG" %
          (os.path.relpath(VECTORS_H, ROOT), len(vectors), total))


# --- wallpapers -------------------------------------------------------
#
# 1280x720 is this project's default mode, so the common case needs no
# scaling at all. That was a real question rather than an obvious one --
# a half-size image would decode in a quarter of the time -- and it was
# settled by MEASURING instead: `imginfo -d` reports 50 ms for 960x540
# under TCG, so a full-size background costs the desktop about a tenth
# of a second at startup, once. At a different resolution uimg_scale()
# resamples it, which is what tools/hires_test.py's mode exercises.

def wallpaper_aurora(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(10 + 26 * t), int(22 + 60 * t), int(46 + 78 * t)))
    for i in range(5):
        phase = i * 0.7
        amp = h * (0.05 + 0.02 * i)
        pts = []
        for x in range(0, w + 8, 8):
            u = x / w
            y = h * (0.30 + 0.09 * i) + amp * math.sin(6.3 * u + phase)
            pts.append((x, y))
        pts += [(w, h), (0, h)]
        d.polygon(pts, fill=(int(14 + 8 * i), int(60 + 14 * i), int(96 + 10 * i)))
    return im.filter(ImageFilter.GaussianBlur(9))


def wallpaper_dusk(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(48 + 150 * (1 - t) ** 2), int(30 + 70 * (1 - t)),
                     int(60 + 40 * t)))
    d.ellipse([w * 0.62, h * 0.12, w * 0.62 + h * 0.30, h * 0.12 + h * 0.30],
              fill=(255, 214, 170))
    for i in range(4):
        base = h * (0.62 + 0.09 * i)
        pts = [(0, h), (0, base)]
        for x in range(0, w + 12, 12):
            u = x / w
            pts.append((x, base - h * 0.06 * math.sin(3.0 * u + i * 1.3) ** 2))
        pts.append((w, h))
        shade = 46 - 9 * i
        d.polygon(pts, fill=(shade, shade - 6, shade + 14))
    return im.filter(ImageFilter.GaussianBlur(2))


# THREE MORE, AND THE CONSTRAINT IS THE DESKTOP RATHER THAN THE PICTURE.
# Icons and their captions are drawn over these, so each keeps its
# detail LOW-FREQUENCY and its top-left quiet -- that is where the icon
# grid starts. Busy texture there costs legibility and JPEG bytes at
# once; `ugfx_draw_string_shadowed()` covers the rest.

def wallpaper_slate(w, h):
    """A cool neutral with a faint grid -- the quiet one, for reading."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(36 + 22 * t), int(41 + 26 * t), int(50 + 32 * t)))
    # A grid a couple of units above the ground it sits on: visible as
    # structure, never as lines to read past.
    step = h // 12
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=(52, 58, 70))
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=(52, 58, 70))
    # One soft diagonal light, bottom-right, away from the icon grid.
    glow = Image.new("RGB", (w, h), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([w * 0.55, h * 0.45, w * 1.25, h * 1.35], fill=(40, 46, 58))
    glow = glow.filter(ImageFilter.GaussianBlur(h // 6))
    return Image.blend(im, Image.blend(im, glow, 0.5), 0.6)


def wallpaper_ember(w, h):
    """Warm and dark: a low horizon glow under a near-black sky."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        # The warmth arrives late, so the top two thirds stay dark
        # enough for white icon captions.
        k = max(0.0, (t - 0.45) / 0.55) ** 2
        d.line([(0, y), (w, y)],
               fill=(int(18 + 150 * k), int(16 + 62 * k), int(20 + 30 * k)))
    for i in range(6):
        r = h * (0.18 + 0.13 * i)
        cx, cy = w * 0.5, h * 1.02
        d.ellipse([cx - r * 1.6, cy - r, cx + r * 1.6, cy + r],
                  outline=(70 + 14 * i, 34 + 8 * i, 26), width=3)
    return im.filter(ImageFilter.GaussianBlur(h // 55))


def wallpaper_tide(w, h):
    """Daylight teal: layered water, lighter than aurora."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(120 - 70 * t), int(178 - 74 * t), int(188 - 66 * t)))
    for i in range(6):
        base = h * (0.34 + 0.11 * i)
        amp = h * (0.030 + 0.008 * i)
        pts = [(0, h), (0, base)]
        for x in range(0, w + 10, 10):
            u = x / w
            pts.append((x, base + amp * math.sin(4.1 * u + i * 0.9)))
        pts.append((w, h))
        d.polygon(pts, fill=(int(96 - 11 * i), int(150 - 15 * i), int(166 - 13 * i)))
    return im.filter(ImageFilter.GaussianBlur(3))


# TWO SAUNAS, drawn at 1280x720 in pixels (their sizes are absolute):
# the island cabin at dusk -- the default -- and the room itself.

def _vgrad(w, h, top, bottom, ease=1.0):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = (y / (h - 1)) ** ease
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return im


def wallpaper_lakeside(w, h):
    """A sauna cabin on an island at dusk: its window lit, smoke from the
    chimney, a rowing boat at the jetty, spruce on the far shore."""
    im = _vgrad(w, h, (26, 30, 58), (196, 120, 92), 1.6)
    d = ImageDraw.Draw(im)
    horizon = int(h * 0.66)
    rnd = random.Random(11)

    def ridge(x):   # the far shore's hills, above the waterline
        u = x / w
        return h * (0.012 + 0.030 * (0.5 + 0.5 * math.sin(u * 4.1 + 0.6))
                    + 0.018 * (0.5 + 0.5 * math.sin(u * 9.3 + 2.0)))
    # The far shore as a MASK, so the water can reflect it.
    shore = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(shore)
    sd.polygon([(0, horizon)] + [(x, horizon - ridge(x) - h * 0.022) for x in range(0, w + 8, 8)]
               + [(w, horizon)], fill=255)
    x = -10
    while x <= w + 10:
        base = horizon - ridge(x) + 4
        if rnd.random() < 0.22:   # a birch: a round crown on a short trunk
            r = rnd.uniform(7, 12)
            sd.ellipse([x - r, base - 2.6 * r, x + r, base - 0.6 * r], fill=255)
            sd.rectangle([x - 1, base - r, x + 1, base], fill=255)
        else:                     # a spruce: five tiers, each wider than the one above
            th = h * rnd.uniform(0.045, 0.09)
            tw = th * rnd.uniform(0.28, 0.36)
            for t in range(5):
                y0 = base - th + th * t / 5 * 0.9
                half = tw * (0.25 + 0.75 * (t + 1) / 5)
                sd.polygon([(x, y0), (x - half, y0 + th * 0.32), (x + half, y0 + th * 0.32)], fill=255)
            sd.rectangle([x - 1, base - th * 0.15, x + 1, base], fill=255)
        x += rnd.randint(6, 13)
    for y in range(horizon, h):   # the water: the sky mirrored, darker
        t = (y - horizon) / (h - horizon)
        src = im.getpixel((w // 2, max(0, horizon - 1 - int((y - horizon) * 1.3))))
        d.line([(0, y), (w, y)], fill=tuple(int(c * (0.62 - 0.22 * t)) for c in src))
    refl = shore.crop((0, 0, w, horizon)).transpose(Image.FLIP_TOP_BOTTOM)
    refl = refl.resize((w, int(horizon * 0.8))).filter(ImageFilter.GaussianBlur(3))
    rmask = Image.new("L", (w, h), 0)
    rmask.paste(refl.point(lambda v: int(v * 0.55)), (0, horizon))
    im.paste(Image.new("RGB", (w, h), (24, 20, 32)), (0, 0), rmask)
    im.paste(Image.new("RGB", (w, h), (30, 26, 40)), (0, 0), shore)
    d = ImageDraw.Draw(im)

    # The island: an uneven bank, the cabin, the jetty and the boat.
    cx, cy = int(w * 0.70), horizon + int(h * 0.02)
    rnd2 = random.Random(23)
    bank = [(cx - 330 + 600 * i / 40,
             cy + 44 - 34 * math.sin(math.pi * i / 40) ** 0.6 - rnd2.uniform(-3, 3)) for i in range(41)]
    d.polygon(bank + [(cx + 270, cy + 52), (cx - 330, cy + 52)], fill=(22, 18, 26))
    bw, bh = 150, 80
    d.rectangle([cx - bw // 2, cy - bh, cx + bw // 2, cy + 30], fill=(20, 16, 22))
    d.polygon([(cx - bw // 2 - 14, cy - bh), (cx, cy - bh - 46), (cx + bw // 2 + 14, cy - bh)],
              fill=(20, 16, 22))
    d.rectangle([cx + 30, cy - bh - 40, cx + 44, cy - bh - 4], fill=(20, 16, 22))   # chimney
    d.rectangle([cx - 46, cy - 46, cx - 18, cy - 22], fill=(255, 180, 96))         # window
    d.rectangle([cx + 210, cy + 41, cx + 372, cy + 45], fill=(24, 19, 27))          # jetty
    d.line([(cx + 210, cy + 41), (cx + 372, cy + 41)], fill=(64, 46, 50), width=1)
    for px in (cx + 300, cx + 336, cx + 368):
        d.line([(px, cy + 45), (px, cy + 56)], fill=(24, 19, 27), width=3)
    bx, by, L = cx + 262, cy + 66, 54   # the boat: hull middle at the waterline, half-length
    hull = [(bx - L + 2 * L * i / 24, by - 12 - 9 * (2 * i / 24 - 1) ** 4) for i in range(25)]
    hull += [(bx - L * 0.86 + 2 * L * 0.86 * i / 24, by + 4 * math.sin(math.pi * i / 24))
             for i in range(24, -1, -1)]
    d.polygon(hull, fill=(20, 16, 24))
    d.line(hull[:25], fill=(92, 64, 62), width=2)                                    # lit gunwale
    d.line([(bx + 8, by - 13), (bx + 46, by - 27)], fill=(20, 16, 24), width=3)      # an oar
    d.line([(bx + L - 4, by - 18), (cx + 336, cy + 45)], fill=(70, 54, 58), width=1) # mooring rope
    for k in range(5):
        y = by + 8 + k * 5
        d.line([(bx - L * 0.8 + k * 6, y), (bx + L * 0.8 - k * 7, y)], fill=(40, 30, 40), width=2)

    # The window's reflection: a soft warm smear, wavering, fading with depth.
    glow = Image.new("L", (w, h), 0)
    gd = ImageDraw.Draw(glow)
    for k in range(14):
        y = cy + 56 + k * 6
        half, mid = 12 + 3 * math.sin(k * 1.9), cx - 32 + 4 * math.sin(k * 1.3)
        gd.rectangle([mid - half, y, mid + half, y + 3], fill=int(110 * (1 - k / 14)))
    im.paste(Image.new("RGB", (w, h), (200, 130, 76)), (0, 0), glow.filter(ImageFilter.GaussianBlur(3)))
    # The smoke leaves the chimney's TOP and widens as it drifts left.
    smoke = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(smoke)
    top_x, top_y = cx + 37, cy - bh - 42
    for k in range(26):
        t = k / 25
        r = 6 + 46 * t
        x = top_x - 160 * t ** 1.4 + 10 * math.sin(t * 7)
        y = top_y - 8 - 300 * t
        md.ellipse([x - r, y - r, x + r, y + r], fill=int(120 * (1 - t) ** 1.3))
    im.paste(Image.new("RGB", (w, h), (150, 142, 152)), (0, 0), smoke.filter(ImageFilter.GaussianBlur(h // 60)))
    d = ImageDraw.Draw(im)
    for sx, sy in ((0.55, 0.10), (0.83, 0.16), (0.92, 0.07)):
        d.ellipse([w * sx - 1.5, h * sy - 1.5, w * sx + 1.5, h * sy + 1.5], fill=(230, 230, 240))
    return im.filter(ImageFilter.GaussianBlur(1.0))


def _shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


def wallpaper_sauna(w, h):
    """The room, in one-point perspective: lauteet as boxes, the floor
    receding, the kiuas with its stones, a thermometer, steam, the light
    from a ceiling lamp out of frame. The top-left stays plain wall."""
    WOOD = (150, 104, 64)
    VX, VY = w * 0.58, h * 0.40          # the vanishing point
    WALL_Y = h * 0.80                    # where the back wall meets the floor

    def P(x, y, s):
        """(x, y) on the back wall's plane, brought forward by s (1 = the wall)."""
        return (VX + (x - VX) * s, VY + (y - VY) * s)

    im = _vgrad(w, h, _shade(WOOD, 0.62), _shade(WOOD, 0.92), 1.1)
    d = ImageDraw.Draw(im)
    board = h // 14
    for i, y in enumerate(range(0, int(WALL_Y), board)):   # the back wall's boards
        k = 1.0 + 0.05 * math.sin(i * 2.3)
        for yy in range(y, min(int(WALL_Y), y + board)):
            d.line([(0, yy), (w, yy)], fill=_shade(im.getpixel((w // 2, yy)), k))
        d.line([(0, y), (w, y)], fill=_shade(WOOD, 0.5), width=1)
    glow = Image.new("L", (w, h), 0)     # the ceiling light: a wash from the top
    gd = ImageDraw.Draw(glow)
    for y in range(h):
        gd.line([(0, y), (w, y)], fill=int(255 * max(0.0, 1 - y / (h * 0.75)) ** 1.6))
    gd.ellipse([w * 0.25, -h * 0.3, w * 1.05, h * 0.45], fill=255)
    im.paste(Image.new("RGB", (w, h), (255, 176, 100)), (0, 0),
             glow.filter(ImageFilter.GaussianBlur(h // 6)).point(lambda v: int(v * 0.32)))
    d = ImageDraw.Draw(im)

    d.polygon([P(-4 * w, WALL_Y, 1), P(5 * w, WALL_Y, 1), P(5 * w, WALL_Y, 1.6), P(-4 * w, WALL_Y, 1.6)],
              fill=_shade(WOOD, 0.55))   # the floor, its boards receding
    for x in range(-2 * w, 3 * w, 46):
        d.line([P(x, WALL_Y, 1), P(x, WALL_Y, 1.6)], fill=_shade(WOOD, 0.42), width=2)
    d.line([(0, WALL_Y), (w, WALL_Y)], fill=_shade(WOOD, 0.35), width=3)

    END = w * 0.70                       # where the benches end, on the wall's plane

    def bench(y_top, s0, s1, boards):
        """A bench box from off-screen left to END: its seat at y_top from
        depth s0 to s1, down to the floor -- top, front and right end face."""
        left = -4 * w
        d.polygon([P(END, y_top, s0), P(END, y_top, s1), P(END, WALL_Y, s1), P(END, WALL_Y, s0)],
                  fill=_shade(WOOD, 0.78))
        for k in range(boards):          # the seat's boards, gaps between them
            a, b = s0 + (s1 - s0) * k / boards, s0 + (s1 - s0) * (k + 1) / boards
            d.polygon([P(left, y_top, a), P(END, y_top, a), P(END, y_top, b), P(left, y_top, b)],
                      fill=_shade(WOOD, 1.02 + 0.10 * k / max(1, boards - 1)))
            d.line([P(left, y_top, b), P(END, y_top, b)], fill=_shade(WOOD, 0.45), width=2)
        fy0, fy1 = P(0, y_top, s1)[1], P(0, y_top + h * 0.022, s1)[1]
        fb, ex = P(0, WALL_Y, s1)[1], P(END, y_top, s1)[0]
        d.rectangle([0, fy0, ex, fy1], fill=_shade(WOOD, 1.30))   # the lit front edge board
        d.line([(0, fy0), (ex, fy0)], fill=_shade(WOOD, 1.55), width=2)
        rb = (fb - fy1) / 6
        for k in range(6):               # the riser, in the seat's shadow
            y0 = fy1 + rb * k
            d.rectangle([0, y0, ex, y0 + rb - 2], fill=_shade(WOOD, 0.66 - 0.02 * k))
            d.line([(0, y0 + rb - 2), (ex, y0 + rb - 2)], fill=_shade(WOOD, 0.44), width=2)
        sh = Image.new("L", (w, h), 0)
        ImageDraw.Draw(sh).rectangle([0, fy1, ex, fy1 + rb * 1.6], fill=255)
        im.paste(Image.new("RGB", (w, h), (30, 18, 10)), (0, 0),
                 sh.filter(ImageFilter.GaussianBlur(8)).point(lambda v: int(v * 0.45)))
        ImageDraw.Draw(im).line([(ex, fy0), (ex, fb)], fill=_shade(WOOD, 1.15), width=2)

    bench(h * 0.60, 1.0, 1.22, 4)        # the upper bench, against the wall
    bench(h * 0.70, 1.22, 1.45, 3)       # the step in front of it
    d = ImageDraw.Draw(im)

    # THE KIUAS: a black box, its left side face visible, a top plate,
    # angular stones packed over the top in a low heap.
    kx0, kx1, top_y = w * 0.76, w * 0.88, h * 0.47
    FL, FR = P(kx0, 0, 1.30)[0], P(kx1, 0, 1.30)[0]
    BL, BR = P(kx0, 0, 1.12)[0], P(kx1, 0, 1.12)[0]
    fy_top, fy_floor = P(0, top_y, 1.30)[1], P(0, WALL_Y, 1.30)[1]
    by_top, by_floor = P(0, top_y, 1.12)[1], P(0, WALL_Y, 1.12)[1]
    d.polygon([(BL, by_top), (FL, fy_top), (FL, fy_floor), (BL, by_floor)], fill=(24, 24, 28))
    d.rectangle([FL, fy_top, FR, fy_floor], fill=(34, 34, 38))
    d.line([(FL, fy_top), (FL, fy_floor)], fill=(70, 70, 76), width=2)
    d.polygon([(BL, by_top), (BR, by_top), (FR, fy_top), (FL, fy_top)], fill=(48, 48, 54))
    d.line([(BL, by_top), (FL, fy_top)], fill=(80, 80, 88), width=2)
    for k in range(10):                  # the side's perforations under the plate
        t = (k + 0.5) / 10
        x, y = BL + (FL - BL) * t, by_top + (fy_top - by_top) * t + 7
        d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(10, 10, 12))

    # The stones: faceted rocks in rows from back to front, every one
    # inside the top's outline, drawn at 4x and scaled down. Each is a
    # chunky polygon fanned into triangles round an off-centre peak, every
    # facet shaded by how squarely it faces the light (up-left). A dark
    # mound under them keeps the plate from showing between rocks.
    rnd = random.Random(5)
    heap = h * 0.035

    def span(t):                         # the top's left and right at depth t (0 back, 1 front)
        return BL + (FL - BL) * t, BR + (FR - BR) * t

    def rise(x, t):                      # the heap's height there; some even at the edges
        l, r_ = span(t)
        u = (x - (l + r_) / 2) / ((r_ - l) / 2)
        return heap * max(0.25, 1 - u * u) * (0.55 + 0.45 * (1 - t))
    mound = [(BL + (BR - BL) * k / 20, by_top - rise(BL + (BR - BL) * k / 20, 0.0) - 6) for k in range(21)]
    d.polygon(mound + [(FR, fy_top), (FL, fy_top)], fill=(30, 32, 36))
    SS = 4
    ox, oy = int(BL) - 40, int(by_top - heap) - 50
    lw, lh = int(FR - BL) + 80, int(fy_top - oy) + 30
    layer = Image.new("RGBA", (lw * SS, lh * SS), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    placed = []
    for row in range(4):
        t = row / 3
        l, r_ = span(t)
        y_plate = by_top + (fy_top - by_top) * t
        x = l + 2
        while True:
            r = rnd.uniform(16, 24)
            cx = x + r
            if cx + r > r_ - 2:
                break
            placed.append((y_plate - rise(cx, t) - r * 0.2 + rnd.uniform(-3, 3), cx, r, t))
            x += r * rnd.uniform(0.95, 1.2)
    # Nothing floats: under every rock, stones in shadow down to the plate,
    # clipped to the top's outline at that depth.
    for cy, cx, r, t in placed:
        l, r_ = span(t)
        y_plate = by_top + (fy_top - by_top) * t
        x0, x1 = max(l, cx - r * 0.9), min(r_, cx + r * 0.9)
        ld.rectangle([(x0 - ox) * SS, (cy - oy) * SS, (x1 - ox) * SS, (y_plate + 2 - oy) * SS],
                     fill=(30, 32, 36, 255))
    for cy, cx, r, t in sorted(placed):
        n = rnd.randint(5, 7)
        angs = sorted(rnd.uniform(0, 2 * math.pi) for _ in range(n))
        pts = [(cx + r * rnd.uniform(0.82, 1.0) * math.cos(a), cy + r * 0.7 * rnd.uniform(0.82, 1.0) * math.sin(a))
               for a in angs]
        peak = (cx - r * 0.18, cy - r * 0.22)
        v = rnd.uniform(-10, 10)
        base = (66 + v, 70 + v, 78 + v)  # blue-grey diabase
        for k in range(n):
            p1, p2 = pts[k], pts[(k + 1) % n]
            mx, my = (p1[0] + p2[0]) / 2 - peak[0], (p1[1] + p2[1]) / 2 - peak[1]
            lit = max(0.0, (mx * 0.55 + my * 0.83) / (math.hypot(mx, my) or 1))
            col = tuple(max(0, min(255, int(c * (0.55 + 1.05 * (1 - lit))))) for c in base) + (255,)
            ld.polygon([((q[0] - ox) * SS, (q[1] - oy) * SS) for q in (peak, p1, p2)], fill=col)
        ld.line([((q[0] - ox) * SS, (q[1] - oy) * SS) for q in pts + [pts[0]]], fill=(24, 26, 30, 255), width=SS)
    layer = layer.resize((lw, lh), Image.LANCZOS)
    im.paste(layer, (ox, oy), layer)
    d = ImageDraw.Draw(im)
    d.line([(FL, fy_top), (FR, fy_top)], fill=(90, 90, 98), width=3)   # the plate's front edge

    # The firebox door, flames behind its glass, and the ash drawer under it.
    dx0, dx1 = FL + 26, FR - 26
    dy0, dy1 = fy_floor - (fy_floor - fy_top) * 0.62, fy_floor - (fy_floor - fy_top) * 0.26
    d.rounded_rectangle([dx0, dy0, dx1, dy1], radius=6, fill=(26, 24, 28), outline=(84, 78, 78), width=3)
    gx0, gx1, gy0, gy1 = int(dx0 + 12), int(dx1 - 12), int(dy0 + 12), int(dy1 - 18)
    fire = Image.new("RGB", (gx1 - gx0, gy1 - gy0))
    fd = ImageDraw.Draw(fire)
    fw, fh = fire.size
    for y in range(fh):
        t = y / max(1, fh - 1)
        fd.line([(0, y), (fw, y)], fill=(int(90 + 140 * t), int(24 + 90 * t ** 1.5), int(8 + 20 * t)))
    rnd2 = random.Random(4)
    for k in range(5):
        fx = fw * (k + 0.5) / 5 + rnd2.uniform(-4, 4)
        tall = fh * rnd2.uniform(0.45, 0.85)
        fd.polygon([(fx - 8, fh), (fx + 8, fh), (fx + 2, fh - tall), (fx - 1, fh - tall * 0.9)],
                   fill=(255, int(170 + rnd2.uniform(0, 50)), 70))
    im.paste(fire.filter(ImageFilter.GaussianBlur(1.5)), (gx0, gy0))
    d = ImageDraw.Draw(im)
    d.rectangle([gx0, gy0, gx1, gy1], outline=(50, 46, 48), width=2)
    d.rectangle([dx1 - 36, dy1 - 12, dx1 - 10, dy1 - 7], fill=(110, 100, 96))   # the handle
    ay0, ay1 = dy1 + 10, fy_floor - 12
    d.rectangle([dx0, ay0, dx1, ay1], fill=(28, 28, 32), outline=(72, 70, 74), width=2)
    d.line([(dx0 + (dx1 - dx0) * 0.3, (ay0 + ay1) / 2), (dx0 + (dx1 - dx0) * 0.7, (ay0 + ay1) / 2)],
           fill=(110, 100, 96), width=4)
    ember = Image.new("L", (w, h), 0)
    ImageDraw.Draw(ember).ellipse([FL - 60, dy0, FR + 60, h + 80], fill=255)
    im.paste(Image.new("RGB", (w, h), (255, 120, 50)), (0, 0),
             ember.filter(ImageFilter.GaussianBlur(40)).point(lambda v: int(v * 0.20)))
    d = ImageDraw.Draw(im)

    # The kiulu and kauha on the upper bench, near its end.
    bx, by = P(w * 0.60, h * 0.60, 1.12)
    bw_top, bw_bot, bh = 40, 31, 44
    d.polygon([(bx - bw_top, by - bh), (bx + bw_top, by - bh), (bx + bw_bot, by), (bx - bw_bot, by)],
              fill=_shade(WOOD, 0.95))
    for k in range(-3, 4):               # its staves
        d.line([(bx + k * bw_top / 3.5, by - bh), (bx + k * bw_bot / 3.5, by)], fill=_shade(WOOD, 0.7), width=1)
    for f in (0.22, 0.78):               # two metal bands
        y = by - bh * (1 - f)
        half = bw_bot + (bw_top - bw_bot) * (1 - f)
        d.line([(bx - half, y), (bx + half, y)], fill=(70, 66, 64), width=3)
    d.ellipse([bx - bw_top, by - bh - 6, bx + bw_top, by - bh + 6], fill=_shade(WOOD, 0.55))
    d.line([(bx + 10, by - bh - 2), (bx + 64, by - bh - 62)], fill=_shade(WOOD, 0.55), width=6)

    # The thermometer, high on the right: a round wooden plaque, ~80 C.
    mx, my, r = w * 0.945, h * 0.20, 26
    d.ellipse([mx - 38, my - 38, mx + 38, my + 38], fill=_shade(WOOD, 1.25), outline=_shade(WOOD, 0.6), width=2)
    d.ellipse([mx - r - 3, my - r - 3, mx + r + 3, my + r + 3], fill=(70, 66, 64))
    d.ellipse([mx - r, my - r, mx + r, my + r], fill=(238, 232, 220))
    for k in range(13):                  # ticks over a 270-degree sweep, 0-120
        a = math.radians(135 + 270 * k / 12)
        r0 = r * (0.70 if k % 2 == 0 else 0.80)
        d.line([(mx + r0 * math.cos(a), my + r0 * math.sin(a)),
                (mx + r * 0.92 * math.cos(a), my + r * 0.92 * math.sin(a))], fill=(60, 56, 54), width=1)
    a = math.radians(135 + 270 * 80 / 120)
    d.line([(mx, my), (mx + r * 0.78 * math.cos(a), my + r * 0.78 * math.sin(a))], fill=(180, 40, 30), width=2)
    d.ellipse([mx - 2.5, my - 2.5, mx + 2.5, my + 2.5], fill=(60, 56, 54))

    # Steam off the stones, rising and drifting left.
    steam = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(steam)
    rnd = random.Random(9)
    for p in range(7):
        x0 = rnd.uniform(FL + 20, FR - 20)
        for k in range(18):
            t = k / 17
            r = 10 + 46 * t
            x = x0 - 120 * t ** 1.3 * (0.6 + 0.4 * math.sin(p)) + 14 * math.sin(t * 6 + p)
            y = fy_top - 30 - 300 * t
            sd.ellipse([x - r, y - r, x + r, y + r], fill=int(85 * (1 - t) ** 1.2))
    im.paste(Image.new("RGB", (w, h), (246, 238, 230)), (0, 0), steam.filter(ImageFilter.GaussianBlur(18)))
    return im.filter(ImageFilter.GaussianBlur(1.0))


def wallpaper_rain(w=320, h=180, n=16):
    """An ANIMATED picture -- a live wallpaper, listed beside the effects
    from /usr/share/wallpapers/animated (gui/wallpapers/players/gif.c). Low resolution and a loop
    of streaks over a night gradient: what a GIF wallpaper usually is, and
    small enough to ship. The loop is seamless because every streak falls
    a whole number of screen heights per cycle."""
    rnd = random.Random(7)
    drops = [(rnd.uniform(0, w), rnd.uniform(0, h), rnd.choice((1, 2)), rnd.uniform(5, 11))
             for _ in range(90)]
    frames = []
    for k in range(n):
        im = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(im)
        for y in range(h):
            t = y / (h - 1)
            d.line([(0, y), (w, y)], fill=(int(12 + 14 * t), int(20 + 30 * t), int(38 + 44 * t)))
        for x0, y0, laps, length in drops:
            y = (y0 + laps * h * k / n) % h
            x = x0 - 0.18 * y
            d.line([(x, y), (x - 0.18 * length, y - length)],
                   fill=(int(110 + 20 * laps), int(140 + 20 * laps), 190))
        frames.append(im.quantize(colors=32, dither=Image.Dither.NONE))
    return frames


def build_wallpapers():
    os.makedirs(WALLPAPER_DIR, exist_ok=True)
    for name, fn in (("aurora", wallpaper_aurora), ("dusk", wallpaper_dusk),
                     ("slate", wallpaper_slate), ("ember", wallpaper_ember),
                     ("tide", wallpaper_tide), ("lakeside", wallpaper_lakeside),
                     ("sauna", wallpaper_sauna)):
        im = fn(1280, 720)
        path = os.path.join(WALLPAPER_DIR, name + ".jpg")
        im.save(path, format="JPEG", quality=88, subsampling=2, optimize=True)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT),
                                       os.path.getsize(path)))
    frames = wallpaper_rain()
    os.makedirs(os.path.join(WALLPAPER_DIR, "animated"), exist_ok=True)
    path = os.path.join(WALLPAPER_DIR, "animated", "rain.gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=80, loop=0,
                   optimize=False)
    print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT), os.path.getsize(path)))


# --- sample pictures ---------------------------------------------------

PICTURE_DIR = os.path.join(ROOT, "data", "usr", "share", "pictures")


def bmp_bytes(w, h, bpp, comp, data, palette=b"", masks=b"", hs=40):
    """A BMP from parts; masks inside the header from V2 (hs >= 52) on."""
    info = struct.pack("<IiiHHIIiiII", hs, w, h, 1, bpp, comp, len(data), 2835, 2835,
                       len(palette) // 4, 0)
    if hs > 40:
        info = (info + masks.ljust(16, b"\0")[:hs - 40]).ljust(hs, b"\0")
    off = 14 + len(info) + len(palette)
    return b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off) + info + palette + data


def rle8(im):
    """An 8-bit palette image as BI_RLE8: encoded runs, an end of line per
    row, an end of bitmap -- bottom row first, as the format is."""
    w, h = im.size
    px = im.load()
    out = bytearray()
    for y in range(h - 1, -1, -1):
        x = 0
        while x < w:
            n = 1
            while x + n < w and n < 255 and px[x + n, y] == px[x, y]:
                n += 1
            out += bytes((n, px[x, y]))
            x += n
        out += b"\0\0"
    out[-2:] = b"\0\1"
    return bytes(out)


def palette_bytes(im):
    pal = im.getpalette()[:768]
    return b"".join(bytes((pal[i + 2], pal[i + 1], pal[i], 0)) for i in range(0, len(pal), 3))


def picture_orbit(n=24, size=200):
    """Twelve dots round a ring, a bright head chasing round: opaque."""
    frames = []
    for k in range(n):
        im = Image.new("RGB", (size, size), (22, 26, 34))
        d = ImageDraw.Draw(im)
        c = size / 2
        for i in range(12):
            a = 2 * math.pi * i / 12
            age = (k * 12 // n - i) % 12
            t = 1 - age / 12
            r = 6 + 9 * t
            x, y = c + 70 * math.cos(a), c + 70 * math.sin(a)
            col = (int(40 + 200 * t), int(90 + 120 * t), int(200 + 40 * t))
            d.ellipse([x - r, y - r, x + r, y + r], fill=col)
        frames.append(im.quantize(64))
    return frames


def picture_bounce(n=20, size=160):
    """A ball bouncing over TRANSPARENCY, each frame cleared after it
    (disposal 2) -- the stage's own tint shows round the ball."""
    frames = []
    for k in range(n):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        # Height |sin| of a half turn per bounce: the floor at k = 0, the
        # top half way, squashed for the one frame it touches.
        y = 20 + (size - 70) * (1 - abs(math.sin(math.pi * k / n)))
        squash = 1.25 if k == 0 else 1.0
        rx, ry = 22 * squash, 22 / squash
        cx = size / 2
        d.ellipse([cx - rx, y - ry + 22, cx + rx, y + ry + 22], fill=(240, 120, 40, 255))
        d.ellipse([cx - rx / 2.5 - 6, y + 22 - ry / 2 - 4, cx - 2, y + 22 - 2], fill=(255, 210, 160, 255))
        frames.append(im)
    return frames


def build_pictures():
    os.makedirs(PICTURE_DIR, exist_ok=True)
    scene = wallpaper_dusk(320, 180)
    out = {}

    buf = io.BytesIO()
    scene.save(buf, format="BMP")
    out["dusk-24bit.bmp"] = buf.getvalue()

    pal = scene.quantize(256)
    buf = io.BytesIO()
    pal.save(buf, format="BMP")
    out["dusk-256-colours.bmp"] = buf.getvalue()

    flat = wallpaper_slate(320, 180).quantize(16)
    out["slate-rle8.bmp"] = bmp_bytes(320, 180, 8, 1, rle8(flat), palette_bytes(flat))

    # 32-bit with an alpha mask in a V5 header: a soft badge on nothing.
    badge = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    bd = ImageDraw.Draw(badge)
    for r in range(60, 0, -1):
        a = 255 if r < 54 else int(255 * (60 - r) / 6)
        bd.ellipse([64 - r, 64 - r, 64 + r, 64 + r],
                   fill=(int(40 + 3 * r), int(110 + r), 220, a))
    rows = bytearray()
    px = badge.load()
    for y in range(127, -1, -1):
        for x in range(128):
            r_, g_, b_, a_ = px[x, y]
            rows += bytes((b_, g_, r_, a_))
    masks = struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    out["badge-alpha.bmp"] = bmp_bytes(128, 128, 32, 3, bytes(rows), masks=masks, hs=124)

    buf = io.BytesIO()
    sw = Image.new("RGB", (256, 128))
    sd = ImageDraw.Draw(sw)
    for i in range(128):
        hue = i / 128
        r_ = int(255 * max(0, min(1, abs(hue * 6 - 3) - 1)))
        g_ = int(255 * max(0, min(1, 2 - abs(hue * 6 - 2))))
        b_ = int(255 * max(0, min(1, 2 - abs(hue * 6 - 4))))
        sd.rectangle([(i % 16) * 16, (i // 16) * 16, (i % 16) * 16 + 15, (i // 16) * 16 + 15],
                     fill=(r_, g_, b_))
    sw.quantize(128).save(buf, format="GIF")
    out["swatches.gif"] = buf.getvalue()

    fr = picture_orbit()
    buf = io.BytesIO()
    fr[0].save(buf, format="GIF", save_all=True, append_images=fr[1:], duration=60, loop=0)
    out["orbit-animated.gif"] = buf.getvalue()

    fr = picture_bounce()
    buf = io.BytesIO()
    fr[0].save(buf, format="GIF", save_all=True, append_images=fr[1:], duration=50, loop=0,
               disposal=2, optimize=False)
    out["bounce-animated.gif"] = buf.getvalue()

    for name, data in out.items():
        with open(os.path.join(PICTURE_DIR, name), "wb") as f:
            f.write(data)
        print("wrote %s: %d bytes" % (os.path.relpath(os.path.join(PICTURE_DIR, name), ROOT),
                                      len(data)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vectors", action="store_true")
    ap.add_argument("--wallpapers", action="store_true")
    ap.add_argument("--pictures", action="store_true")
    args = ap.parse_args()
    if not args.vectors and not args.wallpapers and not args.pictures:
        args.vectors = args.wallpapers = args.pictures = True
    if args.vectors:
        write_vectors(build_vectors(), build_anim_vectors())
    if args.wallpapers:
        build_wallpapers()
    if args.pictures:
        build_pictures()


if __name__ == "__main__":
    main()
