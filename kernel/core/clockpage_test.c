// KTESTs for the clock page and the syscalls that run without the kernel
// lock (docs/smp-design.md, stage 7): a retake's arithmetic, the page
// mapped read-only in every address space and surviving their teardown,
// readers on every core while the boot core retakes, and which rows are
// MPSAFE. The cross-core one is in the "smp" suite, which ktest_run.py
// boots on four vCPUs.
#include "ktest.h"
#include "clocksource.h"
#include "clock_abi.h"
#include "uaddr.h"
#include "vmm.h"
#include "smp.h"
#include "syscall_abi.h"
#include "syscall_table.h"

// A 3 GHz counter's conversion, as calc_mult_shift() gives it.
static struct clock_snap test_snap(void) {
    struct clock_snap s = {0};
    clocksource_calc_mult_shift(&s.mult, &s.shift, 3000000000ull, 600);
    s.mask = ~0ull;
    s.max_delta = (~0ull - ((1ull << s.shift) - 1)) / s.mult;
    s.base_raw = 1000;
    return s;
}

KTEST("clocksource", "a thousand small retakes keep the time one large one does") {
    struct clock_snap one = test_snap(), many = test_snap();
    int clamped = 0;
    struct clock_snap n;
    // 7 cycles a slice: each converts to a fraction of a nanosecond over
    // two, so a fold that truncates loses most of a nanosecond each time.
    for (int i = 1; i <= 1000; i++) {
        KTEST_ASSERT(clocksource_snap_fold(&many, 1000 + 7ull * i, &n, &clamped));
        many = n;
    }
    KTEST_ASSERT(clocksource_snap_fold(&one, 1000 + 7000, &n, &clamped));
    one = n;
    KTEST_ASSERT(!clamped);
    KTEST_ASSERT_EQ(many.base_ns, one.base_ns);
    KTEST_ASSERT_EQ(many.base_frac, one.base_frac);
    // And a read lands on the same time either way.
    KTEST_ASSERT_EQ(clock_snap_ns(&many, 9000), clock_snap_ns(&one, 9000));
}

KTEST("clocksource", "a counter behind the snapshot reads as its base, and is not folded") {
    struct clock_snap s = test_snap();
    s.base_ns = 5000000;
    KTEST_ASSERT_EQ(clock_snap_ns(&s, s.base_raw - 3), 5000000ull);
    struct clock_snap n;
    int clamped = 0;
    KTEST_ASSERT(!clocksource_snap_fold(&s, s.base_raw - 3, &n, &clamped));
    KTEST_ASSERT_EQ(n.base_raw, s.base_raw);
    KTEST_ASSERT_EQ(n.base_ns, s.base_ns);
}

KTEST("clocksource", "a narrow counter idled past half its span still moves the clock on") {
    // The ACPI PM timer: 24 bits at 3.58 MHz, wrapping in 4.7 s. A
    // tickless idle sleeps half of that from a moment already past the
    // last retake, so the wake's delta is a little over half the mask --
    // which must be time gone by, not a counter "behind" the base.
    struct clock_snap s = {0};
    clocksource_calc_mult_shift(&s.mult, &s.shift, 3579545ull, 600);
    s.mask = (1ull << 24) - 1;
    s.max_delta = (~0ull - ((1ull << s.shift) - 1)) / s.mult;
    s.base_raw = 100;
    s.base_ns = 1000000000ull;
    uint64_t raw = (s.base_raw + s.mask / 2 + 50000) & s.mask;
    uint64_t want = s.base_ns + 2300000000ull;   // ~2.36 s on
    KTEST_ASSERT(clock_snap_ns(&s, raw) > want);
    struct clock_snap n;
    int clamped = 0;
    KTEST_ASSERT(clocksource_snap_fold(&s, raw, &n, &clamped));
    KTEST_ASSERT(n.base_ns > want);
    KTEST_ASSERT_EQ(n.base_raw, raw);
}

KTEST("clocksource", "a delta past what the multiply converts is clamped, not wrapped") {
    struct clock_snap s = test_snap();
    s.mask = (1ull << 24) - 1;   // the PM timer's width
    s.max_delta = 1000;
    struct clock_snap n;
    int clamped = 0;
    KTEST_ASSERT(clocksource_snap_fold(&s, s.base_raw + 5000, &n, &clamped));
    KTEST_ASSERT(clamped);
    KTEST_ASSERT_EQ(clock_snap_ns(&s, s.base_raw + 5000), clock_snap_ns(&s, s.base_raw + 1000));
}

