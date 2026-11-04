// icon16.c -- a program's own icon, from its resources: the first
// RT_GROUP_ICON names its pictures, the 32x32 one (else the first) is an
// RT_ICON -- a DIB twice as tall as the icon, its colour rows and then a
// 1-bit AND mask, a set bit transparent. What `w16 --check` hands the
// Retro Launcher, so the launcher itself never parses a program.
//
// THE RESOURCE IS THE FILE'S, ne.c checked its span; every read here is
// checked against that span again before it is made.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// The icon as ARGB pixels (alpha 0 where transparent), at most 32x32 in
// `px`; its size in *w, *h. 0, or -1 when the program has none we read.
int w16_icon(const struct ne_module *m, uint32_t *px, int *w, int *h) {
    struct ne_resource grp;
    int gi = -1;
    for (unsigned i = 0; i < m->nres; i++)
        if (ne_resource_at(m, i, &grp) == 0 && !grp.type_name[0] && grp.type == NE_RT_GROUP_ICON) {
            gi = (int)i;
            break;
        }
    return gi < 0 ? -1 : w16_icon_group(m, &grp, px, w, h);
}

int w16_icon_named(const struct ne_module *m, uint16_t id, const char *name, uint32_t *px, int *w, int *h) {
    struct ne_resource grp;
    if (ne_resource_find(m, NE_RT_GROUP_ICON, 0, id, name, &grp) < 0) return -1;
    return w16_icon_group(m, &grp, px, w, h);
}

int w16_icon_group(const struct ne_module *m, const struct ne_resource *gr, uint32_t *px, int *w, int *h) {
    struct ne_resource grp = *gr, ic;
    if (grp.len < 6) return -1;
    const uint8_t *g = m->img + grp.off;
    unsigned count = u16(g + 4);
    if (count == 0 || 6 + 14u * count > grp.len) return -1;
    // The 32x32 picture with the most colours up to 8 bits, else the first.
    int pick = -1;
    unsigned pick_bits = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = g + 6 + 14 * i;
        unsigned bits = u16(e + 6);
        if (e[0] == 32 && e[1] == 32 && bits <= 8 && (pick < 0 || bits > pick_bits)) {
            pick = (int)i;
            pick_bits = bits;
        }
    }
    if (pick < 0) pick = 0;
    uint16_t id = u16(g + 6 + 14 * pick + 12);
    if (ne_resource_find(m, NE_RT_ICON, 0, id, 0, &ic) < 0) return -1;
    return w16_icon_dib(m->img + ic.off, ic.len, px, w, h);
}

// An icon's picture: a DIB twice as tall as the icon, its colour rows then its AND mask -- an
// RT_ICON's bytes, or an .ICO file's.
int w16_icon_dib(const uint8_t *b, uint32_t len, uint32_t *px, int *w, int *h) {
    struct gdi_dib d;
    if (gdi_dib_header(b, len, 0, &d) < 0 || d.h <= 0) return -1;
    int iw = d.w, ih = d.h / 2;
    if (iw <= 0 || ih <= 0 || iw > 32 || ih > 32) return -1;
    uint32_t and_stride = ((uint32_t)iw + 31) / 32 * 4;
    if (d.header_len + d.stride * (uint32_t)ih + and_stride * (uint32_t)ih > len) return -1;
    const uint8_t *xor_bits = b + d.header_len, *and_bits = xor_bits + d.stride * (uint32_t)ih;
    for (int y = 0; y < ih; y++) {
        const uint8_t *row = xor_bits + (size_t)(ih - 1 - y) * d.stride;
        const uint8_t *mask = and_bits + (size_t)(ih - 1 - y) * and_stride;
        for (int x = 0; x < iw; x++) {
            int clear = mask[x >> 3] >> (7 - (x & 7)) & 1;
            px[y * iw + x] = clear ? 0 : 0xFF000000u | gdi_dib_px(&d, row, x);
        }
    }
    *w = iw;
    *h = ih;
    return 0;
}

