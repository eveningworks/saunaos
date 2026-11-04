#ifndef KERNEL_IMGCACHE_H
#define KERNEL_IMGCACHE_H

// The system image cache: one frame per page of a /bin, /lib or /tests
// file, shared by every process that maps it (kernel/mm/imgcache.c).
// Two kinds of page live in it, told apart by the KEY:
//
//   - a FILE page, keyed by its page-aligned offset -- what an mmap of
//     a library faults in (mmap.c), the raw bytes zero-padded past EOF;
//   - a LOADED page, keyed by IMGCACHE_KEY_LOADED() -- what elf.c builds
//     for one PT_LOAD segment's page: zeroed, with the segment's file
//     bytes copied in. The same build every time from the same file, so
//     it is as immutable as the file.
//
// A frame handed out for a BORROWED mapping is never freed (no refcount
// says when the last mapping went); one only ever copied from is freed
// when its file is forgotten.

#include <stdint.h>

#define IMGCACHE_KEY_LOADED(seg, va) ((1ULL << 63) | ((uint64_t)(seg) << 48) | (va))

// 1 if the cache may hold pages of `path`: under /bin/, /lib/ or
// /tests/ (what the build seeds, rewritten only through the vfs calls
// that forget) and spelled canonically -- a second spelling of one file
// would be a second key that imgcache_forget() does not know about.
int imgcache_path_ok(const char *path);

// The cached frame for (path, key), MARKED LENT -- the caller maps it
// borrowed and it is never freed -- or 0 on a miss. Never sleeps.
uint64_t imgcache_lookup(const char *path, uint64_t key);

// Read BEFORE reading the file a page is filled from, and handed to
// imgcache_adopt(): a forget in between makes the offer stale.
uint64_t imgcache_epoch(void);

// Offers `frame`, which the caller filled, as (path, key)'s page. Returns
// the frame now cached for it -- `frame` itself (the cache owns it now),
// or the one another process cached first (the caller frees its own) --
// or 0 when the entry could not be recorded: no memory, or the file was
// forgotten since `epoch` (`frame` is still the caller's either way).
// `lend`: the caller will map the answer borrowed.
uint64_t imgcache_adopt(const char *path, uint64_t key, uint64_t frame, int lend,
                        uint64_t epoch);

// Copies (path, key)'s cached page into `dst` (4 KiB) under the cache's
// guard, so a forget cannot free it mid-copy. 1 on a hit, 0 on a miss.
int imgcache_copy(const char *path, uint64_t key, void *dst);

// A page that should have been shared and was not: counted (meminfo's
// "not shared"), and the first few logged with `why`.
void imgcache_fallback(const char *path, uint64_t key, const char *why);

// Unhooks `path` and every cached file under it (a directory renamed or
// mounted over): the next mapping reads the file again. Called by every
// vfs path that changes what a name holds (kernel/fs/vfs.c, mount.c).
void imgcache_forget(const char *path);

struct imgcache_stats {
    uint64_t frames;     // frames held for files still hooked
    uint64_t files;
    uint64_t fallbacks;  // pages that should have been shared and were not
    uint64_t unhooked;   // lent frames of forgotten files: held until reboot
};
void imgcache_get_stats(struct imgcache_stats *out);

#endif
