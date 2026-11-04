// Picture headers: QOI (the 14-byte header, the chunks, the end
// marker) and BMP (the file header, the DIB header, the palette and the
// pixels) -- the layouts lib/uimg's decoders read, here only described.
#include "base/ubinmap_internal.h"

#include <stdio.h>
#include <string.h>

int ubinmap_qoi(struct ubinmap_ctx *c) {
    uint8_t h[14];
    if (bm_read(c, 0, h, 14) || memcmp(h, "qoif", 4)) return 0;
    c->be = 1;
    uint32_t w = bm_get32(c, h + 4), ht = bm_get32(c, h + 8);
    unsigned ch = h[12], cs = h[13];
    bm_format(c, "QOI picture, %u x %u, %s", (unsigned)w, (unsigned)ht, ch == 4 ? "RGBA" : "RGB");
    bm_add(c, 0, 0, 14, 1, "Header", "%u x %u", (unsigned)w, (unsigned)ht);
    bm_add(c, 1, 0, 4, 0, "Magic", "qoif");
    bm_add(c, 1, 4, 4, 0, "Width", "%u", (unsigned)w);
    bm_add(c, 1, 8, 4, 0, "Height", "%u", (unsigned)ht);
    bm_add(c, 1, 12, 1, 0, "Channels", "%u (%s)", ch, ch == 4 ? "RGBA" : ch == 3 ? "RGB" : "invalid");
    bm_add(c, 1, 13, 1, 0, "Colour space", "%s", cs == 0 ? "sRGB" : cs == 1 ? "linear" : "invalid");
    uint64_t size = c->src->size;
    static const uint8_t end[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    uint8_t tail[8];
    int marked = size >= 22 && !bm_read(c, size - 8, tail, 8) && !memcmp(tail, end, 8);
    bm_add(c, 0, 14, size > 14 ? size - 14 - (marked ? 8 : 0) : 0, 2, "Pixel data", "chunks");
    if (marked) bm_add(c, 0, size - 8, 8, 3, "End marker", "00 x7, 01");
    else bm_note(c, "the file does not end with QOI's end marker");
    return 1;
}

static const char *bmp_compression(uint32_t v) {
    switch (v) {
    case 0: return "none";
    case 1: return "RLE8";
    case 2: return "RLE4";
    case 3: return "bit fields";
    case 4: return "JPEG";
    case 5: return "PNG";
    case 6: return "alpha bit fields";
    default: return "unknown";
    }
}

int ubinmap_bmp(struct ubinmap_ctx *c) {
    uint8_t h[54];
    if (bm_read(c, 0, h, 18) || h[0] != 'B' || h[1] != 'M') return 0;
    c->be = 0;
    uint32_t fsize = bm_get32(c, h + 2), pix = bm_get32(c, h + 10), dib = bm_get32(c, h + 14);
    if (dib != 12 && dib != 40 && dib != 52 && dib != 56 && dib != 64 && dib != 108 && dib != 124) return 0;
    bm_add(c, 0, 0, 14, 1, "File header", "14 bytes");
    bm_add(c, 1, 0, 2, 0, "Magic", "BM");
    bm_add(c, 1, 2, 4, 0, "File size", "%u", (unsigned)fsize);
    bm_add(c, 1, 10, 4, 0, "Pixels at", "0x%X", (unsigned)pix);
    if (fsize != c->src->size) bm_note(c, "the header says %u bytes; the file has %llu", (unsigned)fsize,
                                       (unsigned long long)c->src->size);
    int32_t w = 0, ht = 0;
    unsigned bpp = 0;
    uint32_t comp = 0, colours = 0;
    bm_add(c, 0, 14, dib, 2, "DIB header", "%u bytes", (unsigned)dib);
    bm_add(c, 1, 14, 4, 0, "Header size", "%u", (unsigned)dib);
    if (dib == 12) {
        uint8_t d[12];
        if (!bm_read(c, 14, d, 12)) {
            w = bm_get16(c, d + 4);
            ht = bm_get16(c, d + 6);
            bpp = bm_get16(c, d + 10);
        }
        bm_add(c, 1, 18, 2, 0, "Width", "%d", (int)w);
        bm_add(c, 1, 20, 2, 0, "Height", "%d", (int)ht);
        bm_add(c, 1, 24, 2, 0, "Bits per pixel", "%u", bpp);
    } else if (!bm_read(c, 14, h + 14, 40)) {
        w = (int32_t)bm_get32(c, h + 18);
        ht = (int32_t)bm_get32(c, h + 22);
        bpp = bm_get16(c, h + 28);
        comp = bm_get32(c, h + 30);
        colours = bm_get32(c, h + 46);
        bm_add(c, 1, 18, 4, 0, "Width", "%d", (int)w);
        bm_add(c, 1, 22, 4, 0, "Height", "%d%s", (int)ht, ht < 0 ? " (top-down)" : "");
        bm_add(c, 1, 26, 2, 0, "Planes", "%u", bm_get16(c, h + 26));
        bm_add(c, 1, 28, 2, 0, "Bits per pixel", "%u", bpp);
        bm_add(c, 1, 30, 4, 0, "Compression", "%s", bmp_compression(comp));
        bm_add(c, 1, 34, 4, 0, "Image size", "%u", (unsigned)bm_get32(c, h + 34));
        bm_add(c, 1, 46, 4, 0, "Colours used", "%u", (unsigned)colours);
    } else {
        bm_note(c, "the file ends inside the DIB header");
    }
    bm_format(c, "BMP picture, %d x %d, %u-bit", (int)w, (int)(ht < 0 ? -ht : ht), bpp);
    uint64_t pal = 14 + (uint64_t)dib;
    if (pix > pal) bm_add(c, 0, pal, pix - pal, 3, "Colour table", "%llu entries",
                          (unsigned long long)((pix - pal) / (dib == 12 ? 3 : 4)));
    if (pix >= c->src->size) bm_note(c, "the pixels' offset is past the end of the file");
    else bm_add(c, 0, pix, c->src->size - pix, 4, "Pixels", "%llu bytes", (unsigned long long)(c->src->size - pix));
    return 1;
}
