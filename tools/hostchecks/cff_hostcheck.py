#!/usr/bin/env python3
"""CFF outlines and font collections in ttf.c, against FreeType and the TrueType originals, on the HOST.

`userland/libs/media/ttf_cff.c` runs a glyph's Type 2 charstring into the
outline ttf.c fills; `ttf_open_index()` opens a member of a `.ttc`. This
compiles the tree's ttf.c, ttf_cff.c and ttf_hint.c with the host gcc --
under AddressSanitizer and UBSan -- and checks them against fonts
`tools/lib/cffgen.py` writes from the faces this repository ships: a
NAME-KEYED font from Liberation Sans, a CID-KEYED one (two font DICTs,
FDSelect format 3) from Liberation Serif, and a collection of the two.

  * each font's every glyph, unhinted at 12, 24 and 48 px, against
    FreeType's render of the SAME bytes: mean difference under 4/255 and
    fewer than 2% of pixels off by more than 48 -- the two rasterizers'
    edge coverage, as `emoji_hostcheck.py` bounds it;
  * each glyph against ttf.c's own render of the TrueType glyph it was
    converted from (its quadratics raised to cubics, exactly but for
    rounding to whole units): worst glyph's mean under 2/255;
  * every synthetic glyph -- one per Type 2 operator the converted ones
    never write (hhcurveto, vvcurveto, hv/vh chains, rcurveline,
    rlinecurve, the four flexes, 16.16 operands, cntrmask) -- drawn, with
    ink, and within the same bars of FreeType;
  * the blue zones the autohinter finds from CFF outlines equal those it
    finds from the glyf boxes, so a CFF face is hinted alike;
  * `ttf_face_count()` and `ttf_open_index()` on a font, a collection and
    garbage, and a member past the end refused;
  * HOSTILE INPUT: 1500 seeded mutations of each font -- bytes flipped,
    the CFF table's INDEX counts and offsets favoured -- opened and every
    glyph drawn, with ASan/UBSan aborting on any bad access;
  * when the host has Noto Sans CJK (`/usr/share/fonts/noto-cjk/`), a
    real CID-keyed collection too: a sample of glyphs from two members.
    Said as skipped when absent; the gate does not depend on it.

`--positive-control` breaks the reader six ways -- hvcurveto's alternation,
the subroutine bias, FDSelect's ranges, the collection's member offset,
the cubic flattening, and the width before rmoveto -- and, separately,
lets an INDEX item leave the table with rd_u8()'s own check gone (either
alone still keeps every read inside the file); each must go RED.

Needs gcc and libfreetype's headers (`pkg-config freetype2`); says SKIPPED
without them.
"""

# CFF (Type 2 charstrings, name- and CID-keyed) and .ttc collections in
# ttf.c, against FreeType and the TrueType originals, fuzzed under ASan.
# Host gcc + libfreetype. Carries a --positive-control that must go red.
RUNNER = {"name": "cff_host", "covers": "CFF outlines and .ttc members vs FreeType and the glyf originals; "
                                        "fuzzed under ASan", "gate": True, "parallel": True}

import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402
import cffgen  # noqa: E402
import otl  # noqa: E402

ROOT = toolpath.REPO
TOOL = "cff_hostcheck"
SANS = os.path.join(ROOT, "data", "fonts", "liberation-sans.ttf")
SERIF = os.path.join(ROOT, "data", "fonts", "liberation-serif.ttf")
TEXT = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789&@%?!ÅÄÖåäöéß"
SIZES = (12, 24, 48)
MEAN_MAX, OFF_MAX, ORIG_MAX = 4.0, 0.02, 2.0
FUZZ_ROUNDS = 1500
NOTO = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"

DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"
#include <ft2build.h>
#include FT_FREETYPE_H

#define W 160
#define H 160
#define BASE 120
static uint8_t a[W * H], b[W * H];
static struct ttf_scratch sc;

static uint8_t *slurp(const char *p, long *len) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*len);
    if (!d || fread(d, 1, (size_t)*len, f) != (size_t)*len) exit(2);
    fclose(f);
    return d;
}

static void diff(long *sum, long *off, long *n) {
    *sum = *off = *n = 0;
    for (int k = 0; k < W * H; k++) {
        if (!a[k] && !b[k]) continue;
        int d = abs((int)a[k] - (int)b[k]);
        *sum += d;
        *n += 1;
        if (d > 48) *off += 1;
    }
}

