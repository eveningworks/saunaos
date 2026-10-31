#ifndef ULIB_URANGE_H
#define ULIB_URANGE_H

// A FILE READ BY RANGE, through a small page cache, never held whole: a
// hex viewer's bytes, a structure decoder's fields -- whatever reads a
// few bytes here and a few there from a file that may be gigabytes.
// The cache is a handful of pages kept by last use, so the rows on
// screen and the header a decoder keeps returning to cost one read each.
//
// **A READ PAST THE END COMES BACK SHORT, NEVER PADDED**: the count is
// what the file holds there, so a caller cannot mistake zeros the file
// does not have for bytes it does. The size is the one fstat() gave at
// open -- a file that grows meanwhile is not seen to.
//
// ONE THREAD PER urange: a worker that scans opens its own.
#include <stddef.h>
#include <stdint.h>

#define URANGE_PAGE  4096
#define URANGE_PAGES 16      // the cache: 64 KiB -- a policy, not a bound on the file

struct urange_page {
    uint64_t index;          // page number, or UINT64_MAX for empty
    uint32_t len;            // bytes held (short only for the file's last page)
    uint32_t used;           // the clock at its last use
};

struct urange {
    int fd;                  // -1 when closed
    uint64_t size;
    uint8_t *data;           // URANGE_PAGES * URANGE_PAGE, one allocation
    struct urange_page page[URANGE_PAGES];
    uint32_t clock;
    unsigned long misses;    // pages read from the file, for a test to count
};

// 0, or a negative errno: the path does not open, is a directory, or
// the cache cannot be allocated.
int    urange_open(struct urange *r, const char *path);
void   urange_close(struct urange *r);
// Up to `n` bytes at `off` into `buf`; the count, short only at the end
// of the file or on a read error.
size_t urange_read(struct urange *r, uint64_t off, void *buf, size_t n);
// urange_read() in the shape a callback takes (`ctx` is the urange):
// lib/ubinmap.h's source and ui/uui_hexview.h's reader.
size_t urange_reader(void *ctx, uint64_t off, void *buf, size_t n);

#endif
