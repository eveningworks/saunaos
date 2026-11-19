// SYS_MPROTECT's engine (mmap_protect) and W^X, from the kernel side: a
// test address space and region list, driven through the same function
// the syscall calls. Ring 3's view -- code actually run, a store that
// faults, the sandbox's promise -- is userland/tests/mmap_test.c and
// sandbox_test.c.
//
// What a broken version would still pass, and what stops it: a protect
// that set the region and not the leaves passes every region check, so
// each case reads the PTE back (vmm_user_page_prot); one that rewrote
// the leaves and not the region passes those, so a page faulted in
// AFTER the call is checked too; a W|X refusal that refused everything
// would pass the refusal, so the legal flips are checked first.
#include "ktest.h"
#include "kapi.h"
#include "vmm.h"
#include "mmap.h"
#include "scheduler.h"
#include "uaddr.h"
#include "errno.h"
#include "syscall_abi.h"  // SYS_PROT_*
#include "klimit.h"
#include "pmm.h"

#define R  SYS_PROT_READ
#define RW (SYS_PROT_READ | SYS_PROT_WRITE)
#define RX (SYS_PROT_READ | SYS_PROT_EXEC)
#define NONE SYS_PROT_NONE
#define PG 4096ULL

struct fixture { struct sched_mm *mm; uint64_t as; };

static int fx_open(struct fixture *f) {
    f->mm = kzalloc(sizeof *f->mm);
    f->as = vmm_create_address_space();
    if (f->mm) f->mm->data_cur = SYS_RLIM_INFINITY; // a case that charges sets its own
    return f->mm && f->as;
}

static void fx_close(struct fixture *f) {
    if (f->as) vmm_destroy_address_space(f->as);
    if (f->mm) { mmap_regions_reset(f->mm); kfree(f->mm); }
}

static int fx_region(struct fixture *f, uint64_t base, uint64_t npages, uint8_t prot,
                     uint8_t kind) {
    struct mmap_region *r = mmap_test_free_slot(f->mm);
    if (!r) return 0;
    r->npages = npages;
    r->prot = prot;
    r->kind = kind;
    r->shm_idx = -1;
    r->base = base;
    return 1;
}

static int live_regions(const struct sched_mm *mm) {
    int n = 0;
    for (int i = 0; i < mm->region_cap; i++) n += mm->regions[i].base != 0;
    return n;
}

static int prot_at(const struct sched_mm *mm, uint64_t va) {
    for (int i = 0; i < mm->region_cap; i++) {
        const struct mmap_region *r = &mm->regions[i];
        if (r->base && va >= r->base && va < r->base + r->npages * PG) return r->prot;
    }
    return -1;
}

KTEST("mm", "mprotect's prot check refuses W|X with EACCES, unknown bits with EINVAL") {
    KTEST_ASSERT_EQ(mmap_prot_check(R), 0);
    KTEST_ASSERT_EQ(mmap_prot_check(RW), 0);
    KTEST_ASSERT_EQ(mmap_prot_check(RX), 0);
    KTEST_ASSERT_EQ(mmap_prot_check(RW | SYS_PROT_EXEC), -EACCES);
    KTEST_ASSERT_EQ(mmap_prot_check(SYS_PROT_WRITE), -EINVAL);   // READ is required
    KTEST_ASSERT_EQ(mmap_prot_check(NONE), 0);                     // ...unless it is NONE
    KTEST_ASSERT_EQ(mmap_prot_check(R | 0x8), -EINVAL);
}

KTEST("mm", "mprotect makes present pages RX and back, splitting and refolding the region") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 4, RW, MMAP_KIND_ANON));
    for (int i = 0; i < 4; i++) KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b + i * PG), 1);

    // The middle two pages become code.
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b + PG, 2 * PG, RX), 0);
    int p0 = vmm_user_page_prot(f.as, b), p1 = vmm_user_page_prot(f.as, b + PG);
    int p2 = vmm_user_page_prot(f.as, b + 2 * PG), p3 = vmm_user_page_prot(f.as, b + 3 * PG);
    int split = live_regions(f.mm);

    // A W|X request changes nothing.
    int wx = mmap_protect(f.mm, f.as, b + PG, PG, RW | SYS_PROT_EXEC);
    int p1_after_wx = vmm_user_page_prot(f.as, b + PG);

    // Back to data: one region again, and a length that is not a page
    // multiple rounds up (POSIX), so 1 byte covers the page.
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b + PG, PG + 1, RW), 0);
    int back = vmm_user_page_prot(f.as, b + 2 * PG);
    int merged = live_regions(f.mm);
    fx_close(&f);

    KTEST_ASSERT_EQ(p0, VMM_PAGE_W);
    KTEST_ASSERT_EQ(p1, VMM_PAGE_X);
    KTEST_ASSERT_EQ(p2, VMM_PAGE_X);
    KTEST_ASSERT_EQ(p3, VMM_PAGE_W);
    KTEST_ASSERT_EQ(split, 3);
    KTEST_ASSERT_EQ(wx, -EACCES);
    KTEST_ASSERT_EQ(p1_after_wx, VMM_PAGE_X);
    KTEST_ASSERT_EQ(back, VMM_PAGE_W);
    KTEST_ASSERT_EQ(merged, 1);
}

