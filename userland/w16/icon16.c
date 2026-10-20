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
