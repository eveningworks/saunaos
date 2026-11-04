// wing16.c -- WING, WinG 1.0's 16-bit DLL: an 8-bit DIB the program draws
// into with its own code, copied to the screen fast. Wine's shape, WinG
// over a DIB section, with w16's own twist: GDI draws on a bitmap's 32-bit
// `px`, while a WinG bitmap's pixels are bytes in a global block the
// program writes through a huge pointer. The two are kept as one:
//
// - fill_target() PULLS the bytes into px (through the colour table)
//   before any GDI call reads or draws on the bitmap;
// - after every Windows call the dispatcher PUSHES back what GDI changed
//   (w16_wing_push), each changed pixel as the table's nearest index --
//   an unchanged pixel keeps its byte, so a table with a colour twice
//   never loses which one the program chose.
#include "utils/w16/w16_internal.h"

#include <stdlib.h>
#include <string.h>

// A colour's index in a table: exact when there is one, else the nearest;
// remembered in a small open hash so a whole frame of one colour is one
// search.
#define REV_SLOTS 4096
struct rev { uint32_t key[REV_SLOTS]; uint8_t idx[REV_SLOTS]; };   // key: colour | 1 << 24

static void rev_clear(struct w16_wing *g) {
    if (g->rev) memset(g->rev->key, 0, sizeof g->rev->key);
}