// --- icon handles ---------------------------------------------------------------------

// LOADICON'S HANDLE IS A GLOBAL BLOCK, as 3.1's resource handle was (the icon in the layout below),
// so a program may lock it, grow it and write other bits into it -- Program Manager grows one of
// its own icons into each item's from the .GRP's bytes. The table remembers what each handle was
// loaded from: the main thread draws a minimized window's class icon from the module, never from
// the task's memory. The same icon asked twice is one handle.
static struct icon_ref { const struct ne_module *m; uint16_t id; char name[32]; uint16_t handle; } *g_icons;
static int g_nicons;

static int loaded_icon(uint16_t h) {
    for (int i = 0; h && i < g_nicons; i++)
        if (g_icons[i].handle == h) return i;
    return -1;
}

uint16_t w16_icon_handle(struct w16 *w, const struct ne_module *m, uint16_t id, const char *name) {
    struct ne_resource grp;
    if (!m || ne_resource_find(m, NE_RT_GROUP_ICON, 0, id, name, &grp) < 0) return 0;
    for (int i = 0; i < g_nicons; i++)
        if (g_icons[i].m == m && g_icons[i].id == id && !strcmp(g_icons[i].name, name ? name : "")) return g_icons[i].handle;
    static uint32_t px[32 * 32];
    int iw, ih;
    if (w16_icon_group(m, &grp, px, &iw, &ih) < 0) return 0;
    struct icon_ref *g = realloc(g_icons, sizeof *g * (size_t)(g_nicons + 1));
    if (!g) return 0;
    g_icons = g;
    uint16_t h = w16_icon_block_new(w, px, iw, ih);
    if (!h) return 0;
    g_icons[g_nicons] = (struct icon_ref){ m, id, "", h };
    snprintf(g_icons[g_nicons].name, sizeof g_icons[0].name, "%s", name ? name : "");
    g_nicons++;
    return h;
}

int w16_icon_of_handle(uint16_t h, uint32_t *px, int *w, int *hh) {
    int i = loaded_icon(h);
    if (i < 0) return -1;
    const struct icon_ref *r = &g_icons[i];
    return w16_icon_named(r->m, r->id, r->name[0] ? r->name : 0, px, w, hh);
}

// --- a program's own cursors --------------------------------------------------------
//
// An RT_GROUP_CURSOR names RT_CURSORs, each a hotspot (two words) and then a
// monochrome DIB twice as tall as the cursor: its XOR rows, then its AND
// rows. Per pixel AND 1 XOR 0 leaves the screen, 1 1 inverts it, 0 0 is
// black and 0 1 white -- what w16_cursor_image() hands toywm, which draws
// the inverting ones itself (abi/win_proto.h, WIN_REQ_CURSOR_IMAGE).
//
// LOADED AS 3.1 HELD IT: an HCURSOR is a global block, a CURSORICONINFO
// (the hotspot, width, height, bytes a row, planes, bits a pixel) and then
// its AND rows and its XOR rows, top down. A program may GlobalLock it and
// write new bits -- Paintbrush draws its brush's outline into DUMMY so --
// so SetCursor and SetClassWord re-read the block (w16_cursor_refresh()),
// and a cursor whose bits changed gets a new `gen` for the UI thread to
// define again. The table is the task thread's to grow and the UI thread's
// to read, both under g_cursor_lock; the blocks themselves only the task
// thread touches.
#define CII_LEN 12                  // sizeof CURSORICONINFO
struct w16_own_cursor {
    const struct ne_module *m;
    uint16_t id;
    char name[32];
    uint16_t handle;                // the global block
    uint32_t gen;                   // bumped each time the bits change
    int w, h, hot_x, hot_y;
    uint8_t px[32 * 32];            // 0 clear, 1 invert, 2 black, 3 white
};
static struct w16_own_cursor *g_cursors;
static int g_ncursors;
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;

