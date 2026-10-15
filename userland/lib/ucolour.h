#ifndef ULIB_UCOLOUR_H
#define ULIB_UCOLOUR_H

// COLOUR ARITHMETIC for a picker and a contrast check: 0xRRGGBB to and
// from hue, saturation and lightness (CSS's HSL) or value (HSV, what a
// colour square shows), and WCAG 2's contrast ratio between two colours.
// Ring 3 only (it uses floating point).

#include <stdint.h>

struct ucolour_hsl {
    int h;   // 0..359 degrees
    int s;   // 0..100 percent
    int l;   // 0..100 percent
};

void ucolour_to_hsl(uint32_t rgb, struct ucolour_hsl *out);
uint32_t ucolour_from_hsl(const struct ucolour_hsl *c);

// HSV in floats, so dragging across a colour square is not stepped.
// A grey has no hue and black no saturation: to_hsv() then LEAVES those
// fields as they were, so a picker dragged through black keeps its hue.
struct ucolour_hsv {
    float h;   // 0 <= h < 360 degrees
    float s;   // 0..1
    float v;   // 0..1
};

void ucolour_to_hsv(uint32_t rgb, struct ucolour_hsv *io);
uint32_t ucolour_from_hsv(const struct ucolour_hsv *c);

// WCAG 2.x contrast ratio, times ten: 10 (identical) to 210 (black on
// white). 45 is AA for body text, 30 for large text and for a control's
// edge against what is around it.
int ucolour_contrast10(uint32_t a, uint32_t b);

#endif
