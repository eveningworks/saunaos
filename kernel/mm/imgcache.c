// The system image cache: the pages of /bin, /lib and /tests, one frame
// each systemwide (imgcache.h has the two kinds of page and the API).
//
// Every dynamic process faults the same libc.so pages in and every
// instance of a program loads the same text; without this each was a
// disk read and a frame PER PROCESS. Filled on first touch:
//
//   - a READ-ONLY page (text, rodata) is mapped BORROWED into every
//     process -- one frame total;
//   - a WRITABLE one (data, GOT) is COPIED from the cache -- private by
//     definition, but a memcpy instead of a disk read.
//
// **IMMUTABLE WITHIN A BOOT, NEVER EVICTED, AND THAT IS ONLY HONEST
// BECAUSE OF THE PATH POLICY.** The build seeds these directories and
// nothing rewrites them except through the vfs calls that call
// imgcache_forget(), so the cache is bounded by the size of the system
// image -- it GROWS to fit it (no frame cap: a fixed 512 filled before
// /lib's read-only pages alone did, and every later page silently went
// private). Not a general page cache: that needs per-frame refcounts and
// eviction, docs/decisions/kernel.md.
//
// **A LENT FRAME IS NEVER FREED.** A borrowed mapping holds no reference,
// so nothing says when the last process stopped using it; a forgotten
// file's lent frames stay allocated until reboot (`unhooked`). Frames
// only ever copied from are freed with their file.
//
// The guard is the preemption guard, the same one kmalloc's heap uses
// (heap_os.c): every table walk and change runs under it, and nothing
// that can sleep -- the disk read that fills a page -- runs inside it.

#include "imgcache.h"
#include "heap.h"
#include "pmm.h"
#include "scheduler.h"
#include "string.h"
#include "kfmt.h"
#include "klog.h"
#include "query.h"
#include "initcall.h"
#include <stddef.h>

struct imgfile;

struct imgpage {
    struct imgpage *hnext;   // the page table's chain
    struct imgpage *fnext;   // its file's list, for a forget
    struct imgfile *file;
    uint64_t key;
    uint64_t frame;
    int      lent;           // mapped borrowed somewhere: never freed
};

struct imgfile {
    struct imgfile *hnext;
    struct imgpage *pages;
    uint64_t frames;
    uint64_t fallbacks;
    uint32_t hash;
    char     path[];         // sized to fit
};

// Two chained hash tables, each grown x2 past one entry per bucket. A
// failed grow keeps the old table: longer chains, still correct.
static struct imgfile **g_files;
static uint32_t g_files_nb, g_nfiles;
static struct imgpage **g_pages;
static uint32_t g_pages_nb, g_npages;

static uint64_t g_fallbacks, g_unhooked;

// Bumped by every forget that could concern a cached file. A filler
// reads the file BEFORE it offers the page, and a write may land in
// between -- vfs forgets AFTER its write, so a reader that started
// earlier would otherwise cache the old bytes for the new file.
static uint64_t g_epoch;

#define FALLBACK_LOGGED 4   // CLAUDE.md's probe rule: the first few, then the counter

static const char *const g_prefixes[] = { "/bin/", "/lib/", "/tests/" };
#define NPREFIX (sizeof g_prefixes / sizeof g_prefixes[0])

int imgcache_path_ok(const char *path) {
    if (!path) return 0;
    int under = 0;
    for (size_t i = 0; i < NPREFIX && !under; i++)
        under = k_strncmp(path, g_prefixes[i], k_strlen(g_prefixes[i])) == 0;
    // "/." also refuses a dot file, which costs nothing worth having.
    return under && !k_strstr(path, "//") && !k_strstr(path, "/.");
}

static uint32_t page_bucket(const struct imgfile *f, uint64_t key, uint32_t nb) {
    uint64_t x = (uint64_t)(uintptr_t)f ^ (key * 0x9E3779B97F4A7C15ULL);
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 32;
    return (uint32_t)x & (nb - 1);
}

static struct imgfile *file_find(const char *path, uint32_t h) {
    if (!g_files) return 0;
    for (struct imgfile *f = g_files[h & (g_files_nb - 1)]; f; f = f->hnext)
        if (f->hash == h && !k_strcmp(f->path, path)) return f;
    return 0;
}

static struct imgpage *page_find(const struct imgfile *f, uint64_t key) {
    if (!f || !g_pages) return 0;
    for (struct imgpage *p = g_pages[page_bucket(f, key, g_pages_nb)]; p; p = p->hnext)
        if (p->file == f && p->key == key) return p;
    return 0;
}

