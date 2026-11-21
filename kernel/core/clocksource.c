// The monotonic-time registry. See kernel/clocksource.h for what a
// clocksource IS and why wall-clock time is deliberately not one.
//
// A SNAPSHOT AND A DELTA, and why it is not just "return read() *
// mult >> shift": a raw counter can WRAP (the mask), and a better source
// can arrive partway through boot. So the time is a snapshot -- a base
// nanosecond total and the raw value it was taken at -- plus the delta
// since, converted on each read: wraparound is a masked subtraction, and
// switching sources is a retake that keeps the total.
//
// **A READ WRITES NOTHING** (it did until SMP stage 7: each read folded
// its delta into one global total, which made every clock read on every
// core a write behind the kernel lock). The snapshot is retaken by the
// boot core's tick, so a delta can never outgrow the multiply; the cost
// is that the snapshot must be retaken within half the source's span --
// clocksource_max_idle_ns(), which bounds a tickless idle.
#include "clocksource.h"
#include "timer.h"
#include "barrier.h" // cpu_relax -- the delay below spins
#include "klog.h"
#include "kfmt.h"
#include "driver.h" // DRIVER_DECLARE, driver_bound -- `lsdrv`
#include "multiboot.h" // clocksource=
#include "string.h"
#include "spinlock.h"
#include "lockstat.h"   // SPINLOCK_COUNTED
#include "clock_abi.h"

static const struct clocksource *g_cs;   // the current source
static uint64_t g_max_idle_ns;

// THE CLOCK IS THE CLOCK PAGE (abi/clock_abi.h), a latch every process
// also has mapped read-only (vmm.c's clock slot): the kernel and ring 3
// read one structure, and no reader writes anything, so the clock costs
// no shared cache line and no lock on any core. `g_snap_cs` is latched
// beside the page's two copies -- the source each was taken against,
// which ring 3 never needs to know.
static union {
    struct clock_page page;
    uint8_t bytes[4096];
} g_clock __attribute__((aligned(4096)));
static const struct clocksource *g_snap_cs[2];

// The writer's: a retake of the snapshot (the boot core's tick, a source
// switch, a reader that found it stale). Readers never take it.
SPINLOCK_COUNTED(g_clock_lock, "clock");
static uint64_t g_clamps, g_clamp_reported, g_clamp_delta;

#define compiler_barrier() __asm__ volatile ("" ::: "memory")

int clocksource_deadline_capable(void) {
    const struct clocksource *cs = clocksource_current();
    return cs && cs->irq_independent;
}

const struct clocksource *clocksource_current(void) { return g_cs; }

void clocksource_calc_mult_shift(uint32_t *mult, uint32_t *shift,
                                  uint64_t freq, uint32_t maxsec) {
    if (!freq) { *mult = 0; *shift = 0; return; }

    // The largest shift that keeps (maxsec worth of cycles) * mult
    // inside 64 bits. Walking DOWN from the biggest useful shift finds
    // the most precise pair rather than a merely adequate one -- and
    // the loop terminates at 0, which is always representable for any
    // frequency at or below 1GHz-per-unit.
    uint64_t maxcycles = (uint64_t)maxsec * freq;
    for (int sh = 32; sh >= 0; sh--) {
        uint64_t m = ((uint64_t)1000000000ULL << sh) / freq;
        if (m == 0) continue;              // shift too small to represent
        if (m > 0xFFFFFFFFULL) continue;   // multiplier does not fit
        // The product has to survive too, or the precision we just
        // bought is spent on an overflow.
        if (maxcycles && m > 0xFFFFFFFFFFFFFFFFULL / maxcycles) continue;
        *mult = (uint32_t)m;
        *shift = (uint32_t)sh;
        return;
    }
    // Unreachable for any sane frequency; a mult of 0 is refused by
    // clocksource_register() rather than silently stopping time.
    *mult = 0;
    *shift = 0;
}