// The resource's bits as a CURSORICONINFO block: 0, or -1 unreadable.
static int cursor_block(const struct ne_module *m, const struct ne_resource *gr, uint8_t *out, uint32_t cap,
                        uint32_t *len) {
    if (gr->len < 6) return -1;
    const uint8_t *g = m->img + gr->off;
    unsigned count = u16(g + 4);
    if (count == 0 || 6 + 14u * count > gr->len) return -1;
    // The 32x32 one (its height is given doubled, as the DIB's is), else the first.
    unsigned pick = 0;
    for (unsigned i = 0; i < count; i++)
        if (u16(g + 6 + 14 * i) == 32 && u16(g + 6 + 14 * i + 2) == 64) { pick = i; break; }
    struct ne_resource cr;
    if (ne_resource_find(m, NE_RT_CURSOR, 0, u16(g + 6 + 14 * pick + 12), 0, &cr) < 0 || cr.len < 4) return -1;
    const uint8_t *b = m->img + cr.off;
    struct gdi_dib d;
    if (gdi_dib_header(b + 4, cr.len - 4, 0, &d) < 0 || d.bpp != 1 || d.h <= 0) return -1;
    int cw = d.w, ch = d.h / 2;
    if (cw <= 0 || ch <= 0 || cw > 32 || ch > 32) return -1;
    if (4 + d.header_len + 2 * d.stride * (uint32_t)ch > cr.len) return -1;
    unsigned wb = ((unsigned)cw + 15) / 16 * 2;  // a row, word-aligned as a DDB's
    if (CII_LEN + 2 * wb * (unsigned)ch > cap) return -1;
    const uint8_t *xor_bits = b + 4 + d.header_len, *and_bits = xor_bits + d.stride * (uint32_t)ch;
    memset(out, 0, CII_LEN + 2 * wb * (unsigned)ch);
    out[0] = b[0]; out[1] = b[1];                // ptHotSpot
    out[2] = b[2]; out[3] = b[3];
    out[4] = (uint8_t)cw;                        // nWidth, nHeight, nWidthBytes
    out[6] = (uint8_t)ch;
    out[8] = (uint8_t)wb;
    out[10] = 1;                                 // bPlanes, bBitsPerPixel
    out[11] = 1;
    for (int y = 0; y < ch; y++) {
        memcpy(out + CII_LEN + (unsigned)y * wb, and_bits + (size_t)(ch - 1 - y) * d.stride, (cw + 7) / 8);
        memcpy(out + CII_LEN + wb * (unsigned)ch + (unsigned)y * wb, xor_bits + (size_t)(ch - 1 - y) * d.stride,
               (cw + 7) / 8);
    }
    *len = CII_LEN + 2 * wb * (unsigned)ch;
    return 0;
}

// A CURSORICONINFO block's pixels into `c`: 1 if they differ from what it
// held, 0 if the same, -1 if the block is not a cursor w16 can show.
static int cursor_parse(const uint8_t *b, uint32_t len, struct w16_own_cursor *c) {
    if (len < CII_LEN) return -1;
    int cw = u16(b + 4), ch = u16(b + 6), wb = u16(b + 8);
    if (cw <= 0 || ch <= 0 || cw > 32 || ch > 32 || b[10] != 1 || b[11] != 1 || wb * 8 < cw ||
        CII_LEN + 2u * (unsigned)wb * (unsigned)ch > len) return -1;
    uint8_t px[32 * 32];
    const uint8_t *and_bits = b + CII_LEN, *xor_bits = and_bits + wb * ch;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            int an = and_bits[y * wb + (x >> 3)] >> (7 - (x & 7)) & 1;
            int xo = xor_bits[y * wb + (x >> 3)] >> (7 - (x & 7)) & 1;
            px[y * cw + x] = (uint8_t)(an ? xo : 2 + xo);
        }
    int hx = u16(b) < cw ? u16(b) : 0, hy = u16(b + 2) < ch ? u16(b + 2) : 0;
    int same = c->w == cw && c->h == ch && c->hot_x == hx && c->hot_y == hy && !memcmp(c->px, px, (size_t)(cw * ch));
    if (same) return 0;
    c->w = cw;
    c->h = ch;
    c->hot_x = hx;
    c->hot_y = hy;
    memcpy(c->px, px, (size_t)(cw * ch));
    return 1;
}

