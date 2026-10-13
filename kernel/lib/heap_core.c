// A general-purpose kernel-space heap: kmalloc()/kfree() over a
// doubly-linked list of blocks, coalescing adjacent free blocks back
// together on kfree(), with the FREE blocks also kept in size-class
// lists (g_bin, below) so a request never walks the blocks in use.
// See heap.h for the
// public contract and why this exists (multi-instance app windows --
// gui_apps.h's `multi_instance` flag -- were the first thing that
// actually needed dynamic allocation; before this, every app's state
// was a single static struct because there was nothing else to
// allocate it from).
//
// Built directly on pmm.h's physical frame allocator: boot.asm
// identity-maps the entire low 4GiB as kernel/supervisor-only (see
// paging.c's top comment), so a physical frame pmm_alloc_frame()/
// pmm_alloc_contiguous() hands back is ALREADY a valid kernel-space
// pointer -- no separate vmm_map_user_page()-style mapping step needed.
// This file just carves that identity-mapped space into
// variable-sized blocks.
//
// There's no single contiguous region growing via something like
// sbrk()/brk() -- each time the free list can't satisfy a request, a
// fresh region is pulled from pmm (pmm_alloc_contiguous(), NOT
// guaranteed physically adjacent to a previous region) and linked in
// as one new free block. Blocks are kept in a doubly-linked list in
// the order they were added/split, but coalescing on kfree() checks
// REAL address adjacency ((uint8_t*)(b+1) + b->size == (uint8_t*)next),
// not list adjacency -- two blocks next to each other in the list
// might come from separate, non-adjacent pmm regions.
//
// Not interrupt-safe or reentrant, and NOTHING HERE MAY BE CALLED FROM
// AN ISR -- that is the standing invariant both rings depend on, and
// the one an interrupt handler that starts allocating would break.
//
// kmalloc()/kfree()/heap_check() take heap_os_lock(), which is a real
// lock in ring 3 (two threads of one process are preempted at any
// instruction and share one free list) and preemption-off in the
// kernel. The kernel's half USED to be a no-op, on the argument that
// nothing preempts kernel code mid-kmalloc; making the syscall gate a
// trap gate is what ended that. See api/heap_os.h and
// kernel/mm/heap_os.c.
//
// DEBUG MODE (`heap debug on`, heap_set_debug()) wraps every subsequent
// allocation in red-zones and poisons what it frees:
//
//   off: [header][............ payload ............]
//   on:  [header][span][MAGIC][ payload ][MAGIC][MAGIC]
//                            ^-- the pointer the caller gets
//
// so an overflow past the request, an underflow just before it, and a
// write through an already-freed pointer each land in a known byte
// pattern that kfree() (or the next kmalloc() to reuse the block)
// checks. The mode is a RUNTIME switch rather than a build flag so it
// is reachable in a booted OS and so one build covers both states --
// which means blocks allocated before and after a toggle coexist, and
// kfree() has to tell them apart from the pointer alone.
//
// The trap that makes that sound: kfree() decides by reading the eight
// bytes immediately before the payload, which are HEAP_RZ_MAGIC in a
// red-zoned block and the header's `prev` pointer in a plain one. That
// can never be ambiguous because every heap pointer is a canonical
// address with bits 63:48 clear -- the kernel map ends at 512 GiB and
// the ring-3 map sits just above it -- while HEAP_RZ_MAGIC's top 16
// bits are 0xC0DE. Change either fact -- a pointer with high bits set,
// or a magic whose top 16 bits are zero -- and the two cases become
// indistinguishable, silently, on the freeing path.
#include "heap.h"
#include "heap_os.h"  // the three things this file deliberately does not know
#include "kfmt.h"     // k_snprintf -- freestanding half, see kfmt.h
#include "string.h"
#include "kasan.h"    // no-ops unless this is the KERNEL's copy in a KASAN=1 build

#define HEAP_ALIGN 16
#define HEAP_PAGE_SIZE 4096
#define HEAP_MIN_GROW_PAGES 16 // 64KiB -- avoids growing one page at a time under a run of small allocations

// Per side, and a multiple of HEAP_ALIGN so the payload stays 16-byte
// aligned with the red-zones in front of it.
#define HEAP_RZ_SIZE 16
// Top half nonzero on purpose -- see this file's top comment.
#define HEAP_RZ_MAGIC 0xC0DEFACE5A5A5A5AULL
#define HEAP_POISON 0xDE