static uint8_t nearest(const struct w16_wing *g, uint32_t px) {
    int best = 0;
    long bd = -1;
    for (int i = 0; i < 256; i++) {
        uint32_t t = g->table[i];
        if (t == px) return (uint8_t)i;
        long dr = (long)(t >> 16 & 0xFF) - (px >> 16 & 0xFF), dg = (long)(t >> 8 & 0xFF) - (px >> 8 & 0xFF);
        long db = (long)(t & 0xFF) - (px & 0xFF), d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

static uint8_t index_of(struct w16_wing *g, uint32_t px) {
    if (!g->rev) {
        g->rev = calloc(1, sizeof *g->rev);
        if (!g->rev) return nearest(g, px);
    }
    uint32_t key = (px & 0xFFFFFF) | 1u << 24;
    unsigned h = (unsigned)((px * 2654435761u) >> 20) % REV_SLOTS;
    for (unsigned k = 0; k < 8; k++, h = (h + 1) % REV_SLOTS) {
        if (g->rev->key[h] == key) return g->rev->idx[h];
        if (!g->rev->key[h]) {
            g->rev->key[h] = key;
            return g->rev->idx[h] = nearest(g, px & 0xFFFFFF);
        }
    }
    return nearest(g, px & 0xFFFFFF);            // a crowded run: answer, keep nothing
}

// The block's row for bitmap row y.
static uint8_t *row_of(struct w16 *w, struct w16_gdiobj *bm, int y) {
    struct w16_wing *g = bm->wing;
    uint32_t size;
    uint8_t *bits = w16_global_ptr(w, g->bits, &size);
    int r = g->topdown ? y : bm->bh - 1 - y;
    if (!bits || (uint64_t)(r + 1) * g->stride > size) return 0;
    return bits + (size_t)r * g->stride;
}

void w16_wing_pull(struct w16 *w, struct w16_gdiobj *bm) {
    struct w16_wing *g = bm->wing;
    if (g->pulled) return;
    for (int y = 0; y < bm->bh; y++) {
        const uint8_t *row = row_of(w, bm, y);
        if (!row) return;
        uint32_t *px = bm->px + (size_t)y * bm->bw;
        for (int x = 0; x < bm->bw; x++) px[x] = g->table[row[x]];
    }
    g->pulled = 1;
    w->wing_pulled++;
}

void w16_wing_push(struct w16 *w) {
    for (int i = 0; i < w->nwing && w->wing_pulled; i++) {
        struct w16_gdiobj *bm = w16_gdi_obj(w, w->wing[i], GDI_BITMAP);
        if (!bm || !bm->wing || !bm->wing->pulled) continue;
        struct w16_wing *g = bm->wing;
        for (int y = 0; y < bm->bh; y++) {
            uint8_t *row = row_of(w, bm, y);
            if (!row) break;
            const uint32_t *px = bm->px + (size_t)y * bm->bw;
            for (int x = 0; x < bm->bw; x++)
                if ((px[x] & 0xFFFFFF) != g->table[row[x]]) row[x] = index_of(g, px[x]);
        }
        g->pulled = 0;
        w->wing_pulled--;
    }
    w->wing_pulled = 0;
}

void w16_wing_free(struct w16 *w, struct w16_gdiobj *bm) {
    struct w16_wing *g = bm->wing;
    if (g->pulled) w->wing_pulled--;
    w16_global_free(w, g->bits);
    for (int i = 0; i < w->nwing; i++)
        if (w->wing[i] == g->hbm) w->wing[i] = w->wing[--w->nwing];
    free(g->rev);
    free(g);
    bm->wing = 0;
}

// The WinG bitmap selected into a DC, or 0.
static struct w16_gdiobj *dc_bitmap(struct w16 *w, uint16_t hdc) {
    struct w16_gdiobj *dc = w16_gdi_obj(w, hdc, GDI_DC);
    struct w16_gdiobj *bm = dc ? w16_gdi_obj(w, dc->bitmap, GDI_BITMAP) : 0;
    return bm && bm->wing ? bm : 0;
}

// Room for one more WinG bitmap in w->wing, grown as needed; 0 out of memory.
static int list_room(struct w16 *w) {
    if (w->nwing < w->wing_cap) return 1;
    int cap = w->wing_cap ? 2 * w->wing_cap : 8;
    uint16_t *grown = realloc(w->wing, (size_t)cap * sizeof *grown);
    if (!grown) return 0;
    w->wing = grown;
    w->wing_cap = cap;
    return 1;
}

// A BITMAPINFOHEADER for an 8-bit DIB of w x h (h < 0: top-down) at `p`.
static void put_header(uint8_t *p, int32_t bw, int32_t bh, uint32_t image) {
    memset(p, 0, 40);
    uint32_t v[] = { 40, (uint32_t)bw, (uint32_t)bh };
    memcpy(p, v, 12);
    p[12] = 1;                                   // planes
    p[14] = 8;                                   // bits per pixel
    memcpy(p + 20, &image, 4);
}

// WinGCreateDC() -> HDC
// Windows: A device context for WinG bitmaps: a memory DC their pixels are drawn and copied from.
// w16: A memory DC, CreateCompatibleDC's.
static int api_WinGCreateDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    w->cpu.r[R_AX] = w16_gdi_new_memory_dc(w);
    return 0;
}

// WinGRecommendDIBFormat(lpbmi) -> BOOL
// Windows: The DIB format that copies to the screen fastest: 8 bits a pixel, rows top-down or
// bottom-up as the display driver works best.
// w16: 8 bits, top-down (a height of -1), as Wine answers; any order copies as fast.
static int api_WinGRecommendDIBFormat(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 40, 1);
    if (!p) return -1;
    put_header(p, 1, -1, 0);
    w->cpu.r[R_AX] = 1;
    return 4;
}

