// The sparse disk the fuzz driver hands every parser.
#ifndef MEDIAFUZZ_H
#define MEDIAFUZZ_H
#include <stdint.h>
#include <stddef.h>
#include "block.h"

#define FZ_SLOTS 16384u // sectors held: the input's plus what the backend writes

struct fz_disk {
    uint64_t sectors;
    uint64_t reads, oob_io, written;
    uint64_t guard_lo, guard_hi;
    uint32_t nused;
    uint8_t used[FZ_SLOTS];
    uint64_t lba[FZ_SLOTS];
    uint8_t data[FZ_SLOTS][512];
};
extern struct fz_disk fz;
extern const struct block_device fz_dev;
extern int fz_verbose;
extern size_t fz_biggest_alloc;
void fz_put_sector(uint64_t lba, const uint8_t *d);
void fz_read_bytes(uint64_t off, void *buf, uint32_t len);
#endif
