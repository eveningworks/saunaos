// The boot splash's geometry -- see api/splash_layout.h. Freestanding
// and shared with ring 3, so it may name nothing kernel-only.
#include "splash_layout.h"

// tools/gen_icons.py's icon_saunaos(): a 56-unit plate (radius 12) at 4
// in the 64-unit box, and the stones below. Laid out from the plate's
// own size rather than scaled corner by corner, so rounding cannot push
// the group off centre at any size.
#define U_PLATE 56

// cos and sin x 1024 of -8, +6 and 0 degrees.
const struct splash_stone splash_stones_with_plate[SPLASH_STONES] = {
    { 20, 42, 10, 8, 1014, -143 },
    { 44, 42, 10, 8, 1018,  107 },
    { 32, 23, 10, 8, 1024,    0 },
};
const struct splash_stone splash_stones_bare[SPLASH_STONES] = {
    { 18, 43, 12, 9, 1014, -143 },
    { 46, 43, 12, 9, 1018,  107 },
    { 32, 21, 12, 9, 1024,    0 },
};

int splash_round_inset(const struct splash_rect *r, int row) {
    int rad = r->r;
    int dy = row < rad ? rad - row : row >= r->h - rad ? row - (r->h - rad - 1) : 0;
    if (!dy) return 0;
    // The widest x within the circle on this row, by integer search: a
    // radius is a few dozen pixels, and the kernel has no sqrt.
    int dx = 0;
    while (dx < rad && (dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++;
    return rad - dx;
}

void splash_layout(int screen_w, int screen_h, struct splash_layout *out) {
    // A seventh of the height: 144 at 1080 lines, clamped so a tiny mode
    // still shows a mark and a 4K one does not shout.
    int size = screen_h * 2 / 15;
    if (size < 48) size = 48;
    if (size > 192) size = 192;
    int p = size * U_PLATE / 64;                  // the plate, inside the icon's margin
    int px = (screen_w - p) / 2;
    int py = screen_h * 2 / 5 - p / 2;
    struct splash_rect plate = { px, py, p, p, p * 12 / U_PLATE };
    out->plate = plate;
    for (int i = 0; i < SPLASH_STONES; i++) {
        struct splash_stone u = splash_stones_with_plate[i];
        out->stone[i].cx = px + (u.cx - 4) * p / U_PLATE;
        out->stone[i].cy = py + (u.cy - 4) * p / U_PLATE;
        out->stone[i].a = u.a * p / U_PLATE;
        out->stone[i].b = u.b * p / U_PLATE;
        out->stone[i].cos1024 = u.cos1024;
        out->stone[i].sin1024 = u.sin1024;
    }
    // Windows' arrangement: the mark alone in the middle, the spinner and
    // a line of text together near the bottom.
    out->spin_r = size / 7;
    out->dot_r = size / 32 < 2 ? 2 : size / 32;
    out->spin_cx = screen_w / 2;
    out->spin_cy = screen_h * 3 / 4;
    out->label_y = out->spin_cy + out->spin_r + size / 5;
}

// Inside when u^2/a^2 + v^2/b^2 <= 1, (u, v) being the pixel turned back
// into the stone's own frame; scaled by a^2 b^2 1024^2 to stay in
// integers. A stone is a few dozen pixels across, so a scan of the row
// costs nothing.
static int stone_has(const struct splash_stone *st, int x, int y) {
    long long dx = x - st->cx, dy = y - st->cy;
    long long u = dx * st->cos1024 + dy * st->sin1024;
    long long v = dy * st->cos1024 - dx * st->sin1024;
    long long a2 = (long long)st->a * st->a, b2 = (long long)st->b * st->b;
    return u * u * b2 + v * v * a2 <= a2 * b2 * 1024 * 1024;
}

int splash_stone_span(const struct splash_stone *st, int y, int *x0, int *x1) {
    int reach = st->a > st->b ? st->a : st->b;
    int first = st->cx - reach, last = st->cx + reach;
    while (first <= last && !stone_has(st, first, y)) first++;
    if (first > last) return 0;
    while (!stone_has(st, last, y)) last--;
    *x0 = first;
    *x1 = last;
    return 1;
}
