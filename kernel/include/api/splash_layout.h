#ifndef SPLASH_LAYOUT_H
#define SPLASH_LAYOUT_H

// Where the BOOT SPLASH puts things: the SaunaOS logo and the spinner
// under it. Compiled twice from kernel/lib/splash_layout.c -- the
// kernel draws the logo from the framebuffer's first moment
// (kernel/drivers/splash.c), the compositor draws the same logo plus a
// spinner until the desktop or First-boot setup is up
// (userland/wm/wm_setup_hold.c) -- and the two must land on the same
// pixels, or the handover jumps. One function, so they cannot drift.
//
// The mark is the Start button's three stones on the blue plate,
// tools/gen_icons.py's icon_saunaos(), here as ellipses filled a row at
// a time rather than a picture: the kernel decodes no images.

// RGB, 0xRRGGBB: each side packs it for its own surface.
#define SPLASH_BG     0x0B0F17u   // near-black, the firmware's and Plymouth's
#define SPLASH_PLATE  0x2860E6u
#define SPLASH_STONE  0xFFFFFFu
#define SPLASH_TEXT   0xD8DCE6u

struct splash_rect { int x, y, w, h, r; };

// An ellipse with semi-axes a (along its own x) and b, centred on
// (cx, cy) and turned by an angle given as cos and sin times 1024 --
// y DOWN, so a positive angle turns it clockwise, as SVG's rotate().
struct splash_stone { int cx, cy, a, b, cos1024, sin1024; };

#define SPLASH_STONES 3

// The mark in the icon's 64-unit box (tools/gen_icons.py draws the same
// numbers): two stones below, turned slightly apart, one on top.
// WITH_PLATE is the logo's; BARE is the Start button's, larger because
// it has no plate around it.
extern const struct splash_stone splash_stones_with_plate[SPLASH_STONES];
extern const struct splash_stone splash_stones_bare[SPLASH_STONES];

struct splash_layout {
    struct splash_rect plate;
    struct splash_stone stone[SPLASH_STONES];
    int spin_cx, spin_cy, spin_r, dot_r;   // the spinner's ring and its dots
    int label_y;                           // the top of a line of text under it
};

void splash_layout(int screen_w, int screen_h, struct splash_layout *out);

// How far row `row` (0 = the rect's top) is inset at each end by the
// rounded corners: both rings fill a rect as these spans, so the logo is
// the same pixels whichever ring drew it. Hard-edged, the kernel's
// limit (no blending into a surface it does not keep).
int splash_round_inset(const struct splash_rect *r, int row);

// The pixels row `y` covers of a stone, [*x0, *x1] inclusive; 0 when the
// row misses it. An ellipse is convex, so a row is one span. Pixel
// centres in, hard-edged out, for the same reason as above.
int splash_stone_span(const struct splash_stone *st, int y, int *x0, int *x1);

#endif
