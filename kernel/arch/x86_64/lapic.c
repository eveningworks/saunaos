// The Local APIC. See kernel/lapic.h for what this is for and for the
// one trap in it (enabling it moves the 8259's wire).
#include "lapic.h"
#include "msr.h"
#include "acpi.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "cpuinfo.h"
#include "clockevent.h"
#include "timer.h"   // coarse_ticks(), COARSE_HZ -- the reference without a PM timer
#include "clocksource.h" // clocksource_rate_of() -- the reference with one
#include "barrier.h" // cpu_relax()

// The MSR that says where the LAPIC is and whether it is enabled. Its
// base field is the same address the MADT reports; they are read from
// different places on purpose (see lapic_init()).
#define IA32_APIC_BASE      0x1B
#define APIC_BASE_BSP       (1u << 8)
#define APIC_BASE_ENABLE    (1u << 11)
#define APIC_BASE_ADDR_MASK 0xFFFFF000u

// Register offsets, in bytes from the base. Every one is 32 bits wide
// and must be accessed as a single aligned dword.
#define LAPIC_REG_ID        0x020
#define LAPIC_REG_VERSION   0x030
#define LAPIC_REG_TPR       0x080
#define LAPIC_REG_EOI       0x0B0
#define LAPIC_REG_SVR       0x0F0
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define SVR_ENABLE          (1u << 8)

// LVT delivery modes, in bits 10:8.
#define LVT_DELIVERY_EXTINT (7u << 8)
#define LVT_DELIVERY_NMI    (4u << 8)
#define LVT_MASKED          (1u << 16)
#define LVT_TIMER_PERIODIC  (1u << 17)

// Divide-by-16. The divisor is encoded across bits 3,1,0 with bit 2
// skipped, which is why 16 is 0x3 and not 0x4. Sixteen keeps a 100 Hz
// period inside 32 bits on every bus frequency this will meet while
// leaving the counter coarse enough that calibration is not measuring
// its own overhead.
#define TIMER_DIV_16        0x3

static volatile uint8_t *g_base;
static uint8_t g_id;
static void (*g_vector_handlers[LAPIC_VECTOR_COUNT])(uint64_t *regs);
static uint32_t g_delivered;

static uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(g_base + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(g_base + reg) = value;
}