int clocksource_snap_fold(const struct clock_snap *cur, uint64_t raw,
                          struct clock_snap *n, int *clamped) {
    *n = *cur;
    raw &= cur->mask;
    uint64_t delta = (raw - cur->base_raw) & cur->mask;
    // Behind the base: this core's counter trails the one that took it.
    // Nothing to fold in, and moving base_raw back would let the next
    // delta count the same interval twice.
    if (delta > (cur->mask >> 1)) return 0;
    if (delta > cur->max_delta) {
        // Nobody retook it for longer than the multiply can convert.
        // Clamping loses time; wrapping would run it BACKWARDS.
        *clamped = 1;
        delta = cur->max_delta;
    }
    // THE REMAINDER IS KEPT: truncating each fold to whole nanoseconds
    // lost up to one per retake, and a reader just before a retake could
    // then see a later time than one just after it.
    uint64_t total = delta * cur->mult + cur->base_frac;
    n->base_ns = cur->base_ns + (total >> cur->shift);
    n->base_frac = total & ((1ull << cur->shift) - 1);
    n->base_raw = raw;
    return 1;
}

// RETAKES THE SNAPSHOT, against `next` (a switch) or the source it was
// taken with. g_clock_lock held, interrupts off.
//
// The latch write: the idle copy is rewritten, then `seq` moves readers
// onto it, then the copy they left is made the same -- a reader still on
// it sees `seq` change and retries. One increment per retake.
static void snap_retake_locked(const struct clocksource *next) {
    struct clock_page *pg = &g_clock.page;
    uint32_t s = pg->seq;
    const struct clock_snap *cur = &pg->snap[s & 1];
    const struct clocksource *cs = g_snap_cs[s & 1];
    struct clock_snap n = *cur;

    if (cs) {
        int clamped = 0;
        uint64_t raw = cs->read();
        if (!clocksource_snap_fold(cur, raw, &n, &clamped) && next == cs) return;
        if (clamped) { g_clamps++; g_clamp_delta = (raw - cur->base_raw) & cur->mask; }
    }
    if (next != cs) {
        // A NEW SOURCE CONTINUES THE TOTAL, so the clock never steps back
        // at the switch; the part of a nanosecond below it is dropped.
        // THE FIRST STARTS AT ITS OWN ORIGIN, not at zero: the PIT's tick
        // count began before this ran, and coarse_ticks() -- counted
        // until now, derived from this clock later -- must not step back.
        uint64_t raw = next->read() & next->mask;
        n.mask = next->mask;
        n.mult = next->mult;
        n.shift = next->shift;
        // Room for base_frac on top of the largest product.
        n.max_delta = (~0ull - ((1ull << next->shift) - 1)) / next->mult;
        if (!cs) n.base_ns = raw <= n.max_delta ? (raw * next->mult) >> next->shift : 0;
        n.base_frac = 0;
        n.base_raw = raw;
        n.mode = next->user_mode;
        n.coarse_ns = next->irq_independent ? 1000000000u / COARSE_HZ : 0;
    }

    int idle = (int)((s + 1) & 1);
    pg->snap[idle] = n;
    g_snap_cs[idle] = next;
    compiler_barrier();
    pg->seq = s + 1;
    compiler_barrier();
    pg->snap[s & 1] = n;
    g_snap_cs[s & 1] = next;
}

static void clamp_report(void) {
    if (g_clamps == g_clamp_reported) return;
    g_clamp_reported = g_clamps;
    klog_printf("clocksource: %s delta %lu exceeds max %lu -- time clamped\n",
                g_cs ? g_cs->name : "?", (unsigned long)g_clamp_delta,
                (unsigned long)g_clock.page.snap[g_clock.page.seq & 1].max_delta);
}

void clocksource_tick(void) {
    if (!g_cs) return;
    uint64_t f = spin_lock_irqsave(&g_clock_lock);
    snap_retake_locked(g_cs);
    spin_unlock_irqrestore(&g_clock_lock, f);
    clamp_report();
}

