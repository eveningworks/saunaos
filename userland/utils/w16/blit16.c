// blit16.c -- GDI's pixel work: raster operations, lines, and decoding a
// device-independent bitmap. No objects or handles here (gdi16.c owns
// those); everything draws into a struct gdi_target, a 32-bit pixel
// buffer, `mono` when it stands for a 1-bit bitmap (pixels black or white).
//
// THE RASTER OPERATIONS ARE WINDOWS' OWN TRUTH TABLES, applied bitwise
// across the palette indices where every colour is one of the static 20
// (as the display's own pixels were indices), else across the 24 colour
// bits: a ROP3's code byte has bit P*4 + S*2 + D set when the result is 1
// for that pattern, source and destination (SRCCOPY, 0xCC, is "S"); a
// ROP2's value minus one is the same table over P*2 + D. So every one of
// the 256 ROP3s and 16 ROP2s works, none special-cased but the commonest
// for speed.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>

#define WHITE 0xFFFFFFu

uint32_t w16_colorref_px(uint32_t cr) {
    uint32_t rgb = (cr & 0xFF) << 16 | (cr & 0xFF00) | (cr >> 16 & 0xFF);
    return w16_vga(w16_colors16() ? w16_vga_nearest(rgb) : rgb);   // 16 colours: a pen's or text's is solid
}

uint32_t gdi_to_mono(uint32_t px) {
    unsigned r = px >> 16 & 0xFF, g = px >> 8 & 0xFF, b = px & 0xFF;
    return r * 30 + g * 59 + b * 11 >= 128 * 100 ? WHITE : 0;
}

// Windows' 20 static colours of a 256-colour screen, by palette index: 0-9 and 246-255.
static const struct { uint8_t index; uint32_t rgb; } STATIC[20] = {
    { 0, 0x000000 }, { 1, 0x800000 }, { 2, 0x008000 }, { 3, 0x808000 }, { 4, 0x000080 },
    { 5, 0x800080 }, { 6, 0x008080 }, { 7, 0xC0C0C0 }, { 8, 0xC0DCC0 }, { 9, 0xA6CAF0 },
    { 246, 0xFFFBF0 }, { 247, 0xA0A0A4 }, { 248, 0x808080 }, { 249, 0xFF0000 }, { 250, 0x00FF00 },
    { 251, 0xFFFF00 }, { 252, 0x0000FF }, { 253, 0xFF00FF }, { 254, 0x00FFFF }, { 255, 0xFFFFFF },
};

// Pixels hold the colours as 3.11's VGA driver showed them (w16_vga(): its light grey is 0xC0C7C8),
// so a static colour is matched, and answered, in that form -- worked out once, as a blit asks per pixel.
static uint32_t g_shade[20];

static void shades(void) {
    if (g_shade[19]) return;
    for (unsigned i = 0; i < 20; i++) g_shade[i] = w16_vga(STATIC[i].rgb);
}

static int static_index(uint32_t rgb) {
    rgb &= WHITE;
    if (rgb == 0) return 0;
    if (rgb == WHITE) return 255;
    shades();
    for (unsigned i = 1; i < 19; i++)
        if (g_shade[i] == rgb) return STATIC[i].index;
    return -1;
}

static int static_rgb(unsigned index, uint32_t *rgb) {
    shades();
    for (unsigned i = 0; i < 20; i++)
        if (STATIC[i].index == index) { *rgb = g_shade[i]; return 1; }
    return 0;
}

// THE RASTER OPERATIONS WORK ON PALETTE INDICES TOO, as the display did -- gdi_rop2's rule, over the
// operands a ROP3 uses: DSTINVERT turns teal (6) to red (249), as Paintbrush's selected tool shows,
// where RGB's NOT gives pink. An operand the code ignores may be any colour; where the used ones are
// all static and so is the answer, that answer, else the RGB.
uint32_t gdi_rop3(uint8_t code, uint32_t p, uint32_t s, uint32_t d) {
    switch (code) {
    case 0xCC: return s;                         // SRCCOPY
    case 0xF0: return p;                         // PATCOPY
    case 0x00: return 0;                         // BLACKNESS
    case 0xFF: return WHITE;                     // WHITENESS
    default: break;
    }
    int use_p = ((code >> 4 ^ code) & 0x0F) != 0, use_s = ((code >> 2 ^ code) & 0x33) != 0;
    int use_d = ((code >> 1 ^ code) & 0x55) != 0;
    int pi = use_p ? static_index(p) : 0, si = use_s ? static_index(s) : 0, di = use_d ? static_index(d) : 0;
    if (pi >= 0 && si >= 0 && di >= 0) {
        unsigned out = 0;
        for (unsigned i = 0; i < 8; i++)
            if (code >> i & 1)
                out |= (i & 4 ? (unsigned)pi : ~(unsigned)pi) & (i & 2 ? (unsigned)si : ~(unsigned)si) &
                       (i & 1 ? (unsigned)di : ~(unsigned)di);
        uint32_t rgb;
        if (static_rgb(out & 0xFF, &rgb)) return rgb;
    }
    uint32_t out = 0;
    for (unsigned i = 0; i < 8; i++)
        if (code >> i & 1)
            out |= (i & 4 ? p : ~p) & (i & 2 ? s : ~s) & (i & 1 ? d : ~d);
    return out & WHITE;
}

