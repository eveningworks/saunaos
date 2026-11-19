#!/usr/bin/env python3
"""GPOS pair kerning and GSUB ligatures in ttf.c, against the font's own data, on the HOST.

`ttf_pair_kern_units()` reads kerning from GPOS PairPos lookups, and
`ttf_ligate()` applies GSUB ligature lookups (userland/libs/media/ttf.c).
This compiles the tree's ttf.c with the host gcc and checks both against
ORACLES IN THE SAME FILES that the C never reads for the same answer:

  kerning     the Liberation faces carry their kerning TWICE, in GPOS and
              in the old format-0 `kern` table, with the same values. Every
              pair of Latin, Greek and Cyrillic glyphs (and every pair the
              `kern` table lists) must kern through GPOS exactly as `kern`
              says -- read here in Python by tools/lib/otl.py.
  ligatures   Twemoji names each sequence glyph after its code points
              (`u1f468_200d_1f469_200d_1f467`, in `post`). Every
              fully-qualified sequence in tools/gen/data/emoji-test.txt that
              has such a glyph must ligate to exactly it -- and the Python
              reader gen_emojidata.py uses must agree with the C.
  HarfBuzz    when the host has its headers: every same-script pair in
              Noto Sans (class-pair, format 2, lookups) and Liberation Sans
              must kern as HarfBuzz shapes it. Without them, SKIPPED alone.

`--positive-control` reads a class-pair kern with its classes swapped and,
separately, matches a ligature's components one position early; each must
go RED.
"""

# GPOS PairPos kerning vs the kern table; GSUB ligatures vs Twemoji's glyph
# names. Host gcc only. Carries a --positive-control that must go red.
RUNNER = {"name": "otl_host", "covers": "GPOS pair kerning vs the kern table; GSUB emoji ligatures vs glyph names",
          "gate": True, "parallel": True}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402
import otl  # noqa: E402

ROOT = toolpath.REPO
TOOL = "otl_hostcheck"
FONTS = os.path.join(ROOT, "data", "fonts")
KERN_FACES = ("liberation-sans.ttf", "liberation-sans-bold.ttf", "liberation-serif.ttf",
              "liberation-serif-italic.ttf")
EMOJI_FACE = os.path.join(FONTS, "fallback", "twemoji.ttf")
EMOJI_TEST = os.path.join(ROOT, "tools", "gen", "data", "emoji-test.txt")
MIN_SEQUENCES = 2000   # Twemoji 16 names ~2,270 of emoji-test.txt's sequences

# Script codes as ttf.h's enum ttf_script orders them.
LATIN, GREEK, CYRILLIC = 0, 1, 2
CHARS = ([(c, LATIN) for c in list(range(0x21, 0x7F)) + list(range(0xC0, 0x100))]
         + [(c, GREEK) for c in range(0x386, 0x3CF)]
         + [(c, CYRILLIC) for c in range(0x400, 0x460)])

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) return 2;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)len);
    if (fread(d, 1, (size_t)len, fp) != (size_t)len) return 2;
    fclose(fp);
    struct ttf_font f;
    if (!ttf_open(&f, d, (uint32_t)len)) return 3;
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        if (!strcmp(argv[1], "kern")) {
            int s, l, r;
            if (sscanf(line, "%d %d %d", &s, &l, &r) == 3)
                printf("%d\n", ttf_pair_kern_units(&f, (enum ttf_script)s, l, r));
        } else {
            uint16_t g[64];
            int n = 0;
            for (char *p = strtok(line, " \n"); p && n < 64; p = strtok(NULL, " \n")) g[n++] = (uint16_t)atoi(p);
            n = ttf_ligate(&f, TTF_SCRIPT_OTHER, TTF_FEATURE('c', 'c', 'm', 'p'), g, n);
            for (int i = 0; i < n; i++) printf("%s%d", i ? " " : "", g[i]);
            printf("\n");
        }
    }
    return 0;
}
"""

# HarfBuzz's own pair kerning, when the host has it: the oracle for class-pair
# (format 2) lookups, which the Liberation faces do not use and Noto Sans does.
HB_DRIVER = r"""
#include <stdio.h>
#include <hb.h>

