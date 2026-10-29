// Cursor-set and picture tiles -- see ui/uui_tiles.h.
#include "ui/uui_tiles.h"

#include <stdio.h>
#include <string.h>

#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "lib/ulivewall.h"   // LIVEWALL_VIDEO_EXT
#include "lib/uvid.h"

static const char *const CURSOR_SAMPLES[UUI_TILE_CURSOR_SAMPLES] = {
    "arrow", "text", "hand", "wait", "resize-diag"
};

void uui_cursor_tiles_reset(struct uui_cursor_tiles *c) {
    for (int t = 0; t < UUI_TILES_MAX; t++) {
        for (int k = 0; k < UUI_TILE_CURSOR_SAMPLES; k++) ucursor_release(&c->shape[t][k]);
        c->loaded[t] = 0;
        c->theme[t][0] = '\0';
    }
}

static void load_theme(struct uui_cursor_tiles *c, int t, const char *theme) {
    for (int k = 0; k < UUI_TILE_CURSOR_SAMPLES; k++) {
        ucursor_release(&c->shape[t][k]);   // parse overwrites `px`: free it first
        ucursor_load(theme, CURSOR_SAMPLES[k], 100, &c->shape[t][k]);
    }
    strlcpy(c->theme[t], theme, sizeof c->theme[t]);
    c->loaded[t] = 1;
}

void uui_cursor_tile(struct ugfx_surface *s, struct uui_cursor_tiles *c, int slot,
                     const char *theme, int x, int y, int w, int h) {
    int r = ugfx_char_h() / 3, half = h / 2;
    uui_fill_round_rect(s, x, y, w, h, r, UUI_TILE_DARK);
    uui_fill_round_rect(s, x, y, w, half + r, r, UTHEME_WINDOW_BG);
    ugfx_fill_rect(s, x, y + half - r, w, r, UTHEME_WINDOW_BG);   // square the seam
    ugfx_fill_rect(s, x, y + half, w, r, UUI_TILE_DARK);

    if (slot < 0 || slot >= UUI_TILES_MAX || !theme) return;
    if (!c->loaded[slot] || strcmp(c->theme[slot], theme) != 0) load_theme(c, slot, theme);

    // Each shape centred in its fifth of the tile, straddling the seam.
    for (int k = 0; k < UUI_TILE_CURSOR_SAMPLES; k++) {
        const struct cursor_shape *cs = &c->shape[slot][k];
        if (!cs->loaded) continue;
        int sx = x + (2 * k + 1) * w / (2 * UUI_TILE_CURSOR_SAMPLES) - cs->w / 2;
        int sy = y + half - cs->h / 2;
        for (int dy = 0; dy < cs->h; dy++)
            for (int dx = 0; dx < cs->w; dx++) {
                uint32_t p = ucursor_pixel(cs, dx, dy, 1, 0xFFFFFF);
                if (p >> 24) ugfx_blend_pixel(s, sx + dx, sy + dy, p & 0xFFFFFF, (uint8_t)(p >> 24));
            }
    }
}

void uui_picture_tiles_reset(struct uui_picture_tiles *c) {
    for (int i = 0; i < UUI_TILES_MAX; i++) {
        uimg_free(&c->img[i]);
        c->name[i][0] = '\0';
    }
}

void uui_picture_tile(struct ugfx_surface *s, struct uui_picture_tiles *c, int slot,
                      const char *name, int x, int y, int w, int h,
                      uui_tile_loader load, void *ctx) {
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 3, UUI_TILE_DARK);
    if (slot < 0 || slot >= UUI_TILES_MAX || !name || w <= 0 || h <= 0) return;
    struct uimg *p = &c->img[slot];
    if (strcmp(c->name[slot], name) != 0 || p->w < w || p->h < h) {
        uimg_free(p);
        strlcpy(c->name[slot], name, sizeof c->name[slot]);
        load(name, w, h, p, ctx);
    }
    if (!p->px) return;
    int ox = (p->w - w) / 2, oy = (p->h - h) / 2;
    ugfx_blit(s, x, y, w, h, p->px + (size_t)oy * (size_t)p->w + ox, p->w);
}

int uui_tile_load_cover(const char *dir, const char *stem, int w, int h, struct uimg *out) {
    static const char *const EXT[] = { "jpg", "gif", "png", "qoi" };
    struct uimg full;
    memset(&full, 0, sizeof full);
    char path[128];
    for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0]; e++) {
        snprintf(path, sizeof path, "%s/%s.%s", dir, stem, EXT[e]);
        if (uimg_load(path, &full) == 0) break;
    }
    // A VIDEO stands for itself by a frame a second in.
    for (int e = 0; !full.px && LIVEWALL_VIDEO_EXT[e]; e++) {
        snprintf(path, sizeof path, "%s/%s.%s", dir, stem, LIVEWALL_VIDEO_EXT[e]);
        if (uvid_still(path, 1000, 0, 0, &full) != 0) memset(&full, 0, sizeof full);
    }
    int ok = 0;
    if (full.px && full.w > 0 && full.h > 0) {
        int sw = w, sh = (int)((int64_t)full.h * w / full.w);
        if (sh < h) { sh = h; sw = (int)((int64_t)full.w * h / full.h); }
        ok = uimg_scale(&full, sw, sh, out) == 0;
    }
    uimg_free(&full);
    return ok;
}

int uui_tile_load_dir(const char *name, int w, int h, struct uimg *out, void *dir) {
    return uui_tile_load_cover(dir, name, w, h, out);
}
