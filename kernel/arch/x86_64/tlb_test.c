// KTESTs for the TLB shootdown (tlb.c). A parked core is made to cache
// a user page, the page is remapped under it, and it must read the NEW
// frame: without the shootdown its TLB still points at the old one
// (ktest_run.py boots -smp 4, so these run rather than skip).
#include "tlb.h"
#include "smp.h"
#include "percpu.h"
#include "vmm.h"
#include "pmm.h"
#include "ktest.h"

#define TEST_VADDR 0x9100000000ULL   // a space of our own; nothing else is mapped there

struct probe {
    uint64_t as;        // loaded first when non-zero
    uint64_t va;
    volatile uint64_t seen;
};

// On the AP, interrupts off: its own CR3 and one read through it. SMAP
// would fault a plain kernel read of a user page.
static void ap_read(void *arg) {
    struct probe *p = arg;
    if (p->as) tlb_load_cr3(p->as);
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    int smap = (cr4 >> 21) & 1;
    if (smap) __asm__ volatile ("stac" ::: "memory");
    p->seen = *(volatile uint64_t *)(uintptr_t)p->va;
    if (smap) __asm__ volatile ("clac" ::: "memory");
}

static uint64_t fill_frame(uint64_t word) {
    uint64_t f = pmm_alloc_frame(PMM_ZONE_DMA32);
    if (f) for (int i = 0; i < 512; i++) ((volatile uint64_t *)(uintptr_t)f)[i] = word;
    return f;
}

KTEST("tlb", "a core that cached a page reads the new frame after it is remapped") {
    if (smp_online_count() < 2) KTEST_SKIP("one core");
    const struct cpu *ap = smp_cpu_at(1);
    if (!ap || ap->state != CPU_PARKED) KTEST_SKIP("core 1 is not parked");

    uint64_t a = fill_frame(0xA1A1A1A1A1A1A1A1ull), b = fill_frame(0xB2B2B2B2B2B2B2B2ull);
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(a && b && as);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, a));

    struct probe p = { as, TEST_VADDR, 0 };
    KTEST_ASSERT(smp_call(1, ap_read, &p, 1));
    KTEST_ASSERT_EQ(p.seen, 0xA1A1A1A1A1A1A1A1ull);
    KTEST_ASSERT_EQ(ap->cr3, as);   // a target from here on

    struct tlb_stats before, after;
    tlb_stats_get(&before);
    KTEST_ASSERT(vmm_unmap_user_page(as, TEST_VADDR));
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, b));

    p.as = 0;   // the same CR3: only a shootdown can have flushed it
    KTEST_ASSERT(smp_call(1, ap_read, &p, 1));
    KTEST_ASSERT_EQ(p.seen, 0xB2B2B2B2B2B2B2B2ull);
    tlb_stats_get(&after);
    KTEST_ASSERT(after.shootdowns > before.shootdowns);
    KTEST_ASSERT_EQ(after.timeouts, before.timeouts);

    // Still loaded on core 1 (lazily), so destroying it must move that
    // core off first -- it would otherwise walk freed page tables.
    vmm_destroy_address_space(as);   // frees b with it
    KTEST_ASSERT_EQ(ap->cr3, vmm_kernel_pml4_phys());
    pmm_free_frame(a);
}

KTEST("tlb", "a kernel-map flush is answered by every online core") {
    if (smp_online_count() < 2) KTEST_SKIP("one core");
    int n = smp_cpu_count();
    uint32_t done[n];
    for (int i = 1; i < n; i++) done[i] = smp_cpu_at(i)->tlb_done;
    struct tlb_stats before, after;
    tlb_stats_get(&before);
    tlb_flush_kernel();
    tlb_stats_get(&after);
    int online = 0;
    for (int i = 1; i < n; i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c->state != CPU_PARKED && c->state != CPU_RUNNING) continue;
        online++;
        KTEST_ASSERT_EQ(c->tlb_done, done[i] + 1);
        KTEST_ASSERT_EQ(c->tlb_seq, c->tlb_done);
    }
    KTEST_ASSERT_EQ(after.ipis - before.ipis, (uint64_t)online);
    KTEST_ASSERT_EQ(after.timeouts, before.timeouts);
}

KTEST("tlb", "a space no other core has loaded sends no shootdown") {
    // Parked cores hold the kernel's CR3: an ordinary user unmap must
    // cost nothing until stage 5 runs processes on them.
    uint64_t f = fill_frame(0);
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(f && as);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, f));
    struct tlb_stats before, after;
    tlb_stats_get(&before);
    KTEST_ASSERT(vmm_unmap_user_page(as, TEST_VADDR));
    tlb_stats_get(&after);
    KTEST_ASSERT_EQ(after.ipis, before.ipis);
    vmm_destroy_address_space(as);
    pmm_free_frame(f);
}