static void files_grow(void) {
    uint32_t nb = g_files_nb ? g_files_nb * 2 : 32;
    struct imgfile **t = kzalloc(nb * (uint32_t)sizeof *t);
    if (!t) return;
    for (uint32_t i = 0; i < g_files_nb; i++) {
        for (struct imgfile *f = g_files[i], *n; f; f = n) {
            n = f->hnext;
            f->hnext = t[f->hash & (nb - 1)];
            t[f->hash & (nb - 1)] = f;
        }
    }
    if (g_files) kfree(g_files);
    g_files = t;
    g_files_nb = nb;
}

static void pages_grow(void) {
    uint32_t nb = g_pages_nb ? g_pages_nb * 2 : 256;
    struct imgpage **t = kzalloc(nb * (uint32_t)sizeof *t);
    if (!t) return;
    for (uint32_t i = 0; i < g_pages_nb; i++) {
        for (struct imgpage *p = g_pages[i], *n; p; p = n) {
            n = p->hnext;
            uint32_t b = page_bucket(p->file, p->key, nb);
            p->hnext = t[b];
            t[b] = p;
        }
    }
    if (g_pages) kfree(g_pages);
    g_pages = t;
    g_pages_nb = nb;
}

uint64_t imgcache_lookup(const char *path, uint64_t key) {
    uint32_t h = k_fnv1a(path, k_strlen(path));
    scheduler_preempt_disable();
    struct imgpage *p = page_find(file_find(path, h), key);
    uint64_t frame = 0;
    if (p) { p->lent = 1; frame = p->frame; }
    scheduler_preempt_enable();
    return frame;
}

int imgcache_copy(const char *path, uint64_t key, void *dst) {
    uint32_t h = k_fnv1a(path, k_strlen(path));
    scheduler_preempt_disable();
    struct imgpage *p = page_find(file_find(path, h), key);
    if (p) k_memcpy(dst, (const void *)(uintptr_t)p->frame, 4096);
    scheduler_preempt_enable();
    return p != 0;
}

uint64_t imgcache_epoch(void) {
    return __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
}

uint64_t imgcache_adopt(const char *path, uint64_t key, uint64_t frame, int lend,
                        uint64_t epoch) {
    size_t plen = k_strlen(path);
    uint32_t h = k_fnv1a(path, plen);
    // Both allocations BEFORE the guard's re-check: kmalloc takes the
    // same guard, so it nests, but a miss should not be paid for twice.
    struct imgpage *np = kmalloc(sizeof *np);
    struct imgfile *nf = 0;
    if (!np) return 0;

    scheduler_preempt_disable();
    if (g_epoch != epoch) {   // the file changed while the caller read it
        scheduler_preempt_enable();
        kfree(np);
        return 0;
    }
    struct imgfile *f = file_find(path, h);
    // THE FILL CAN SLEEP (mmap.c reads the disk before offering), and
    // another process may have cached this page meanwhile -- so the
    // lookup runs AGAIN here, with nothing that can switch away between
    // it and the insert. Linux's add_to_page_cache() -EEXIST, same shape.
    struct imgpage *p = page_find(f, key);
    if (p) {
        if (lend) p->lent = 1;
        uint64_t have = p->frame;
        scheduler_preempt_enable();
        kfree(np);
        return have;
    }
    if (!f) {
        nf = kmalloc((uint32_t)(sizeof *nf + plen + 1));
        if (!nf) {
            scheduler_preempt_enable();
            kfree(np);
            return 0;
        }
        k_memset(nf, 0, sizeof *nf);
        k_memcpy(nf->path, path, plen + 1);
        nf->hash = h;
        if (g_nfiles >= g_files_nb) files_grow();
        if (!g_files) {
            scheduler_preempt_enable();
            kfree(nf);
            kfree(np);
            return 0;
        }
        nf->hnext = g_files[h & (g_files_nb - 1)];
        g_files[h & (g_files_nb - 1)] = nf;
        g_nfiles++;
        f = nf;
    }
    if (g_npages >= g_pages_nb) pages_grow();
    if (!g_pages) {   // the file record stays, empty: harmless, forgotten with the path
        scheduler_preempt_enable();
        kfree(np);
        return 0;
    }
    np->file = f;
    np->key = key;
    np->frame = frame;
    np->lent = lend;
    np->fnext = f->pages;
    f->pages = np;
    f->frames++;
    uint32_t b = page_bucket(f, key, g_pages_nb);
    np->hnext = g_pages[b];
    g_pages[b] = np;
    g_npages++;
    scheduler_preempt_enable();
    return frame;
}