void lapic_init(void) {
    if (g_base) return;   // idempotent, like every other *_init() here

    const char *cmdline = multiboot_cmdline();
    if (cmdline && k_strstr(cmdline, "nomsi")) {
        klog_write("lapic: disabled by `nomsi` -- every device stays on the PIC\n");
        return;
    }
    // CPUID.01H:EDX bit 9. A CPU without it has no LAPIC to enable, and
    // writing the MSR would #GP.
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (!cpu_has_feature(&ci, CPU_WORD_1_EDX, 9)) {
        klog_write("lapic: no APIC on this CPU -- every device stays on the PIC\n");
        return;
    }

    // WHERE IT IS, from the MSR rather than from the MADT. The two agree
    // on every machine that has both, but the MSR is the one the CPU
    // actually decodes, and a machine with no ACPI tables still has it.
    // The MADT's value is reported beside it so a disagreement is
    // visible rather than silently resolved.
    uint64_t base_msr = rdmsr(IA32_APIC_BASE);
    uint64_t phys = base_msr & APIC_BASE_ADDR_MASK;
    const struct acpi_state *acpi = acpi_get_state();
    if (acpi && acpi->lapic_phys && acpi->lapic_phys != phys)
        klog_printf("lapic: MSR says 0x%llx, MADT says 0x%llx -- using the MSR\n",
                    (unsigned long long)phys,
                    (unsigned long long)acpi->lapic_phys);
    if (!phys || phys >= 0x100000000ull) {
        klog_printf("lapic: base 0x%llx is outside the identity map\n",
                    (unsigned long long)phys);
        return;
    }

    // The global enable bit. Clearing it again is architecturally
    // one-way until reset on some parts, which is why nothing here ever
    // turns the LAPIC back off.
    wrmsr(IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    g_base = (volatile uint8_t *)(uintptr_t)phys;   // identity-mapped
    g_id = (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24);

    // Accept every priority. The task-priority register comes out of
    // reset at 0 on real parts and on QEMU, but a firmware that raised
    // it would silently block every vector below its class.
    lapic_write(LAPIC_REG_TPR, 0);

    // Software enable, with the spurious vector. Until this bit is set
    // the LAPIC accepts nothing at all.
    lapic_write(LAPIC_REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    // VIRTUAL WIRE MODE, and this is the line the machine lives or dies
    // by. The CPU's INTR pin is the LAPIC's now, so the 8259 reaches it
    // only through LINT0 -- programmed for ExtINT, the PIC keeps
    // delivering the timer, the keyboard and every legacy line exactly
    // as before. Masked or set to any other delivery mode, they all
    // stop, and the machine appears to hang on the first tick.
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_REG_LVT_LINT1, LVT_DELIVERY_NMI);

    uint32_t ver = lapic_read(LAPIC_REG_VERSION);
    klog_printf("lapic: id %u, version 0x%x, %u LVT entries, at 0x%llx%s\n",
                g_id, ver & 0xFF, ((ver >> 16) & 0xFF) + 1,
                (unsigned long long)phys,
                (base_msr & APIC_BASE_BSP) ? " (BSP)" : "");
    klog_printf("lapic: MSI vectors %u-%u available\n",
                LAPIC_VECTOR_BASE, LAPIC_VECTOR_LAST);
}

// ---- the other cores' half (smp.c) ----------------------------------

#define LAPIC_REG_ICR_LO    0x300
#define LAPIC_REG_ICR_HI    0x310
#define LAPIC_REG_LVT_PERF  0x340
#define LAPIC_REG_LVT_ERROR 0x370
#define ICR_DM_INIT         (5u << 8)
#define ICR_DM_STARTUP      (6u << 8)
#define ICR_PENDING         (1u << 12)   // delivery status: still being sent
#define ICR_ASSERT          (1u << 14)
#define ICR_LEVEL           (1u << 15)

// Every core sees ITS OWN LAPIC at the same physical address, so
// g_base serves an AP unchanged; only g_id is the BSP's.
uint8_t lapic_read_id(void) {
    return g_base ? (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24) : 0;
}

// 1 once the ICR has gone idle, 0 if it is still busy after ~10 ms --
// a send that never completes is a LAPIC that is not listening.
static int icr_wait_idle(void) {
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 1000; j++)
            if (!(lapic_read(LAPIC_REG_ICR_LO) & ICR_PENDING)) return 1;
            else cpu_relax();
        clocksource_delay_ms(1);
    }
    return 0;
}

static int icr_send(uint8_t apic_id, uint32_t lo) {
    lapic_write(LAPIC_REG_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO, lo);   // writing the low half sends it
    return icr_wait_idle();
}

int lapic_send_init(uint8_t apic_id) {
    if (!g_base) return 0;
    // Assert, then de-assert: Pentium-era parts need the second, every
    // later one ignores it (Linux's wakeup_secondary_cpu_via_init()).
    if (!icr_send(apic_id, ICR_DM_INIT | ICR_LEVEL | ICR_ASSERT)) return 0;
    return icr_send(apic_id, ICR_DM_INIT | ICR_LEVEL);
}

int lapic_send_startup(uint8_t apic_id, uint8_t page) {
    if (!g_base) return 0;
    return icr_send(apic_id, ICR_DM_STARTUP | page);
}

// Fixed delivery: the SDM wants the level bit set for every mode but
// an INIT de-assert.
int lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    if (!g_base) return 0;
    return icr_send(apic_id, ICR_ASSERT | vector);
}