uint32_t gdi_rop2(int r2, uint32_t p, uint32_t d) {
    unsigned code = (unsigned)(r2 - 1) & 15;
    if (code == 12) return p;                    // R2_COPYPEN
    // THE MIX WORKS ON PALETTE INDICES, as on the 256-colour screen w16 reports: the static palette is
    // laid out so an inverted index is a colour's opposite -- light grey (7) inverts to dark grey (248),
    // not RGB's (63, 56, 55): Clock's second hand. Where pen and pixel are both static colours and the
    // answer is one too, that answer; anything else mixes the RGB.
    int pi = static_index(p), di = static_index(d);
    if (pi >= 0 && di >= 0) {
        unsigned out = 0;
        for (unsigned i = 0; i < 4; i++)
            if (code >> i & 1) out |= (i & 2 ? (unsigned)pi : ~(unsigned)pi) & (i & 1 ? (unsigned)di : ~(unsigned)di);
        uint32_t rgb;
        if (static_rgb(out & 0xFF, &rgb)) return rgb;
    }
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; i++)
        if (code >> i & 1) out |= (i & 2 ? p : ~p) & (i & 1 ? d : ~d);
    return out & WHITE;
}

void gdi_target_whole(struct gdi_target *t) {
    t->ox = t->oy = 0;
    t->x0 = t->y0 = 0;
    t->x1 = t->w;
    t->y1 = t->h;
}

// DC coordinates into the buffer's, cut to what the target may touch; the
// source offsets move with a cut left or top edge.
static int clip(const struct gdi_target *t, int *x, int *y, int *w, int *h, int *sx, int *sy) {
    *x += t->ox;
    *y += t->oy;
    if (*x < t->x0) { int d = t->x0 - *x; *w -= d; if (sx) *sx += d; *x = t->x0; }
    if (*y < t->y0) { int d = t->y0 - *y; *h -= d; if (sy) *sy += d; *y = t->y0; }
    if (*x + *w > t->x1) *w = t->x1 - *x;
    if (*y + *h > t->y1) *h = t->y1 - *y;
    return *w > 0 && *h > 0;
}

// The source pixel as the destination sees it: Windows' conversions
// between a 1-bit and a colour bitmap (a 0 bit is the destination's text
// colour, a 1 its background; a colour pixel equal to the source's
// background becomes a 1).
static uint32_t convert(const struct gdi_blit *b, uint32_t s) {
    if (b->src->mono && !b->dst->mono) return s ? b->dst_bk : b->dst_text;
    if (!b->src->mono && b->dst->mono) return s == b->src_bk ? WHITE : 0;
    return s;
}

void gdi_bitblt(const struct gdi_blit *b) {
    int x = b->x, y = b->y, w = b->w, h = b->h, sx = b->sx, sy = b->sy;
    if (!b->dst->px || !clip(b->dst, &x, &y, &w, &h, &sx, &sy)) return;
    uint32_t pat = b->dst->mono ? gdi_to_mono(b->brush) : b->brush;
    uint8_t code = (uint8_t)(b->rop >> 16);
    int uses_src = ((code >> 2) & 0x33) != (code & 0x33);
    if (uses_src) {
        if (!b->src || !b->src->px) return;
        // The source rectangle clips too; the offsets move both.
        const struct gdi_target *st = b->src;
        sx += st->ox;
        sy += st->oy;
        if (sx < st->x0) { int d = st->x0 - sx; x += d; w -= d; sx = st->x0; }
        if (sy < st->y0) { int d = st->y0 - sy; y += d; h -= d; sy = st->y0; }
        if (sx + w > st->x1) w = st->x1 - sx;
        if (sy + h > st->y1) h = st->y1 - sy;
        if (w <= 0 || h <= 0) return;
    }
    // A buffer copy first when source and destination overlap.
    uint32_t *tmp = 0;
    const uint32_t *spx = uses_src ? b->src->px : 0;
    int sstride = uses_src ? b->src->w : 0;
    if (uses_src && b->src->px == b->dst->px) {
        tmp = malloc((size_t)w * h * 4);
        if (!tmp) return;
        for (int r = 0; r < h; r++) memcpy(tmp + r * w, spx + (size_t)(sy + r) * sstride + sx, (size_t)w * 4);
        spx = tmp; sstride = w; sx = 0; sy = 0;
    }
    for (int r = 0; r < h; r++) {
        uint32_t *d = b->dst->px + (size_t)(y + r) * b->dst->w + x;
        const uint32_t *s = uses_src ? spx + (size_t)(sy + r) * sstride + sx : 0;
        const uint32_t *prow = b->pat ? b->pat + ((y + r - b->pat_y) & 7) * 8 : 0;
        for (int c = 0; c < w; c++) {
            uint32_t sv = s ? convert(b, s[c]) : 0;
            uint32_t pv = prow ? (b->dst->mono ? gdi_to_mono(prow[(x + c - b->pat_x) & 7]) : prow[(x + c - b->pat_x) & 7])
                               : pat;
            d[c] = gdi_rop3(code, pv, sv, d[c]);
        }
    }
    free(tmp);
}

