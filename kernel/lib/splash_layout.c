// The boot splash's geometry -- see api/splash_layout.h. Freestanding
// and shared with ring 3, so it may name nothing kernel-only.
#include "splash_layout.h"

// tools/gen_icons.py's icon_toyos(), in its 64-pixel box: a 56-pixel
// plate (radius 12) holding three 20x18 bricks (radius 4) 4 apart, two
// below and one centred on top, the group CENTRED on the plate. Laid
// out from the plate's own size rather than scaled corner by corner, so
// rounding cannot push the group off centre at any size.
#define U_PLATE 56
#define U_W 20
#define U_H 18
#define U_GAP 4

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
    int bw = p * U_W / U_PLATE, bh = p * U_H / U_PLATE, g = p * U_GAP / U_PLATE;
    int r = p * 4 / U_PLATE;
    int left = px + (p - (2 * bw + g)) / 2;
    int top = py + (p - (2 * bh + g)) / 2;
    struct splash_rect b0 = { px + (p - bw) / 2, top, bw, bh, r };
    struct splash_rect b1 = { left, top + bh + g, bw, bh, r };
    struct splash_rect b2 = { left + bw + g, top + bh + g, bw, bh, r };
    out->brick[0] = b0;
    out->brick[1] = b1;
    out->brick[2] = b2;
    // Windows' arrangement: the mark alone in the middle, the spinner and
    // a line of text together near the bottom.
    out->spin_r = size / 7;
    out->dot_r = size / 32 < 2 ? 2 : size / 32;
    out->spin_cx = screen_w / 2;
    out->spin_cy = screen_h * 3 / 4;
    out->label_y = out->spin_cy + out->spin_r + size / 5;
}