// WinGCreateBitmap(hdc, lpbmi, lplpvBits) -> HBITMAP
// Windows: An 8-bit DIB of the size and colours lpbmi gives, selected into a WinGDC to draw on with
// GDI; its pixels are the program's, at the pointer stored at lplpvBits.
// w16: The same, the pixels a global block of their own -- a run of selectors __AHINCR apart past
// 64 KB, so the pointer is a huge one -- that GDI's drawing is kept in step with (this file's top).
// A DIB of another depth is refused, as WinG refused it.
static int api_WinGCreateBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    w16_arg16(a);
    uint32_t bmi = w16_arg32(a), ppv = w16_arg32(a);
    c->r[R_AX] = 0;
    const uint8_t *h = cpu16_ptr(c, (uint16_t)(bmi >> 16), (uint16_t)bmi, 40, 0);
    if (!h) return -1;
    uint32_t used = (uint32_t)(h[32] | h[33] << 8), colors = used && used < 256 ? used : 256;
    h = cpu16_ptr(c, (uint16_t)(bmi >> 16), (uint16_t)bmi, 40 + 4 * colors, 0);
    struct gdi_dib d;
    if (!h || gdi_dib_header(h, 40 + 4 * colors, 0, &d) < 0 || d.bpp != 8) return 10;
    int bh = d.h < 0 ? -d.h : d.h;
    uint16_t hbm;
    struct w16_gdiobj *bm = w16_gdi_new_bitmap(w, d.w, bh, &hbm);
    struct w16_wing *g = bm && list_room(w) ? calloc(1, sizeof *g) : 0;
    if (g) g->bits = w16_global_alloc(w, d.stride * (uint32_t)bh, 0);
    if (!g || !g->bits) {
        free(g);
        if (bm) w16_gdi_delete(w, hbm);
        return 10;
    }
    g->topdown = d.h < 0;
    g->stride = d.stride;
    g->hbm = hbm;
    memcpy(g->table, d.pal, sizeof g->table);
    bm->wing = g;
    w->wing[w->nwing++] = hbm;
    if (ppv) {
        uint8_t *p = cpu16_ptr(c, (uint16_t)(ppv >> 16), (uint16_t)ppv, 4, 1);
        if (!p) return -1;
        p[0] = p[1] = 0;
        p[2] = (uint8_t)g->bits;
        p[3] = (uint8_t)(g->bits >> 8);
    }
    c->r[R_AX] = hbm;
    return 10;
}

// WinGGetDIBPointer(hbm, lpbmi) -> void FAR *
// Windows: Where a WinG bitmap's pixels are, and its format into lpbmi when that is not NULL.
// w16: The same: the global block's selector, offset 0; lpbmi gets the BITMAPINFOHEADER.
static int api_WinGGetDIBPointer(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    uint16_t hbm = w16_arg16(a);
    uint32_t bmi = w16_arg32(a);
    struct w16_gdiobj *bm = w16_gdi_obj(w, hbm, GDI_BITMAP);
    c->r[R_AX] = c->r[R_DX] = 0;
    if (!bm || !bm->wing) return 6;
    if (bmi) {
        uint8_t *p = cpu16_ptr(c, (uint16_t)(bmi >> 16), (uint16_t)bmi, 40, 1);
        if (!p) return -1;
        put_header(p, bm->bw, bm->wing->topdown ? -bm->bh : bm->bh, bm->wing->stride * (uint32_t)bm->bh);
    }
    c->r[R_DX] = bm->wing->bits;
    return 6;
}

// WinGGetDIBColorTable(hdc, iStart, nEntries, lprgb) -> UINT
// WinGSetDIBColorTable(hdc, iStart, nEntries, lprgb) -> UINT
// Windows: Read or change entries of the colour table of the WinG bitmap selected into hdc, as
// RGBQUADs; how many were.
// w16: The same. A new table recolours the bitmap's pixels from GDI's next look at them.
static int api_WinGDIBColorTable(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint16_t hdc = w16_arg16(a), start = w16_arg16(a), n = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    struct w16_gdiobj *bm = dc_bitmap(w, hdc);
    c->r[R_AX] = 0;
    if (!bm || start >= 256) return 10;
    if (n > 256 - start) n = (uint16_t)(256 - start);
    uint8_t *p = n ? cpu16_ptr(c, (uint16_t)(far >> 16), (uint16_t)far, 4u * n, ordinal == 1005) : 0;
    if (n && !p) return -1;
    struct w16_wing *g = bm->wing;
    if (ordinal == 1006 && g->pulled) w16_wing_push(w);   // what GDI drew keeps the old table's colours
    for (unsigned i = 0; i < n; i++) {
        uint32_t *t = &g->table[start + i];
        if (ordinal == 1005) {
            p[4 * i] = (uint8_t)*t; p[4 * i + 1] = (uint8_t)(*t >> 8); p[4 * i + 2] = (uint8_t)(*t >> 16);
            p[4 * i + 3] = 0;
        } else {
            *t = w16_vga((uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 1] << 8 | p[4 * i]);
        }
    }
    if (ordinal == 1006) rev_clear(g);
    c->r[R_AX] = n;
    return 10;
}