// An AP's LAPIC, parked: software-enabled so a later stage's IPIs reach
// it, and every local source masked. LINT0 and LINT1 are the BSP's
// alone -- ExtINT and NMI wired to two cores deliver twice.
void lapic_init_ap(void) {
    if (!g_base) return;
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_PERF, LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

int lapic_present(void) { return g_base != 0; }

uint8_t lapic_id(void) { return g_id; }

void lapic_eoi(void) {
    if (g_base) lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_mask_lint0(void) {
    if (g_base) lapic_write(LAPIC_REG_LVT_LINT0, LVT_MASKED);
}

// The performance-counter LVT: the lockup detector's NMI (kernel/lockup.h).
void lapic_set_lvt_perf(uint32_t value) {
    if (g_base) lapic_write(0x340, value);
}


uint8_t lapic_alloc_vector(void (*handler)(uint64_t *regs)) {
    if (!g_base || !handler) return 0;
    for (int i = 0; i < LAPIC_VECTOR_COUNT; i++) {
        if (g_vector_handlers[i]) continue;
        g_vector_handlers[i] = handler;
        return (uint8_t)(LAPIC_VECTOR_BASE + i);
    }
    return 0;
}

void lapic_free_vector(uint8_t vector) {
    int i = vector - LAPIC_VECTOR_BASE;
    if (i < 0 || i >= LAPIC_VECTOR_COUNT) return;
    g_vector_handlers[i] = 0;
}

void lapic_dispatch_vector(uint8_t vector, uint64_t *regs) {
    int i = vector - LAPIC_VECTOR_BASE;
    if (i < 0 || i >= LAPIC_VECTOR_COUNT) return;
    g_delivered++;
    // **BEFORE THE HANDLER, NOT AFTER -- A HANDLER CAN SWITCH AWAY AND
    // NEVER COME BACK.** lapic_timer_isr() reaches scheduler_tick(),
    // and a context switch moves the CPU on the spot (scheduler.c's
    // switch_to()), so an EOI below the call is simply never sent: the
    // in-service bit stays set, the LAPIC delivers no further timer
    // interrupt, and nothing preempts a ring-3 process that makes no
    // syscalls. It presented as one background compute job starving the
    // whole machine while the PIT kept coarse_ticks() limping along, so
    // the machine looked alive and merely unfair. irq.c's EOI moved for
    // the same reason and on the same day; this is its twin, and the
    // one that was missed first time.
    //
    // Safe for the MSIs that share this path: an MSI is edge-triggered
    // by construction, which is the case handle_edge_irq() acks up
    // front for, and every gate here is an interrupt gate, so IF is
    // clear for the whole handler and an early ack cannot re-enter one.
    lapic_eoi();
    if (g_vector_handlers[i]) g_vector_handlers[i](regs);
}

// --- the LAPIC timer -------------------------------------------------
//
// A per-core tick, and the reason this file is part of docs/smp-design.md
// rather than only of the MSI work: the PIT delivers one interrupt for
// the whole machine, so an application processor started in stage 3
// would have nothing to preempt it with.

// How many times the timer counts in a second, at TIMER_DIV_16.
// Measured once; the bus frequency it derives from does not change.
static uint32_t g_timer_rate;
static uint8_t g_timer_vector;
static uint32_t g_timer_ticks;
static int g_timer_oneshot;     // LVT in one-shot mode (lapic_ce_set_oneshot)

// How long to count for. Against a clock that runs without interrupts,
// 10 ms: the timer only sets tick periods and deadlines, which a
// fraction of a percent does not move. Against the PIT's ticks, four of
// them (40ms), so the whole-tick quantisation is under a percent.
#define CALIBRATE_MS 10
#define CALIBRATE_PIT_TICKS 4

static int interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & (1u << 9)) != 0;
}

// Counts down, so the counted total is the distance from the start.
static uint64_t lapic_counted(void) { return 0xFFFFFFFFu - lapic_read(LAPIC_REG_TIMER_CUR); }

// Counts down from the maximum and asks the clock how long that took --
// or, with only the PIT, across a whole number of its ticks.
static uint32_t lapic_timer_calibrate(void) {
    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED); // count, deliver nothing

    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t rate = clocksource_rate_of(lapic_counted, CALIBRATE_MS);
    uint32_t left = lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    // A counter that reached ZERO ran out mid-window, so the count is a
    // floor rather than a measurement -- and believing it would set a
    // silently wrong tick rate, which makes every timeout in the kernel
    // wrong rather than failing anywhere visible.
    if (rate && left && rate <= 0xFFFFFFFFu) return (uint32_t)rate;

    // WITHOUT INTERRUPTS THIS NEVER RETURNS. coarse_ticks() advances only
    // from the timer interrupt on the PIT, so the loops below would spin
    // forever -- the deadlock cpuinfo.h describes for the TSC, refused
    // here rather than hit.
    if (!interrupts_enabled()) return 0;

    uint64_t edge = coarse_ticks();
    while (coarse_ticks() == edge) cpu_relax(); // start on a tick boundary

    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t t0 = coarse_ticks();
    while (coarse_ticks() - t0 < CALIBRATE_PIT_TICKS) cpu_relax();
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CUR);
    uint64_t elapsed = coarse_ticks() - t0;
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    uint32_t counted = 0xFFFFFFFFu - remaining;
    if (!elapsed || !counted || !remaining) return 0;
    return (uint32_t)(((uint64_t)counted * COARSE_HZ) / elapsed); // counts a second
}

