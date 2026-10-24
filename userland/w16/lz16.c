// lz16.c -- LZEXPAND.DLL: a file COMPRESS.EXE squeezed (SZDD, KWAJ) read as
// if it were not. Control Panel reads SETUP.INF and CONTROL.INF through it,
// and Setup's own tools expand drivers off a disk with LZCopy.
//
// A COMPRESSED FILE IS EXPANDED WHOLE AT LZInit, into memory, behind an LZ
// handle numbered from 400 as LZEXPAND's were (LZ_TABLE_BIAS); LZRead and
// LZSeek then work on those bytes. A file that is not compressed keeps its
// DOS handle, and every call passes it on to the file calls -- what
// LZEXPAND did. The expanding is lib/umsexpand's, which the installer uses.
#include "w16_internal.h"
#include "lib/umsexpand.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LZ_TABLE_BIAS 400
#define LZERROR_BADINHANDLE  (-1)
#define LZERROR_BADOUTHANDLE (-2)
#define LZERROR_READ         (-3)
#define LZERROR_WRITE        (-4)
#define LZERROR_GLOBALLOC    (-5)
#define LZERROR_BADVALUE     (-7)
#define LZERROR_UNKNOWNALG   (-8)
#define LZ_FILE_MAX          (16u << 20)   // no 3.x setup file expands past a few MB

struct lzfile {
    int used;
    unsigned dos;           // the DOS handle it was made from, closed with it
    uint8_t *data;
    uint32_t n, pos;
};
static struct lzfile *g_lz;
static int g_nlz;

static struct lzfile *lz_of(int h) {
    int i = h - LZ_TABLE_BIAS;
    return i >= 0 && i < g_nlz && g_lz[i].used ? &g_lz[i] : 0;
}

struct grow { uint8_t *p; size_t n, cap; };

static int grow_put(void *ctx, const uint8_t *d, size_t n) {
    struct grow *g = ctx;
    if (g->n + n > LZ_FILE_MAX) return 1;
    if (g->n + n > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 65536;
        while (cap < g->n + n) cap *= 2;
        uint8_t *q = realloc(g->p, cap);
        if (!q) return 1;
        g->p = q;
        g->cap = cap;
    }
    memcpy(g->p + g->n, d, n);
    g->n += n;
    return 0;
}

// The whole of DOS handle `h` from its start into a fresh buffer; NULL when it cannot be read.
static uint8_t *read_all(unsigned h, size_t *n) {
    struct grow g = { 0 };
    if (w16_file_seek(h, 0, 0) < 0) return 0;
    for (;;) {
        if (g.n == g.cap) {
            size_t cap = g.cap ? g.cap * 2 : 65536;
            uint8_t *q = cap <= LZ_FILE_MAX ? realloc(g.p, cap) : 0;
            if (!q) { free(g.p); return 0; }
            g.p = q;
            g.cap = cap;
        }
        long r = w16_file_read(h, g.p + g.n, g.cap - g.n);
        if (r < 0) { free(g.p); return 0; }
        if (r == 0) break;
        g.n += (size_t)r;
    }
    *n = g.n;
    return g.p;
}

// LZInit's work: an LZ handle for a compressed file, the DOS handle itself for any other, or an
// LZERROR_ code.
static int lz_init(unsigned dos) {
    size_t n = 0;
    uint8_t *raw = read_all(dos, &n);
    if (!raw) return LZERROR_READ;
    struct umsx_info info;
    if (umsx_probe(raw, n, &info) == 0 && info.kind == UMSX_PLAIN) {
        free(raw);
        w16_file_seek(dos, 0, 0);
        return (int)dos;
    }
    static struct umsx_scratch s;
    struct grow g = { 0 };
    uint32_t written;
    int r = umsx_expand(raw, n, &s, grow_put, &g, &written);
    free(raw);
    if (r) { free(g.p); return r == -ENOTSUP ? LZERROR_UNKNOWNALG : LZERROR_READ; }
    int i = 0;
    while (i < g_nlz && g_lz[i].used) i++;
    if (i == g_nlz) {
        struct lzfile *q = realloc(g_lz, sizeof *q * (size_t)(g_nlz + 1));
        if (!q) { free(g.p); return LZERROR_GLOBALLOC; }
        g_lz = q;
        g_nlz++;
    }
    g_lz[i] = (struct lzfile){ 1, dos, g.p ? g.p : calloc(1, 1), (uint32_t)g.n, 0 };
    return LZ_TABLE_BIAS + i;
}