static int ft_render(FT_Face fa, int gid) {
    memset(b, 0, sizeof b);
    if (FT_Load_Glyph(fa, (FT_UInt)gid, FT_LOAD_NO_HINTING) ||
        FT_Render_Glyph(fa->glyph, FT_RENDER_MODE_NORMAL)) return 0;
    FT_Bitmap *bm = &fa->glyph->bitmap;
    for (unsigned y = 0; y < bm->rows; y++)
        for (unsigned x = 0; x < bm->width; x++) {
            int X = 16 + fa->glyph->bitmap_left + (int)x, Y = BASE - fa->glyph->bitmap_top + (int)y;
            if (X >= 0 && X < W && Y >= 0 && Y < H) b[Y * W + X] = bm->buffer[y * bm->pitch + x];
        }
    return 1;
}

// cmp FONT INDEX TTF PX LIST: per line "cp gid ttf_gid" of LIST, one line
// G cp mapped drew ink sumFT offFT nFT sumTT offTT nTT
static int cmp(char **v) {
    long l1, l2;
    uint8_t *d1 = slurp(v[0], &l1), *d2 = slurp(v[2], &l2);
    int idx = atoi(v[1]), px = atoi(v[3]);
    struct ttf_font c, t;
    if (!ttf_open_index(&c, d1, (uint32_t)l1, idx)) { puts("OPEN 0"); return 0; }
    if (!ttf_open(&t, d2, (uint32_t)l2)) return 2;
    ttf_set_hinting(&c, 0);
    ttf_set_hinting(&t, 0);
    FT_Library lib;
    FT_Face fa;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, v[0], idx, &fa) || FT_Set_Pixel_Sizes(fa, 0, (FT_UInt)px))
        return 3;
    FILE *g = fopen(v[4], "r");
    int cp, gid, tg;
    while (g && fscanf(g, "%d %d %d", &cp, &gid, &tg) == 3) {
        int mapped = ttf_glyph_index(&c, (uint32_t)cp) == gid;
        memset(a, 0, sizeof a);
        int drew = ttf_render_glyph(&c, gid, px, a, W, H, 16, BASE, &sc);
        int ink = 0;
        for (int k = 0; k < W * H; k++) ink += a[k] != 0;
        long s1 = -1, o1 = 0, n1 = 0, s2 = 0, o2 = 0, n2 = 0;
        if (ft_render(fa, gid)) diff(&s1, &o1, &n1);
        if (tg) {
            memset(b, 0, sizeof b);
            ttf_render_glyph(&t, tg, px, b, W, H, 16, BASE, &sc);
            diff(&s2, &o2, &n2);
        }
        printf("G %d %d %d %d %ld %ld %ld %ld %ld %ld\n", cp, mapped, drew, ink, s1, o1, n1, s2, o2, n2);
    }
    puts("OPEN 1");
    return 0;
}

// blues FONT TTF: each face's zones, "B ok ref shoot ref shoot ..."
static int blues(char **v) {
    long l;
    for (int i = 0; i < 2; i++) {
        uint8_t *d = slurp(v[i], &l);
        struct ttf_font f;
        if (!ttf_open(&f, d, (uint32_t)l)) { puts("B -1"); continue; }
        printf("B %d", f.blue_ok);
        for (int z = 0; z < TTF_BLUES; z++) printf(" %d %d", f.blue_ref[z], f.blue_shoot[z]);
        puts("");
    }
    return 0;
}

// count FILE: "C faces opened-0 .. opened-faces" (the last must be 0)
static int count(char **v) {
    long l;
    uint8_t *d = slurp(v[0], &l);
    int n = ttf_face_count(d, (uint32_t)l);
    printf("C %d", n);
    struct ttf_font f;
    for (int i = 0; i <= n; i++) printf(" %d", ttf_open_index(&f, d, (uint32_t)l, i));
    puts("");
    return 0;
}

