// A BUTTON'S CORNERS ARE THE THEME'S CONTROL RADIUS, plain or outlined,
// asserted as pixels. The plain face once filled a square rect while the
// outlined one was rounded, so System Settings' buttons (plain) were
// square beside Device Manager's (outlined) -- the corner pixel is the
// discriminating one: the ground there on a rounded button, the face on
// a square one. The middle of the top edge is the control: the face on
// both, so a button that drew nothing at all fails too.
#include <stdint.h>
#include <stdio.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_button.h"
#include "ui/uui_primitives.h"
#include "uitest.h"

#define SURF_W 200
#define SURF_H 60
#define GROUND 0x00102030u

static uint32_t g_px[SURF_W * SURF_H];
static struct ugfx_surface g_surf = { .pixels = g_px, .w = SURF_W, .h = SURF_H };

static void paint(struct uui_button *b) {
    for (int i = 0; i < SURF_W * SURF_H; i++) g_px[i] = GROUND;
    uui_button_ops.draw(&g_surf, b);
}

static void check(const char *what, struct uui_button *b) {
    paint(b);
    uint32_t corner = g_px[b->y * SURF_W + b->x], edge = g_px[b->y * SURF_W + b->x + b->w / 2];
    char d[96];
    snprintf(d, sizeof d, "corner %06x, top edge %06x, ground %06x", (unsigned)corner, (unsigned)edge,
             (unsigned)GROUND);
    utest_check_detail(edge != GROUND && corner == GROUND, what, d);
}

int main(void) {
    uitest_begin("button_corners", "a button's corners are rounded, plain or outlined");
    char d[48];
    snprintf(d, sizeof d, "radius %d", uui_control_radius(0));
    utest_check_detail(uui_control_radius(0) > 1, "the theme's controls are rounded (the default corners)", d);

    static struct uui_button b;
    uui_button_init(&b, 20, 10, 120, 30, "Apply", UUI_COLOR_UNSET, UUI_COLOR_UNSET, 1);
    check("a plain button's corner shows the ground", &b);
    b.outlined = 1;
    check("an outlined button's corner shows the ground", &b);
    return uitest_end();
}