void gdi_pixel(const struct gdi_target *t, int x, int y, uint32_t px, int r2) {
    x += t->ox;
    y += t->oy;
    if (!t->px || x < t->x0 || y < t->y0 || x >= t->x1 || y >= t->y1) return;
    uint32_t *d = t->px + (size_t)y * t->w + x;
    *d = gdi_rop2(r2, t->mono ? gdi_to_mono(px) : px, *d);
}

// Bresenham from (x0,y0) to (x1,y1), the last point left out, as LineTo
// draws; a wider pen is a square of its width at each point.
void gdi_line(const struct gdi_target *t, int x0, int y0, int x1, int y1, uint32_t px,
              int width, int r2) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, half = width > 1 ? width / 2 : 0;
    for (;;) {
        if (x0 == x1 && y0 == y1) break;
        for (int oy = -half; oy <= (width > 1 ? width - 1 - half : 0); oy++)
            for (int ox = -half; ox <= (width > 1 ? width - 1 - half : 0); ox++)
                gdi_pixel(t, x0 + ox, y0 + oy, px, r2);
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// --- device-independent bitmaps ----------------------------------------------

// A BITMAPINFO at `bmi` (host bytes the caller checked for `bmi_len`): its
// header read, its palette converted. 0, or -1 for a format w16 cannot
// read (compressed, or a depth other than 1, 4, 8 and 24).
int gdi_dib_header(const uint8_t *bmi, uint32_t bmi_len, int pal_indices, struct gdi_dib *out) {
    memset(out, 0, sizeof *out);
    if (bmi_len < 12) return -1;
    uint32_t hsize = (uint32_t)(bmi[0] | bmi[1] << 8 | bmi[2] << 16 | (uint32_t)bmi[3] << 24);
    unsigned pal_entry;
    if (hsize == 12) {                                   // BITMAPCOREHEADER
        out->w = bmi[4] | bmi[5] << 8;
        out->h = (int16_t)(bmi[6] | bmi[7] << 8);
        out->bpp = bmi[10] | bmi[11] << 8;
        pal_entry = 3;
    } else if (hsize >= 40 && hsize <= bmi_len) {        // BITMAPINFOHEADER
        out->w = (int32_t)(bmi[4] | bmi[5] << 8 | bmi[6] << 16 | (uint32_t)bmi[7] << 24);
        out->h = (int32_t)(bmi[8] | bmi[9] << 8 | bmi[10] << 16 | (uint32_t)bmi[11] << 24);
        out->bpp = bmi[14] | bmi[15] << 8;
        uint32_t comp = (uint32_t)(bmi[16] | bmi[17] << 8);
        if (comp != 0) return -1;
        out->ncolors = (unsigned)(bmi[32] | bmi[33] << 8);
        pal_entry = 4;
    } else {
        return -1;
    }
    if (out->bpp != 1 && out->bpp != 4 && out->bpp != 8 && out->bpp != 24) return -1;
    if (out->w <= 0 || out->w > 0x7FFF || out->h == 0 || out->h > 0x7FFF || out->h < -0x7FFF) return -1;
    if (out->bpp <= 8 && (out->ncolors == 0 || out->ncolors > (1u << out->bpp))) out->ncolors = 1u << out->bpp;
    if (out->bpp == 24) out->ncolors = 0;
    unsigned pal_bytes = out->ncolors * (pal_indices ? 2 : pal_entry);
    if (hsize + pal_bytes > bmi_len) return -1;
    const uint8_t *p = bmi + hsize;
    for (unsigned i = 0; i < out->ncolors; i++) {
        if (pal_indices) {                               // DIB_PAL_COLORS: a grey ramp stand-in
            unsigned v = (unsigned)(p[2 * i] | p[2 * i + 1] << 8) & 0xFF;
            out->pal[i] = v << 16 | v << 8 | v;
        } else {
            const uint8_t *q = p + pal_entry * i;
            out->pal[i] = w16_vga((uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]);
        }
    }
    out->stride = (((uint32_t)out->w * out->bpp + 31) / 32) * 4;
    out->header_len = hsize + pal_bytes;
    return 0;
}

// Pixel x of a row's bytes.
uint32_t gdi_dib_px(const struct gdi_dib *d, const uint8_t *row, int x) {
    switch (d->bpp) {
    case 1:  return d->pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
    case 4:  return d->pal[(row[x >> 1] >> (x & 1 ? 0 : 4)) & 15];
    case 8:  return d->pal[row[x] < d->ncolors ? row[x] : 0];
    default: return w16_vga((uint32_t)row[3 * x + 2] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x]);
    }
}
