#ifndef CACHESIM_H
#define CACHESIM_H

#include <stdint.h>

// A GHOST CACHE: what a page cache or a block cache WOULD have hit, with
// no cache built. Each simulated cache is an LRU of KEYS ONLY -- no data
// -- so its hit count is exactly the real cache's, at a fraction of the
// memory (ZFS's ARC keeps ghost lists for the same reason; Linux's
// workingset.c keeps shadow entries). docs/pagecache-design.md says
// what the numbers decide. Off unless the kernel booted with `cachesim`;
// QUERY_CACHESIM reports them and /bin/cachesim prints them.

// One simulated LRU, over storage the caller owns (so a KTEST can run
// one on a few static entries). Exposed for the test; the kernel's own
// instances live in cachesim.c.
struct cachesim_entry {
    uint64_t key;
    uint32_t gen;       // the key's generation when last touched
    uint32_t prev, next;  // the LRU list, CACHESIM_NIL-terminated
    uint32_t hnext;       // the bucket chain
};

#define CACHESIM_NIL 0xffffffffu

struct cachesim_lru {
    struct cachesim_entry *e;
    uint32_t *bucket;   // `cap` heads; cap is a power of two
    uint32_t cap, used;
    uint32_t mru, lru;
    uint64_t lookups, hits;
};

// `cap` must be a power of two; `e` holds cap entries, `bucket` cap heads.
void cachesim_lru_init(struct cachesim_lru *c, struct cachesim_entry *e,
                       uint32_t *bucket, uint32_t cap);

// Look `key` up and make it most-recent, inserting it (evicting the
// least-recent) when absent. A key found with a different `gen` is
// STALE -- a miss, refreshed in place. `count` 0 is a write keeping the
// key current without counting a lookup. Returns whether it hit.
int cachesim_lru_touch(struct cachesim_lru *c, uint64_t key, uint32_t gen, int count);

// The hooks. Each is a no-op unless the `cachesim` boot word made the
// tables, and costs four LRU touches per 4 KiB page when it did.
//
// A file's pages are keyed by its PATH's fswatch hash (the one
// cachesim_file_changed() takes), so a rename moves them to new keys and
// fs_link's second name shares nothing -- both err towards fewer hits.
// A block's are keyed by device and 512-byte LBA, as block.h counts.
// Every VFS call that names a path, for QUERY_CACHESIM_PATH's lookups.
void cachesim_lookup(const char *path);
void cachesim_file_read(const char *path, uint64_t offset, uint32_t len);
void cachesim_file_changed(uint64_t path_hash);
void cachesim_block_io(const void *dev, uint64_t lba, uint32_t count, int write);

#endif
