// dither16.c -- Windows 3.11's 16-colour VGA, for a program whose Colours
// setting is 16 (w16prefs.h): a brush of any colour but the sixteen is
// DITHERED as the VGA driver dithered it, and a pen or text takes the
// nearest of the sixteen.
//
// MEASURED, NOT DERIVED FROM A SPECIFICATION: 23 colours set in Paintbrush's
// Edit Colors on the reference VM, every pixel of each swatch matching, 9 of
// them held out of the fitting (docs/decisions/gui.md has the rule's
// derivation). The rule, in 64ths of a pixel block, each channel c' =
// (c + 1) >> 2:
//   - every channel at most 32 (128): the dark cube, split by its sorted
//     channels into black, a dark primary, a dark secondary and dark grey,
//     each weight doubled;
//   - else the whole cube: black (64 - the largest), a primary, a secondary,
//     white; then black pairs off -- with white into dark grey, then with the
//     secondary, then with the primary, each pair a half-intensity colour.
// The weights fill an 8x8 Bayer matrix, in the order of the colours' VGA
// indices, aligned to the SCREEN; light grey is used only for itself.
#include "w16_internal.h"

#include <string.h>
#include "w16/w16prefs.h"

// Windows' 16 VGA colours, by palette index.
static const uint32_t VGA16[16] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };

// The standard 8x8 Bayer matrix, as 3.11's driver laid it on the screen: offset by (5, 7).
static int rank(int x, int y) {
    static const uint8_t B[8][8] = {
        { 0, 32, 8, 40, 2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 }, { 12, 44, 4, 36, 14, 46, 6, 38 },
        { 60, 28, 52, 20, 62, 30, 54, 22 }, { 3, 35, 11, 43, 1, 33, 9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
        { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
    };
    return B[(y + 7) & 7][(x + 5) & 7];
}

int w16_colors16(void) { return g_w16_prefs.colors == W16_COLORS_16; }

// The VGA index of the colour with these channels on (bit 0 red, 1 green, 2 blue) at full or half.
static int vga_of(unsigned bits, int bright) {
    static const int8_t HALF[8] = { 0, 1, 2, 3, 4, 5, 6, 8 }, FULL[8] = { 0, 9, 10, 11, 12, 13, 14, 15 };
    return bright ? FULL[bits & 7] : HALF[bits & 7];
}

// How many of the 64 each VGA colour takes for 0xRRGGBB.
static void weights(uint32_t rgb, int w[16]) {
    memset(w, 0, 16 * sizeof *w);
    for (int i = 0; i < 16; i++)
        if (VGA16[i] == (rgb & 0xFFFFFF)) { w[i] = 64; return; }
    int c[3] = { (int)((rgb >> 16 & 0xFF) + 1) >> 2, (int)((rgb >> 8 & 0xFF) + 1) >> 2, (int)((rgb & 0xFF) + 1) >> 2 };
    int o[3] = { 0, 1, 2 };                      // the channels, largest first
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (c[o[j]] < c[o[j + 1]]) { int t = o[j]; o[j] = o[j + 1]; o[j + 1] = t; }
    int hi = c[o[0]], mid = c[o[1]], lo = c[o[2]];
    unsigned one = 1u << o[0], two = one | 1u << o[1];
    if (hi <= 32) {                              // the dark cube
        w[0] += 2 * (32 - hi);
        w[vga_of(one, 0)] += 2 * (hi - mid);
        w[vga_of(two, 0)] += 2 * (mid - lo);
        w[8] += 2 * lo;
        return;
    }
    int K = 64 - hi, P = hi - mid, S = mid - lo, W = lo, d;
    d = K < W ? K : W; w[8] += 2 * d; K -= d; W -= d;
    d = K < S ? K : S; w[vga_of(two, 0)] += 2 * d; K -= d; S -= d;
    d = K < P ? K : P; w[vga_of(one, 0)] += 2 * d; K -= d; P -= d;
    w[0] += K;
    w[vga_of(one, 1)] += P;
    w[vga_of(two, 1)] += S;
    w[15] += W;
}

int w16_dither(uint32_t rgb, uint32_t pat[64]) {
    int w[16], kinds = 0;
    weights(rgb, w);
    for (int i = 0; i < 16; i++) kinds += w[i] > 0;
    if (kinds < 2) return 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int r = rank(x, y), at = 0, i = 0;
            while (i < 15 && r >= at + w[i]) at += w[i++];
            pat[y * 8 + x] = w16_vga(VGA16[i]);
        }
    return 1;
}

uint32_t w16_vga_nearest(uint32_t rgb) {
    int r = rgb >> 16 & 0xFF, g = rgb >> 8 & 0xFF, b = rgb & 0xFF, best = 0;
    long bd = -1;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)(VGA16[i] >> 16), dg = g - (int)(VGA16[i] >> 8 & 0xFF), db = b - (int)(VGA16[i] & 0xFF);
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return VGA16[best];
}
