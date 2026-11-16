// The TrueType rasterizer, in RING 3, over the font on the real disk.
//
// userland/libs/media/ttf.c (with its autohinter, ttf_hint.c) is checked
// on the host against FreeType by tools/hostchecks/ttf_hint_hostcheck.py;
// this is the other half -- the same source as LINKED into a ring-3
// program on this machine, reading the font file from its disk, gives the
// answers a program draws with.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "ttf.h"

#define FONT_PATH "/usr/share/fonts/dejavu-sans-mono.ttf"
#define FONT_CAP (1024u * 1024u)

#include "base/utest.h"

int main(void) {
    utest_begin("ttf_test", "the ring-3 build of ttf.c over the shipped face", 0);

    int64_t fd = sys_open(FONT_PATH, 0);
    if (fd < 0) {
        // Not a failure: an image built without data/fonts/ legitimately
        // has no font here, and a test that goes red for that would
        // train someone to ignore it. Same rule as the KTESTs.
        return utest_skip("no " FONT_PATH " on this image");
    }
    uint8_t *buf = (uint8_t *)malloc(FONT_CAP);
    if (!buf) { utest_check(0, "memory for the font"); return utest_end(); }

    uint32_t len = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, buf + len, FONT_CAP - len);
        if (n <= 0) break;
        len += (uint32_t)n;
        if (len >= FONT_CAP) break;
    }
    sys_close((int)fd);
    utest_notef("read %u bytes", (unsigned)len);

    struct ttf_font f;
    utest_check(ttf_open(&f, buf, len), "ttf_open() accepts the shipped face");
    if (utest_failed()) return utest_end();
    utest_notef("upem=%d glyphs=%d ascent=%d descent=%d",
           f.units_per_em, f.num_glyphs, f.ascent, f.descent);

    int gid_h = ttf_glyph_index(&f, 'H');
    utest_check(gid_h > 0, "cmap maps 'H' to a real glyph");
    utest_check(ttf_glyph_index(&f, 0x10FFFF) == 0, "an unmapped codepoint is .notdef");
    char fam[64];
    utest_checkf(ttf_family(&f, fam, sizeof fam) > 0 && !strcmp(fam, "DejaVu Sans Mono"),
                 "the name table gives the family (\"%s\")", fam);
    utest_check(ttf_family(&f, fam, 4) == 3 && !strcmp(fam, "Dej"), "a short buffer is cut, terminated");
    utest_check(ttf_style(&f) == 0, "the regular face is neither bold nor italic");
    int wa, wd;
    ttf_win_metrics(&f, &wa, &wd);
    utest_checkf(wa > 0 && wd > 0 && wa + wd >= f.ascent + f.descent - 1, "OS/2's Windows cell (%d, %d)", wa, wd);
    int ym, yn, adv;
    utest_check(!ttf_vdmx(&f, 13, &ym, &yn) && !ttf_hdmx(&f, 13, gid_h, &adv),
                "a font with no VDMX or hdmx says so (the caller then scales)");
    utest_check(ttf_advance_px(&f, gid_h, 16) > 0, "'H' has a positive advance at 16px");

    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    utest_check(sc != 0, "the scratch struct allocates in ring 3");
    if (!sc) return utest_end();

    const int w = 16, h = 20, baseline = 15;
    uint8_t *cov = (uint8_t *)malloc((uint32_t)(w * h));
    utest_check(cov != 0, "a coverage buffer allocates");
    if (!cov) return utest_end();
    for (int i = 0; i < w * h; i++) cov[i] = 0;

    utest_check(ttf_render_glyph(&f, gid_h, 16, cov, w, h, 0, baseline, sc),
          "ttf_render_glyph() succeeds in ring 3");

    int ink = 0, partial = 0, below = 0;
    for (int i = 0; i < w * h; i++) {
        if (cov[i]) ink++;
        if (cov[i] > 0 && cov[i] < 255) partial++;
    }
    for (int y = baseline + 1; y < h; y++)
        for (int x = 0; x < w; x++) if (cov[y * w + x]) below++;

    utest_check(ink > 20, "the glyph has ink");
    utest_check(partial > 0, "the glyph is anti-aliased, not a 1-bit mask");
    utest_check(below == 0, "a capital puts nothing below the baseline");

    // Hinted (the default), 'H' at 16 px stands on whole rows: its top row
    // is as dark as the rows under it. ttf_glyph_extent() names that row.
    int up = 0, down = 0, top_row = -1;
    for (int y = 0; y < h && top_row < 0; y++)
        for (int x = 0; x < w; x++) if (cov[y * w + x]) { top_row = y; break; }
    int top_max = 0, mid_max = 0;
    for (int x = 0; top_row >= 0 && x < w; x++) {
        if (cov[top_row * w + x] > top_max) top_max = cov[top_row * w + x];
        if (cov[(top_row + 3) * w + x] > mid_max) mid_max = cov[(top_row + 3) * w + x];
    }
    utest_checkf(top_row >= 0 && top_max * 10 >= mid_max * 9,
                 "hinted, 'H' starts on a whole row (top %d, below it %d)", top_max, mid_max);
    int ext_ok = ttf_glyph_extent(&f, gid_h, 16, &up, &down, sc);
    utest_checkf(ext_ok && baseline - up == top_row && down == 0,
                 "ttf_glyph_extent() names that row (%d up, %d down)", up, down);

    // Unhinted it is the raw outline, which differs somewhere.
    uint8_t *raw = (uint8_t *)calloc((uint32_t)(w * h), 1);
    ttf_set_hinting(&f, 0);
    utest_check(raw && ttf_render_glyph(&f, gid_h, 16, raw, w, h, 0, baseline, sc) &&
                memcmp(raw, cov, (size_t)(w * h)) != 0,
                "ttf_set_hinting(f, 0) draws the outline as it is");

    return utest_end();
}