static int cursor_find(uint16_t h) {
    for (int i = 0; i < g_ncursors; i++)
        if (g_cursors[i].handle == h) return i;
    return -1;
}

uint16_t w16_cursor_handle(struct w16 *w, const struct ne_module *m, uint16_t id, const char *name) {
    struct ne_resource grp;
    if (!m || ne_resource_find(m, NE_RT_GROUP_CURSOR, 0, id, name, &grp) < 0) return 0;
    pthread_mutex_lock(&g_cursor_lock);
    for (int i = 0; i < g_ncursors; i++)
        if (g_cursors[i].m == m && g_cursors[i].id == id && !strcmp(g_cursors[i].name, name ? name : "")) {
            uint16_t h = g_cursors[i].handle;
            pthread_mutex_unlock(&g_cursor_lock);
            return h;
        }
    pthread_mutex_unlock(&g_cursor_lock);
    uint8_t blk[CII_LEN + 2 * 4 * 32];
    uint32_t len;
    if (cursor_block(m, &grp, blk, sizeof blk, &len) < 0) return 0;
    uint16_t h = w16_global_alloc(w, len, blk);   // outside the lock: the task's memory, not the table
    if (!h) return 0;
    pthread_mutex_lock(&g_cursor_lock);
    struct w16_own_cursor *g = realloc(g_cursors, sizeof *g * (size_t)(g_ncursors + 1));
    if (g) {
        g_cursors = g;
        struct w16_own_cursor *c = &g_cursors[g_ncursors];
        memset(c, 0, sizeof *c);
        c->m = m;
        c->id = id;
        c->handle = h;
        c->gen = 1;
        snprintf(c->name, sizeof c->name, "%s", name ? name : "");
        cursor_parse(blk, len, c);
        g_ncursors++;
    }
    pthread_mutex_unlock(&g_cursor_lock);
    if (!g) {
        w16_global_free(w, h);
        return 0;
    }
    return h;
}

int w16_cursor_index(uint16_t h) {
    pthread_mutex_lock(&g_cursor_lock);
    int i = h ? cursor_find(h) : -1;
    pthread_mutex_unlock(&g_cursor_lock);
    return i;
}

void w16_cursor_refresh(struct w16 *w, uint16_t h) {
    if (w16_cursor_index(h) < 0) return;
    uint32_t len = 0;
    const uint8_t *b = w16_global_ptr(w, h, &len);
    if (!b) return;
    uint8_t copy[CII_LEN + 2 * 4 * 32];          // the block is the program's: read it once
    if (len > sizeof copy) len = sizeof copy;
    memcpy(copy, b, len);
    pthread_mutex_lock(&g_cursor_lock);
    int i = cursor_find(h);
    if (i >= 0) {
        struct w16_own_cursor t = g_cursors[i];
        if (cursor_parse(copy, len, &t) == 1) {
            t.gen++;
            g_cursors[i] = t;
        }
    }
    pthread_mutex_unlock(&g_cursor_lock);
}