KTEST("mm", "a page first touched after mprotect is mapped with the new protection") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 2, RW, MMAP_KIND_ANON));
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b + PG, PG, RX), 0);
    int before = vmm_user_page_prot(f.as, b + PG);          // nothing there yet
    int got1 = mmap_fault_in(f.mm, f.as, b + PG);
    int got0 = mmap_fault_in(f.mm, f.as, b);
    int p1 = vmm_user_page_prot(f.as, b + PG), p0 = vmm_user_page_prot(f.as, b);
    fx_close(&f);

    KTEST_ASSERT_EQ(before, -1);
    KTEST_ASSERT_EQ(got1, 1);
    KTEST_ASSERT_EQ(got0, 1);
    KTEST_ASSERT_EQ(p1, VMM_PAGE_X);
    KTEST_ASSERT_EQ(p0, VMM_PAGE_W);
}

KTEST("mm", "mprotect spans adjacent anonymous regions, refuses a hole or a file, and changes nothing then") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 1, RW, MMAP_KIND_ANON));
    KTEST_ASSERT(fx_region(&f, b + PG, 1, R, MMAP_KIND_ANON));   // adjacent, another prot
    KTEST_ASSERT(fx_region(&f, b + 3 * PG, 1, RW, MMAP_KIND_ANON)); // a hole at b + 2 pages
    KTEST_ASSERT(fx_region(&f, b + 5 * PG, 1, R, MMAP_KIND_FILE));

    int hole = mmap_protect(f.mm, f.as, b, 4 * PG, RX);
    int n_after_hole = live_regions(f.mm);
    int prot0 = prot_at(f.mm, b);
    int file = mmap_protect(f.mm, f.as, b + 5 * PG, PG, RX);
    int unaligned = mmap_protect(f.mm, f.as, b + 1, PG, RX);
    int below = mmap_protect(f.mm, f.as, UADDR_MMAP_BASE - PG, PG, RX);
    int empty = mmap_protect(f.mm, f.as, b, 0, RX);
    // The two adjacent ones, as one call: one RX region after.
    int both = mmap_protect(f.mm, f.as, b, 2 * PG, RX);
    int n_after_both = live_regions(f.mm);
    int prot1 = prot_at(f.mm, b + PG);
    fx_close(&f);

    KTEST_ASSERT_EQ(hole, -ENOMEM);
    KTEST_ASSERT_EQ(n_after_hole, 4);
    KTEST_ASSERT_EQ(prot0, RW);
    KTEST_ASSERT_EQ(file, -EACCES);
    KTEST_ASSERT_EQ(unaligned, -EINVAL);
    KTEST_ASSERT_EQ(below, -EINVAL);
    KTEST_ASSERT_EQ(empty, -EINVAL);
    KTEST_ASSERT_EQ(both, 0);
    KTEST_ASSERT_EQ(n_after_both, 3);
    KTEST_ASSERT_EQ(prot1, RX);
}

// THE FORK CASE, which is where a naive bit flip goes wrong twice: a
// shared frame given W would let the parent write the child's page, and
// an RX page left COW would get W back from the first store's fault.
KTEST("mm", "mprotect never makes a page shared after fork writable, nor an RX page COW") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 1, RW, MMAP_KIND_ANON));
    KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b), 1);
    uint64_t child = vmm_fork_address_space(f.as, 0);
    KTEST_ASSERT(child != 0);
    uint64_t frame = vmm_user_phys(f.as, b);

    int shared = vmm_user_page_prot(f.as, b);                  // COW after the fork
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b, PG, RX), 0);
    int rx = vmm_user_page_prot(f.as, b);
    int store_on_rx = vmm_fault_in(f.as, b, 2);                // a write fault: stays fatal
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b, PG, RW), 0);
    int rw = vmm_user_page_prot(f.as, b);                      // still shared: COW, not W
    int broke = vmm_fault_in(f.as, b, 2);
    int own = vmm_user_page_prot(f.as, b);
    uint64_t now = vmm_user_phys(f.as, b);
    uint64_t childs = vmm_user_phys(child, b);
    vmm_destroy_address_space(child);
    fx_close(&f);

    KTEST_ASSERT_EQ(shared, VMM_PAGE_COW);
    KTEST_ASSERT_EQ(rx, VMM_PAGE_X);
    KTEST_ASSERT_EQ(store_on_rx, 0);
    KTEST_ASSERT_EQ(rw, VMM_PAGE_COW);
    KTEST_ASSERT_EQ(broke, 1);
    KTEST_ASSERT_EQ(own, VMM_PAGE_W);
    KTEST_ASSERT(now != frame);                                 // a copy of its own
    KTEST_ASSERT_EQ(childs, frame);
}

