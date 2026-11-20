// KTESTs for the other cores (smp.c) and the per-CPU pointer (percpu.h).
// Every count here is against the MADT, so the same suite passes on one
// vCPU and on `-smp 4` -- ktest_run.py boots the latter.
#include "smp.h"
#include "percpu.h"
#include "gdt.h"
#include "acpi.h"
#include "paging.h"
#include "msr.h"
#include "multiboot.h"
#include "lapic.h"
#include "tlb.h"
#include "irqflags.h"
#include "barrier.h"      // cpu_relax()
#include "cpuinfo.h"      // cpu_tsc_hz()
#include "random_hw.h"    // arch_rdtsc()
#include "clocksource.h"
#include "ktest.h"

KTEST("smp", "GS.base is this core's struct cpu, and ring 3's is 0") {
    struct cpu *c = this_cpu();
    // KTESTs run on the BSP until stage 5.
    KTEST_ASSERT(c == gdt_boot_cpu());
    KTEST_ASSERT(c->self == c);
    KTEST_ASSERT_EQ(rdmsr(MSR_GS_BASE), (uint64_t)(uintptr_t)c);
    // In the kernel the swapped-out value is ring 3's: always 0, which
    // is the test isr.asm's paranoid entry depends on (percpu.h).
    KTEST_ASSERT_EQ(rdmsr(MSR_KERNEL_GS_BASE), 0);
}

KTEST("smp", "every core the firmware enabled is online") {
    if (multiboot_cmdline_has("nosmp")) KTEST_SKIP("booted with nosmp");
    char word[16];
    if (multiboot_cmdline_value("maxcpus=", word, sizeof word)) KTEST_SKIP("booted with maxcpus=");
    int enabled = 0;
    for (int i = 0; i < acpi_cpu_count(); i++) {
        const struct acpi_cpu *a = acpi_cpu_at(i);
        if ((a->flags & ACPI_CPU_ENABLED) && a->apic_id <= 254) enabled++;
    }
    if (enabled == 0) KTEST_SKIP("no MADT");
    KTEST_ASSERT_EQ(smp_online_count(), enabled);
    for (int i = 1; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        KTEST_ASSERT(c && c->self == c);
        // Scheduling under smp=run (smp_start_scheduling()), parked otherwise.
        KTEST_ASSERT_EQ(c->state, (uint32_t)(smp_scheduling() ? CPU_RUNNING : CPU_PARKED));
        KTEST_ASSERT(smp_apic_online(c->apic_id));
    }
}

KTEST("smp", "each parked core copied the BSP's control registers") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    uint64_t cr0, cr4, pat, def;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    paging_memory_types_now(&pat, &def);
    for (int i = 1; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c->state != CPU_PARKED) continue;
        KTEST_ASSERT_EQ(c->seen_cr0, cr0);
        KTEST_ASSERT_EQ(c->seen_cr4, cr4);
        KTEST_ASSERT_EQ(c->seen_efer, rdmsr(MSR_EFER));
        KTEST_ASSERT_EQ(c->seen_pat, pat);
        KTEST_ASSERT_EQ(c->seen_mtrr_def, def);
    }
}

KTEST("smp", "the trampoline page is writable and NX again") {
    uint64_t phys = smp_trampoline_phys();
    if (!phys) KTEST_SKIP("no trampoline (one core)");
    uint64_t leaf = paging_kernel_leaf(phys);
    KTEST_ASSERT(leaf & (1ULL << 63));   // NX
    KTEST_ASSERT(leaf & (1ULL << 1));    // writable
}

// ---- a sender's wait (smp.c, tlb.c) -----------------------------------

static volatile int g_ran;
static volatile int g_result;

static void mark(void *arg) { (void)arg; g_ran = 1; }

// The wait a call's own code makes, interrupts off, answering nothing.
static int spin_until(volatile int *flag, uint32_t ms) {
    uint64_t hz = cpu_tsc_hz() ? cpu_tsc_hz() : 3000000000ull;
    uint64_t start = arch_rdtsc(), limit = hz / 1000 * ms;
    while (!*flag && arch_rdtsc() - start < limit) cpu_relax();
    return *flag;
}

KTEST("smp", "a call no core took in time is taken back, never run later") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    // A parked core that does not take a call is written off for the
    // rest of the run; a scheduling one is only slow.
    struct cpu *c = (struct cpu *)smp_cpu_at(1);
    if (c->state != CPU_RUNNING) KTEST_SKIP("core 1 is not scheduling (smp=park)");
    g_ran = 0;
    // As a call spinning inside itself: core 1 takes no other.
    c->call_running = 1;
    int r = smp_call(1, mark, 0, 1);
    c->call_running = 0;
    // Now it can: its next look at the mailbox finds the call taken back.
    (void)lapic_send_ipi((uint8_t)c->apic_id, SMP_CALL_VECTOR);
    uint64_t end = clocksource_now_ns() + 1000000000ull;
    while (c->call_done != c->call_seq && clocksource_now_ns() < end) cpu_relax();
    KTEST_ASSERT_EQ(r, 0);
    KTEST_ASSERT_EQ(c->call_done, c->call_seq);
    KTEST_ASSERT_EQ(g_ran, 0);
    KTEST_ASSERT_EQ(c->state, (uint32_t)CPU_RUNNING);
    KTEST_ASSERT(smp_call(1, mark, 0, 1));
    KTEST_ASSERT_EQ(g_ran, 1);
}

static void call_core_0(void *arg) { (void)arg; g_result = smp_call(0, mark, 0, 1); }

KTEST("smp", "a caller waiting with interrupts off answers its target's call back") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    g_ran = 0;
    g_result = -1;
    uint64_t f = irq_save();
    int r = smp_call(1, call_core_0, 0, 1);
    irq_restore(f);
    KTEST_ASSERT(r);
    KTEST_ASSERT_EQ(g_result, 1);   // the old wait timed it out after 1 s
    KTEST_ASSERT_EQ(g_ran, 1);
}

static volatile int g_entered;

static void call_core_0_and_spin(void *arg) {
    (void)arg;
    g_entered = 1;
    g_result = smp_call(0, mark, 0, 0);
    g_result = g_result && spin_until(&g_ran, 2000);
}

KTEST("tlb", "a shootdown's sender answers a call from a core it waits on") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    g_ran = 0;
    g_entered = 0;
    g_result = -1;
    // Core 1 waits on this core with interrupts off, so it cannot take
    // the shootdown until this core runs its call: only the sender's
    // wait can.
    uint64_t f = irq_save();
    int sent = smp_call(1, call_core_0_and_spin, 0, 0);
    int entered = sent && spin_until(&g_entered, 1000);
    if (entered) tlb_flush_kernel();
    irq_restore(f);
    KTEST_ASSERT(sent);
    KTEST_ASSERT(entered);
    KTEST_ASSERT(smp_call_wait(1, 3000));
    KTEST_ASSERT_EQ(g_result, 1);   // the old sender left it spinning 2 s
}