int w16_cursor_image(int index, int scale_num, int scale_den, uint32_t *argb, uint8_t *inv,
                     int *w, int *h, int *hot_x, int *hot_y, uint32_t *gen) {
    pthread_mutex_lock(&g_cursor_lock);
    if (index < 0 || index >= g_ncursors) {
        pthread_mutex_unlock(&g_cursor_lock);
        return -1;
    }
    const struct w16_own_cursor *c = &g_cursors[index];
    // Nearest neighbour at the program's scale, as its window's pixels are.
    int dw = c->w * scale_num / scale_den, dh = c->h * scale_num / scale_den;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > W16_CURSOR_IMG_MAX) dw = W16_CURSOR_IMG_MAX;
    if (dh > W16_CURSOR_IMG_MAX) dh = W16_CURSOR_IMG_MAX;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int sx = x * scale_den / scale_num, sy = y * scale_den / scale_num;
            uint8_t p = c->px[(sy < c->h ? sy : c->h - 1) * c->w + (sx < c->w ? sx : c->w - 1)];
            argb[y * dw + x] = p == 2 ? 0xFF000000u : p == 3 ? 0xFFFFFFFFu : 0;
            inv[y * dw + x] = p == 1;
        }
    *w = dw;
    *h = dh;
    *hot_x = c->hot_x * scale_num / scale_den;
    *hot_y = c->hot_y * scale_num / scale_den;
    if (*hot_x >= dw) *hot_x = dw - 1;
    if (*hot_y >= dh) *hot_y = dh - 1;
    *gen = c->gen;
    pthread_mutex_unlock(&g_cursor_lock);
    return 0;
}

// --- an icon as 3.1 held one: a global block ------------------------------------------
//
// THE HICON IS A GLOBAL BLOCK, as in 3.1: a CURSORICONINFO, then the AND rows, then the XOR rows,
// top down, each row word-aligned. Program Manager depends on it -- it GlobalLocks what
// ExtractIcon gave, DumpIcon finds the bits, and the bytes go into its .GRP file; reading the file
// back it GlobalAllocs a block of the same bytes and uses that as the HICON. w16 makes its blocks as
// 3.1's VGA driver did -- four planes of one bit (bPlanes 4, bBitsPerPixel 1), each row the four
// planes' bytes in turn, a pixel's four bits its colour in Windows' default palette, bit 0 red's --
// so a .GRP either writes is one the other reads. It reads 1 bit and 4 bits a pixel too.

static const uint32_t PAL16[16] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };

