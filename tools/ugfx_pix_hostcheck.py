#!/usr/bin/env python3
"""Check ugfx_pix's SIMD tiers BYTE FOR BYTE against its C table, on the host.

WHAT IS UNDER TEST. userland/ui/ugfx_pix_sse2.c (and any later tier): the
pixel loops under the glass and the blends -- box blur passes, the 2x
down/up-sample, mix (rounded) and blend (truncated) -- whose contract
(ugfx_pix.h) is IDENTICAL output to userland/ui/ugfx_pix.c, the
reference, so a tier changes speed and never a pixel. "Within one" is a
failure: a +-1 that differs between tiers shows as a seam where glass
from one meets glass from the other, and as a blur cache that never
matches itself after a switch.

What it asserts, compiling the REAL files with the host gcc:

  - mix and blend EXHAUSTIVELY: every (under, over, alpha) channel
    triple through mix_const and mix_rows, every (colour, dest, alpha)
    through blend_const and blend_cov -- the /255 replacements are where
    an off-by-one lives, and 2^24 cases is a fraction of a second
  - box_h and box_v on random pictures of every small size, odd and
    even, at radii 1..140 (past 128 the SIMD tier hands the call to C),
    random top bytes, unaligned starts
  - down2 and up2 at every small width, fy in {0, 64, 192}
  - glass with random marked/unmarked pixels, its backdrop aliasing the
    destination as ugfx_blit_glass passes it
  - ugfx_blur_rect END TO END (ugfx_blur.c compiled in) with each tier
    selected through ugfx_simd_set(): random rects, clips and radii,
    whole and half resolution

--positive-control drops the carry from the SIMD box average and the +1
from its /255, and requires both the box ops and the mix/blend ops to go
RED. --bench prints each op's time per pixel, C against SIMD.

    python3 tools/ugfx_pix_hostcheck.py
    python3 tools/ugfx_pix_hostcheck.py --positive-control   # must FAIL
    python3 tools/ugfx_pix_hostcheck.py --bench

Needs gcc and the standard library.
"""

import argparse
import os
import subprocess
import sys
import tempfile

import hostcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui/ugfx.h"
#include "ui/ugfx_pix.h"

void ugfx_mark_dirty_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    (void)s; (void)x; (void)y; (void)w; (void)h;
}

static const struct ugfx_pix_ops *T[] = { &ugfx_pix_sse2 };
#define NT (int)(sizeof T / sizeof T[0])
static unsigned g_fail[16];
static const char *g_name[16];
static int g_nop;