// The 20 static colours a Windows palette device keeps, its first and last ten.
static const uint32_t STATIC20[20] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0, 0xC0DCC0, 0xA6CAF0,
    0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

// WinGCreateHalfTonePalette() -> HPALETTE
// Windows: WinG's halftone palette: the 20 static colours and a 6 x 6 x 6 colour cube between them,
// what WinGCreateHalfToneBrush dithers with.
// w16: The same 256: the static colours at 0-9 and 246-255, the cube from 10, greys in what is left.
static int api_WinGCreateHalfTonePalette(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    uint32_t pal[256];
    for (int i = 0; i < 10; i++) { pal[i] = STATIC20[i]; pal[246 + i] = STATIC20[10 + i]; }
    for (int i = 0; i < 216; i++)
        pal[10 + i] = (uint32_t)(i / 36 * 51) << 16 | (uint32_t)(i / 6 % 6 * 51) << 8 | (uint32_t)(i % 6 * 51);
    for (int i = 226; i < 246; i++) {
        uint32_t v = (uint32_t)(i - 225) * 12;
        pal[i] = v << 16 | v << 8 | v;
    }
    w->cpu.r[R_AX] = w16_gdi_new_palette(w, pal, 256);
    return 0;
}

// WinGCreateHalfToneBrush(hdc, crColor, ditherType) -> HBRUSH
// Windows: A brush that dithers crColor from the halftone palette's colours, in a 2 x 2, 4 x 4 or
// 8 x 8 pattern.
// w16: A solid brush of crColor itself: w16's display shows any colour, so there is nothing to dither.
static int api_WinGCreateHalfToneBrush(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t cr = w16_arg32(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = w16_gdi_new_brush(w, cr);
    return 8;
}

// WinGStretchBlt(hdcDest, nXDest, nYDest, nWidthDest, nHeightDest, hdcSrc, nXSrc, nYSrc, nWidthSrc, nHeightSrc) -> BOOL
// WinGBitBlt(hdcDest, nXDest, nYDest, nWidth, nHeight, hdcSrc, nXSrc, nYSrc) -> BOOL
// Windows: Copy from a WinGDC to another DC -- the screen, most often -- stretched or not.
// w16: StretchBlt's and BitBlt's SRCCOPY.
static int api_WinGBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cw = (int16_t)w16_arg16(a), ch = (int16_t)w16_arg16(a);
    uint16_t hs = w16_arg16(a);
    int16_t sx = (int16_t)w16_arg16(a), sy = (int16_t)w16_arg16(a);
    if (ordinal == 1009) {
        int16_t sw = (int16_t)w16_arg16(a), sh = (int16_t)w16_arg16(a);
        w->cpu.r[R_AX] = (uint16_t)w16_gdi_stretchblt(w, hd, x, y, cw, ch, hs, sx, sy, sw, sh, 0x00CC0020);
        return 20;
    }
    w->cpu.r[R_AX] = (uint16_t)w16_gdi_bitblt(w, hd, x, y, cw, ch, hs, sx, sy, 0x00CC0020);
    return 16;
}

const struct w16_api g_wing_api[] = {
    { 1001, api_WinGCreateDC },
    { 1002, api_WinGRecommendDIBFormat },
    { 1003, api_WinGCreateBitmap },
    { 1004, api_WinGGetDIBPointer },
    { 1005, api_WinGDIBColorTable },
    { 1006, api_WinGDIBColorTable },
    { 1007, api_WinGCreateHalfTonePalette },
    { 1008, api_WinGCreateHalfToneBrush },
    { 1009, api_WinGBlt },
    { 1010, api_WinGBlt },
};
const unsigned g_wing_api_n = sizeof g_wing_api / sizeof g_wing_api[0];