static unsigned nearest16(uint32_t rgb) {
    unsigned best = 0;
    long bd = -1;
    for (unsigned i = 0; i < 16; i++) {
        long dr = (long)((rgb >> 16) & 0xFF) - (long)((PAL16[i] >> 16) & 0xFF);
        long dg = (long)((rgb >> 8) & 0xFF) - (long)((PAL16[i] >> 8) & 0xFF);
        long db = (long)(rgb & 0xFF) - (long)(PAL16[i] & 0xFF);
        long d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

uint32_t w16_icon_block_len(int iw, int ih) {
    uint32_t wb = ((uint32_t)iw + 15) / 16 * 2;  // a plane's row, word-aligned
    return CII_LEN + (uint32_t)ih * wb + (uint32_t)ih * wb * 4;
}

// ARGB pixels (alpha 0 clear) as a block's bytes, in `out` (w16_icon_block_len()).
void w16_icon_block_fill(uint8_t *out, const uint32_t *px, int iw, int ih) {
    unsigned wb = ((unsigned)iw + 15) / 16 * 2;
    memset(out, 0, w16_icon_block_len(iw, ih));
    out[0] = (uint8_t)(iw / 2);                  // ptHotSpot: the middle, as 3.1 gave an icon
    out[2] = (uint8_t)(ih / 2);
    out[4] = (uint8_t)iw;
    out[6] = (uint8_t)ih;
    out[8] = (uint8_t)wb;                        // nWidthBytes: a plane's row
    out[10] = 4;
    out[11] = 1;
    uint8_t *and_bits = out + CII_LEN, *xor_bits = and_bits + wb * (unsigned)ih;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            uint32_t p = px[y * iw + x];
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            if (!(p >> 24)) { and_bits[y * wb + (x >> 3)] |= bit; continue; }
            unsigned v = nearest16(p & 0xFFFFFF);
            for (int k = 0; k < 4; k++)
                if (v >> k & 1) xor_bits[((unsigned)y * 4 + (unsigned)k) * wb + (x >> 3)] |= bit;
        }
}

uint16_t w16_icon_block_new(struct w16 *w, const uint32_t *px, int iw, int ih) {
    uint32_t len = w16_icon_block_len(iw, ih);
    uint8_t *b = malloc(len);
    if (!b) return 0;
    w16_icon_block_fill(b, px, iw, ih);
    uint16_t h = w16_global_alloc(w, len, b);
    free(b);
    return h;
}

// A block's picture as ARGB (alpha 0 clear; a pixel 3.1 inverted the screen with is drawn clear),
// at most 64x64: 0, or -1 when the bytes are no icon w16 reads.
int w16_icon_block_argb(const uint8_t *b, uint32_t len, uint32_t *px, int *pw, int *ph) {
    if (len < CII_LEN) return -1;
    int iw = u16(b + 4), ih = u16(b + 6), wx = u16(b + 8), planes = b[10], bpp = b[11];
    if (iw <= 0 || ih <= 0 || iw > 64 || ih > 64) return -1;
    int wa = (iw + 15) / 16 * 2;
    if (!((planes == 1 && (bpp == 1 || bpp == 4)) || (planes == 4 && bpp == 1))) return -1;
    if (wx * 8 < iw * (planes == 1 ? bpp : 1)) return -1;
    uint32_t need = CII_LEN + (uint32_t)wa * (uint32_t)ih + (uint32_t)wx * (uint32_t)ih * (uint32_t)(planes == 4 ? 4 : 1);
    if (need > len) return -1;
    const uint8_t *and_bits = b + CII_LEN, *xor_bits = and_bits + wa * ih;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            int clear = and_bits[y * wa + (x >> 3)] >> (7 - (x & 7)) & 1;
            unsigned v;
            uint32_t rgb;
            if (planes == 4) {
                v = 0;
                for (int p = 0; p < 4; p++) v |= (unsigned)(xor_bits[(y * 4 + p) * wx + (x >> 3)] >> (7 - (x & 7)) & 1) << p;
                rgb = PAL16[v];
            } else if (bpp == 4) {
                uint8_t byte = xor_bits[y * wx + (x >> 1)];
                v = x & 1 ? byte & 0xF : byte >> 4;
                rgb = PAL16[v];
            } else {
                v = xor_bits[y * wx + (x >> 3)] >> (7 - (x & 7)) & 1;
                rgb = v ? 0xFFFFFF : 0;
            }
            px[y * iw + x] = clear ? 0 : 0xFF000000u | rgb;
        }
    *pw = iw;
    *ph = ih;
    return 0;
}

// Any icon handle's picture: LoadIcon's, or a block (a program's own made from .GRP bytes too).
// Its block first: the program may have written other bits into it.
int w16_icon_any(struct w16 *w, uint16_t h, uint32_t *px, int *iw, int *ih) {
    if (w16_cursor_index(h) >= 0) return -1;     // a cursor is DrawIcon's other path
    uint32_t len = 0;
    const uint8_t *b = w16_global_ptr(w, h, &len);
    if (b && w16_icon_block_argb(b, len, px, iw, ih) == 0) return 0;
    return w16_icon_of_handle(h, px, iw, ih);
}

// A block for any icon handle, as CopyIcon makes one: 0 when it names none.
uint16_t w16_icon_copy(struct w16 *w, uint16_t h) {
    static uint32_t px[64 * 64];
    int iw, ih;
    if (w16_icon_any(w, h, px, &iw, &ih) < 0) return 0;
    return w16_icon_block_new(w, px, iw, ih);
}

