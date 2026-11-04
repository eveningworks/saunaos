#ifndef ULIB_UBINMAP_INTERNAL_H
#define ULIB_UBINMAP_INTERNAL_H

// What a ubinmap decoder is written with (base/ubinmap.h): reads that
// never leave the file, the file's byte order, and nodes added with
// their range clipped. Shared by ubinmap.c and every ubinmap_<kind>.c.
#include "base/ubinmap.h"

struct ubinmap_ctx {
    const struct ubinmap_src *src;
    struct ubinmap *m;
    int be;                          // fields are big-endian
};

// ALL `n` bytes at `off`, or -1 -- for a range that is not wholly inside
// the file the source is never asked at all.
int bm_read(struct ubinmap_ctx *c, uint64_t off, void *buf, size_t n);
// A field in the context's byte order; 0, or -1 past the end (`*v` 0).
int bm_u8(struct ubinmap_ctx *c, uint64_t off, uint8_t *v);
int bm_u16(struct ubinmap_ctx *c, uint64_t off, uint16_t *v);
int bm_u32(struct ubinmap_ctx *c, uint64_t off, uint32_t *v);
int bm_u64(struct ubinmap_ctx *c, uint64_t off, uint64_t *v);
// Fields out of a buffer already read, in the context's order.
uint16_t bm_get16(const struct ubinmap_ctx *c, const uint8_t *p);
uint32_t bm_get32(const struct ubinmap_ctx *c, const uint8_t *p);
uint64_t bm_get64(const struct ubinmap_ctx *c, const uint8_t *p);

// A node; its range is clipped to the file. Returns its index, or -1 when
// the map is full (counted in `dropped`) or out of memory -- a loop over a
// table stops on bm_full().
int bm_add(struct ubinmap_ctx *c, int depth, uint64_t off, uint64_t len, int tint,
           const char *label, const char *fmt, ...) __attribute__((format(printf, 7, 8)));
int bm_full(const struct ubinmap_ctx *c);
// The map's note, if it has none yet.
void bm_note(struct ubinmap_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void bm_format(struct ubinmap_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// A string from the file: up to `max` bytes at `off`, stopping at a NUL
// or the end, each byte outside printable ASCII written as '?'.
void bm_str(struct ubinmap_ctx *c, uint64_t off, size_t max, char *out, size_t cap);
// Bytes already read, the same way.
void bm_ascii(const uint8_t *p, size_t n, char *out, size_t cap);
// "1.5 KiB" -- a length a person reads beside the byte count.
void bm_human(uint64_t n, char *out, size_t cap);
// A GUID as text, the mixed-endian way GPT stores one.
void bm_guid(const uint8_t g[16], char *out, size_t cap);

// The decoders (each a ubinmap_<kind>.c).
int ubinmap_elf(struct ubinmap_ctx *c);
int ubinmap_wad(struct ubinmap_ctx *c);
int ubinmap_ttf(struct ubinmap_ctx *c);
int ubinmap_gpt(struct ubinmap_ctx *c);
int ubinmap_fat(struct ubinmap_ctx *c);
int ubinmap_mbr(struct ubinmap_ctx *c);
int ubinmap_qoi(struct ubinmap_ctx *c);
int ubinmap_bmp(struct ubinmap_ctx *c);

#endif