// A reader that found the snapshot a quarter of the way to what it can
// convert (a long spin with the boot core's tick held off) retakes it,
// if nobody else is: a trylock, so an NMI or a reader on the writer's own
// core never waits.
static void retake_if_free(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    if (spin_trylock(&g_clock_lock)) {
        snap_retake_locked(g_cs);
        spin_unlock(&g_clock_lock);
    }
    if (f & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");
}

uint64_t clocksource_now_ns(void) {
    const struct clock_page *pg = &g_clock.page;
    for (;;) {
        uint32_t s = pg->seq;
        compiler_barrier();
        const struct clocksource *cs = g_snap_cs[s & 1];
        if (!cs) return 0;
        const struct clock_snap *c = &pg->snap[s & 1];
        uint64_t raw = cs->read();
        uint64_t ns = clock_snap_ns(c, raw);
        uint64_t span = c->mask < c->max_delta ? c->mask : c->max_delta;
        int stale = ((raw - c->base_raw) & c->mask) > (span >> 2) &&
                    ((raw - c->base_raw) & c->mask) <= (c->mask >> 1);
        compiler_barrier();
        if (pg->seq != s) continue;
        if (stale) retake_if_free();
        return ns;
    }
}

uint64_t clocksource_page_phys(void) { return (uint64_t)(uintptr_t)&g_clock; }

int clocksource_register(const struct clocksource *cs) {
    // The honesty check. A source that cannot be read, cannot wrap, or
    // converts every delta to zero is not a worse clock -- it is a
    // stopped one, and a stopped clock installed over a working one is
    // the failure this refuses to perform quietly.
    if (!cs || !cs->read || !cs->mask || !cs->mult) {
        klog_printf(KLOG_ERR "clocksource: REFUSED %s -- incomplete (read=%d mask=%d mult=%u)\n",
                     cs && cs->name ? cs->name : "(unnamed)",
                     cs && cs->read ? 1 : 0, cs && cs->mask ? 1 : 0,
                     cs ? cs->mult : 0);
        return 0;
    }

    // Ties keep the incumbent, so registration ORDER cannot decide
    // which source wins -- only the rating can. `clocksource=` is the
    // one exception, in both directions.
    const char *forced = clocksource_forced();
    int is_forced = forced && k_strcmp(forced, cs->name) == 0;
    if (g_cs && forced && k_strcmp(forced, g_cs->name) == 0) {
        klog_printf("clocksource: %s kept behind %s -- clocksource=%s\n",
                     cs->name, g_cs->name, forced);
        return 0;
    }
    if (g_cs && !is_forced && cs->rating <= g_cs->rating) {
        klog_printf("clocksource: %s (rating %d) kept behind %s (rating %d)\n",
                     cs->name, cs->rating, g_cs->name, g_cs->rating);
        return 0;
    }

    // THE SWITCH IS ONE CRITICAL SECTION. The timer interrupt reads this
    // clock, and one landing between publishing `g_cs` and setting its
    // limits found a max_delta of 0: every read clamped, and at 1000 Hz
    // the ISR's warning outlasted the tick, so registration never
    // resumed -- a boot that spun printing "exceeds max 0" forever.
    uint64_t max_delta = (~0ull - ((1ull << cs->shift) - 1)) / cs->mult;
    uint64_t span = cs->mask < max_delta ? cs->mask : max_delta;
    // THE SWITCH IS ONE RETAKE: everything the outgoing source measured
    // is folded into the total in the same latch write that starts the
    // new one, so no interval is lost and no reader sees a half-switch.
    uint64_t flags = spin_lock_irqsave(&g_clock_lock);
    if (!g_clock.page.magic) {
        g_clock.page.magic = CLOCK_PAGE_MAGIC;
        g_clock.page.version = CLOCK_PAGE_VERSION;
    }
    // Half the span: what a tickless idle may sleep, and the most a
    // reader's delta can be before clock_snap_ns() reads it as behind.
    g_max_idle_ns = ((span >> 1) * cs->mult) >> cs->shift;
    snap_retake_locked(cs);
    g_cs = cs;
    spin_unlock_irqrestore(&g_clock_lock, flags);

    // Here, not at registration: a source that was refused or out-rated
    // drives nothing, and said so on the way past.
    driver_bound(cs->name, "clock0");

    klog_printf("clocksource: using %s (rating %d, mult %u shift %u)\n",
                 cs->name, cs->rating, cs->mult, cs->shift);
    return 1;
}

uint64_t clocksource_max_idle_ns(void) { return g_max_idle_ns; }

const char *clocksource_forced(void) {
    static char name[16];
    static int looked;
    if (!looked) {
        looked = 1;
        if (!multiboot_cmdline_value("clocksource=", name, sizeof name)) name[0] = 0;
    }
    return name[0] ? name : 0;
}

// --- the PIT source ---------------------------------------------------
//
// Correct and coarse: it counts tick INTERRUPTS, so it advances in
// 1/CONFIG_HZ steps and can say nothing about anything shorter -- and
// nothing at all while the tick is stopped, which is why a tickless idle
// refuses to run on it. That is precisely the limitation the TSC and
// ACPI PM sources remove.
static uint64_t pit_cs_read(void) { return timer_irq_ticks(); }

DRIVER_DECLARE("pit", "clock", "8253/8254 interval timer, counted per tick");

static struct clocksource g_pit_cs = {
    .name   = "pit",
    .read   = pit_cs_read,
    .mask   = CLOCKSOURCE_MASK(64),
    .rating = CLOCKSOURCE_RATING_PIT,
};

void clocksource_delay_ms(uint32_t ms) {
    if (!ms) return;
    if (clocksource_deadline_capable()) {
        uint64_t end = clocksource_now_ns() + (uint64_t)ms * 1000000ull;
        while (clocksource_now_ns() < end) cpu_relax();
        return;
    }
    // THE FIRST TICK MAY LAND IMMEDIATELY, so ask for one more than the
    // arithmetic needs -- a caller that wanted 1 ms must not get 0.
    uint64_t start = timer_irq_ticks();
    uint64_t want = (uint64_t)ms * CONFIG_HZ / 1000 + 1;
    while (timer_irq_ticks() - start < want) cpu_relax();
}

// The sample taken on BOTH sides of the clock read, and the narrowest of
// a few tries kept: an interrupt or SMI landing between them widens a try
// that then loses, rather than skewing the answer (Linux's tsc_read_refs()).
static void paired_read(uint64_t (*sample)(void), uint64_t *s, uint64_t *ns) {
    uint64_t best = ~0ull;
    for (int i = 0; i < 5; i++) {
        uint64_t a = sample(), t = clocksource_now_ns(), b = sample();
        if (b - a < best) { best = b - a; *s = a + (b - a) / 2; *ns = t; }
    }
}

uint64_t clocksource_rate_of(uint64_t (*sample)(void), uint32_t window_ms) {
    if (!window_ms || !clocksource_deadline_capable()) return 0;
    uint64_t s0 = 0, t0 = 0, s1 = 0, t1 = 0;
    paired_read(sample, &s0, &t0);
    while (clocksource_now_ns() - t0 < (uint64_t)window_ms * 1000000ull) cpu_relax();
    paired_read(sample, &s1, &t1);
    if (t1 <= t0 || s1 <= s0) return 0;
    return (s1 - s0) * 1000000000ull / (t1 - t0);
}

void clocksource_init(void) {
    // A generous maxsec: this counter is 64-bit and increments
    // CONFIG_HZ times a second, so nothing here can overflow in any
    // realistic uptime -- the bound exists to pick the shift, not to guard.
    clocksource_calc_mult_shift(&g_pit_cs.mult, &g_pit_cs.shift, CONFIG_HZ, 3600);
    clocksource_register(&g_pit_cs);
}