void imgcache_fallback(const char *path, uint64_t key, const char *why) {
    uint32_t h = k_fnv1a(path, k_strlen(path));
    scheduler_preempt_disable();
    uint64_t n = ++g_fallbacks;
    struct imgfile *f = file_find(path, h);
    if (f) f->fallbacks++;
    scheduler_preempt_enable();
    if (n > FALLBACK_LOGGED) return;
    klog_printf(KLOG_WARN "imgcache: %s page %#lx not shared -- %s%s\n", path, key, why,
                n == FALLBACK_LOGGED ? " (further ones are counted in meminfo, not logged)" : "");
}

// Drops one file: its pages leave both tables, never-lent frames go back
// to the allocator. Under the guard.
static void file_drop(struct imgfile *f) {
    for (struct imgpage *p = f->pages, *n; p; p = n) {
        n = p->fnext;
        struct imgpage **pp = &g_pages[page_bucket(f, p->key, g_pages_nb)];
        while (*pp && *pp != p) pp = &(*pp)->hnext;
        if (*pp) *pp = p->hnext;
        g_npages--;
        if (p->lent) g_unhooked++;
        else pmm_free_frame(p->frame);
        kfree(p);
    }
    struct imgfile **fp = &g_files[f->hash & (g_files_nb - 1)];
    while (*fp && *fp != f) fp = &(*fp)->hnext;
    if (*fp) *fp = f->hnext;
    g_nfiles--;
    kfree(f);
}

// `path` names a cached file or a directory above one -- a prefix in
// either direction. Paths outside the policy, which is every ordinary
// write, answer here without touching the tables.
static int may_hold(const char *path) {
    size_t n = k_strlen(path);
    for (size_t i = 0; i < NPREFIX; i++) {
        size_t pl = k_strlen(g_prefixes[i]);
        if (k_strncmp(path, g_prefixes[i], pl) == 0) return 1;            // inside
        if (n < pl && k_strncmp(g_prefixes[i], path, n) == 0) return 1;  // "/", "/lib"
    }
    return 0;
}

// A REWRITTEN FILE IS NO LONGER IMMUTABLE, and the development loop
// rewrites them: `remote.py put` lands a rebuilt .so on a running
// machine, and without this the next spawn ran the OLD code with the new
// file on disk -- which reads as the fix not working. A directory forgets
// everything under it: a renamed /lib, a mount over /bin.
void imgcache_forget(const char *path) {
    if (!path || !may_hold(path)) return;
    size_t n = k_strlen(path);
    int all = n == 1 && path[0] == '/';
    scheduler_preempt_disable();
    g_epoch++;
    for (uint32_t i = 0; g_files && i < g_files_nb; i++) {
        for (struct imgfile *f = g_files[i], *next; f; f = next) {
            next = f->hnext;
            if (all || (k_strncmp(f->path, path, n) == 0 &&
                        (f->path[n] == '\0' || f->path[n] == '/')))
                file_drop(f);
        }
    }
    scheduler_preempt_enable();
}

void imgcache_get_stats(struct imgcache_stats *out) {
    scheduler_preempt_disable();
    out->frames = g_npages;
    out->files = g_nfiles;
    out->fallbacks = g_fallbacks;
    out->unhooked = g_unhooked;
    scheduler_preempt_enable();
}

// --- QUERY_IMGCACHE: one record per file --------------------------------

_Static_assert(sizeof(struct query_imgcache) <= QUERY_RECORD_MAX,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

static int imgq_count(void) {
    scheduler_preempt_disable();
    int n = (int)g_nfiles;
    scheduler_preempt_enable();
    return n;
}

static int imgq_fill(int index, void *out) {
    if (index < 0) return 0;
    struct query_imgcache *q = out;
    k_memset(q, 0, sizeof *q);
    int found = 0;
    scheduler_preempt_disable();
    for (uint32_t i = 0; g_files && i < g_files_nb && !found; i++) {
        for (struct imgfile *f = g_files[i]; f; f = f->hnext) {
            if (index--) continue;
            if (k_strlen(f->path) < sizeof q->path) k_strlcpy(q->path, f->path, sizeof q->path);
            q->frames = f->frames;
            q->fallbacks = f->fallbacks;
            found = 1;
            break;
        }
    }
    scheduler_preempt_enable();
    return found;
}

static const struct query_provider imgcache_provider = {
    .cls = QUERY_IMGCACHE,
    .name = "imgcache",
    .record_size = sizeof(struct query_imgcache),
    .flags = QUERY_F_LIST,
    .count = imgq_count,
    .fill = imgq_fill,
    .fields = 0,
    .field_count = 0,
};

void imgcache_query_init(void) {
    query_register(&imgcache_provider);
}
INITCALL(imgcache_query_init, INIT_QUERY);