static int op(const char *name) {
    for (int i = 0; i < g_nop; i++) if (!strcmp(g_name[i], name)) return i;
    g_name[g_nop] = name;
    return g_nop++;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static void fill(uint32_t *p, int n) { for (int i = 0; i < n; i++) p[i] = rnd(); }

// Compares n words; reports the first difference of an op once per case.
static int same(const char *name, const struct ugfx_pix_ops *t, const uint32_t *ref,
                const uint32_t *got, int n, const char *what) {
    for (int i = 0; i < n; i++) {
        if (ref[i] == got[i]) continue;
        int k = op(name);
        if (g_fail[k]++ < 3)
            printf("FAIL %s %s %s: at %d want %08x got %08x\n", t->name, name, what, i,
                   ref[i], got[i]);
        return 0;
    }
    return 1;
}

#define MAXN 4096
static uint32_t A[MAXN + 8], B[MAXN + 8], R[MAXN * 64], G[MAXN * 64], S[4 * MAXN];
static uint32_t P[MAXN * 64];

static void check_mix(void) {
    // Channel r = u, g = 255 - u, b = u ^ 0x5A under; every over and alpha.
    uint32_t under[256], overrow[256];
    for (int u = 0; u < 256; u++) under[u] = rnd() << 24 | (uint32_t)u << 16 | (uint32_t)(255 - u) << 8 | (u ^ 0x5A);
    for (int t = 0; t < NT; t++)
        for (unsigned a = 0; a < 256; a++)
            for (unsigned o = 0; o < 256; o++) {
                uint32_t over = rnd() << 24 | o << 16 | (o ^ 0xA5) << 8 | (255 - o);
                char what[32];
                snprintf(what, sizeof what, "a=%u o=%u", a, o);
                ugfx_pix_c.mix_const(R, under, over, a, 256);
                T[t]->mix_const(G, under, over, a, 256);
                same("mix_const", T[t], R, G, 256, what);
                for (int i = 0; i < 256; i++) overrow[i] = over ^ (uint32_t)i << 8;
                ugfx_pix_c.mix_rows(R, under, overrow, a, 256);
                T[t]->mix_rows(G, under, overrow, a, 256);
                same("mix_rows", T[t], R, G, 256, what);
            }
}

static void check_blend(void) {
    uint32_t dst[256];
    uint8_t cov[256];
    for (int t = 0; t < NT; t++)
        for (unsigned a = 1; a < 255; a++)
            for (unsigned c = 0; c < 256; c++) {
                uint32_t col = rnd() << 24 | c << 16 | (c ^ 0x3C) << 8 | (255 - c);
                for (int i = 0; i < 256; i++) dst[i] = rnd() << 24 | (uint32_t)i << 16 | (uint32_t)(255 - i) << 8 | (i ^ 0x77);
                char what[32];
                snprintf(what, sizeof what, "a=%u c=%u", a, c);
                memcpy(R, dst, sizeof dst); memcpy(G, dst, sizeof dst);
                ugfx_pix_c.blend_const(R, col, a, 256);
                T[t]->blend_const(G, col, a, 256);
                same("blend_const", T[t], R, G, 256, what);
                // Coverage: this alpha, with 0 and 255 sprinkled in.
                for (int i = 0; i < 256; i++) cov[i] = (rnd() & 7) == 0 ? 0 : (rnd() & 7) == 0 ? 255 : (uint8_t)a;
                memcpy(R, dst, sizeof dst); memcpy(G, dst, sizeof dst);
                ugfx_pix_c.blend_cov(R, col, cov, 256);
                T[t]->blend_cov(G, col, cov, 256);
                same("blend_cov", T[t], R, G, 256, what);
            }
}

static void check_box(void) {
    char what[64];
    for (int t = 0; t < NT; t++) {
        for (int it = 0; it < 6000; it++) {
            int n = it < 3000 ? 1 + it % 70 : 1 + (int)(rnd() % 1500);
            int r = 1 + (int)(rnd() % (it % 5 == 0 ? 140 : 40));
            int off = (int)(rnd() % 4);
            fill(A, n + off);
            snprintf(what, sizeof what, "n=%d r=%d off=%d", n, r, off);
            ugfx_pix_c.box_h(A + off, R, n, r);
            T[t]->box_h(A + off, G, n, r);
            same("box_h", T[t], R, G, n, what);
        }
        for (int it = 0; it < 1500; it++) {
            int n = 1 + (int)(rnd() % 37), m = 1 + (int)(rnd() % 40);
            int r = 1 + (int)(rnd() % (it % 7 == 0 ? 140 : 30));
            fill(P, n * m);
            snprintf(what, sizeof what, "n=%d m=%d r=%d", n, m, r);
            ugfx_pix_c.box_v(P, R, n, m, r, S);
            T[t]->box_v(P, G, n, m, r, S);
            same("box_v", T[t], R, G, n * m, what);
        }
        for (int n = 1; n <= 300; n++)
            for (int rep = 0; rep < 8; rep++) {
                fill(A, n); fill(B, n);
                int dn = (n + 1) / 2;
                snprintf(what, sizeof what, "n=%d", n);
                ugfx_pix_c.down2(A, B, R, n);
                T[t]->down2(A, B, G, n);
                same("down2", T[t], R, G, dn, what);
                static const unsigned FY[] = { 0, 64, 192 };
                for (int f = 0; f < 3; f++) {
                    snprintf(what, sizeof what, "n=%d fy=%u", n, FY[f]);
                    ugfx_pix_c.up2(A, B, R, n, dn, FY[f]);
                    T[t]->up2(A, B, G, n, dn, FY[f]);
                    same("up2", T[t], R, G, n, what);
                }
            }
    }
}

static void check_glass(void) {
    char what[48];
    for (int t = 0; t < NT; t++)
        for (int it = 0; it < 4000; it++) {
            int n = 1 + (int)(rnd() % 90);
            unsigned a = rnd() % 255;
            fill(A, n); fill(B, n);
            for (int i = 0; i < n; i++) if (rnd() & 1) A[i] &= 0xFFFFFF;
            snprintf(what, sizeof what, "n=%d a=%u", n, a);
            ugfx_pix_c.glass(R, A, B, a, n);
            T[t]->glass(G, A, B, a, n);
            same("glass", T[t], R, G, n, what);
            // The backdrop IS the destination (ugfx_blit_glass with NULL).
            memcpy(R, B, (size_t)n * 4); memcpy(G, B, (size_t)n * 4);
            ugfx_pix_c.glass(R, A, R, a, n);
            T[t]->glass(G, A, G, a, n);
            same("glass", T[t], R, G, n, "aliased");
        }
}

static void check_blur(void) {
    enum { W = 300, H = 200 };
    static uint32_t px[W * H], ref[W * H], got[W * H];
    char what[96];
    for (int t = 0; t < NT; t++)
        for (int it = 0; it < 400; it++) {
            fill(px, W * H);
            struct ugfx_surface s;
            memset(&s, 0, sizeof s);
            s.pixels = px; s.w = W; s.h = H;
            int x = (int)(rnd() % W) - 20, y = (int)(rnd() % H) - 20;
            int w = 1 + (int)(rnd() % W), h = 1 + (int)(rnd() % (it % 4 ? 60 : H));
            int radius = 1 + (int)(rnd() % (it % 6 ? 24 : 70));
            if (it % 3 == 0) {   // the fields ugfx_set_clip_rect() would set
                s.clip_x0 = (int)(rnd() % 100); s.clip_y0 = (int)(rnd() % 80);
                s.clip_x1 = s.clip_x0 + 150; s.clip_y1 = s.clip_y0 + 100; s.clip_active = 1;
            }
            snprintf(what, sizeof what, "rect %d,%d %dx%d radius %d clip %d", x, y, w, h, radius, s.clip_active);
            memset(ref, 0, sizeof ref); memset(got, 0, sizeof got);
            ugfx_simd_set("off");
            ugfx_blur_rect(&s, x, y, w, h, radius, ref);
            ugfx_simd_set(T[t]->name);
            ugfx_blur_rect(&s, x, y, w, h, radius, got);
            same("blur_rect", T[t], ref, got, W * H, what);
        }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bench(void) {
    enum { W = 1920, H = 48 };
    static uint32_t px[W * 1080], out[W * 1080];
    fill(px, W * 1080);
    fill(A, MAXN); fill(B, MAXN);
    struct ugfx_surface s;
    memset(&s, 0, sizeof s);
    s.pixels = px; s.w = W; s.h = 1080;
    const struct ugfx_pix_ops *tiers[] = { &ugfx_pix_c, &ugfx_pix_sse2 };
    for (int t = 0; t < 2; t++) {
        const struct ugfx_pix_ops *p = tiers[t];
        ugfx_simd_set(p->name);
        double t0 = now();
        for (int i = 0; i < 50; i++) ugfx_blur_rect(&s, 0, 1032, W, H, 16, out);
        double blur = (now() - t0) / 50 / (W * H) * 1e9;
        t0 = now();
        for (int i = 0; i < 4; i++) ugfx_blur_rect(&s, 0, 0, W, 1080, 48, out);
        double wall = (now() - t0) / 4 / (W * 1080.0) * 1e9;
        t0 = now();
        for (int i = 0; i < 2000; i++) p->mix_const(R, A, 0x203040, 191, W);
        double mixc = (now() - t0) / 2000 / W * 1e9;
        t0 = now();
        for (int i = 0; i < 2000; i++) p->blend_const(R, 0x203040, 100, W);
        double bl = (now() - t0) / 2000 / W * 1e9;
        printf("BENCH %-5s blur(taskbar r16) %.2f  blur(screen r48) %.2f  mix %.3f  blend %.3f ns/px\n",
               p->name, blur, wall, mixc, bl);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
    op("mix_const"); op("mix_rows"); op("blend_const"); op("blend_cov"); op("box_h");
    op("box_v"); op("down2"); op("up2"); op("glass"); op("blur_rect");
    check_mix(); check_blend(); check_box(); check_glass(); check_blur();
    for (int i = 0; i < g_nop; i++) printf("%s %s %u\n", g_fail[i] ? "RED" : "ok", g_name[i], g_fail[i]);
    return 0;
}
"""

# The control: no carry out of the low half in the box average, and the
# /255 without its +1 -- each a +-1 somewhere, which is the whole point.
CONTROL = [
    ("return _mm_add_epi16(_mm_mulhi_epu16(s, vinv), _mm_srli_epi16(_mm_mullo_epi16(s, vinv), 15));",
     "return _mm_mulhi_epu16(s, vinv);"),
    ("_mm_add_epi16(_mm_add_epi16(y, _mm_set1_epi16(1)),",
     "_mm_add_epi16(_mm_add_epi16(y, _mm_set1_epi16(0)),"),
]
BOX_OPS = {"box_h", "box_v", "blur_rect"}
MIX_OPS = {"mix_const", "mix_rows", "blend_const", "blend_cov", "glass"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break the SIMD rounding; the check must go RED")
    ap.add_argument("--bench", action="store_true", help="time each tier per pixel")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        simd = hostcheck.stage(tmp, "userland/ui/ugfx_pix_sse2.c", edits=CONTROL,
                               apply=args.positive_control, tool="ugfx_pix_hostcheck")
        srcs = [drv, simd, os.path.join(ROOT, "userland/ui/ugfx_pix.c"),
                os.path.join(ROOT, "userland/ui/ugfx_blur.c")]
        flags = list(hostcheck.STRICT) + [
            "-I" + os.path.join(ROOT, "userland"),
            # AFTER the system's: kernel/include/api has a string.h of its own.
            "-idirafter", os.path.join(ROOT, "kernel", "include", "api"),
            "-idirafter", os.path.join(ROOT, "kernel", "include", "abi")]
        exe = hostcheck.compile(tmp, "ugfx_pix_host", srcs, flags=flags, tool="ugfx_pix_hostcheck")
        if args.bench:
            print(subprocess.run([exe, "bench"], capture_output=True, text=True, check=True).stdout, end="")
            return 0
        out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
    red = set()
    for line in out.splitlines():
        if line.startswith("FAIL") and not args.positive_control:
            print(line)
        word = line.split()
        if word and word[0] in ("ok", "RED"):
            if word[0] == "RED":
                red.add(word[1])
            if not args.positive_control:
                print(f"  {word[0]:3} {word[1]:12} {word[2]} differing cases")
    if args.positive_control:
        if red & BOX_OPS and red & MIX_OPS:
            print(f"positive control ok: went RED on {', '.join(sorted(red))}")
            return 0
        print("positive control FAILED: the broken rounding passed "
              f"(red: {', '.join(sorted(red)) or 'none'})")
        return 1
    if red:
        print(f"ugfx_pix_hostcheck: FAIL -- the SIMD tier differs from C in {', '.join(sorted(red))}")
        return 1
    print("ugfx_pix_hostcheck: PASS -- every SIMD op byte-identical to C")
    return 0


if __name__ == "__main__":
    sys.exit(main())
