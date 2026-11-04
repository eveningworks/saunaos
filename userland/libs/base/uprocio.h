#ifndef ULIB_UPROCIO_H
#define ULIB_UPROCIO_H

// Each process's I/O RATE, from two reads of QUERY_PROCIO (Linux's
// /proc/<pid>/io) and the time between them -- what iotop and Windows'
// Resource Monitor show. Task Manager's Read and Write columns and its
// Disk page's top users read it; so can anything else that samples.
//
// A process missing from the earlier read STARTED in between, so its
// whole count is the interval's (Windows does the same); the first
// update has nothing to compare with and every rate is 0.
#include <stdint.h>
#include "query_abi.h"

struct uprocio_rate {
    int pid;                        // 0 = the kernel
    uint64_t read_bps, write_bps;   // disk bytes a second
    uint64_t read_mops, write_mops; // disk commands a second, x1000
    uint64_t file_read_bps, file_write_bps;  // every read()/write() on a file
    struct query_procio total;      // since the process started
};

struct uprocio {
    struct query_procio *prev, *cur;
    int nprev, ncur, cap;
    uint64_t prev_ns, cur_ns;       // when each was read
};

// Read the counters now; the previous read becomes the base. Returns the
// number of records, or -1 when out of memory (the last read stands).
int uprocio_update(struct uprocio *u);

// One process's rates over the last interval. 0 for a pid not in the
// latest read (it has exited).
int uprocio_rate(const struct uprocio *u, int pid, struct uprocio_rate *out);

// The i'th record of the latest read, for a caller that ranks them (the
// Disk page's top users). 0 past the end.
int uprocio_rate_at(const struct uprocio *u, int i, struct uprocio_rate *out);

void uprocio_free(struct uprocio *u);

#endif
