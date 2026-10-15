// See ucolour.h.
#include "lib/ucolour.h"
#include <math.h>

static float chan(uint32_t rgb, int shift) { return (float)((rgb >> shift) & 0xFF) / 255.0f; }

void ucolour_to_hsl(uint32_t rgb, struct ucolour_hsl *out) {
    float r = chan(rgb, 16), g = chan(rgb, 8), b = chan(rgb, 0);
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float l = (mx + mn) / 2.0f, d = mx - mn, h = 0, s = 0;
    if (d > 0.0001f) {
        s = d / (1.0f - (2.0f * l - 1.0f < 0 ? 1.0f - 2.0f * l : 2.0f * l - 1.0f));
        if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h *= 60.0f;
    }
    out->h = (int)(h + 0.5f) % 360;
    out->s = (int)(s * 100.0f + 0.5f);
    out->l = (int)(l * 100.0f + 0.5f);
}

static float hue_part(float p, float q, float t) {
    if (t < 0) t += 1.0f;
    if (t > 1) t -= 1.0f;
    if (t < 1.0f / 6) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6.0f;
    return p;
}

static uint32_t byte(float v) {
    int b = (int)(v * 255.0f + 0.5f);
    return (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

uint32_t ucolour_from_hsl(const struct ucolour_hsl *c) {
    float h = (float)(((c->h % 360) + 360) % 360) / 360.0f;
    float s = (float)(c->s < 0 ? 0 : c->s > 100 ? 100 : c->s) / 100.0f;
    float l = (float)(c->l < 0 ? 0 : c->l > 100 ? 100 : c->l) / 100.0f;
    if (s <= 0) return byte(l) << 16 | byte(l) << 8 | byte(l);
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    return byte(hue_part(p, q, h + 1.0f / 3)) << 16 | byte(hue_part(p, q, h)) << 8 |
           byte(hue_part(p, q, h - 1.0f / 3));
}

void ucolour_to_hsv(uint32_t rgb, struct ucolour_hsv *io) {
    float r = chan(rgb, 16), g = chan(rgb, 8), b = chan(rgb, 0);
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    io->v = mx;
    if (mx <= 0) return;   // black: hue and saturation stay what they were
    io->s = d / mx;
    if (d <= 0) return;    // a grey: the hue stays
    float h;
    if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h *= 60.0f;
    io->h = h >= 360.0f ? h - 360.0f : h;
}

uint32_t ucolour_from_hsv(const struct ucolour_hsv *c) {
    float h = c->h, s = c->s < 0 ? 0 : c->s > 1 ? 1 : c->s, v = c->v < 0 ? 0 : c->v > 1 ? 1 : c->v;
    while (h < 0) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    float f = h / 60.0f;
    int i = (int)f;
    f -= (float)i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f)), r, g, b;
    switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return byte(r) << 16 | byte(g) << 8 | byte(b);
}

// sRGB to linear, WCAG's own formula.
static float lin(float c) {
    return c <= 0.03928f ? c / 12.92f : (float)pow((c + 0.055) / 1.055, 2.4);
}

static float lum(uint32_t rgb) {
    return 0.2126f * lin(chan(rgb, 16)) + 0.7152f * lin(chan(rgb, 8)) + 0.0722f * lin(chan(rgb, 0));
}

int ucolour_contrast10(uint32_t a, uint32_t b) {
    float la = lum(a), lb = lum(b);
    float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (int)((hi + 0.05f) / (lo + 0.05f) * 10.0f);
}
