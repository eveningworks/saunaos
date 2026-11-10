#ifndef ULIB_UWALK_H
#define ULIB_UWALK_H

// A DIRECTORY TREE WALK, one directory at a time, for a caller that has
// to stop, report progress or share a thread: the File Manager's search
// into subfolders and its folder sizes. find(1)'s traversal, as a
// resumable state machine instead of a recursion.
//
// **DEPTH-FIRST OVER AN EXPLICIT STACK**, not breadth-first over a queue:
// a queue holds every directory found and not yet read, which grows with
// the tree's WIDTH and has no bound worth naming; a stack holds one
// frame per LEVEL, and a level is bounded by the path length. Each frame
// is a directory and how far into its listing the walk has got, so a
// listing is paged (SYS_GETDENTS) and a huge directory costs no more
// memory than a small one.
//
// The state is the caller's (static, or a heap: never a 2 KiB ring-3
// frame), and it touches nothing else, so a worker thread may own one.
//
// **THE PATH AND THE PAGE ARE ALLOCATED, AND SIZED TO THE DATA.** One
// path buffer serves every level -- a depth-first walk's directories
// are prefixes of each other, so a frame keeps only its length -- and
// grows for a long path; the page of records grows for a long name. A
// walk that FINISHES frees both; one stopped early (the callback's -1,
// or a caller that stops stepping) is freed by uwalk_end().

#include "rt/sys.h"
#include "base/ulistdir.h"   // struct ulist_entry

#define UWALK_PATH  256    // a size for CALLERS' own path buffers; the walk has no limit
#define UWALK_DEPTH 48     // levels; a deeper subtree is skipped, and counted

// Called for every entry below the root, in walk order. `path` is the
// entry's full path, valid during the call. Return 0 to leave a
// DIRECTORY unvisited (or to skip nothing for a file), 1 to go on, -1 to
// stop the whole walk. A name longer than ULIST_NAME_MAX - 1 is not
// reported (ulistdir.h), only counted.
typedef int (*uwalk_fn)(void *ctx, const char *path, const struct ulist_entry *e);

struct uwalk {
    struct { int len; uint32_t offset; } frame[UWALK_DEPTH];   // its directory = path[0, len)
    int depth;                       // frames in use; 0 = finished
    char *path;                      // between steps, the directory being walked;
                                     // "<it>/<entry>" during a callback
    size_t path_cap;
    char *page;                      // SYS_GETDENTS records (ulistdir_page())
    uint32_t page_cap;
    unsigned long long entries;      // seen so far
    unsigned dirs;                   // directories entered
    unsigned too_deep;               // subtrees past UWALK_DEPTH, skipped
    unsigned unreadable;             // directories that could not be listed
    int stopped;                     // the callback said -1
};

// Start at `root` (a directory). 0 when it is not one. `w`'s old
// contents are not looked at: end a stopped walk before reusing it.
int uwalk_begin(struct uwalk *w, const char *root);

// Read ONE page of the current directory, calling `fn` for each entry
// and descending where it says to. 1 while there is more to walk, 0 when
// the walk is over (finished, or stopped).
int uwalk_step(struct uwalk *w, uwalk_fn fn, void *ctx);

// Frees what a walk holds. Safe on a finished walk, twice, and on a
// ZEROED one never begun -- so a walk lives in zeroed memory (static,
// calloc), never plain malloc: there it would free garbage.
void uwalk_end(struct uwalk *w);

#endif