// `free` is tri-state rather than a bool: a block freed while debug
// mode was on still carries intact red-zones and a poisoned payload,
// and the next kmalloc() to reuse it verifies both before handing it
// out. That check is the only thing that can catch a write through an
// already-freed pointer.
#define HEAP_IN_USE 0
#define HEAP_FREE 1
#define HEAP_FREE_POISONED 2
// KASAN only: freed, and parked in the quarantine below -- neither
// reusable nor mergeable until it leaves, so a stale pointer still
// finds poison rather than somebody else's allocation.
#define HEAP_HELD 3

// KASAN only: every block carries this much more, at its END. Live, the
// last word is the size the caller asked for (kmalloc_size()'s answer,
// since the payload is opened to exactly that); held, the two words are
// the quarantine link and the state the block leaves in.
#ifdef TOYOS_KASAN
#define HEAP_KASAN_TAIL 16
#define HEAP_KASAN_HOLD_BYTES (1u << 20)
#else
#define HEAP_KASAN_TAIL 0
#endif

// Sits in the four bytes of padding the compiler was inserting after
// `free` anyway, so it costs nothing, and it is what makes kfree()'s
// plain path able to reject a pointer that isn't one of ours.
#define HEAP_HDR_MAGIC 0x48454150u // 'HEAP'

struct heap_block {
    uint64_t size;            // usable payload size, NOT including this header
    int free;
    uint32_t magic;           // HEAP_HDR_MAGIC -- see header_plausible()
    struct heap_block *fnext; // its size class's free list -- meaningful only while
    struct heap_block *fprev; //   free (HEAP_FREE / HEAP_FREE_POISONED); see bin_insert()
    struct heap_block *next;  // list order, not necessarily address order across regions -- see top comment
    struct heap_block *prev;  // MUST stay the last field: kfree() reads these 8 bytes to tell a red-zoned block from a plain one
};

static struct heap_block *g_head = 0;
static struct heap_block *g_tail = 0;

// THE FREE BLOCKS, BY SIZE CLASS -- what kmalloc() searches, so its cost
// follows the number of size classes rather than the number of blocks.
// It used to walk the whole block list, in-use blocks included, which
// made a program holding N allocations pay O(N) for the next one: a
// web page's half a million DOM and CSS objects took minutes to lay out.
//
// Below 1 KiB a class is one exact size (the payload is a multiple of
// 16), so any block in it fits. Above, TLSF's shape: a power of two cut
// into 8 slices, where a block in a HIGHER class always fits and one in
// the request's own class may not (bin_find() scans that one). A block
// is in exactly one list while it is free and in none otherwise; every
// change to a free block's size goes remove, resize, insert.
#define HEAP_EXACT_BINS 64
#define HEAP_BINS 512
static struct heap_block *g_bin[HEAP_BINS];
static uint64_t g_binmap[HEAP_BINS / 64];
static uint64_t g_total_bytes = 0; // sum of every block's payload size ever claimed from pmm, used + free
static uint64_t g_used_bytes = 0;

static int g_debug = 0;
static uint64_t g_rz_checks = 0;
static uint64_t g_rz_violations = 0;
static uint64_t g_quarantined_bytes = 0;

