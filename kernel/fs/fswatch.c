// Path watches for the compositor -- see kernel/fswatch.h.
#include "fswatch.h"
#include "errno.h"
#include "kslots.h"
#include "klimit.h"
#include "win_role.h" // win_server_fswatch_fired()

struct watch {
    int pid;
    uint64_t hash;
    uint32_t fires;  // for a test, and nothing else
};

// IN kslots, because fswatch_note() walks the table from vfs.c with no
// lock of this file's held -- after a backend op, possibly preempted by
// the compositor adding a watch. kslots never moves an object or frees
// a slot's memory, so the walk reads a live or a dead watch, never a
// freed one. The id is the slot + 1.
static struct kslots g_w = KSLOTS_INIT(sizeof(struct watch));

// A thousandth of RAM, at what a watch costs (its object, its pointer,
// its live byte). Capped because fswatch_note() scans every watch on
// every write in the machine.
struct klimit fswatch_limit = KLIMIT("fswatch: path watches", 1024,
                                     sizeof(struct watch) + sizeof(void *) + 1, 16, 65536);

static uint64_t fnv(const char *s, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
    return h;
}

void fswatch_hash(const char *path, uint64_t *self, uint64_t *parent) {
    uint32_t n = 0;
    while (path[n]) n++;
    while (n > 1 && path[n - 1] == '/') n--;
    uint32_t cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;
    // "/a" -> parent "/", "/a/b" -> parent "/a"
    uint32_t pn = cut > 1 ? cut - 1 : 1;
    if (self) *self = fnv(path, n);
    if (parent) *parent = fnv(path, pn);
}

int fswatch_add(int pid, const char *path) {
    uint64_t h;
    fswatch_hash(path, &h, 0);
    int live = 0;
    for (int i = 0; i < kslots_cap(&g_w); i++) {
        struct watch *w = kslots_at(&g_w, i);
        if (!w) continue;
        if (w->pid == pid && w->hash == h) return i + 1;
        live++;
    }
    if (live >= (int)klimit_get(&fswatch_limit)) {
        klimit_hit(&fswatch_limit);
        return -ENOSPC;
    }
    int i = kslots_alloc(&g_w);
    if (i < 0) return -ENOMEM;
    struct watch *w = kslots_at(&g_w, i);
    w->hash = h;
    w->fires = 0;
    __atomic_store_n(&w->pid, pid, __ATOMIC_RELEASE);   // last: a walk matches it from here
    return i + 1;
}

void fswatch_note(uint64_t self, uint64_t parent) {
    for (int i = 0; i < kslots_cap(&g_w); i++) {
        struct watch *w = kslots_at(&g_w, i);
        if (!w || !__atomic_load_n(&w->pid, __ATOMIC_ACQUIRE)) continue;
        if (w->hash != self && w->hash != parent) continue;
        w->fires++;
        win_server_fswatch_fired(i + 1);
    }
}

void fswatch_owner_gone(int pid) {
    for (int i = 0; i < kslots_cap(&g_w); i++) {
        struct watch *w = kslots_at(&g_w, i);
        if (!w || w->pid != pid) continue;
        w->pid = 0;
        kslots_free(&g_w, i);
    }
}

uint32_t fswatch_fires(int id) {
    struct watch *w = kslots_at(&g_w, id - 1);
    return w ? w->fires : 0;
}
