// base/upixscale.h, sharp bilinear scaling: what each column and row
// must come out as, at a whole factor, at 1.25x, and for pictures whose
// answer needs no arithmetic to check.
#include <stdlib.h>
#include <string.h>
#include "base/upixscale.h"
#include "base/utest.h"

static int near(uint32_t got, uint32_t want, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)((got >> sh) & 0xff) - (int)((want >> sh) & 0xff);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

int main(void) {
    utest_begin("pixscale_test", "sharp bilinear picture scaling", 0);
    struct upixscale s;
    memset(&s, 0, sizeof s);

    // 2x2 -> 4x4: every source pixel becomes an exact 2x2 block.
    uint32_t q[4] = { 0xff112233, 0xff445566, 0xff778899, 0xffaabbcc };
    uint32_t big[16];
    int ok = upixscale_prepare(&s, 2, 2, 4, 4);
    upixscale_run(&s, q, 2, big, 4);
    int exact = ok;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (big[y * 4 + x] != q[(y / 2) * 2 + x / 2]) exact = 0;
    utest_check(exact, "a whole factor (2x) is exact pixel duplication, alpha included");

    // 1:1 is a copy.
    uint32_t same[4];
    upixscale_prepare(&s, 2, 2, 2, 2);
    upixscale_run(&s, q, 2, same, 2);
    utest_check(memcmp(same, q, sizeof q) == 0, "the same size is a copy");

    // 4 -> 5 (1.25x): the ends are source pixels untouched, the middle
    // column is an even mix of the two middle sources (enlarged 2x, a
    // sample half-way across their boundary), the others lean 9:1.
    uint32_t row[4] = { 0x000000ff, 0x0000ff00, 0x00ff0000, 0x00ffffff };
    uint32_t five[5];
    upixscale_prepare(&s, 4, 1, 5, 1);
    upixscale_run(&s, row, 4, five, 5);
    utest_checkf(five[0] == row[0] && five[4] == row[3],
                 "1.25x keeps the end columns exact (%08x, %08x)", five[0], five[4]);
    utest_checkf(near(five[2], 0x00807f00, 2), "1.25x: the middle column is a 50/50 mix (%08x)", five[2]);
    utest_checkf(near(five[1], 0x0000e619, 2) && near(five[3], 0x00ff1919, 2),
                 "1.25x: the others lean 9:1 to the nearer source (%08x, %08x)", five[1], five[3]);

    // A solid picture stays solid at any ratio: a weight that does not
    // sum to 256 would show here as a colour that drifts.
    int sw = 640, sh = 400, dw = 800, dh = 500;
    uint32_t *src = malloc((size_t)sw * sh * 4), *dst = malloc((size_t)dw * dh * 4);
    int solid = src && dst && upixscale_prepare(&s, sw, sh, dw, dh);
    if (solid) {
        for (int i = 0; i < sw * sh; i++) src[i] = 0xff3a5a9a;
        upixscale_run(&s, src, sw, dst, dw);
        for (int i = 0; i < dw * dh; i++)
            if (dst[i] != 0xff3a5a9a) { solid = 0; break; }
    }
    utest_check(solid, "a solid 640x400 picture is still solid at 800x500");

    // Smaller than the source is plain bilinear (factor 1), and in range.
    if (src && dst) {
        for (int i = 0; i < sw * sh; i++) src[i] = (i % sw) < sw / 2 ? 0xff000000 : 0xffffffff;
        upixscale_prepare(&s, sw, sh, 320, 200);
        upixscale_run(&s, src, sw, dst, 320);
        utest_checkf(dst[0] == 0xff000000 && dst[319] == 0xffffffff,
                     "shrinking keeps the far ends (%08x, %08x)", dst[0], dst[319]);
    }
    utest_check(!upixscale_prepare(&s, 0, 4, 4, 4), "a size of 0 is refused");
    free(src);
    free(dst);
    upixscale_free(&s);
    return utest_end();
}
