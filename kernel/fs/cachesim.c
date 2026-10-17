// The ghost caches behind QUERY_CACHESIM -- see cachesim.h for what a
// ghost cache is and docs/pagecache-design.md for what it measures.
//
// driver-none: a measurement over the VFS's and the block layer's reads
#include "cachesim.h"
#include "query.h"
#include "heap.h"
#include "klog.h"
#include "kfmt.h"    // klog_printf
#include "string.h"
#include "multiboot.h"
#include "irqflags.h"
#include "fswatch.h"
#include "scheduler.h" // who is reading: the attribution tables
#include "initcall.h"
#include <stddef.h>

#define PAGE 4096u

// ---- one LRU ----------------------------------------------------------

void cachesim_lru_init(struct cachesim_lru *c, struct cachesim_entry *e,
                       uint32_t *bucket, uint32_t cap) {
    k_memset(c, 0, sizeof *c);
    c->e = e;
    c->bucket = bucket;
    c->cap = cap;
    c->mru = c->lru = CACHESIM_NIL;
    for (uint32_t i = 0; i < cap; i++) bucket[i] = CACHESIM_NIL;
}

static void unlink_lru(struct cachesim_lru *c, uint32_t i) {
    struct cachesim_entry *x = &c->e[i];
    if (x->prev != CACHESIM_NIL) c->e[x->prev].next = x->next; else c->mru = x->next;
    if (x->next != CACHESIM_NIL) c->e[x->next].prev = x->prev; else c->lru = x->prev;
}

static void push_mru(struct cachesim_lru *c, uint32_t i) {
    struct cachesim_entry *x = &c->e[i];
    x->prev = CACHESIM_NIL;
    x->next = c->mru;
    if (c->mru != CACHESIM_NIL) c->e[c->mru].prev = i; else c->lru = i;
    c->mru = i;
}

static void unlink_bucket(struct cachesim_lru *c, uint32_t i) {
    uint32_t *link = &c->bucket[c->e[i].key & (c->cap - 1)];
    while (*link != i) link = &c->e[*link].hnext;
    *link = c->e[i].hnext;
}

int cachesim_lru_touch(struct cachesim_lru *c, uint64_t key, uint32_t gen, int count) {
    if (count) c->lookups++;
    uint32_t b = (uint32_t)(key & (c->cap - 1));
    for (uint32_t i = c->bucket[b]; i != CACHESIM_NIL; i = c->e[i].hnext) {
        if (c->e[i].key != key) continue;
        int hit = c->e[i].gen == gen;
        c->e[i].gen = gen;
        unlink_lru(c, i);
        push_mru(c, i);
        if (hit && count) c->hits++;
        return hit;
    }
    uint32_t i;
    if (c->used < c->cap) {
        i = c->used++;
    } else {
        i = c->lru;
        unlink_lru(c, i);
        unlink_bucket(c, i);
    }
    c->e[i].key = key;
    c->e[i].gen = gen;
    c->e[i].hnext = c->bucket[b];
    c->bucket[b] = i;
    push_mru(c, i);
    return 0;
}

// ---- the kernel's instances ---------------------------------------------

// 4, 16, 64 and 256 MiB of 4 KiB pages: from "a few apps' worth" to an
// eighth of a 2 GiB machine, the budget ramfs-style sizing would give.
static const uint32_t TIER_PAGES[] = { 1024, 4096, 16384, 65536 };
#define TIERS (sizeof TIER_PAGES / sizeof TIER_PAGES[0])

enum { LVL_FILE, LVL_BLOCK, LEVELS };
static const char *const LEVEL_NAME[LEVELS] = { "file", "block" };

static struct cachesim_lru g_lru[LEVELS][TIERS];
static int g_on;

// A file's generation, by path hash: a change bumps its bucket, and
// every page cached under the old value goes stale. A collision only
// makes an unrelated file look changed -- a missed hit, never a false one.
#define FILE_GENS 4096
static uint32_t g_file_gen[FILE_GENS];