// CopyIcon(hInstance, hIcon) -> HICON
// Windows: A new icon, a copy of one: a block of the program's own to change or keep.
// w16: The same, as a block in 3.1's layout whatever the icon came from.
static int api_CopyIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w->cpu.r[R_AX] = w16_icon_copy(w, w16_arg16(a));
    return 4;
}

// DestroyIcon(hIcon) -> BOOL
// Windows: Frees an icon CreateIcon, CopyIcon or ExtractIcon made.
// w16: The same: a block is freed; LoadIcon's own icons are the module's, and stay.
static int api_DestroyIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t len = 0;
    int block = loaded_icon(h) < 0 && w16_cursor_index(h) < 0 && w16_global_ptr(w, h, &len) != 0;
    if (block) w16_global_free(w, h);
    w->cpu.r[R_AX] = 1;
    return 2;
}

// DumpIcon(lpIcon, lpcbHeader, lplpXorBits, lplpAndBits) -> DWORD
// Windows: Finds the parts of an icon in memory -- Program Manager's, to keep an icon's bits in
// its .GRP file: the header's size with the bits' and far pointers to the XOR and AND rows; the
// XOR rows' size in both words.
// w16: The same, from the block's CURSORICONINFO.
static int api_DumpIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t lp = w16_arg32(a), lplen = w16_arg32(a), lpxor = w16_arg32(a), lpand = w16_arg32(a);
    struct cpu16 *c = &w->cpu;
    c->r[R_AX] = c->r[R_DX] = 0;
    const uint8_t *b = cpu16_ptr(c, (uint16_t)(lp >> 16), (uint16_t)lp, CII_LEN, 0);
    if (!b) return 16;
    unsigned iw = u16(b + 4), ih = u16(b + 6), wx = u16(b + 8);
    unsigned sx = ih * wx * (b[10] ? b[10] : 1), sa = ih * ((iw + 15) / 16 * 2);
    uint32_t pand = lp + CII_LEN, pxor = lp + CII_LEN + sa;
    uint16_t total = (uint16_t)(CII_LEN + sa + sx);
    uint8_t *p;
    if ((lpand >> 16) && (p = cpu16_ptr(c, (uint16_t)(lpand >> 16), (uint16_t)lpand, 4, 1))) memcpy(p, &pand, 4);
    if ((lpxor >> 16) && (p = cpu16_ptr(c, (uint16_t)(lpxor >> 16), (uint16_t)lpxor, 4, 1))) memcpy(p, &pxor, 4);
    if ((lplen >> 16) && (p = cpu16_ptr(c, (uint16_t)(lplen >> 16), (uint16_t)lplen, 2, 1))) memcpy(p, &total, 2);
    c->r[R_AX] = c->r[R_DX] = (uint16_t)sx;
    return 16;
}

// GetInternalIconHeader(lpIcon, lpDestIcon) -> void
// Windows: Undocumented: the CURSORICONINFO header of an icon in memory, copied -- Program Manager's.
// w16: Copies the header when lpIcon holds one w16 can read; else leaves lpDestIcon as it was.
static int api_GetInternalIconHeader(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t src = w16_arg32(a), dst = w16_arg32(a);
    struct cpu16 *c = &w->cpu;
    const uint8_t *b = cpu16_ptr(c, (uint16_t)(src >> 16), (uint16_t)src, CII_LEN, 0);
    uint8_t *d = b ? cpu16_ptr(c, (uint16_t)(dst >> 16), (uint16_t)dst, CII_LEN, 1) : 0;
    unsigned iw = b ? u16(b + 4) : 0, ih = b ? u16(b + 6) : 0;
    if (d && iw && ih && iw <= 64 && ih <= 64) memmove(d, b, CII_LEN);
    return 8;
}

const struct w16_api g_icon_api[] = {
    { 368, api_CopyIcon },
    { 372, api_GetInternalIconHeader },
    { 457, api_DestroyIcon },
    { 459, api_DumpIcon },
};
const unsigned g_icon_api_n = sizeof g_icon_api / sizeof g_icon_api[0];
