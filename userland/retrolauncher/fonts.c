// fonts.c -- the card in Retro Launcher' Options > Display that says which
// of Windows 3.11's own fonts are on drive C:, a row per face: its name,
// file and sizes, and a sample drawn in it ("Win16 real fonts option"
// canvas, A). w16 reads the fonts, in its sandbox; this reads w16
// (`w16 --fonts`, retrolauncher.h), so the launcher still parses nothing hostile.
#include "retrolauncher/retrolauncher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"

struct face {
    char *name;
    char *file;
    char sizes[64];                   // "8 10 12 14 18 24"
    int sw, sh;                       // its sample, black on clear
    uint32_t *sample;
};

struct retro_fontcard {
    int x, y, w, h;
    struct face *f;
    int n;
    int loaded;
};

struct retro_fontcard g_retro_fontcard;

// The faces 3.11 draws a program's chrome and text in: the card names them
// even when missing, since that is what a missing file costs.
static const char *const WANTED[] = { "System", "Fixedsys", "MS Sans Serif" };

static void clear(struct retro_fontcard *c) {
    for (int i = 0; i < c->n; i++) { free(c->f[i].name); free(c->f[i].file); free(c->f[i].sample); }
    free(c->f);
    c->f = 0;
    c->n = 0;
}

static struct face *face_named(struct retro_fontcard *c, const char *name, int add) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->f[i].name, name)) return &c->f[i];
    if (!add) return 0;
    struct face *g = realloc(c->f, sizeof *c->f * (size_t)(c->n + 1));
    if (!g) return 0;
    c->f = g;
    struct face *f = &c->f[c->n];
    memset(f, 0, sizeof *f);
    if (!(f->name = strdup(name))) return 0;
    c->n++;
    return f;
}

void retro_fontcard_load(struct retro_fontcard *c) {
    clear(c);
    for (unsigned i = 0; i < sizeof WANTED / sizeof WANTED[0]; i++) face_named(c, WANTED[i], 1);
    c->loaded = 1;
    char *out = retro_fonts();
    if (!out) return;
    struct face *sample = 0;
    int row = 0;
    for (char *l = out; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char *tab = strchr(l, '\t');
        if (sample && row < sample->sh) {        // a sample's row: its 0s and 1s
            for (int x = 0; x < sample->sw && l[x]; x++)
                if (l[x] == '1') sample->sample[row * sample->sw + x] = 0xFF000000u;
            row++;
        } else if (!strncmp(l, "font ", 5) && tab) {
            // "font FILE POINTS PIXELS WEIGHT<tab>FACE"
            char file[32];
            int pt = 0;
            if (sscanf(l + 5, "%31s %d", file, &pt) == 2) {
                struct face *f = face_named(c, tab + 1, 1);
                if (f) {
                    if (!f->file) f->file = strdup(file);
                    size_t at = strlen(f->sizes);
                    snprintf(f->sizes + at, sizeof f->sizes - at, "%s%d", at ? " " : "", pt);
                }
            }
        } else if (!strncmp(l, "sample ", 7) && tab) {
            int w = 0, h = 0;
            struct face *f = face_named(c, tab + 1, 0);
            if (f && sscanf(l + 7, "%d %d", &w, &h) == 2 && w > 0 && h > 0 && w <= 2048 && h <= 64 &&
                (f->sample = calloc((size_t)w * (size_t)h, sizeof *f->sample))) {
                f->sw = w;
                f->sh = h;
                sample = f;
                row = 0;
            }
        }
        l = nl ? nl + 1 : 0;
    }
    free(out);
}

static int row_h(const struct retro_fontcard *c, int i) {
    int h = ugfx_char_h() + 8;
    return c->f[i].sh + 8 > h ? c->f[i].sh + 8 : h;
}

static void natural(const void *w, int *ow, int *oh) {
    const struct retro_fontcard *c = w;
    int h = 8;
    for (int i = 0; i < c->n; i++) h += row_h(c, i);
    *ow = ugfx_char_w() * 60;
    *oh = h;
}

static void geometry(void *w, int x, int y, int width, int height) {
    struct retro_fontcard *c = w;
    c->x = x; c->y = y; c->w = width; c->h = height;
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct retro_fontcard *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

static void draw(struct ugfx_surface *s, const void *w) {
    const struct retro_fontcard *c = w;
    uint32_t bg = UTHEME_WINDOW_BG, dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int r = utheme_radius(UTHEME_R_CONTROL);
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, r, bg);
    uui_stroke_round_rect(s, c->x, c->y, c->w, c->h, r, UTHEME_OUTLINE);
    int pad = ugfx_char_w(), y = c->y + 4, lh = ugfx_char_h();
    int c1 = c->x + pad, c2 = c1 + pad * 15, c3 = c2 + pad * 14, c4 = c->x + c->w - pad * 9;
    for (int i = 0; i < c->n; i++) {
        const struct face *f = &c->f[i];
        int rh = row_h(c, i), ty = y + (rh - lh) / 2;
        ugfx_draw_string_clipped(s, c1, ty, c2 - c1 - pad, f->name, dim, bg);
        if (f->file) {
            char t[96];
            snprintf(t, sizeof t, "%s  %s pt", f->file, f->sizes);
            ugfx_draw_string_clipped(s, c2, ty, c3 - c2 - pad, t, UTHEME_TEXT, bg);
        }
        if (f->sample) {
            int sw = f->sw < c4 - c3 - pad ? f->sw : c4 - c3 - pad;
            if (sw > 0) ugfx_blit_alpha(s, c3, y + (rh - f->sh) / 2, sw, f->sh, f->sample, f->sw);
        } else if (!f->file) {
            ugfx_draw_string_clipped(s, c3, ty, c4 - c3 - pad, "drawn with its stand-in", dim, bg);
        }
        int found = f->file != 0;
        ugfx_draw_string_clipped(s, c4, ty, c->x + c->w - c4 - pad, found ? "Found" : "Missing",
                                 found ? utheme_action(UTHEME_ACT_CREATE) : utheme_severity(UTHEME_SEV_WARNING), bg);
        y += rh;
    }
}

const struct uui_widget_ops retro_fontcard_ops = {
    .natural_size = natural,
    .set_geometry = geometry,
    .bounds = bounds,
    .draw = draw,
};
