// See include/zlib.h: the corner of zlib NetSurf calls, over uinflate.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "lib/uinflate.h"

// What inflate() decoded on its first call, handed out across calls.
struct ns_inflate {
    int wbits;
    int decoded;
    uint8_t *buf;
    size_t len, pos, cap;
};

static int grow(void *ctx, const uint8_t *data, size_t n) {
    struct ns_inflate *z = ctx;
    if (z->len + n > z->cap) {
        size_t cap = z->cap ? z->cap : 16384;
        while (cap < z->len + n) cap *= 2;
        uint8_t *b = realloc(z->buf, cap);
        if (!b) return 1;
        z->buf = b;
        z->cap = cap;
    }
    memcpy(z->buf + z->len, data, n);
    z->len += n;
    return 0;
}

static enum uinflate_wrap wrap_for(int wbits, const uint8_t *s, size_t n) {
    if (wbits < 0) return UINFLATE_RAW;
    if (wbits >= 32) return (n >= 2 && s[0] == 0x1f && s[1] == 0x8b) ? UINFLATE_GZIP : UINFLATE_ZLIB;
    if (wbits >= 16) return UINFLATE_GZIP;
    return UINFLATE_ZLIB;
}

// Decodes the whole of `src`; 0, or a negative errno from uinflate().
static int decode_all(struct ns_inflate *z, const uint8_t *src, size_t n, enum uinflate_wrap wrap) {
    struct uinflate_scratch *sc = malloc(sizeof *sc);
    if (!sc) return -ENOMEM;
    size_t out_len = 0;
    int r = uinflate(src, n, wrap, sc, grow, z, &out_len);
    free(sc);
    return r;
}

int inflateInit2(z_streamp strm, int windowBits) {
    if (!strm) return Z_STREAM_ERROR;
    struct ns_inflate *z = calloc(1, sizeof *z);
    if (!z) return Z_MEM_ERROR;
    z->wbits = windowBits;
    strm->state = z;
    strm->total_in = strm->total_out = 0;
    strm->msg = 0;
    return Z_OK;
}

int inflate(z_streamp strm, int flush) {
    (void)flush;
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct ns_inflate *z = strm->state;
    if (!z->decoded) {
        int r = decode_all(z, strm->next_in, strm->avail_in,
                           wrap_for(z->wbits, strm->next_in, strm->avail_in));
        if (r == -ENOMEM || r == -EIO) { strm->msg = "out of memory"; return Z_MEM_ERROR; }
        if (r < 0) { strm->msg = "invalid compressed data"; return Z_DATA_ERROR; }
        z->decoded = 1;
        strm->next_in += strm->avail_in;
        strm->total_in += strm->avail_in;
        strm->avail_in = 0;
    } else if (strm->avail_in) {
        strm->msg = "input after the first inflate() call";   // see zlib.h
        return Z_STREAM_ERROR;
    }
    size_t n = z->len - z->pos;
    if (n > strm->avail_out) n = strm->avail_out;
    memcpy(strm->next_out, z->buf + z->pos, n);
    z->pos += n;
    strm->next_out += n;
    strm->avail_out -= (uInt)n;
    strm->total_out += n;
    if (z->pos == z->len) return Z_STREAM_END;
    return n ? Z_OK : Z_BUF_ERROR;
}

int inflateEnd(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct ns_inflate *z = strm->state;
    free(z->buf);
    free(z);
    strm->state = 0;
    return Z_OK;
}

struct ns_gzfile {
    uint8_t *data;
    size_t len, pos;
};

gzFile gzopen(const char *path, const char *mode) {
    if (!path || !mode || mode[0] != 'r') { errno = EINVAL; return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *raw = 0;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t *b = realloc(raw, cap);
            if (!b) { free(raw); fclose(f); errno = ENOMEM; return 0; }
            raw = b;
        }
        size_t got = fread(raw + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    gzFile g = calloc(1, sizeof *g);
    if (!g) { free(raw); errno = ENOMEM; return 0; }
    if (len >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        struct ns_inflate z = { 0 };
        int r = decode_all(&z, raw, len, UINFLATE_GZIP);
        free(raw);
        if (r < 0) { free(z.buf); free(g); errno = r == -ENOMEM ? ENOMEM : EINVAL; return 0; }
        g->data = z.buf;
        g->len = z.len;
    } else {
        g->data = raw;
        g->len = len;
    }
    return g;
}

char *gzgets(gzFile g, char *buf, int len) {
    if (!g || !buf || len <= 0) return 0;
    if (g->pos >= g->len) return 0;
    int n = 0;
    while (n < len - 1 && g->pos < g->len) {
        char c = (char)g->data[g->pos++];
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return buf;
}

int gzclose(gzFile g) {
    if (!g) return Z_STREAM_ERROR;
    free(g->data);
    free(g);
    return Z_OK;
}