// A JIT FLIPS PAGES ONE AT A TIME, so the list must hold one region per
// run of one protection -- and a cut refused at the ceiling must leave
// the memory as it was.
KTEST("mm", "mprotect at the region ceiling refuses whole and leaves no stray split") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 8, RW, MMAP_KIND_ANON));
    // Every other page RX, then all back: 8 runs, then 1.
    for (int i = 1; i < 8; i += 2) KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b + i * PG, PG, RX), 0);
    int runs = live_regions(f.mm);
    for (int i = 1; i < 8; i += 2) KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b + i * PG, PG, RW), 0);
    int one = live_regions(f.mm);

    // A list already at its ceiling: a middle cut needs two slots.
    uint32_t was = klimit_test_set(&mmap_regions_limit, (uint32_t)f.mm->region_cap);
    while (live_regions(f.mm) < f.mm->region_cap)
        fx_region(&f, b + (uint64_t)(100 + live_regions(f.mm)) * PG, 1, R, MMAP_KIND_FILE);
    int full = mmap_protect(f.mm, f.as, b + 3 * PG, PG, RX);
    int prot3 = prot_at(f.mm, b + 3 * PG), prot0 = prot_at(f.mm, b);
    int n_full = live_regions(f.mm);
    int cap = f.mm->region_cap;
    klimit_test_set(&mmap_regions_limit, was);
    fx_close(&f);

    KTEST_ASSERT_EQ(runs, 8);
    KTEST_ASSERT_EQ(one, 1);
    KTEST_ASSERT_EQ(full, -ENOMEM);
    KTEST_ASSERT_EQ(prot3, RW);
    KTEST_ASSERT_EQ(prot0, RW);
    KTEST_ASSERT_EQ(n_full, cap);
}

// PROT_NONE. What a broken version would still pass, and what stops it:
// a hide that FREED the frame passes "the access faults", so the data
// is written before and read after, and the frame compared; one that
// kept the frame but left the PTE present passes the data check, so the
// fault is asked for while hidden; a walker that read the hidden leaf as
// a hole passes both, so teardown's and fork's refcounts are checked.
static uint64_t *frame_words(uint64_t frame) { return (uint64_t *)(uintptr_t)frame; }

KTEST("mm", "PROT_NONE hides a touched page, faults while hidden, and gives the same data back") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 2, RW, MMAP_KIND_ANON));
    KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b), 1);
    uint64_t frame = vmm_user_phys(f.as, b);
    frame_words(frame)[7] = 0x5A17A05ULL;

    int hide = mmap_protect(f.mm, f.as, b, PG, NONE);
    int hidden = vmm_page_hidden(f.as, b);
    uint64_t phys_hidden = vmm_user_phys(f.as, b);          // not a present mapping
    int read_hidden = vmm_fault_in(f.as, b, 0);
    int untouched = mmap_fault_in(f.mm, f.as, b + PG);      // the RW neighbour still maps
    int show = mmap_protect(f.mm, f.as, b, PG, RW);
    uint64_t back = vmm_user_phys(f.as, b);
    uint64_t word = back ? frame_words(back)[7] : 0;
    int prot = vmm_user_page_prot(f.as, b);
    fx_close(&f);

    KTEST_ASSERT_EQ(hide, 0);
    KTEST_ASSERT_EQ(hidden, 1);
    KTEST_ASSERT_EQ(phys_hidden, 0);
    KTEST_ASSERT_EQ(read_hidden, 0);
    KTEST_ASSERT_EQ(untouched, 1);
    KTEST_ASSERT_EQ(show, 0);
    KTEST_ASSERT_EQ(back, frame);
    KTEST_ASSERT_EQ(word, 0x5A17A05ULL);
    KTEST_ASSERT_EQ(prot, VMM_PAGE_W);
}

KTEST("mm", "an untouched PROT_NONE page faults and maps nothing") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 1, NONE, MMAP_KIND_ANON));
    int got = mmap_fault_in(f.mm, f.as, b);
    int prot = vmm_user_page_prot(f.as, b);
    int hidden = vmm_page_hidden(f.as, b);
    fx_close(&f);
    KTEST_ASSERT_EQ(got, 0);
    KTEST_ASSERT_EQ(prot, -1);
    KTEST_ASSERT_EQ(hidden, 0);
}

