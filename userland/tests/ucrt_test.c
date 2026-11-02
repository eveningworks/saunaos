// The CRT effect's scanlines and its read-only source, as pixels.
//
// SCANLINES PER SOURCE LINE (`src_lines`): a flat grey rect through
// scanlines alone must darken exactly one pixel row per source line, and
// that row must be the LAST of its line -- a pitch that drifts against
// the picture's own rows is what the field exists to stop. A line four
// or more rows tall shades the row above at half strength as well.
//
// ucrt_glass_point() is ucrt_source_point() backwards: a point placed on
// the glass (w16's SetCursorPos) must read back as the point asked for,
// on every curve -- wherever it says the point shows, and it must show
// everything but the corners the round glass cuts off.
//
// ucrt_apply_from() must give the same pixels as ucrt_apply() run in
// place, and must not write its source: DOOM hands it a private buffer
// precisely so the surface is never read, and a pass that scribbled on
// the source would still look right on screen.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ucrt.h"
#include "lib/utest.h"

#define GREY 0x808080u

static const struct ucrt_look SCAN_ONLY = { UCRT_LEVEL_MAX, 0, 0, UCRT_CURVE_OFF, UCRT_MASK_OFF, 0, 0 };

// Applies scanlines to a flat w x h rect of `lines` source lines and
// classifies each row: 0 untouched, 1 half-dark, 2 dark.
static int rows_of(int w, int h, int lines, int *cls) {
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    for (int i = 0; i < w * h; i++) px[i] = GREY;
    struct ugfx_surface s = { .pixels = px, .w = w, .h = h };
    struct ucrt c;
    ucrt_init(&c);
    c.look = SCAN_ONLY;
    c.src_lines = lines;
    int rc = ucrt_apply(&c, &s, 0, 0, w, h);
    // The darkest row is a full scanline; anything between it and the
    // untouched grey is a half one.
    int full = 0x80;
    for (int r = 0; r < h; r++) {
        int v = (int)(px[r * w + w / 2] & 0xFF);
        if (v < full) full = v;
    }
    for (int r = 0; r < h; r++) {
        int v = (int)(px[r * w + w / 2] & 0xFF);
        cls[r] = v == 0x80 ? 0 : v == full ? 2 : 1;
    }
    ucrt_free(&c);
    free(px);
    return rc;
}

static void check_lines(int w, int h, int lines, const char *what) {
    int *cls = calloc((size_t)h, sizeof *cls);
    if (!cls || rows_of(w, h, lines, cls) != 0) {
        utest_check(0, what);
        free(cls);
        return;
    }
    int dark = 0, half = 0, misplaced = 0;
    for (int r = 0; r < h; r++) {
        long l0 = (long)r * lines / h, l1 = (long)(r + 1) * lines / h, l2 = (long)(r + 2) * lines / h;
        int want = l1 != l0 ? 2 : (h >= 4 * lines && l2 != l0) ? 1 : 0;
        if (cls[r] == 2) dark++;
        if (cls[r] == 1) half++;
        if (cls[r] != want) misplaced++;
    }
    int want_half = h >= 4 * lines ? lines : 0;
    utest_checkf(dark == lines && half == want_half && !misplaced,
                 "%s: %d dark rows (want %d), %d half (want %d), %d misplaced",
                 what, dark, lines, half, want_half, misplaced);
    free(cls);
}

static void check_apply_from(void) {
    enum { W = 160, H = 120 };
    uint32_t *src = malloc(W * H * 4), *keep = malloc(W * H * 4);
    uint32_t *a = malloc(W * H * 4), *b = malloc(W * H * 4);
    if (!src || !keep || !a || !b) { utest_check(0, "apply_from: buffers"); return; }
    for (int i = 0; i < W * H; i++) src[i] = (uint32_t)(i * 2654435761u) & 0xFFFFFF;
    memcpy(keep, src, W * H * 4);
    memcpy(a, src, W * H * 4);
    memset(b, 0x5A, W * H * 4);

    struct ugfx_surface sa = { .pixels = a, .w = W, .h = H }, sb = { .pixels = b, .w = W, .h = H };
    struct ucrt ca, cb;
    ucrt_init(&ca);
    ucrt_init(&cb);
    ca.look = cb.look = ucrt_presets[UCRT_PRESET_CURVED];
    ca.src_lines = cb.src_lines = 50;
    int ra = ucrt_apply(&ca, &sa, 0, 0, W, H);
    int rb = ucrt_apply_from(&cb, src, W, &sb, 0, 0, W, H);
    utest_checkf(ra == 0 && rb == 0 && !memcmp(a, b, W * H * 4),
                 "apply_from gives ucrt_apply's pixels (Curved, %dx%d)", W, H);
    utest_check(!memcmp(src, keep, W * H * 4), "apply_from leaves its source unwritten");

    struct ucrt off;
    ucrt_init(&off);
    memset(b, 0x5A, W * H * 4);
    ucrt_apply_from(&off, src, W, &sb, 0, 0, W, H);
    utest_check(!memcmp(b, src, W * H * 4), "apply_from with the effect off copies the picture plain");

    ucrt_free(&ca);
    ucrt_free(&cb);
    free(src); free(keep); free(a); free(b);
}

static void check_glass_point(void) {
    enum { W = 320, H = 240 };
    uint32_t *px = malloc(W * H * 4);
    if (!px) { utest_check(0, "glass point: buffer"); return; }
    for (int curve = UCRT_CURVE_SLIGHT; curve < UCRT_CURVE_COUNT; curve++) {
        struct ucrt c;
        ucrt_init(&c);
        c.look = (struct ucrt_look){ 1, 0, 0, curve, UCRT_MASK_OFF, 0, 0 };
        for (int i = 0; i < W * H; i++) px[i] = GREY;
        struct ugfx_surface s = { .pixels = px, .w = W, .h = H };
        ucrt_apply(&c, &s, 0, 0, W, H);
        // Every point it says shows must read back as itself; and every point
        // well inside must show -- only the corners the round glass cuts may not.
        int bad = 0, hidden = 0, n = 0, bx = 0, by = 0;
        for (int y = 0; y < H; y += 7)
            for (int x = 0; x < W; x += 7) {
                int gx, gy, sx, sy, inner = x >= W / 20 && x < W - W / 20 && y >= H / 20 && y < H - H / 20;
                int shown = ucrt_glass_point(&c, x, y, &gx, &gy);
                ucrt_source_point(&c, gx, gy, &sx, &sy);
                n++;
                if (shown && (sx - x < -1 || sx - x > 1 || sy - y < -1 || sy - y > 1)) { bad++; bx = x; by = y; }
                if (!shown && inner) { hidden++; bx = x; by = y; }
            }
        utest_checkf(bad == 0 && hidden == 0, "glass_point reads back as the point asked for, %s curve: %d of %d "
                     "off by more than a pixel, %d inside not shown (last at %d,%d)",
                     ucrt_curve_words[curve], bad, n, hidden, bx, by);
        ucrt_free(&c);
    }
    free(px);
}

int main(void) {
    utest_begin("ucrt_test", "the CRT effect's scanlines and apply_from", UTEST_VERDICT_FILE);
    check_lines(64, 480, 200, "200 lines in 480 rows (DOOM's window)");
    check_lines(64, 1080, 200, "200 lines in 1080 rows (fullscreen)");
    check_lines(64, 400, 200, "200 lines in 400 rows (two each)");
    check_apply_from();
    check_glass_point();
    return utest_end();
}
