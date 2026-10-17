// The disk cache -- see kernel/include/kernel/block_cache.h for what it
// is and why it may sit in the block layer.
//
// driver-none: the block layer's read cache, not a device
//
// ONE PAGE IS 4 KiB OF ONE DISK, with a bit per 512-byte sector saying
// which of them it holds: a partition need not start on a 4 KiB boundary
// and TFS3 reads single sectors, so a page fills piecemeal. A read is a
// hit only when every sector it asks for is held.
//
// TWO LISTS, Linux's inactive/active shape: a page enters INACTIVE and
// moves to ACTIVE on a second use, and eviction takes from inactive,
// topped up from active's cold end so inactive keeps a quarter of the
// cache. A one-off read -- a big file copied once -- passes through
// inactive and leaves the bitmaps, inode tables and program images that
// are read again where they are.
//
// THE LOCK IS irq_save(), on one core, held for one page at a time:
// never across a device command, which can sleep.
#include "block_cache.h"
#include "block.h"
#include "heap.h"
#include "irqflags.h"
#include "klog.h"
#include "kfmt.h"
#include "multiboot.h"
#include "pmm.h"
#include "query.h"
#include "initcall.h"
#include "string.h"
#include <stddef.h>

#define SECT         512u
#define PAGE_SECT    8u
#define PAGE_BYTES   (SECT * PAGE_SECT)
#define CHUNK_PAGES  16u                     // page memory comes 64 KiB at a time
#define NIL          0xffffffffu
#define AUTO_MIN_MIB 4u
#define AUTO_MAX_MIB 64u

enum { L_FREE, L_INACTIVE, L_ACTIVE };

struct ent {
    const struct block_device *disk;
    uint64_t page;        // absolute sector / 8
    uint8_t *data;        // PAGE_BYTES, owned by a chunk
    uint32_t prev, next;  // in its list (the free list uses next only)
    uint32_t hnext;       // hash chain
    uint8_t mask;         // sectors held
    uint8_t list;
};

struct list { uint32_t head, tail, n; };

struct state {
    struct ent *e;
    uint32_t *bucket;      // nbucket heads, a power of two
    uint32_t cap, nbucket;
    uint32_t backed;       // entries with page memory so far
    uint32_t free;         // free list of BACKED entries
    struct list l[3];      // indexed by L_*; l[L_FREE] unused
    uint8_t **chunks;      // cap / CHUNK_PAGES of them, NULL until needed
};

static struct state *g_st;
static int g_off = -1;      // -1 = not decided yet; 1 = `nodiskcache`
static uint32_t g_budget_mib;   // 0 = automatic
static struct bcache_stats g_stats;

// WRITE GENERATIONS, per hashed page, bumped under the lock by every
// write's update of that page. A fill compares the sum over its range
// with the ticket taken before its device read, page by page and under
// the same lock as its insert: a write that landed in between always
// shows. A collision only drops a fill that was safe -- never the reverse.
#define GENS 4096u
static uint32_t g_gen[GENS];