static void lapic_timer_isr(uint64_t *regs) {
    g_timer_ticks++;
    clockevent_tick(regs);
}

static int lapic_ce_start(uint32_t hz) {
    if (!g_base || !hz) return 0;

    if (!g_timer_rate) g_timer_rate = lapic_timer_calibrate();
    if (!g_timer_rate) return 0;

    uint32_t count = g_timer_rate / hz;
    if (!count) return 0; // asked for a rate faster than the timer counts

    if (!g_timer_vector) {
        g_timer_vector = lapic_alloc_vector(lapic_timer_isr);
        if (!g_timer_vector) return 0;
    }

    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_TIMER_PERIODIC | g_timer_vector);
    g_timer_oneshot = 0;
    // THE INITIAL COUNT IS THE ARMING WRITE, and it must be last: the
    // timer starts the instant it is written, so an LVT still holding a
    // masked or stale vector delivers the first tick somewhere wrong.
    lapic_write(LAPIC_REG_TIMER_INIT, count);

    klog_printf("lapic: timer at %u Hz -- %u counts, %u/s calibrated, vector %u\n",
                hz, count, g_timer_rate, g_timer_vector);
    return 1;
}

// ONE-SHOT: LVT mode bits clear, and each arm is a single write of the
// initial count -- which is also what cancels the previous one. The LVT
// is rewritten only on the first arm, out of periodic mode.

static void lapic_ce_set_oneshot(uint64_t ns) {
    if (!g_base || !g_timer_rate) return;
    if (!g_timer_oneshot) {
        lapic_write(LAPIC_REG_TIMER_INIT, 0);
        lapic_write(LAPIC_REG_LVT_TIMER, g_timer_vector);
        g_timer_oneshot = 1;
    }
    // Capped BEFORE the multiply, which a TSC's hour-scale idle bound
    // would overflow; an event that fires early is simply re-armed.
    if (ns > 10000000000ull) ns = 10000000000ull;
    // Rounded UP, so an event never lands before its deadline for want
    // of a count -- early is harmless but costs a second interrupt.
    uint64_t count = (ns * g_timer_rate + 999999999ull) / 1000000000ull;
    if (count == 0) count = 1;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull; // fires early; re-armed then
    lapic_write(LAPIC_REG_TIMER_INIT, (uint32_t)count);
}

static void lapic_ce_stop(void) {
    if (!g_base) return;
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    g_timer_oneshot = 0;
}

static const struct clockevent g_lapic_ce = {
    .name    = "lapic-timer",
    .start   = lapic_ce_start,
    .stop    = lapic_ce_stop,
    .set_oneshot = lapic_ce_set_oneshot,
    .rating  = CLOCKEVENT_RATING_LAPIC,
    .per_cpu = 1,
};

void clockevent_init_lapic(void) {
    // No LAPIC, or `nomsi`: the PIT keeps the tick, and stays periodic.
    if (g_base) clockevent_register(&g_lapic_ce);
    clockevent_select_mode();
}

uint32_t lapic_timer_rate(void) { return g_timer_rate; }
uint32_t lapic_timer_ticks(void) { return g_timer_ticks; }

int lapic_summary(char *buf, uint32_t cap) {
    if (!g_base || !buf || cap == 0) return 0;
    int claimed = 0;
    for (int i = 0; i < LAPIC_VECTOR_COUNT; i++)
        if (g_vector_handlers[i]) claimed++;
    k_snprintf(buf, cap, "id %u, %d of %d MSI vector(s) claimed, %u delivered",
               g_id, claimed, LAPIC_VECTOR_COUNT, (unsigned)g_delivered);
    return 1;
}
