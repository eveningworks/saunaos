// TLB shootdown (tlb.h; docs/smp-design.md, stage 6). Each target has a
// mailbox in its struct cpu -- the request, a sequence number, and the
// number it last answered -- as smp_call()'s does; the kernel lock is
// the one-sender rule, so no lock of its own.
#include "tlb.h"
#include "percpu.h"
#include "gdt.h"          // gdt_boot_cpu(): this core before GS.base is set
#include "smp.h"
#include "lapic.h"
#include "bkl.h"
#include "vmm.h"          // vmm_kernel_pml4_phys()
#include "spinlock.h"     // spin_cpu_index()
#include "irqflags.h"
#include "barrier.h"      // cpu_relax()
#include "cpuinfo.h"      // cpu_tsc_hz()
#include "random_hw.h"    // arch_rdtsc()
#include "klog.h"
#include "kfmt.h"

// How long a target has to answer. A core with interrupts on answers in
// microseconds; one spinning for the kernel lock answers from the spin.
#define ACK_MS 1000

static struct tlb_stats g_stats;

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

// This core's struct cpu, also before gdt_init() (the BSP is index 0
// until GS.base is set) -- paging.c flushes from early boot.
static struct cpu *here(void) {
    return spin_cpu_index() ? this_cpu() : gdt_boot_cpu();
}

void tlb_load_cr3(uint64_t pml4_phys) {
    write_cr3(pml4_phys);
    here()->cr3 = pml4_phys;
}

void tlb_service(void) {
    struct cpu *c = here();
    uint32_t seq = __atomic_load_n(&c->tlb_seq, __ATOMIC_ACQUIRE);
    if (seq == c->tlb_done) return;
    uint64_t leave = c->tlb_leave;
    if (leave && c->cr3 == leave) {
        uint64_t k = vmm_kernel_pml4_phys();
        write_cr3(k);
        c->cr3 = k;
    } else {
        write_cr3(read_cr3());
    }
    __atomic_store_n(&c->tlb_done, seq, __ATOMIC_RELEASE);
}

// The sender's half. `space` 0 means every online core; `leave` is the
// space a target drops for the kernel's. A target is any core whose
// tlb_seq is ahead of its tlb_done, so the wait needs no list.
static int answers(const struct cpu *c, uint32_t me) {
    if (!c || c->index == me) return 0;
    uint32_t st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    return st == CPU_PARKED || st == CPU_RUNNING;   // a wedged core runs nothing
}

static void shoot(uint64_t space, uint64_t leave, uintptr_t site) {
    int n = smp_cpu_count();
    if (n < 2) return;
    uint32_t me = spin_cpu_index();
    if (smp_online_count() > 1 && !bkl_held_here()) {
        // The rule, counted rather than enforced: without the kernel
        // lock a second sender could share a mailbox.
        static int once;
        if (!once && me == 0) {
            once = 1;
            klog_printf(KLOG_ERR "tlb: shootdown without the kernel lock from %p\n",
                        (void *)site);
        }
    }
    uint64_t f = irq_save();
    int count = 0;
    for (int i = 0; i < n; i++) {
        struct cpu *c = (struct cpu *)smp_cpu_at(i);
        if (!answers(c, me) || (space && c->cr3 != space)) continue;
        c->tlb_leave = leave;
        __atomic_store_n(&c->tlb_seq, c->tlb_done + 1, __ATOMIC_RELEASE);
        // Not accepted is caught by the wait: it times out.
        (void)lapic_send_ipi((uint8_t)c->apic_id, TLB_FLUSH_VECTOR);
        count++;
    }
    if (count) {
        __atomic_fetch_add(&g_stats.shootdowns, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_stats.ipis, (uint64_t)count, __ATOMIC_RELAXED);
        uint64_t hz = cpu_tsc_hz();
        uint64_t limit = (hz ? hz : 3000000000ull) / 1000 * ACK_MS;
        uint64_t start = arch_rdtsc();
        for (int i = 0; i < n; i++) {
            struct cpu *c = (struct cpu *)smp_cpu_at(i);
            if (!answers(c, me)) continue;
            while (__atomic_load_n(&c->tlb_done, __ATOMIC_ACQUIRE) !=
                   __atomic_load_n(&c->tlb_seq, __ATOMIC_ACQUIRE)) {
                cpu_relax();
                if (arch_rdtsc() - start > limit) {
                    // It may still hold the stale entries, so it is given
                    // nothing more to run -- smp_call()'s rule.
                    __atomic_store_n(&c->state, CPU_WEDGED, __ATOMIC_RELEASE);
                    __atomic_fetch_add(&g_stats.timeouts, 1, __ATOMIC_RELAXED);
                    if (me == 0)
                        klog_printf(KLOG_ERR "tlb: core %u did not answer a shootdown "
                                    "within %d ms -- given no more\n", c->index, ACK_MS);
                    break;
                }
            }
        }
    }
    irq_restore(f);
}

#define SITE ((uintptr_t)__builtin_return_address(0))

void tlb_flush_user_page(uint64_t pml4_phys, uint64_t vaddr) {
    if (read_cr3() == pml4_phys)
        __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
    shoot(pml4_phys, 0, SITE);
}

void tlb_flush_user_space(uint64_t pml4_phys) {
    if (read_cr3() == pml4_phys) write_cr3(pml4_phys);
    shoot(pml4_phys, 0, SITE);
}

void tlb_flush_kernel(void) {
    write_cr3(read_cr3());
    shoot(0, 0, SITE);
}

void tlb_leave_space(uint64_t pml4_phys) {
    if (pml4_phys) shoot(pml4_phys, pml4_phys, SITE);
}

void tlb_stats_get(struct tlb_stats *out) {
    out->shootdowns = __atomic_load_n(&g_stats.shootdowns, __ATOMIC_RELAXED);
    out->ipis = __atomic_load_n(&g_stats.ipis, __ATOMIC_RELAXED);
    out->timeouts = __atomic_load_n(&g_stats.timeouts, __ATOMIC_RELAXED);
}