static uint64_t mix(const void *disk, uint64_t page) {
    uint64_t z = (uint64_t)(uintptr_t)disk ^ (page * 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t *gen_of(const void *disk, uint64_t page) { return &g_gen[mix(disk, page) % GENS]; }

// ---- lists and the hash -------------------------------------------------

static void unlink(struct state *s, uint32_t i) {
    struct ent *x = &s->e[i];
    struct list *l = &s->l[x->list];
    if (x->prev != NIL) s->e[x->prev].next = x->next; else l->head = x->next;
    if (x->next != NIL) s->e[x->next].prev = x->prev; else l->tail = x->prev;
    l->n--;
}

static void push_head(struct state *s, uint32_t i, int list) {
    struct ent *x = &s->e[i];
    struct list *l = &s->l[list];
    x->list = (uint8_t)list;
    x->prev = NIL;
    x->next = l->head;
    if (l->head != NIL) s->e[l->head].prev = i; else l->tail = i;
    l->head = i;
    l->n++;
}

static uint32_t *chain(struct state *s, const void *disk, uint64_t page) {
    return &s->bucket[mix(disk, page) & (s->nbucket - 1)];
}

static uint32_t lookup(struct state *s, const struct block_device *disk, uint64_t page) {
    for (uint32_t i = *chain(s, disk, page); i != NIL; i = s->e[i].hnext)
        if (s->e[i].disk == disk && s->e[i].page == page) return i;
    return NIL;
}

static void unhash(struct state *s, uint32_t i) {
    uint32_t *p = chain(s, s->e[i].disk, s->e[i].page);
    while (*p != i) p = &s->e[*p].hnext;
    *p = s->e[i].hnext;
}

static void release(struct state *s, uint32_t i) {
    unhash(s, i);
    unlink(s, i);
    s->e[i].list = L_FREE;
    s->e[i].mask = 0;
    s->e[i].next = s->free;
    s->free = i;
}

// A second use: to the head of active.
static void touch(struct state *s, uint32_t i) {
    unlink(s, i);
    push_head(s, i, L_ACTIVE);
}

// A backed entry to put a new page in: a free one, or the coldest page.
static uint32_t take(struct state *s) {
    if (s->free != NIL) {
        uint32_t i = s->free;
        s->free = s->e[i].next;
        return i;
    }
    while (s->l[L_INACTIVE].n < s->backed / 4 && s->l[L_ACTIVE].n) {
        uint32_t i = s->l[L_ACTIVE].tail;
        unlink(s, i);
        push_head(s, i, L_INACTIVE);
    }
    uint32_t v = s->l[L_INACTIVE].n ? s->l[L_INACTIVE].tail : s->l[L_ACTIVE].tail;
    if (v == NIL) return NIL;
    unhash(s, v);
    unlink(s, v);
    g_stats.evictions++;
    return v;
}

static uint32_t insert(struct state *s, const struct block_device *disk, uint64_t page) {
    uint32_t i = take(s);
    if (i == NIL) return NIL;
    struct ent *x = &s->e[i];
    x->disk = disk;
    x->page = page;
    x->mask = 0;
    uint32_t *c = chain(s, disk, page);
    x->hnext = *c;
    *c = i;
    push_head(s, i, L_INACTIVE);
    return i;
}

// ---- the state: built lazily, rebuilt by a budget change ------------------

static uint32_t budget_pages(void) {
    uint64_t mib = g_budget_mib;
    if (!mib) {
        mib = pmm_total_frames() * pmm_frame_size() / 32 / (1024 * 1024);
        if (mib < AUTO_MIN_MIB) mib = AUTO_MIN_MIB;
        if (mib > AUTO_MAX_MIB) mib = AUTO_MAX_MIB;
    }
    return (uint32_t)(mib * (1024 * 1024 / PAGE_BYTES));
}

static void state_free(struct state *s) {
    if (!s) return;
    for (uint32_t c = 0; s->chunks && c < s->cap / CHUNK_PAGES; c++) kfree(s->chunks[c]);
    kfree(s->chunks);
    kfree(s->bucket);
    kfree(s->e);
    kfree(s);
}

static struct state *state_new(uint32_t cap) {
    cap = (cap + CHUNK_PAGES - 1) / CHUNK_PAGES * CHUNK_PAGES;
    struct state *s = kzalloc(sizeof *s);
    if (!s) return 0;
    s->cap = cap;
    s->nbucket = 1;
    while (s->nbucket < cap) s->nbucket <<= 1;
    s->e = kzalloc(cap * sizeof *s->e);
    s->bucket = kmalloc(s->nbucket * sizeof *s->bucket);
    s->chunks = kzalloc((cap / CHUNK_PAGES) * sizeof *s->chunks);
    if (!s->e || !s->bucket || !s->chunks) { state_free(s); return 0; }
    for (uint32_t b = 0; b < s->nbucket; b++) s->bucket[b] = NIL;
    for (int l = 0; l < 3; l++) s->l[l].head = s->l[l].tail = NIL;
    s->free = NIL;
    return s;
}

static int is_off(void) {
    if (g_off < 0) {
        g_off = multiboot_cmdline_has("nodiskcache") ? 1 : 0;
        if (g_off) klog_write("block: nodiskcache -- the disk cache is off\n");
    }
    return g_off;
}

// The live state, built on first use. Allocation happens outside the
// lock; a racing builder's copy is thrown away.
static struct state *state(void) {
    if (g_st || is_off()) return g_st;
    uint32_t cap = budget_pages();
    struct state *s = state_new(cap);
    if (!s) return 0;
    uint64_t f = irq_save();
    int mine = !g_st;
    if (mine) g_st = s;
    irq_restore(f);
    if (!mine) { state_free(s); return g_st; }
    klog_printf("block: disk cache %u MiB%s\n", (unsigned)(cap / (1024 * 1024 / PAGE_BYTES)),
                g_budget_mib ? "" : " (automatic)");
    return s;
}

// Page memory for one more chunk, if the budget has room: allocated
// outside the lock, joined to the free list under it.
static void grow(struct state *s) {
    // Checked under the lock: a budget change may have freed `s` since
    // the caller looked it up.
    uint64_t f = irq_save();
    int want = g_st == s && s->free == NIL && s->backed < s->cap;
    irq_restore(f);
    if (!want) return;
    uint8_t *mem = kmalloc(CHUNK_PAGES * PAGE_BYTES);
    if (!mem) return;   // a fragmented heap: the cache stops growing and evicts instead
    f = irq_save();
    if (g_st != s || s->backed >= s->cap) { irq_restore(f); kfree(mem); return; }
    uint32_t c = s->backed / CHUNK_PAGES;
    s->chunks[c] = mem;
    for (uint32_t k = 0; k < CHUNK_PAGES; k++) {
        uint32_t i = s->backed++;
        s->e[i].data = mem + k * PAGE_BYTES;
        s->e[i].list = L_FREE;
        s->e[i].next = s->free;
        s->free = i;
    }
    irq_restore(f);
}

// ---- the calls block.c makes -------------------------------------------------

// The sectors of `page` that [alba, alba+count) covers, as a mask, and
// where they sit in the caller's buffer and in the page.
static uint8_t span(uint64_t page, uint64_t alba, int count,
                    uint32_t *first, uint32_t *n, uint64_t *buf_off) {
    uint64_t lo = page * PAGE_SECT, hi = lo + PAGE_SECT;
    uint64_t a = alba > lo ? alba : lo;
    uint64_t b = alba + (uint64_t)count < hi ? alba + (uint64_t)count : hi;
    *first = (uint32_t)(a - lo);
    *n = (uint32_t)(b - a);
    *buf_off = (a - alba) * SECT;
    return (uint8_t)(((1u << *n) - 1) << *first);
}

int bcache_read(const struct block_device *disk, uint64_t alba, int count, void *buf) {
    struct state *s = state();
    if (!s || count <= 0) return 0;
    for (uint64_t p = alba / PAGE_SECT; p <= (alba + (uint64_t)count - 1) / PAGE_SECT; p++) {
        uint32_t first, n;
        uint64_t off;
        uint8_t need = span(p, alba, count, &first, &n, &off);
        uint64_t f = irq_save();
        uint32_t i = g_st == s ? lookup(s, disk, p) : NIL;
        if (i == NIL || (s->e[i].mask & need) != need) {
            g_stats.misses++;
            irq_restore(f);
            return 0;   // the caller reads the whole range from the device
        }
        k_memcpy((uint8_t *)buf + off, s->e[i].data + first * SECT, n * SECT);
        if (s->e[i].list == L_INACTIVE) touch(s, i);
        irq_restore(f);
    }
    uint64_t f = irq_save();
    g_stats.hits++;
    g_stats.hit_sectors += (uint64_t)count;
    irq_restore(f);
    return 1;
}

static uint64_t gen_sum(const struct block_device *disk, uint64_t alba, int count) {
    uint64_t sum = 0;
    for (uint64_t p = alba / PAGE_SECT; p <= (alba + (uint64_t)count - 1) / PAGE_SECT; p++)
        sum += *gen_of(disk, p);
    return sum;
}

uint64_t bcache_ticket(const struct block_device *disk, uint64_t alba, int count) {
    if (count <= 0) return 0;
    uint64_t f = irq_save();
    uint64_t t = gen_sum(disk, alba, count);
    irq_restore(f);
    return t;
}

void bcache_fill(const struct block_device *disk, uint64_t alba, int count,
                 const void *buf, uint64_t ticket) {
    struct state *s = state();
    if (!s || count <= 0) return;
    for (uint64_t p = alba / PAGE_SECT; p <= (alba + (uint64_t)count - 1) / PAGE_SECT; p++) {
        uint32_t first, n;
        uint64_t off;
        uint8_t got = span(p, alba, count, &first, &n, &off);
        grow(s);
        uint64_t f = irq_save();
        if (g_st != s) { irq_restore(f); return; }
        if (gen_sum(disk, alba, count) != ticket) {
            g_stats.dropped_fills++;   // a write landed during the read
            irq_restore(f);
            return;
        }
        uint32_t i = lookup(s, disk, p);
        if (i == NIL) i = insert(s, disk, p);
        if (i != NIL) {
            k_memcpy(s->e[i].data + first * SECT, (const uint8_t *)buf + off, n * SECT);
            s->e[i].mask |= got;
        }
        irq_restore(f);
    }
    uint64_t f = irq_save();
    g_stats.fills++;
    irq_restore(f);
}

void bcache_write_begin(const struct block_device *disk, uint64_t alba, int count) {
    if (count <= 0) return;
    uint64_t f = irq_save();
    for (uint64_t p = alba / PAGE_SECT; p <= (alba + (uint64_t)count - 1) / PAGE_SECT; p++)
        (*gen_of(disk, p))++;
    irq_restore(f);
}

void bcache_write_end(const struct block_device *disk, uint64_t alba, int count,
                      const void *buf, int ok) {
    if (count <= 0) return;
    struct state *s = state();
    for (uint64_t p = alba / PAGE_SECT; p <= (alba + (uint64_t)count - 1) / PAGE_SECT; p++) {
        uint32_t first, n;
        uint64_t off;
        uint8_t got = span(p, alba, count, &first, &n, &off);
        if (s && ok && buf) grow(s);
        uint64_t f = irq_save();
        // THE PAGE'S UPDATE AND ITS SECOND BUMP ARE ONE STEP: a fill that
        // read the old bytes cannot slip in between and put them back.
        (*gen_of(disk, p))++;
        if (s && g_st == s) {
            uint32_t i = lookup(s, disk, p);
            if (ok && buf) {
                if (i == NIL) i = insert(s, disk, p);
                if (i != NIL) {
                    k_memcpy(s->e[i].data + first * SECT, (const uint8_t *)buf + off, n * SECT);
                    s->e[i].mask |= got;
                }
            } else if (i != NIL) {
                s->e[i].mask &= (uint8_t)~got;
                if (!s->e[i].mask) release(s, i);
            }
        }
        irq_restore(f);
    }
}

void bcache_forget(const struct block_device *disk) {
    uint64_t f = irq_save();
    struct state *s = g_st;
    for (uint32_t i = 0; s && i < s->backed; i++)
        if (s->e[i].list != L_FREE && s->e[i].disk == disk) release(s, i);
    irq_restore(f);
}

void bcache_set_budget_mib(uint32_t mib) {
    uint64_t f = irq_save();
    struct state *old = g_st;
    g_st = 0;
    g_budget_mib = mib;
    irq_restore(f);
    // Nobody holds a pointer into `old` across an irq_restore(), so it
    // is unreachable now; the next read builds the new one.
    state_free(old);
}

uint32_t bcache_budget_mib(void) { return g_budget_mib; }

void bcache_drop(void) { bcache_set_budget_mib(g_budget_mib); }

int bcache_verify(void) {
    static int v = -1;
    if (v < 0) v = multiboot_cmdline_has("diskcache=verify") ? 1 : 0;
    return v;
}

void bcache_stats(struct bcache_stats *out) {
    uint64_t f = irq_save();
    *out = g_stats;
    struct state *s = g_st;
    out->on = !is_off();
    out->cap_pages = s ? s->cap : (out->on ? budget_pages() : 0);
    out->pages = s ? s->l[L_INACTIVE].n + s->l[L_ACTIVE].n : 0;
    out->active = s ? s->l[L_ACTIVE].n : 0;
    irq_restore(f);
}

// ---- QUERY_DISKCACHE ---------------------------------------------------------

static int dc_count(void) { return 1; }

static int dc_fill(int index, void *out) {
    if (index != 0) return 0;
    struct bcache_stats st;
    bcache_stats(&st);
    struct query_diskcache *r = out;
    k_memset(r, 0, sizeof *r);
    r->on = (uint64_t)st.on;
    r->pages = st.pages;
    r->cap_pages = st.cap_pages;
    r->active = st.active;
    r->hits = st.hits;
    r->misses = st.misses;
    r->hit_sectors = st.hit_sectors;
    r->fills = st.fills;
    r->dropped_fills = st.dropped_fills;
    r->evictions = st.evictions;
    return 1;
}

static const struct query_field dc_fields[] = {
    QUERY_FIELD(struct query_diskcache, on,            QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, pages,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, cap_pages,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, active,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, hits,          QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, misses,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, hit_sectors,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, fills,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, dropped_fills, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_diskcache, evictions,     QUERY_TYPE_U64),
};

static const struct query_provider dc_provider = {
    .cls = QUERY_DISKCACHE,
    .name = "diskcache",
    .record_size = sizeof(struct query_diskcache),
    .flags = 0,
    .count = dc_count,
    .fill = dc_fill,
    .fields = dc_fields,
    .field_count = sizeof dc_fields / sizeof dc_fields[0],
};

static void bcache_query_init(void) { query_register(&dc_provider); }
INITCALL(bcache_query_init, INIT_QUERY);
