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
    const uint8_t *b = m->img + ic.off;
    struct gdi_dib d;
    if (gdi_dib_header(b, ic.len, 0, &d) < 0 || d.h <= 0) return -1;
    int iw = d.w, ih = d.h / 2;
    if (iw <= 0 || ih <= 0 || iw > 32 || ih > 32) return -1;
    uint32_t and_stride = ((uint32_t)iw + 31) / 32 * 4;
    if (d.header_len + d.stride * (uint32_t)ih + and_stride * (uint32_t)ih > ic.len) return -1;
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

// LoadIcon's handles: ICON_BASE + the index of what it named, a module's
// icon group by number or name. Grown; the same icon asked twice is one handle.
#define ICON_BASE 0x4800
static struct icon_ref { const struct ne_module *m; uint16_t id; char name[32]; } *g_icons;
static int g_nicons;

uint16_t w16_icon_handle(const struct ne_module *m, uint16_t id, const char *name) {
    struct ne_resource grp;
    if (!m || ne_resource_find(m, NE_RT_GROUP_ICON, 0, id, name, &grp) < 0) return 0;
    for (int i = 0; i < g_nicons; i++)
        if (g_icons[i].m == m && g_icons[i].id == id && !strcmp(g_icons[i].name, name ? name : ""))
            return (uint16_t)(ICON_BASE + i);
    if (g_nicons >= 0x7FF) return 0;              // the handle range is full
    struct icon_ref *g = realloc(g_icons, sizeof *g * (size_t)(g_nicons + 1));
    if (!g) return 0;
    g_icons = g;
    g_icons[g_nicons] = (struct icon_ref){ m, id, "" };
    snprintf(g_icons[g_nicons].name, sizeof g_icons[0].name, "%s", name ? name : "");
    return (uint16_t)(ICON_BASE + g_nicons++);
}

int w16_icon_of_handle(uint16_t h, uint32_t *px, int *w, int *hh) {
    if (h < ICON_BASE || h >= ICON_BASE + g_nicons) return -1;
    const struct icon_ref *r = &g_icons[h - ICON_BASE];
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