// LZInit(hfSource) -> HFILE
// Windows: Gets a file ready to be read through LZRead: a compressed one gets an LZ handle (from
// 400), any other keeps the handle it has; an LZERROR_ code if it cannot be read.
// w16: The same: a compressed file (SZDD or KWAJ) is expanded whole into memory there and then.
static int api_LZInit(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = (uint16_t)lz_init(w16_arg16(a));
    return 2;
}

// LZOpenFile(lpFileName, lpReOpenBuf, wStyle) -> int
// Windows: OpenFile, then LZInit when the file was opened for reading: a compressed file's LZ
// handle, else the DOS handle; an LZERROR_ code, or -1 as OpenFile failed.
// w16: The same, over w16's OpenFile.
static int api_LZOpenFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t name = w16_arg32(a), ofs = w16_arg32(a);
    uint16_t style = w16_arg16(a);
    int h = w16_open_file(w, name, ofs, style);
    if (h < 0) return -1;
    if (h != 0xFFFF && (style & 3) == 0 && !(style & 0x7F00)) {   // OF_READ, no create/delete/exist/parse
        int r = lz_init((unsigned)h);
        if (r < 0) w16_file_close((unsigned)h);
        h = r;
    }
    w->cpu.r[R_AX] = (uint16_t)h;
    return 10;
}

// LZRead(hFile, lpBuffer, cbRead) -> int
// Windows: Reads the expanded bytes of an LZ handle, or the file's own of a DOS handle; the count,
// or an LZERROR_ code.
// w16: The same.
static int api_LZRead(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int h = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t n = w16_arg16(a);
    uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, 1) : 0;
    if (n && !p) return -1;
    struct lzfile *f = lz_of(h);
    long r;
    if (f) {
        uint32_t left = f->pos < f->n ? f->n - f->pos : 0;
        r = n < left ? n : left;
        memcpy(p, f->data + f->pos, (size_t)r);
        f->pos += (uint32_t)r;
    } else {
        r = w16_file_read((unsigned)h, p, n);
        if (r < 0) r = LZERROR_BADINHANDLE;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    return 8;
}

// LZSeek(hFile, lOffset, nOrigin) -> LONG
// Windows: Moves an LZ handle's place in its expanded bytes (or a DOS handle's) -- from the start,
// from where it is, or from the end; the new place, or LZERROR_BADVALUE past the ends.
// w16: The same.
static int api_LZSeek(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int h = (int16_t)w16_arg16(a);
    int32_t off = (int32_t)w16_arg32(a);
    int origin = (int16_t)w16_arg16(a);
    struct lzfile *f = lz_of(h);
    long r;
    if (f) {
        long base = origin == 0 ? 0 : origin == 1 ? (long)f->pos : origin == 2 ? (long)f->n : -1;
        r = base < 0 || base + off < 0 || base + off > (long)f->n ? LZERROR_BADVALUE : base + off;
        if (r >= 0) f->pos = (uint32_t)r;
    } else {
        r = w16_file_seek((unsigned)h, off, origin);
        if (r < 0) r = LZERROR_BADINHANDLE;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)((uint32_t)r >> 16);
    return 8;
}

// LZClose(hFile) -> void
// Windows: Closes an LZ handle and the file under it, or a DOS handle.
// w16: The same.
static int api_LZClose(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    int h = (int16_t)w16_arg16(a);
    struct lzfile *f = lz_of(h);
    if (f) {
        w16_file_close(f->dos);
        free(f->data);
        *f = (struct lzfile){ 0 };
    } else {
        w16_file_close((unsigned)h);
    }
    return 2;
}