int main(int argc, char **argv) {
    hb_blob_t *blob = hb_blob_create_from_file(argv[1]);
    hb_face_t *face = hb_face_create(blob, 0);
    hb_font_t *font = hb_font_create(face);
    unsigned upem = hb_face_get_upem(face);
    hb_font_set_scale(font, (int)upem, (int)upem);
    hb_feature_t off[4];
    const char *names[4] = { "-liga", "-clig", "-calt", "-dlig" };
    for (int i = 0; i < 4; i++) hb_feature_from_string(names[i], -1, &off[i]);
    unsigned l, r;
    while (scanf("%x %x", &l, &r) == 2) {
        hb_buffer_t *b = hb_buffer_create();
        hb_codepoint_t cps[2] = { l, r };
        hb_buffer_add_codepoints(b, cps, 2, 0, 2);
        hb_buffer_guess_segment_properties(b);
        hb_shape(font, b, off, 4);
        unsigned n;
        hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(b, &n);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(b, &n);
        if (n != 2) printf("x\n");
        // What the pair moves the second glyph by: HarfBuzz splits an old
        // `kern` table's value over both glyphs, an advance AND an offset.
        else printf("%d\n", gp[0].x_advance - hb_font_get_glyph_h_advance(font, gi[0].codepoint)
                            + gp[1].x_offset);
        hb_buffer_destroy(b);
    }
    return 0;
}
"""
HB_FACES = ("fallback/noto-sans.ttf", "liberation-sans.ttf")

CONTROLS = [
    ("a class pair read with its classes swapped",
     "        *units = vr_xadvance(f, vf1, st + 16 + (c1 * n2 + c2) * (s1 + s2));",
     "        *units = vr_xadvance(f, vf1, st + 16 + (c2 * n2 + c1) * (s1 + s2));"),
    ("ligature components matched one position early",
     "        while (j < comp && rd_u16(f, lg + 4 + (j - 1) * 2) == g[i + (int)j]) j++;",
     "        while (j < comp && rd_u16(f, lg + 4 + (j - 1) * 2) == g[i + (int)j - 1]) j++;"),
]


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    for h in ("kernel/include/api/ttf.h", "kernel/include/api/fixed.h", "userland/libs/media/ttf_internal.h"):
        hostcheck.stage(inc, h, tool=TOOL)
    edits = [(control[1], control[2])] if control else []
    ttf = hostcheck.stage(tmp, "userland/libs/media/ttf.c", edits=edits, apply=bool(edits), tool=TOOL)
    hint = hostcheck.stage(tmp, "userland/libs/media/ttf_hint.c", tool=TOOL)
    cff = hostcheck.stage(tmp, "userland/libs/media/ttf_cff.c", tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "otl_check", [drv, ttf, hint, cff], includes=[inc], tool=TOOL)


def build_hb(tmp):
    """The HarfBuzz oracle, or None when the host has no harfbuzz headers."""
    if subprocess.run(["pkg-config", "--exists", "harfbuzz"]).returncode != 0:
        return None
    cflags = subprocess.run(["pkg-config", "--cflags", "harfbuzz"], capture_output=True, text=True).stdout.split()
    libs = subprocess.run(["pkg-config", "--libs", "harfbuzz"], capture_output=True, text=True).stdout.split()
    drv = hostcheck.write(tmp, "hb_driver.c", HB_DRIVER)
    return hostcheck.compile(tmp, "hb_kern", [drv, *libs], flags=["-O2"] + cflags, tool=TOOL)


def check_hb(exe, hb, quiet):
    """C's kerning against HarfBuzz's, Latin/Greek/Cyrillic pairs."""
    if not hb:
        if not quiet:
            print("  SKIP no harfbuzz headers on this host: class-pair kerning unchecked")
        return 0
    bad = 0
    for name in HB_FACES:
        path = os.path.join(FONTS, name)
        f = otl.Font(path)
        chars = [(c, s) for c, s in CHARS if f.gid(c)]
        pairs = [(c1, s1, c2, s2) for c1, s1 in chars for c2, s2 in chars if s1 == s2]
        ref = subprocess.run([hb, path], input="\n".join(f"{a:x} {b:x}" for a, _, b, _ in pairs) + "\n",
                             capture_output=True, text=True).stdout.splitlines()
        cases = [(p, int(v)) for p, v in zip(pairs, ref) if v != "x"]
        got = drive(exe, "kern", path, [f"{s1} {f.gid(c1)} {f.gid(c2)}" for (c1, s1, c2, _), _ in cases])
        wrong = [(f"{chr(p[0])}{chr(p[2])}", int(g), v) for (p, v), g in zip(cases, got) if int(g) != v]
        kerned = sum(1 for _, v in cases if v)
        ok = not wrong and kerned >= 500
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {name} vs HarfBuzz: {len(cases)} pairs, {kerned} kerned, "
                  f"{len(wrong)} differ" + (f" -- first {wrong[:4]}" if wrong else ""))
    return bad


def drive(exe, mode, face, lines):
    r = subprocess.run([exe, mode, face], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{TOOL}: driver failed on {face} ({r.returncode}): {r.stderr}")
    return r.stdout.splitlines()


def check_kern(exe, quiet):
    bad = 0
    for name in KERN_FACES:
        path = os.path.join(FONTS, name)
        f = otl.Font(path)
        table = f.kern_pairs()
        glyphs = [(f.gid(c), s) for c, s in CHARS if f.gid(c)]
        cases = {(LATIN if s1 == s2 == LATIN else s2, l, r) for l, s1 in glyphs for r, s2 in glyphs}
        cases |= {(LATIN, l, r) for l, r in table}
        cases = sorted(cases)
        got = drive(exe, "kern", path, [f"{s} {l} {r}" for s, l, r in cases])
        wrong = [(c, int(g), table.get(c[1:], 0)) for c, g in zip(cases, got) if int(g) != table.get(c[1:], 0)]
        kerned = sum(1 for c in cases if table.get(c[1:], 0))
        ok = not wrong and len(got) == len(cases) and kerned >= 500
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {name}: {len(cases)} pairs through GPOS, {kerned} kerned, "
                  f"{len(wrong)} differ from the kern table" + (f" -- first {wrong[:3]}" if wrong else ""))
    return bad


def sequences():
    out = []
    for raw in open(EMOJI_TEST, encoding="utf-8"):
        if raw.startswith("#") or ";" not in raw:
            continue
        cps, status = raw.split(";")[0].split(), raw.split(";")[1].split("#")[0].strip()
        # A character and U+FE0F is a selector the text path handles, not a ligature.
        if status == "fully-qualified" and len([c for c in cps if c != "FE0F"]) >= 2:
            out.append([int(c, 16) for c in cps])
    return out


def check_ligatures(exe, quiet):
    f = otl.Font(EMOJI_FACE)
    by_name = {f.glyph_name(g): g for g in range(f.num_glyphs())}
    cases = []
    for seq in sequences():
        want = by_name.get("u" + "_".join(f"{c:x}" for c in seq)) \
            or by_name.get("u" + "_".join(f"{c:x}" for c in seq if c != 0xFE0F))
        gids = [f.gid(c) for c in seq]
        if want and all(gids):
            cases.append((seq, gids, want))
    got = drive(exe, "lig", EMOJI_FACE, [" ".join(map(str, g)) for _, g, _ in cases])
    wrong = [(seq, g) for (seq, _, want), g in zip(cases, got) if g != str(want)]
    py_wrong = [seq for seq, gids, want in cases if f.ligate(list(gids), "ccmp") != [want]]
    ok = not wrong and len(got) == len(cases) and len(cases) >= MIN_SEQUENCES
    ok_py = not py_wrong
    if not quiet:
        print(f"  {'ok  ' if ok else 'FAIL'} twemoji: {len(cases)} named sequences, {len(wrong)} not ligated "
              f"to their own glyph" + (f" -- first {wrong[:2]}" if wrong else ""))
        print(f"  {'ok  ' if ok_py else 'FAIL'} tools/lib/otl.py agrees on every one ({len(py_wrong)} differ)")
    return (not ok) + (not ok_py)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the readers; must go RED")
    args = ap.parse_args()
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                exe = build(tmp, control)
                bad = check_kern(exe, True) + check_ligatures(exe, True) + check_hb(exe, build_hb(tmp), True)
            print(f"  {'RED  ' if bad else 'green'} {control[0]}: {bad} check(s) failing")
            red.append(bad > 0)
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        bad = check_kern(exe, False) + check_ligatures(exe, False) + check_hb(exe, build_hb(tmp), False)
    print(f"{TOOL}: {'PASS' if not bad else f'FAIL ({bad})'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