// splitmix64's finaliser: the bucket is the key's low bits, and neither
// a path hash plus a page number nor a pointer plus an LBA is uniform there.
static uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t z = a ^ (b * 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Returns whether the LARGEST tier held the key: "this page was read
// before", the repeat the attribution tables below count. One page per
// irq_save(): a 20 MB read is thousands of pages, and must not hold
// interrupts off for all of them.
static int touch_all(int lvl, uint64_t key, uint32_t gen, int count) {
    int hit = 0;
    uint64_t f = irq_save();
    for (unsigned t = 0; t < TIERS; t++) hit = cachesim_lru_touch(&g_lru[lvl][t], key, gen, count);
    irq_restore(f);
    return hit;
}

// ---- who reads: two heavy-hitter tables -----------------------------------
//
// Bounded, so a newcomer replaces the entry with the least traffic: a
// steady poller stays, a one-off read may vanish -- the ranking is
// right at the top and lossy at the bottom, which is what finding a
// poller needs. Linear scans, each under its own irq_save().

#define TOP_PROCS 32
#define TOP_PATHS 128

struct top_proc {
    int pid;
    char name[QUERY_CACHESIM_NAME];
    uint64_t pages, repeats;
};

struct top_path {
    uint64_t key;       // mix(path hash, pid)
    int pid;
    char name[QUERY_CACHESIM_NAME];
    char path[QUERY_CACHESIM_PATH_MAX];
    uint64_t lookups, pages, repeats;
};

static struct top_proc *g_tproc;
static struct top_path *g_tpath;
static int g_ntproc, g_ntpath;

static void name_pid(int pid, char *out, unsigned cap) {
    if (!pid || !scheduler_name_of(pid, out, cap)) k_strlcpy(out, pid ? "?" : "kernel", cap);
}

static struct top_proc *proc_slot(int pid) {
    struct top_proc *min = 0;
    for (int i = 0; i < g_ntproc; i++) {
        if (g_tproc[i].pid == pid) return &g_tproc[i];
        if (!min || g_tproc[i].pages < min->pages) min = &g_tproc[i];
    }
    struct top_proc *e = g_ntproc < TOP_PROCS ? &g_tproc[g_ntproc++] : min;
    k_memset(e, 0, sizeof *e);
    e->pid = pid;
    name_pid(pid, e->name, sizeof e->name);
    return e;
}

static struct top_path *path_slot(int pid, uint64_t path_hash, const char *path) {
    uint64_t key = mix(path_hash, (uint64_t)pid);
    struct top_path *min = 0;
    for (int i = 0; i < g_ntpath; i++) {
        if (g_tpath[i].key == key) return &g_tpath[i];
        if (!min || g_tpath[i].lookups + g_tpath[i].pages < min->lookups + min->pages)
            min = &g_tpath[i];
    }
    struct top_path *e = g_ntpath < TOP_PATHS ? &g_tpath[g_ntpath++] : min;
    k_memset(e, 0, sizeof *e);
    e->key = key;
    e->pid = pid;
    name_pid(pid, e->name, sizeof e->name);
    k_strlcpy(e->path, path, sizeof e->path);
    return e;
}

// ---- the hooks --------------------------------------------------------------

void cachesim_lookup(const char *path) {
    if (!g_on || !path) return;
    uint64_t h;
    fswatch_hash(path, &h, 0);
    int pid = scheduler_current_tgid();
    uint64_t f = irq_save();
    path_slot(pid, h, path)->lookups++;
    irq_restore(f);
}

void cachesim_file_read(const char *path, uint64_t offset, uint32_t len) {
    if (!g_on || len == 0) return;
    uint64_t path_hash;
    fswatch_hash(path, &path_hash, 0);
    uint32_t gen = g_file_gen[path_hash % FILE_GENS];
    uint64_t pages = 0, repeats = 0;
    for (uint64_t p = offset / PAGE; p <= (offset + len - 1) / PAGE; p++) {
        pages++;
        repeats += (uint64_t)touch_all(LVL_FILE, mix(path_hash, p), gen, 1);
    }
    int pid = scheduler_current_tgid();
    uint64_t f = irq_save();
    struct top_path *who = path_slot(pid, path_hash, path);
    who->pages += pages;
    who->repeats += repeats;
    irq_restore(f);
}

void cachesim_file_changed(uint64_t path_hash) {
    if (g_on) g_file_gen[path_hash % FILE_GENS]++;
}

void cachesim_block_io(const void *dev, uint64_t lba, uint32_t count, int write) {
    if (!g_on || count == 0) return;
    uint64_t first = lba * 512 / PAGE;
    uint64_t last = ((lba + count) * 512 - 1) / PAGE;
    uint64_t pages = 0, repeats = 0;
    for (uint64_t p = first; p <= last; p++) {
        pages++;
        repeats += (uint64_t)touch_all(LVL_BLOCK, mix((uint64_t)(uintptr_t)dev, p), 0, !write);
    }
    if (write) return;
    int pid = scheduler_current_tgid();
    uint64_t f = irq_save();
    struct top_proc *who = proc_slot(pid);
    who->pages += pages;
    who->repeats += repeats;
    irq_restore(f);
}

static void cachesim_init(void) {
    if (!multiboot_cmdline_has("cachesim")) return;
    for (int l = 0; l < LEVELS; l++) {
        for (unsigned t = 0; t < TIERS; t++) {
            uint32_t cap = TIER_PAGES[t];
            struct cachesim_entry *e = kmalloc(cap * sizeof *e);
            uint32_t *b = kmalloc(cap * sizeof *b);
            if (!e || !b) {
                // Earlier tiers stay allocated: this boot asked for a
                // measurement and gets none, and the log says why.
                klog_printf(KLOG_ERR "cachesim: no memory for the %u-page tier -- off\n", cap);
                return;
            }
            cachesim_lru_init(&g_lru[l][t], e, b, cap);
        }
    }
    g_tproc = kzalloc(TOP_PROCS * sizeof *g_tproc);
    g_tpath = kzalloc(TOP_PATHS * sizeof *g_tpath);
    if (!g_tproc || !g_tpath) {
        klog_printf(KLOG_ERR "cachesim: no memory for the attribution tables -- off\n");
        return;
    }
    g_on = 1;
    klog_printf(KLOG_INFO "cachesim: ghost caches on (file and block, 4..256 MiB)\n");
}

// ---- the QUERY provider ---------------------------------------------------

static int cachesim_count(void) { return g_on ? (int)(LEVELS * TIERS) : 0; }

static int cachesim_fill(int index, void *out) {
    if (!g_on || index < 0 || index >= (int)(LEVELS * TIERS)) return 0;
    struct query_cachesim *r = out;
    k_memset(r, 0, sizeof *r);
    int l = index / (int)TIERS, t = index % (int)TIERS;
    const struct cachesim_lru *c = &g_lru[l][t];
    k_strlcpy(r->level, LEVEL_NAME[l], sizeof r->level);
    uint64_t f = irq_save();
    r->size_kib = (uint64_t)c->cap * (PAGE / 1024);
    r->lookups = c->lookups;
    r->hits = c->hits;
    r->resident = c->used;
    irq_restore(f);
    return 1;
}

static const struct query_field cachesim_fields[] = {
    QUERY_FIELD(struct query_cachesim, size_kib, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim, lookups,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim, hits,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim, resident, QUERY_TYPE_U64),
};

static const struct query_provider cachesim_provider = {
    .cls = QUERY_CACHESIM,
    .name = "cachesim",
    .record_size = sizeof(struct query_cachesim),
    .flags = QUERY_F_LIST,
    .count = cachesim_count,
    .fill = cachesim_fill,
    .fields = cachesim_fields,
    .field_count = sizeof cachesim_fields / sizeof cachesim_fields[0],
};

static int tproc_count(void) { return g_on ? g_ntproc : 0; }

static int tproc_fill(int index, void *out) {
    if (!g_on || index < 0 || index >= g_ntproc) return 0;
    struct query_cachesim_proc *r = out;
    k_memset(r, 0, sizeof *r);
    uint64_t f = irq_save();
    const struct top_proc *e = &g_tproc[index];
    r->pid = (uint64_t)e->pid;
    k_strlcpy(r->name, e->name, sizeof r->name);
    r->pages = e->pages;
    r->repeats = e->repeats;
    irq_restore(f);
    return 1;
}

static const struct query_field tproc_fields[] = {
    QUERY_FIELD(struct query_cachesim_proc, pid,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim_proc, pages,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim_proc, repeats, QUERY_TYPE_U64),
};

static const struct query_provider tproc_provider = {
    .cls = QUERY_CACHESIM_PROC,
    .name = "cachesim_proc",
    .record_size = sizeof(struct query_cachesim_proc),
    .flags = QUERY_F_LIST,
    .count = tproc_count,
    .fill = tproc_fill,
    .fields = tproc_fields,
    .field_count = sizeof tproc_fields / sizeof tproc_fields[0],
};

static int tpath_count(void) { return g_on ? g_ntpath : 0; }

static int tpath_fill(int index, void *out) {
    if (!g_on || index < 0 || index >= g_ntpath) return 0;
    struct query_cachesim_path *r = out;
    k_memset(r, 0, sizeof *r);
    uint64_t f = irq_save();
    const struct top_path *e = &g_tpath[index];
    r->pid = (uint64_t)e->pid;
    k_strlcpy(r->name, e->name, sizeof r->name);
    k_strlcpy(r->path, e->path, sizeof r->path);
    r->lookups = e->lookups;
    r->pages = e->pages;
    r->repeats = e->repeats;
    irq_restore(f);
    return 1;
}

static const struct query_field tpath_fields[] = {
    QUERY_FIELD(struct query_cachesim_path, pid,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim_path, lookups, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim_path, pages,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_cachesim_path, repeats, QUERY_TYPE_U64),
};

static const struct query_provider tpath_provider = {
    .cls = QUERY_CACHESIM_PATH,
    .name = "cachesim_path",
    .record_size = sizeof(struct query_cachesim_path),
    .flags = QUERY_F_LIST,
    .count = tpath_count,
    .fill = tpath_fill,
    .fields = tpath_fields,
    .field_count = sizeof tpath_fields / sizeof tpath_fields[0],
};

void cachesim_query_init(void) {
    cachesim_init();
    query_register(&cachesim_provider);
    query_register(&tproc_provider);
    query_register(&tpath_provider);
}
INITCALL(cachesim_query_init, INIT_QUERY);