// fuzz FILE ROUNDS SEED: "F opened drawn"
static uint32_t rng;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int fuzz(char **v) {
    long l;
    uint8_t *orig = slurp(v[0], &l);
    int rounds = atoi(v[1]);
    rng = (uint32_t)atoi(v[2]);
    uint8_t *d = malloc((size_t)l);
    long opened = 0, drawn = 0;
    struct ttf_font f;
    // Where the CFF table is: its INDEX counts and offsets are what the
    // mutations favour.
    uint32_t cff = 0, cff_len = (uint32_t)l;
    if (ttf_open(&f, orig, (uint32_t)l) && f.cff) { cff = f.cff; cff_len = f.cff_len; }
    for (int r = 0; r < rounds; r++) {
        memcpy(d, orig, (size_t)l);
        uint32_t len = (uint32_t)l;
        int flips = 1 + (int)(rnd() % 8);
        for (int k = 0; k < flips; k++) {
            uint32_t at = rnd() % 4 ? cff + rnd() % (cff_len ? cff_len : 1) : rnd() % (uint32_t)l;
            if (at >= len) continue;
            switch (rnd() % 4) {
            case 0: d[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: d[at] = (uint8_t)rnd(); break;
            case 2: d[at] = rnd() % 2 ? 0xFF : 0x00; break;
            default: d[at] = (uint8_t)(d[at] + (rnd() % 2 ? 1 : 0xFF)); break;
            }
        }
        if (rnd() % 16 == 0) len = rnd() % len;   // truncated
        // The copy is exactly `len` bytes, so ASan sees any read past it.
        uint8_t *exact = malloc(len ? len : 1);
        memcpy(exact, d, len);
        int faces = ttf_face_count(exact, len);
        for (int i = 0; i < (faces > 4 ? 4 : faces); i++) {
            if (!ttf_open_index(&f, exact, len, i)) continue;
            opened++;
            for (int gid = 0; gid < f.num_glyphs && gid < 160; gid++) {
                static uint8_t cov[64 * 64];
                int up, down;
                ttf_glyph_extent(&f, gid, 16, &up, &down, &sc);
                drawn += ttf_render_glyph(&f, gid, 16, cov, 64, 64, 8, 48, &sc);
            }
        }
        free(exact);
    }
    printf("F %ld %ld\n", opened, drawn);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "cmp") && argc == 7) return cmp(argv + 2);
    if (!strcmp(argv[1], "blues") && argc == 4) return blues(argv + 2);
    if (!strcmp(argv[1], "count") && argc == 3) return count(argv + 2);
    if (!strcmp(argv[1], "fuzz") && argc == 5) return fuzz(argv + 2);
    return 2;
}
"""

# (what, [(file, before, after), ...]): each must turn the check RED. The
# last breaks BOTH layers on one path -- rd_u8()'s own check and an INDEX
# item's bound -- since either alone still keeps every read in the file,
# and the fuzz must be seen to catch a read that leaves it.
CFF, TTF, INT = ("userland/libs/media/ttf_cff.c", "userland/libs/media/ttf.c",
                 "userland/libs/media/ttf_internal.h")
CONTROLS = [
    ("hvcurveto starts vertical", [(CFF, "            int vert = op == 30;", "            int vert = op == 31;")]),
    ("subroutine bias always 107", [(CFF, "    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;",
                                     "    return count ? 107 : 107;")]),
    ("FDSelect takes the next range's font DICT",
     [(CFF, "    return (uint32_t)gid < next ? (int)rd_u8(f, p + 3 + r * 3 + 2) : -1;",
       "    return (uint32_t)gid < next ? (int)rd_u8(f, p + 3 + r * 3 + 5) : -1;")]),
    ("every collection member is the first", [(TTF, "        dir = rd_u32(f, 12 + (uint32_t)index * 4);",
                                               "        dir = rd_u32(f, 12);")]),
    ("a cubic's control points swapped",
     [(TTF, "            add_cubic(sc, cx, cy, sc->px[i], sc->py[i], sc->px[b], sc->py[b], sc->px[e], sc->py[e]);",
       "            add_cubic(sc, cx, cy, sc->px[b], sc->py[b], sc->px[i], sc->py[i], sc->px[e], sc->py[e]);")]),
    ("no width before rmoveto", [(CFF, "            if (!have_width && n > 2) base = 1;", "            ;")]),
    ("an INDEX item may leave the table, and rd_u8 is unchecked",
     [(INT, "    if (off >= f->len) return 0;\n    return f->data[off];", "    return f->data[off];"),
      (CFF, "    if (a < 1 || b < a || b > cff_end(f) - ix->data) return 0;", "    if (a < 1 || b < a) return 0;")]),
]

SOURCES = (TTF, CFF, "userland/libs/media/ttf_hint.c")


def have_freetype():
    return bool(shutil.which("pkg-config")) and subprocess.run(
        ["pkg-config", "--exists", "freetype2"]).returncode == 0


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    def edits_for(rel):
        return [(before, after) for f, before, after in (control[1] if control else []) if f == rel]
    for h in ("kernel/include/api/ttf.h", "kernel/include/api/fixed.h", INT):
        edits = edits_for(h)
        hostcheck.stage(inc, h, edits=edits, apply=bool(edits), tool=TOOL)
    srcs = []
    for rel in SOURCES:
        edits = edits_for(rel)
        srcs.append(hostcheck.stage(tmp, rel, edits=edits, apply=bool(edits), tool=TOOL))
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    cflags = subprocess.run(["pkg-config", "--cflags", "freetype2"], capture_output=True, text=True).stdout.split()
    libs = subprocess.run(["pkg-config", "--libs", "freetype2"], capture_output=True, text=True).stdout.split()
    san = ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    return hostcheck.compile(tmp, "cff_check", [drv, *srcs, *libs],
                             flags=list(hostcheck.STRICT) + san + cflags, includes=[inc], tool=TOOL)


def fonts(tmp):
    """The fonts this check reads, written to `tmp`: [(label, path, index, ttf, list)]."""
    sans = cffgen.build(SANS, TEXT, cid=False, name=b"SaunaCheckSans")
    serif = cffgen.build(SERIF, TEXT, cid=True, name=b"SaunaCheckSerif")
    out = []
    paths = {}
    for key, font in (("sans", sans), ("serif", serif)):
        p = os.path.join(tmp, key + ".otf")
        with open(p, "wb") as fh:
            fh.write(font.otf)
        lst = os.path.join(tmp, key + ".list")
        with open(lst, "w") as fh:
            fh.writelines(f"{cp} {g} {tg}\n" for cp, g, tg in font.glyphs)
        paths[key] = (p, lst)
    ttc = os.path.join(tmp, "both.ttc")
    with open(ttc, "wb") as fh:
        fh.write(cffgen.collection([sans.otf, serif.otf]))
    out.append(("name-keyed .otf", paths["sans"][0], 0, SANS, paths["sans"][1]))
    out.append(("CID-keyed .otf", paths["serif"][0], 0, SERIF, paths["serif"][1]))
    out.append((".ttc member 0", ttc, 0, SANS, paths["sans"][1]))
    out.append((".ttc member 1", ttc, 1, SERIF, paths["serif"][1]))
    return out, dict(sans.synth), ttc, paths


def run_exe(exe, *args):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *map(str, args)], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout, r.stderr


def report(quiet, ok, text):
    if not quiet:
        print(f"  {'ok  ' if ok else 'FAIL'} {text}")
    return 0 if ok else 1


def run(exe, tmp, quiet=False):
    """Number of failures."""
    bad = 0
    cases, synth, ttc, paths = fonts(tmp)
    for label, path, idx, ttf, lst in cases:
        for px in SIZES:
            rc, out, err = run_exe(exe, "cmp", path, idx, ttf, px, lst)
            if rc != 0 or "OPEN 1" not in out:
                bad += report(quiet, False, f"{label} {px} px: the driver failed (rc {rc}) {err[-400:]}")
                continue
            rows = [ln.split() for ln in out.splitlines() if ln.startswith("G ")]
            # G cp mapped drew ink sumFT offFT nFT sumTT offTT nTT
            missing = [r for r in rows if r[2] != "1" or r[3] != "1" or r[4] == "0" or r[5] == "-1"]
            s = sum(int(r[5]) for r in rows if r[5] != "-1")
            o = sum(int(r[6]) for r in rows if r[5] != "-1")
            n = sum(int(r[7]) for r in rows if r[5] != "-1")
            mean, frac = s / max(1, n), o / max(1, n)
            orig = max((int(r[8]) / max(1, int(r[10])) for r in rows if int(r[10])), default=0)
            ok = not missing and mean < MEAN_MAX and frac < OFF_MAX and orig < ORIG_MAX
            names = ", ".join(synth.get(int(r[1]), f"U+{int(r[1]):04X}") for r in missing[:4])
            bad += report(quiet, ok, f"{label} {px} px: {len(rows) - len(missing)}/{len(rows)} glyphs mapped and "
                                     f"drawn{' (not: ' + names + ')' if missing else ''}; vs FreeType mean "
                                     f"{mean:.2f}/255, {frac:.2%} past 48; vs the glyf original worst {orig:.2f}")
            if px == SIZES[0] and idx == 0 and "name" in label:
                # The synthetic glyphs, by name, so a failure says which operator.
                for r in rows:
                    cp = int(r[1])
                    if cp not in synth:
                        continue
                    m = int(r[5]) / max(1, int(r[7])) if r[5] != "-1" else 99
                    gok = r[3] == "1" and r[4] != "0" and m < 3 * MEAN_MAX
                    bad += report(quiet, gok, f"  operator: {synth[cp]} ({r[4]} px of ink, mean {m:.2f})")
    rc, out, _ = run_exe(exe, "blues", paths["sans"][0], SANS)
    zones = [ln.split()[1:] for ln in out.splitlines() if ln.startswith("B ")]
    same = len(zones) == 2 and zones[0][0] == zones[1][0] and zones[0][0] != "0" and \
        all(abs(int(x) - int(y)) <= 1 for x, y in zip(zones[0][1:], zones[1][1:]))
    bad += report(quiet, same, f"the blue zones from CFF outlines equal glyf's: {zones}")
    want = {paths["sans"][0]: "C 1 1 0", ttc: "C 2 1 1 0", os.path.join(ROOT, "README.md"): "C 0 0"}
    for p, w in want.items():
        rc, out, _ = run_exe(exe, "count", p)
        got = out.strip()
        bad += report(quiet, got == w, f"faces in {os.path.basename(p)}: {got!r} (want {w!r}: count, then "
                                       f"each member opened, one past the end refused)")
    targets = (("name-keyed", paths["sans"][0]), ("CID-keyed", paths["serif"][0]), ("collection", ttc))
    with cf.ThreadPoolExecutor(max_workers=len(targets)) as ex:   # ~10 s each under ASan
        runs = list(ex.map(lambda t: run_exe(exe, "fuzz", t[1], FUZZ_ROUNDS, 7), targets))
    for (key, p), (rc, out, err) in zip(targets, runs):
        line = next((ln for ln in out.splitlines() if ln.startswith("F ")), "")
        ok = rc == 0 and line != ""
        detail = line[2:] if ok else (err.strip().splitlines() or ["(no output)"])[0][:200]
        bad += report(quiet, ok, f"{FUZZ_ROUNDS} mutations of the {key} font, every glyph drawn under ASan/UBSan: "
                                 f"{'opened, drawn ' + detail if ok else detail}")
    return bad


def real_font(exe, quiet):
    """Noto Sans CJK, when the host has it: not part of the gate's verdict."""
    if not os.path.exists(NOTO):
        if not quiet:
            print(f"  skip a real CID-keyed collection: {NOTO} is not on this host")
        return 0
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        lst = os.path.join(tmp, "cjk.list")
        cps = [0x4E00 + 97 * k for k in range(60)] + [0x3042 + k for k in range(20)] + list(range(0x41, 0x5B))
        for idx in (0, 3):
            f = otl.Font(NOTO, idx)
            with open(lst, "w") as fh:
                fh.writelines(f"{cp} {f.gid(cp)} 0\n" for cp in cps if f.gid(cp))
            rc, out, err = run_exe(exe, "cmp", NOTO, idx, SANS, 24, lst)
            rows = [ln.split() for ln in out.splitlines() if ln.startswith("G ")]
            s = sum(int(r[5]) for r in rows if r[5] != "-1")
            n = sum(int(r[7]) for r in rows if r[5] != "-1")
            o = sum(int(r[6]) for r in rows if r[5] != "-1")
            missing = [r for r in rows if r[2] != "1" or r[3] != "1" or r[4] == "0"]
            ok = rc == 0 and rows and not missing and s / max(1, n) < MEAN_MAX and o / max(1, n) < OFF_MAX
            bad += report(quiet, ok, f"Noto Sans CJK member {idx}, 24 px: {len(rows) - len(missing)}/{len(rows)} "
                                     f"drawn, vs FreeType mean {s / max(1, n):.2f}/255, {o / max(1, n):.2%} past 48")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the reader; each must go RED")
    args = ap.parse_args()
    if not have_freetype():
        print(f"{TOOL}: SKIPPED -- no libfreetype headers on this host")
        return 0
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                bad = run(build(tmp, control), tmp, quiet=True)
            print(f"  {'RED  ' if bad else 'green'} {control[0]}: {bad} check(s) failing")
            red.append(bad > 0)
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        bad = run(exe, tmp)
        bad += real_font(exe, False)
    print(f"{TOOL}: {'all clear' if not bad else f'{bad} FAILED'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