// LZCopy(hfSource, hfDest) -> LONG
// CopyLZFile(hfSource, hfDest) -> LONG
// Windows: Writes a file's bytes, expanded when it is compressed, to another open file; the count
// written, or an LZERROR_ code. CopyLZFile is the same for a program that did its own LZStart.
// w16: The same.
static int api_LZCopy(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int src = (int16_t)w16_arg16(a);
    unsigned dst = w16_arg16(a);
    long r;
    int h = lz_of(src) ? src : lz_init((unsigned)src);
    struct lzfile *f = h >= 0 ? lz_of(h) : 0;
    if (h < 0) {
        r = h;
    } else if (f) {
        long put = w16_file_write(dst, f->data, f->n);
        r = put == (long)f->n ? put : LZERROR_WRITE;
        if (h != src) {                            // made here from a DOS handle: only the copy goes
            free(f->data);
            *f = (struct lzfile){ 0 };
        }
    } else {                                       // not compressed: the file's own bytes
        size_t n = 0;
        uint8_t *raw = read_all((unsigned)src, &n);
        long put = raw ? w16_file_write(dst, raw, n) : -1;
        r = !raw ? LZERROR_READ : put == (long)n ? put : LZERROR_WRITE;
        free(raw);
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)((uint32_t)r >> 16);
    return 4;
}

// LZStart() -> int
// LZDone() -> void
// Windows: Set up and free the buffers CopyLZFile shares across several copies.
// w16: Nothing to set up: each copy expands in its own memory. LZStart answers 1.
static int api_LZStart(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)a;
    if (ordinal == 7) w->cpu.r[R_AX] = 1;
    return 0;
}

// GetExpandedName(lpszSource, lpszBuffer) -> int
// Windows: The name a compressed file expands to: the one COMPRESS -r stored (SND.CP_ -> SND.CPL),
// or the file's own name for one that is not compressed; 1, or an LZERROR_ code.
// w16: The same, from the file's SZDD or KWAJ header.
static int api_GetExpandedName(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t src = w16_arg32(a), buf = w16_arg32(a);
    char dos[160];
    if (cpu16_str(&w->cpu, (uint16_t)(src >> 16), (uint16_t)src, dos, sizeof dos) < 0) return -1;
    char host[1024], out[160];
    int ok = 1;
    snprintf(out, sizeof out, "%s", dos);
    if (w16_dos_path(dos, host, sizeof host, 0, 0, 0) == 0) {
        FILE *f = fopen(host, "rb");
        uint8_t head[64];
        size_t n = f ? fread(head, 1, sizeof head, f) : 0;
        if (f) fclose(f);
        struct umsx_info info;
        if (n && umsx_probe(head, n, &info) == 0 && info.kind != UMSX_PLAIN) {
            const char *base = strrchr(dos, '\\');
            size_t dir = base ? (size_t)(base - dos + 1) : 0;
            char name[16];
            umsx_name(&info, dos + dir, name, sizeof name);
            snprintf(out + dir, sizeof out - dir, "%s", name);
        }
    } else {
        ok = LZERROR_BADINHANDLE;
    }
    size_t len = strlen(out) + 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(buf >> 16), (uint16_t)buf, (uint32_t)len, 1);
    if (!p) return -1;
    memcpy(p, out, len);
    w->cpu.r[R_AX] = (uint16_t)ok;
    return 8;
}

const struct w16_api g_lz_api[] = {
    { 1, api_LZCopy },
    { 2, api_LZOpenFile },
    { 3, api_LZInit },
    { 4, api_LZSeek },
    { 5, api_LZRead },
    { 6, api_LZClose },
    { 7, api_LZStart },
    { 8, api_LZCopy },
    { 9, api_LZStart },
    { 10, api_GetExpandedName },
};
const unsigned g_lz_api_n = sizeof g_lz_api / sizeof g_lz_api[0];