KTEST("clocksource", "the clock page is one read-only frame in every address space") {
    uint64_t va = vmm_clock_page_addr();
    if (!va) KTEST_SKIP("no clock page this boot");
    KTEST_ASSERT_EQ(va, (uint64_t)UADDR_CLOCK_PAGE);
    uint64_t a = vmm_create_address_space();
    KTEST_ASSERT(a);
    uint64_t b = vmm_fork_address_space(a, 0);
    KTEST_ASSERT(b);
    KTEST_ASSERT_EQ(vmm_user_phys(a, va), clocksource_page_phys());
    KTEST_ASSERT_EQ(vmm_user_phys(b, va), clocksource_page_phys());
    uint32_t magic = 0;
    KTEST_ASSERT(vmm_copy_from_user(a, &magic, va, sizeof magic));
    KTEST_ASSERT_EQ(magic, CLOCK_PAGE_MAGIC);
    // No path writes it on a process's behalf: a read() aimed at it is refused.
    uint32_t junk = 0;
    KTEST_ASSERT(!vmm_copy_to_user(a, va, &junk, sizeof junk));
    KTEST_ASSERT(!vmm_user_phys_for_write(a, va));
    // The slot is shared: tearing one space down leaves the next one's.
    vmm_destroy_address_space(a);
    magic = 0;
    KTEST_ASSERT(vmm_copy_from_user(b, &magic, va, sizeof magic));
    KTEST_ASSERT_EQ(magic, CLOCK_PAGE_MAGIC);
    vmm_destroy_address_space(b);
    uint64_t c = vmm_create_address_space();
    KTEST_ASSERT(c);
    KTEST_ASSERT_EQ(vmm_user_phys(c, va), clocksource_page_phys());
    vmm_destroy_address_space(c);
}

KTEST("syscall", "the clock, id, lseek, read and write calls are the rows that skip the kernel lock") {
    static const uint64_t mpsafe[] = { SYS_GETPID, SYS_GETTID, SYS_TICKS, SYS_MONOTONIC_NS,
                                       SYS_CLOCK_PAGE, SYS_LSEEK, SYS_READ, SYS_WRITE };
    for (unsigned i = 0; i < sizeof mpsafe / sizeof mpsafe[0]; i++) {
        const struct syscall_desc *d = syscall_desc_at(mpsafe[i]);
        KTEST_ASSERT(d && d->fn);
        KTEST_ASSERT(d->flags & SYSCALL_F_MPSAFE);
    }
    // And an ordinary one does not.
    const struct syscall_desc *w = syscall_desc_at(SYS_OPEN);
    KTEST_ASSERT(w && !(w->flags & SYSCALL_F_MPSAFE));
}

// Each other core reads the clock READS times, outside the kernel lock,
// while this one retakes the snapshot; a read before the one before it
// is a torn copy or a lost latch.
#define READS 200000
static volatile uint64_t g_backwards[64], g_reads[64];

static void read_clock_there(void *arg) {
    int core = (int)(uintptr_t)arg;
    uint64_t last = clocksource_now_ns(), back = 0;
    for (int i = 0; i < READS; i++) {
        uint64_t t = clocksource_now_ns();
        if (t < last) back++;
        last = t;
    }
    g_backwards[core] = back;
    g_reads[core] = READS;
}

KTEST("smp", "every core reads a clock that never runs back while the boot core retakes it") {
    int cores = smp_cpu_count();
    if (cores < 2) KTEST_SKIP("one core");
    if (cores > 64) cores = 64;
    int started = 0;
    for (int i = 1; i < cores; i++) {
        g_backwards[i] = 0;
        g_reads[i] = 0;
        started += smp_call(i, read_clock_there, (void *)(uintptr_t)i, 0);
    }
    KTEST_ASSERT(started > 0);
    uint64_t last = clocksource_now_ns(), back = 0;
    for (int k = 0; k < 20000; k++) {
        clocksource_tick();
        uint64_t t = clocksource_now_ns();
        if (t < last) back++;
        last = t;
    }
    for (int i = 1; i < cores; i++) KTEST_ASSERT(smp_call_wait(i, 60000));
    KTEST_ASSERT_EQ(back, 0ull);
    for (int i = 1; i < cores; i++) {
        if (!g_reads[i]) continue;   // a core that took no call
        KTEST_ASSERT_EQ(g_backwards[i], 0ull);
    }
}