KTEST("mm", "a hidden page's frame is freed by release and by teardown, and shared by fork") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    KTEST_ASSERT(fx_region(&f, b, 2, RW, MMAP_KIND_ANON));
    KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b), 1);
    KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b + PG), 1);
    uint64_t f0 = vmm_user_phys(f.as, b), f1 = vmm_user_phys(f.as, b + PG);
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b, 2 * PG, NONE), 0);

    uint64_t child = vmm_fork_address_space(f.as, 0);
    KTEST_ASSERT(child != 0);
    unsigned refs_forked = pmm_frame_refs(f0);
    int child_hidden = vmm_page_hidden(child, b);
    // The parent shows its page RW: shared, so COW rather than W.
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b, PG, RW), 0);
    int shown_shared = vmm_user_page_prot(f.as, b);
    vmm_destroy_address_space(child);
    unsigned refs_after_child = pmm_frame_refs(f0);

    int released = vmm_release_user_page(f.as, b + PG);
    int f1_used = pmm_frame_is_used(f1);
    // f0 is present again, f1 gone: teardown must free f0 -- hide it
    // first, so teardown meets a hidden leaf.
    KTEST_ASSERT_EQ(mmap_protect(f.mm, f.as, b, PG, NONE), 0);
    struct vmm_audit a;
    uint64_t dangling = vmm_audit_space(f.as, &a);
    fx_close(&f);
    int f0_used = pmm_frame_is_used(f0);

    KTEST_ASSERT_EQ(refs_forked, 2);
    KTEST_ASSERT_EQ(child_hidden, 1);
    KTEST_ASSERT_EQ(shown_shared, VMM_PAGE_COW);
    KTEST_ASSERT_EQ(refs_after_child, 1);
    KTEST_ASSERT_EQ(released, 1);
    KTEST_ASSERT_EQ(f1_used, 0);
    KTEST_ASSERT_EQ(dangling, 0);
    KTEST_ASSERT_EQ(a.pages, 1);                // the hidden leaf is counted, not skipped
    KTEST_ASSERT_EQ(f0_used, 0);
}

// Linux's accounting: a reservation is free, accessible memory is not.
KTEST("mm", "PROT_NONE is not charged to RLIMIT_DATA until made accessible, and a refusal changes nothing") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    f.mm->data_cur = 4 * PG;
    KTEST_ASSERT(fx_region(&f, b, 1024, NONE, MMAP_KIND_ANON));     // 4 MiB reserved
    int fits = mm_data_allows(f.mm, 4 * PG);
    int some = mmap_protect(f.mm, f.as, b, 3 * PG, RW);
    int over = mmap_protect(f.mm, f.as, b + 3 * PG, 2 * PG, RW);   // 5 pages > 4
    int prot_over = prot_at(f.mm, b + 3 * PG);
    int back = mmap_protect(f.mm, f.as, b, 3 * PG, NONE);           // uncharged again
    int again = mmap_protect(f.mm, f.as, b + 3 * PG, 4 * PG, RW);
    fx_close(&f);

    KTEST_ASSERT_EQ(fits, 1);
    KTEST_ASSERT_EQ(some, 0);
    KTEST_ASSERT_EQ(over, -ENOMEM);
    KTEST_ASSERT_EQ(prot_over, NONE);
    KTEST_ASSERT_EQ(back, 0);
    KTEST_ASSERT_EQ(again, 0);
}

// A loop over a reservation must visit what was touched, not every page:
// 1 TiB page by page is 268 million walks.
KTEST("mm", "mprotect over a 1 TiB reservation skips the page tables that were never built") {
    struct fixture f = {0};
    KTEST_ASSERT(fx_open(&f));
    uint64_t b = UADDR_MMAP_BASE;
    uint64_t pages = (1ULL << 40) / PG;
    KTEST_ASSERT(fx_region(&f, b, pages, NONE, MMAP_KIND_ANON));
    uint64_t t0 = coarse_ticks();
    int rw = mmap_protect(f.mm, f.as, b, pages * PG, RW);
    KTEST_ASSERT_EQ(mmap_fault_in(f.mm, f.as, b + (pages / 2) * PG), 1);
    int none = mmap_protect(f.mm, f.as, b, pages * PG, NONE);
    int hidden = vmm_page_hidden(f.as, b + (pages / 2) * PG);
    uint64_t took = coarse_ticks() - t0;
    fx_close(&f);

    KTEST_ASSERT_EQ(rw, 0);
    KTEST_ASSERT_EQ(none, 0);
    KTEST_ASSERT_EQ(hidden, 1);
    KTEST_ASSERT(took < 2 * COARSE_HZ);          // page by page it is minutes
}
