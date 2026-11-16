// font_compare's renderer (tools/dev/font_compare.py builds and runs it):
// one line of text as a grey8 coverage strip, drawn either by SaunaOS's
// own userland/libs/media/ttf.c or by FreeType's light autohinter.
//
// usage: font_compare <ours|ft> <font.ttf> <px> <utf-8 text> <out.raw>
// out.raw is "W H BASELINE\n" then W*H coverage bytes, row-major.
//
// BOTH RENDERERS ARE LAID OUT WITH ttf.c's ADVANCES AND KERNING, so two
// strips differ only in the glyphs' pixels, never in where they sit. ttf.c
// is compiled from the tree as-is; the Python side sets TTF_SUBSAMPLES by
// editing a COPY of ttf.h.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"
#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

static uint32_t next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static void add(uint8_t *img, int W, int H, int x, int y, int v) {
    if (x < 0 || x >= W || y < 0 || y >= H || !v) return;
    int s = img[y * W + x] + v;
    img[y * W + x] = (uint8_t)(s > 255 ? 255 : s);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <ours|ft> <font.ttf> <px> <text> <out.raw>\n", argv[0]);
        return 2;
    }
    int ft = !strcmp(argv[1], "ft");
    int px = atoi(argv[3]);
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) { perror(argv[2]); return 1; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); return 1; }
    fclose(fp);

    struct ttf_font t;
    if (!ttf_open(&t, data, (uint32_t)len)) { fprintf(stderr, "ttf_open refused %s\n", argv[2]); return 1; }
    struct ttf_scratch *sc = malloc(sizeof *sc);
#ifdef HAVE_FREETYPE
    FT_Library lib;
    FT_Face face;
    if (ft) {
        if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[2], 0, &face) ||
            FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px)) { fprintf(stderr, "freetype failed\n"); return 1; }
    }
#else
    if (ft) { fprintf(stderr, "built without FreeType\n"); return 3; }
#endif

    // The line box fontd's atlas uses (font_atlas_plan, not tight), with
    // two rows of air above and below so ink past it shows.
    fx_t scale = ttf_scale_for_px(&t, px);
    int asc = fx_round(fx_mul(fx_from_int(t.ascent), scale));
    int desc = fx_round(fx_mul(fx_from_int(t.descent < 0 ? -t.descent : t.descent), scale));
    int H = asc + desc + 4, base = asc + 2, W = 8192;
    uint8_t *img = calloc((size_t)W * (size_t)H, 1);

    int pen = 2, prev = 0;
    for (const char *s = argv[4]; *s;) {
        uint32_t cp = next_cp(&s);
        int gid = ttf_glyph_index(&t, cp);
        if (prev) pen += ttf_kern_px(&t, prev, gid, px);
        int adv = ttf_advance_px(&t, gid, px);
        if (!ft) {
            int cw = adv + px;
            uint8_t *cell = calloc((size_t)cw * (size_t)H, 1);
            ttf_render_glyph(&t, gid, px, cell, cw, H, 0, base, sc);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < cw; x++) add(img, W, H, pen + x, y, cell[y * cw + x]);
            free(cell);
        }
#ifdef HAVE_FREETYPE
        else {
            FT_Load_Glyph(face, FT_Get_Char_Index(face, cp),
                          FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT);
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT);
            FT_Bitmap *b = &face->glyph->bitmap;
            int ox = pen + face->glyph->bitmap_left, oy = base - face->glyph->bitmap_top;
            for (unsigned y = 0; y < b->rows; y++)
                for (unsigned x = 0; x < b->width; x++)
                    add(img, W, H, ox + (int)x, oy + (int)y, b->buffer[(int)y * b->pitch + (int)x]);
        }
#endif
        pen += adv;
        prev = gid;
    }

    int w = pen + 4 < W ? pen + 4 : W;
    FILE *o = fopen(argv[5], "wb");
    if (!o) { perror(argv[5]); return 1; }
    fprintf(o, "%d %d %d\n", w, H, base);
    for (int y = 0; y < H; y++) fwrite(img + (size_t)y * W, 1, (size_t)w, o);
    fclose(o);
    return 0;
}
