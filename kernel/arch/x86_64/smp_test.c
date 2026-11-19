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