static uint64_t align_up(uint64_t n, uint64_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static unsigned bin_index(uint64_t size) {
    if (size < HEAP_EXACT_BINS * HEAP_ALIGN) return (unsigned)(size / HEAP_ALIGN);
    unsigned fl = 63u - (unsigned)__builtin_clzll(size);       // >= 10
    unsigned sl = (unsigned)(size >> (fl - 3)) & 7u;
    return HEAP_EXACT_BINS + (fl - 10u) * 8u + sl;
}

static void bin_insert(struct heap_block *b) {
    unsigned i = bin_index(b->size);
    b->fprev = 0;
    b->fnext = g_bin[i];
    if (b->fnext) b->fnext->fprev = b;
    g_bin[i] = b;
    g_binmap[i / 64] |= 1ull << (i % 64);
}

// `b` must still have the size it was inserted with.
static void bin_remove(struct heap_block *b) {
    unsigned i = bin_index(b->size);
    if (b->fprev) b->fprev->fnext = b->fnext;
    else g_bin[i] = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
    if (!g_bin[i]) g_binmap[i / 64] &= ~(1ull << (i % 64));
    b->fnext = b->fprev = 0;
}

static int is_free(const struct heap_block *b) {
    return b->free == HEAP_FREE || b->free == HEAP_FREE_POISONED;
}

static void try_merge_next(struct heap_block *b);

// A free block of at least `need`, still in its list, or NULL. The
// request's own class is scanned a few entries deep (above 1 KiB its
// blocks may be smaller than `need`); every higher class fits outright.
static struct heap_block *bin_find(uint64_t need) {
    unsigned i = bin_index(need);
    int scan = 8;
    for (struct heap_block *b = g_bin[i]; b && scan--; b = b->fnext)
        if (b->size >= need) return b;
    for (unsigned w = (i + 1) / 64; w < HEAP_BINS / 64; w++) {
        uint64_t bits = g_binmap[w];
        if (w == (i + 1) / 64) bits &= ~0ull << ((i + 1) % 64);
        if (bits) return g_bin[w * 64 + (unsigned)__builtin_ctzll(bits)];
    }
    return 0;
}

// Asks the platform for one contiguous run of `pages` and links it in
// as a new free block at the tail of the list.
//
// heap_os_alloc() is the ONLY way this file obtains memory, which is
// what lets the same allocator serve two rings: the kernel hands back
// identity-mapped physical frames, ring 3 hands back sbrk'd address
// space. Neither is visible from here.
static struct heap_block *append_region(uint64_t pages) {
    void *region = heap_os_alloc(pages * HEAP_PAGE_SIZE);
    if (!region) return 0;

    struct heap_block *b = (struct heap_block *)region;
    b->size = pages * HEAP_PAGE_SIZE - sizeof(struct heap_block);
    b->free = HEAP_FREE;
    b->magic = HEAP_HDR_MAGIC;
    b->next = 0;
    b->prev = g_tail;
    if (g_tail) g_tail->next = b;
    g_tail = b;
    if (!g_head) g_head = b;

    g_total_bytes += b->size;
    kasan_poison(region, pages * HEAP_PAGE_SIZE, KASAN_HEAP_UNUSED);
    // sbrk hands out consecutive regions, so the old tail is often
    // right before this one: folding a free tail in lets a request
    // bigger than either be met without a third region.
    bin_insert(b);   // first: try_merge_next() takes `b` out of its list
    struct heap_block *p = b->prev;
    if (p && p->free == HEAP_FREE && (uint8_t *)(p + 1) + p->size == (uint8_t *)b) {
        bin_remove(p);
        try_merge_next(p);
        bin_insert(p);
        b = p;
    }
    return b;
}

static struct heap_block *grow_heap(uint64_t min_payload) {
    uint64_t needed = min_payload + sizeof(struct heap_block);
    uint64_t pages = (needed + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    if (pages < HEAP_MIN_GROW_PAGES) pages = HEAP_MIN_GROW_PAGES;
    return append_region(pages);
}

void heap_init(void) {
    g_head = 0;
    g_tail = 0;
    g_total_bytes = 0;
    g_used_bytes = 0;
    for (unsigned i = 0; i < HEAP_BINS; i++) g_bin[i] = 0;
    for (unsigned i = 0; i < HEAP_BINS / 64; i++) g_binmap[i] = 0;
    // Grows lazily on the first kmalloc() that needs it, rather than
    // reserving anything eagerly here -- a boot that never opens a
    // multi-instance window (or hits any future kmalloc caller)
    // shouldn't cost any memory for this at all.
}

// Shrinks `b` (in no free list) to exactly `size` and, if what's left
// over is large enough to be its own useful block (a header plus at
// least HEAP_ALIGN bytes), carves it off as a new free block right
// after `b` in the list -- keeps a large free region from being handed
// out whole to a small request forever. The remainder is returned, in
// its free list, or NULL when nothing was carved.
static struct heap_block *split_block(struct heap_block *b, uint64_t size) {
    uint64_t remaining = b->size - size;
    if (remaining < sizeof(struct heap_block) + HEAP_ALIGN) return 0; // not worth splitting

    struct heap_block *rem = (struct heap_block *)((uint8_t *)(b + 1) + size);
    rem->size = remaining - sizeof(struct heap_block);
    rem->free = HEAP_FREE;
    rem->magic = HEAP_HDR_MAGIC;
    rem->next = b->next;
    rem->prev = b;
    if (b->next) b->next->prev = rem;
    else g_tail = rem;
    b->next = rem;
    b->size = size;
    // A poisoned `b` was kept apart from a free right neighbour
    // (try_merge_next()); its remainder is plain and may now join it.
    try_merge_next(rem);
    bin_insert(rem);
    return rem;
}

// ---- red-zones and poisoning (debug mode only) ----

// Reports a violation and returns 0, so every caller can end with
// `return rz_fail(...)` and quarantining stays one decision.
static int rz_fail(const char *what, const struct heap_block *b,
                   uint64_t expected, uint64_t found) {
    g_rz_violations++;
    g_quarantined_bytes += b->size;
    // %lx/%lu, not %x/%u: kfmt reads a 32-bit argument without the `l`
    // (see kfmt.c), which silently truncates every value here.
    // Formatted here and handed to the platform as text, rather than
    // calling a logger directly: the kernel's sink is klog and ring
    // 3's is stderr, and this file must not know which it is talking
    // to. k_snprintf is the freestanding half of kfmt (see kfmt.h), so
    // it is available on both sides.
    char msg[192];
    k_snprintf(msg, sizeof msg,
               "heap: RED-ZONE VIOLATION %s block=0x%lx size=%lu\n",
               what, (uint64_t)(uintptr_t)b, b->size);
    heap_os_report(msg);
    k_snprintf(msg, sizeof msg,
               "heap:   expected 0x%lx, found 0x%lx -- block quarantined, not returned to the free list\n",
               expected, found);
    heap_os_report(msg);
    return 0;
}

// Lays out [span][MAGIC] before the payload and [MAGIC][MAGIC] after
// it, and returns the pointer the caller gets. `span` is the payload
// the caller may legitimately touch; b->size can exceed span + 2 *
// HEAP_RZ_SIZE when split_block() declined to carve off the remainder,
// and that slack sits AFTER the right red-zone rather than inside it.
static void *rz_arm(struct heap_block *b, uint64_t span) {
    uint64_t *lead = (uint64_t *)(b + 1);
    lead[0] = span;
    lead[1] = HEAP_RZ_MAGIC;

    uint8_t *payload = (uint8_t *)(lead + 2);
    uint64_t *trail = (uint64_t *)(payload + span);
    trail[0] = HEAP_RZ_MAGIC;
    trail[1] = HEAP_RZ_MAGIC;
    return payload;
}

// Is `ptr` the payload of a red-zoned block? See the top comment for
// why reading the eight bytes before it cannot be ambiguous.
static int rz_armed(const void *ptr) {
    return *((const uint64_t *)ptr - 1) == HEAP_RZ_MAGIC;
}

// Checks both red-zones of an armed block. `span_out` receives the
// recorded payload length. Returns 0 (and has already reported) on a
// violation.
static int rz_check(struct heap_block *b, const void *ptr, uint64_t *span_out) {
    g_rz_checks++;

    const uint64_t *lead = (const uint64_t *)(b + 1);
    uint64_t span = lead[0];
    // The length word is itself inside the left red-zone, so an
    // underflow of 9..16 bytes lands here rather than on the magic.
    // Rejecting an impossible span is what turns that into a report
    // instead of a wild pointer.
    if (span == 0 || (span % HEAP_ALIGN) != 0 || span + 2 * HEAP_RZ_SIZE > b->size)
        return rz_fail("left red-zone (length word)", b, b->size, span);
    if (lead[1] != HEAP_RZ_MAGIC)
        return rz_fail("left red-zone", b, HEAP_RZ_MAGIC, lead[1]);

    const uint64_t *trail = (const uint64_t *)((const uint8_t *)ptr + span);
    if (trail[0] != HEAP_RZ_MAGIC)
        return rz_fail("right red-zone", b, HEAP_RZ_MAGIC, trail[0]);
    if (trail[1] != HEAP_RZ_MAGIC)
        return rz_fail("right red-zone (second word)", b, HEAP_RZ_MAGIC, trail[1]);

    *span_out = span;
    return 1;
}

// Verifies a poisoned free block before it is handed out again: the
// red-zones must still stand AND every payload byte must still be
// HEAP_POISON. Anything else is a write through a pointer whose owner
// already freed it.
static int rz_check_poison(struct heap_block *b) {
    void *ptr = (uint8_t *)(b + 1) + HEAP_RZ_SIZE;
    uint64_t span = 0;
    if (!rz_check(b, ptr, &span)) return 0;

    const uint8_t *p = (const uint8_t *)ptr;
    for (uint64_t i = 0; i < span; i++) {
        if (p[i] != HEAP_POISON)
            return rz_fail("use-after-free (poison overwritten)", b, HEAP_POISON, p[i]);
    }
    return 1;
}

// Takes a block permanently out of circulation. Leaking it is the
// point: its metadata is the thing that proved untrustworthy, so
// putting it back on the free list hands the damage to the next
// allocation.
static void quarantine(struct heap_block *b) {
    if (is_free(b)) bin_remove(b);
    b->free = HEAP_IN_USE;
}

// Could this plausibly be a header this allocator wrote? Checked on
// kfree()'s plain path, and it is not paranoia: an underflow of 1..8
// bytes lands on the magic, which makes an armed block look plain, and
// kfree() would then take its "header" from 16 bytes inside the real
// one and start unlinking whatever it found there. This turns that
// into a report. Deliberately cheap and always on, debug mode or not.
static int header_plausible(const struct heap_block *b) {
    if (b->magic != HEAP_HDR_MAGIC) return 0;
    if (b->free != HEAP_IN_USE && b->free != HEAP_FREE && b->free != HEAP_FREE_POISONED &&
        b->free != HEAP_HELD) return 0;
    if (b->size == 0 || b->size > g_total_bytes) return 0;
    return 1;
}

// ---- KASAN (kernel, KASAN=1 only) ----

static void release(struct heap_block *b);

#ifdef TOYOS_KASAN
static uint64_t *kasan_tail(struct heap_block *b) {
    return (uint64_t *)((uint8_t *)(b + 1) + b->size - HEAP_KASAN_TAIL);
}

// Everything the block owns -- header, red-zones, slack, tail -- is off
// limits, then exactly `open` bytes of payload are opened.
static void *kasan_open(struct heap_block *b, void *p, uint64_t open) {
    kasan_poison(b, sizeof *b + b->size, KASAN_HEAP_REDZONE);
    kasan_unpoison(p, open);
    kasan_tail(b)[1] = open;
    return p;
}

static struct heap_block *g_held_head, *g_held_tail;
static uint64_t g_held_bytes;

// A freed block is poisoned and parked, oldest leaving first once the
// quarantine holds more than HEAP_KASAN_HOLD_BYTES -- Linux's KASAN
// quarantine, which is what lets a use-after-free find poison instead of
// the next owner's data.
static void kasan_hold(struct heap_block *b, int state) {
    kasan_poison(b + 1, b->size, KASAN_HEAP_FREE);
    b->free = HEAP_HELD;
    uint64_t *t = kasan_tail(b);
    t[0] = 0;
    t[1] = (uint64_t)state;
    if (g_held_tail) kasan_tail(g_held_tail)[0] = (uint64_t)(uintptr_t)b;
    else g_held_head = b;
    g_held_tail = b;
    g_held_bytes += b->size;
    while (g_held_bytes > HEAP_KASAN_HOLD_BYTES && g_held_head) {
        struct heap_block *old = g_held_head;
        g_held_head = (struct heap_block *)(uintptr_t)kasan_tail(old)[0];
        if (!g_held_head) g_held_tail = 0;
        g_held_bytes -= old->size;
        old->free = (int)kasan_tail(old)[1];
        release(old);
    }
}
#else
static void *kasan_open(struct heap_block *b, void *p, uint64_t open) {
    (void)b; (void)open;
    return p;
}
#endif

// THE BODY, called with the lock held. The public entry points below
// are thin wrappers so that every one of these `return`s does not have
// to remember to release it -- the shape a `goto out` would otherwise
// force on a function with six of them.
static void *kmalloc_locked(size_t size) {
    // Inert unless a test armed it (the kernel wires this to
    // fault_inject.h; ring 3 answers 0 always). Returning NULL here is
    // exactly what a genuinely exhausted heap does, which is the whole
    // point -- it exercises every caller's failure path.
    if (heap_os_should_fail_alloc()) return 0;
    if (size == 0) return 0;
    uint64_t span = align_up(size, HEAP_ALIGN);
    uint64_t rz = g_debug ? HEAP_RZ_SIZE : 0;
    uint64_t need = span + 2 * rz + HEAP_KASAN_TAIL; // a red-zone on each side of the payload

    struct heap_block *b;
    for (;;) {
        b = bin_find(need);
        if (!b) {
            b = grow_heap(need);
            if (!b) return 0; // out of physical memory
        }
        // A poisoned block is verified BEFORE it is split or handed
        // out -- once it is reused, the evidence is gone.
        if (b->free == HEAP_FREE_POISONED && !rz_check_poison(b)) {
            quarantine(b);
            continue; // damaged; keep looking rather than handing it out
        }
        break;
    }
    bin_remove(b);
    split_block(b, need);
    b->free = HEAP_IN_USE;
    g_used_bytes += b->size;
    return kasan_open(b, rz ? rz_arm(b, span) : (void *)(b + 1), rz ? span : size);
}

void *kmalloc(size_t size) {
    heap_os_lock();
    void *p = kmalloc_locked(size);
    heap_os_unlock();
    return p;
}

void *kzalloc(size_t size) {
    // The memset is OUTSIDE the lock deliberately: the block is this
    // caller's the moment kmalloc() returns it, so zeroing it holds up
    // nobody else.
    void *p = kmalloc(size);
    if (p) k_memset(p, 0, size);
    return p;
}

// Merges `b` with its address-adjacent neighbor if that neighbor is
// also free -- checked by real pointer arithmetic, not list adjacency
// (see this file's top comment for why the two aren't the same thing
// here).
static void try_merge_next(struct heap_block *b) {
    struct heap_block *n = b->next;
    // A POISONED block never merges, either way: merging hides its
    // payload inside a bigger block, and the write through a stale
    // pointer that the poison exists to catch would go unseen -- which
    // used to depend on whether the freed block's neighbours happened to
    // be free. It costs fragmentation only while `heap debug` is on;
    // Linux's KASAN quarantine holds freed memory back for the same reason.
    if (!n || n->free != HEAP_FREE) return;
    if ((uint8_t *)(b + 1) + b->size != (uint8_t *)n) return; // not physically adjacent

    bin_remove(n);
    b->size += sizeof(struct heap_block) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    else g_tail = b;
    b->free = HEAP_FREE;
}

// The usable size of a live block, which realloc() cannot be written
// without: it has to copy min(old, new) bytes and only the allocator
// knows the first. Reading `new` bytes from a shorter block would walk
// off the end of the last block in a region and into an unmapped page.
//
// The discrimination is kfree()'s, because the two must agree about
// which shape a pointer has -- an armed (red-zoned) block's payload
// length is the word recorded inside its left red-zone, not the
// header's `size`, which also covers both zones.
//
// Returns 0 for NULL or for a pointer this heap did not hand out, which
// makes "copy min(size, n)" degrade to copying nothing rather than to
// copying garbage.
uint64_t kmalloc_size(const void *ptr) {
    if (!ptr) return 0;
    if (rz_armed(ptr)) {
        const struct heap_block *b =
            (const struct heap_block *)((const uint8_t *)ptr - HEAP_RZ_SIZE) - 1;
        if (b->free != HEAP_IN_USE) return 0;
        uint64_t span = *(const uint64_t *)(b + 1);
        // The same plausibility rule rz_check() applies, so a corrupt
        // length word answers 0 rather than a huge number.
        if (span == 0 || (span % HEAP_ALIGN) != 0 || span + 2 * HEAP_RZ_SIZE > b->size)
            return 0;
        return span;
    }
    const struct heap_block *b = (const struct heap_block *)ptr - 1;
    if (!header_plausible((struct heap_block *)b)) return 0;
    if (b->free != HEAP_IN_USE) return 0;
#ifdef TOYOS_KASAN
    return kasan_tail((struct heap_block *)b)[1];   // what was opened, exactly
#else
    return b->size;
#endif
}

static void kfree_locked(void *ptr, uintptr_t ip) {
    (void)ip;

    struct heap_block *b;
    uint64_t span = 0;
    int armed = rz_armed(ptr);
    if (armed) {
        b = (struct heap_block *)((uint8_t *)ptr - HEAP_RZ_SIZE) - 1;
#ifdef TOYOS_KASAN
        if (b->free == HEAP_HELD) kasan_report_bad_free(ptr, "double-free", ip);
#endif
        if (b->free != HEAP_IN_USE) return; // double-free, same contract as below
        if (!rz_check(b, ptr, &span)) {
            // Accounting stays as-is: a quarantined block is still
            // held, it just can never be handed out again.
            quarantine(b);
            return;
        }
    } else {
        b = (struct heap_block *)ptr - 1;
        if (!header_plausible(b)) {
            g_rz_violations++;
            char msg[160];
            k_snprintf(msg, sizeof msg,
                       "heap: CORRUPT HEADER at 0x%lx (size=%lu state=%lu) -- refusing to free\n",
                       (uint64_t)(uintptr_t)b, b->size, (uint64_t)b->free);
            heap_os_report(msg);
#ifdef TOYOS_KASAN
            kasan_report_bad_free(ptr, "invalid-free", ip);
#endif
            return;
        }
    }
#ifdef TOYOS_KASAN
    if (b->free == HEAP_HELD) kasan_report_bad_free(ptr, "double-free", ip);
#endif
    if (b->free != HEAP_IN_USE) return; // double-free -- silently ignored, same "trust the caller, don't crash" contract as pmm_free_frame()

    g_used_bytes -= b->size;
    int state = HEAP_FREE;
    if (armed) {
        k_memset(ptr, HEAP_POISON, span);
        state = HEAP_FREE_POISONED;
    }
#ifdef TOYOS_KASAN
    kasan_hold(b, state);
#else
    b->free = state;
    release(b);
#endif
}

// A freed block rejoins the free list: merged with any free neighbour.
static void release(struct heap_block *b) {
    if (b->free == HEAP_FREE_POISONED) {   // kept apart: see try_merge_next()
        bin_insert(b);
        return;
    }
    try_merge_next(b);            // pull a free right-neighbor into b
    // Pull b (now possibly bigger) into a free left-neighbor -- but
    // ONLY if that left-neighbor is itself free. try_merge_next()
    // only checks whether the *next* pointer's target is free before
    // merging; it doesn't check whether the block passed in (here,
    // b->prev) is free. Without this guard, freeing a block whose
    // list-previous neighbor is still in USE would silently fold this
    // freed block's size into that in-use block's `size` field --
    // corrupting its accounting (it would report itself as bigger than
    // it was ever allocated for) without ever adding the extra bytes
    // to g_used_bytes. The bug stayed invisible until something first
    // read heap_used_bytes() for real (Task Manager, see the git history)
    // and displayed an impossible ~16 exabyte figure -- caused by a
    // LATER kfree() of that same corrupted block subtracting its
    // inflated size from g_used_bytes, underflowing the unsigned
    // counter. See docs/decisions.md for the full story.
    bin_insert(b);   // before the merge below, which takes `b` out again
    struct heap_block *p = b->prev;
    if (p && p->free == HEAP_FREE && (uint8_t *)(p + 1) + p->size == (uint8_t *)b) {
        bin_remove(p);
        try_merge_next(p);
        bin_insert(p);
    }
}

void kfree(void *ptr) {
    // The NULL check is out here rather than inside: free(NULL) is
    // legal and common, and it has no business taking a lock.
    if (!ptr) return;
    heap_os_lock();
    kfree_locked(ptr, (uintptr_t)__builtin_return_address(0));
    heap_os_unlock();
}

// Resizes a plain block where it stands -- a shrink gives its tail
// back, a grow takes a free right neighbour -- and moves it only when
// neither fits. A red-zoned block (and every block under KASAN, whose
// payload is opened to an exact length) always moves: its layout is
// sized to the old request.
static void *krealloc_locked(void *p, size_t n) {
    struct heap_block *b = (struct heap_block *)p - 1;
    uint64_t need = align_up(n, HEAP_ALIGN);
#ifndef TOYOS_KASAN
    if (!rz_armed(p) && header_plausible(b) && b->free == HEAP_IN_USE) {
        uint64_t old = b->size;
        struct heap_block *nx = b->next;
        if (need > old && nx && nx->free == HEAP_FREE &&
            (uint8_t *)(b + 1) + old == (uint8_t *)nx &&
            old + sizeof(struct heap_block) + nx->size >= need) {
            bin_remove(nx);
            b->size += sizeof(struct heap_block) + nx->size;
            b->next = nx->next;
            if (nx->next) nx->next->prev = b;
            else g_tail = b;
        }
        if (need <= b->size) {
            struct heap_block *rem = split_block(b, need);
            if (rem) {
                // The tail may sit against a free block: merged, as a free would.
                bin_remove(rem);
                release(rem);
            }
            g_used_bytes = g_used_bytes - old + b->size;
            return p;
        }
    }
#endif
    void *q = kmalloc_locked(n);
    if (!q) return 0; // C: the ORIGINAL block is still valid on failure
    uint64_t have = kmalloc_size(p);
    k_memcpy(q, p, have < n ? have : n);
    kfree_locked(p, (uintptr_t)__builtin_return_address(0));
    return q;
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    if (!n) { kfree(p); return 0; }
    heap_os_lock();
    void *q = krealloc_locked(p, n);
    heap_os_unlock();
    return q;
}

uint64_t heap_total_bytes(void) { return g_total_bytes; }
uint64_t heap_used_bytes(void) { return g_used_bytes; }

// Walks every poisoned free block and verifies it now, rather than
// waiting for the allocation that happens to reuse it. Without this, a
// use-after-free is only ever caught at reuse -- which may be a
// thousand allocations later, in an unrelated subsystem, or never.
// Returns the number of violations found; each one is reported and its
// block quarantined, exactly as at reuse.
uint64_t heap_check(void) {
    // Walks the list, so it locks -- the one diagnostic here that does.
    // The counters below do not: each is a single aligned word, and a
    // reader that catches one an allocation early is reading a heap
    // that is moving anyway.
    heap_os_lock();
    uint64_t before = g_rz_violations;
    for (struct heap_block *b = g_head; b; b = b->next) {
        if (b->free != HEAP_FREE_POISONED) continue;
        if (!rz_check_poison(b)) quarantine(b);
    }
    uint64_t found = g_rz_violations - before;
    heap_os_unlock();
    return found;
}

// The free lists against the block list: every free block in the list
// of its size class and nowhere else, no two plain free blocks side by
// side (release() would have merged them), and the used count equal to the
// in-use payloads. 0 when they agree, else the number of disagreements.
// A walk over everything, under the lock: for tests, never a hot path.
uint64_t heap_validate(void) {
    heap_os_lock();
    uint64_t bad = 0, in_bins = 0, free_blocks = 0, used = 0;
    for (unsigned i = 0; i < HEAP_BINS; i++) {
        int set = (g_binmap[i / 64] >> (i % 64)) & 1;
        if (set != (g_bin[i] != 0)) bad++;
        for (struct heap_block *f = g_bin[i]; f; f = f->fnext) {
            if (!is_free(f) || bin_index(f->size) != i) bad++;
            if (f->fnext && f->fnext->fprev != f) bad++;
            if (++in_bins > g_total_bytes) { bad++; break; } // a cycle
        }
    }
    for (struct heap_block *b = g_head; b; b = b->next) {
        if (is_free(b)) {
            free_blocks++;
            struct heap_block *n = b->next;
            if (b->free == HEAP_FREE && n && n->free == HEAP_FREE &&
                (uint8_t *)(b + 1) + b->size == (uint8_t *)n)
                bad++;   // poisoned blocks stay apart on purpose
        } else if (b->free == HEAP_IN_USE) {
            used += b->size;
        }
    }
    if (free_blocks != in_bins) bad++;
    // A block quarantined while free is IN_USE without ever having been
    // counted as used, so the walk may exceed the count by up to that.
    if (used < g_used_bytes || used > g_used_bytes + g_quarantined_bytes) bad++;
    heap_os_unlock();
    return bad;
}

// Affects allocations made from here on, not existing ones -- see the
// top comment on why both kinds have to coexist.
void heap_set_debug(int on) { g_debug = on ? 1 : 0; }
int heap_debug(void) { return g_debug; }
uint64_t heap_rz_checks(void) { return g_rz_checks; }
uint64_t heap_violations(void) { return g_rz_violations; }
uint64_t heap_quarantined_bytes(void) { return g_quarantined_bytes; }

int heap_selftest(void) {
    // Baseline rather than an absolute 0. This used to check
    // heap_used_bytes() == 0 after freeing everything it allocated,
    // which was true only because it ran from kernel_main() immediately
    // after heap_init(), before anything else existed. Run from `ktest`
    // in a booted system -- shell, filesystem buffers, GUI state all
    // holding allocations -- that check fails on a perfectly healthy
    // heap. Comparing against the level on entry keeps exactly the
    // property this test exists for (see the comment below: a
    // coalescing bug that corrupts a neighbour shows up as an
    // accounting mismatch) without assuming it owns the machine.
    uint64_t used_before = heap_used_bytes();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kzalloc(32);
    if (!a || !b || !c) {
        heap_os_report("heap: selftest FAILED (allocation returned 0)\n");
        return 0; // failure -- see the message above
    }

    uint8_t *cz = (uint8_t *)c;
    for (int i = 0; i < 32; i++) {
        if (cz[i] != 0) {
            heap_os_report("heap: selftest FAILED (kzalloc didn't zero)\n");
            return 0; // failure -- see the message above
        }
    }

    k_memset(a, 0xAA, 64);
    kfree(b); // free the middle block first, then its neighbors -- exercises coalescing on both sides
    kfree(a);
    kfree(c);

    // Every block from a/b/c is free again at this point -- g_used_bytes
    // should be back to whatever it was before this test allocated
    // anything. This
    // check exists specifically because a real bug once slipped past
    // this self-test entirely: freeing a's still-in-use LEFT neighbor
    // was silently folded into it by the coalescing logic, corrupting
    // that neighbor's size without it ever showing up as a crash or a
    // failed allocation here -- only a later kfree() of the corrupted
    // block would underflow g_used_bytes, and nothing here used to
    // check g_used_bytes at all. See docs/decisions.md.
    if (heap_used_bytes() != used_before) {
        heap_os_report("heap: selftest FAILED (used_bytes not back to its starting level after freeing everything)\n");
        return 0; // failure -- see the message above
    }

    void *d = kmalloc(64);
    if (!d) {
        heap_os_report("heap: selftest FAILED (alloc after free-and-coalesce)\n");
        return 0; // failure -- see the message above
    }
    kfree(d);

    if (heap_used_bytes() != used_before) {
        heap_os_report("heap: selftest FAILED (used_bytes not back to its starting level after final free)\n");
        return 0; // failure -- see the message above
    }

    heap_os_report("heap: selftest passed\n");
    return 1;
}
