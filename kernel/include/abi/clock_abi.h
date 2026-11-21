#ifndef ABI_CLOCK_ABI_H
#define ABI_CLOCK_ABI_H

#include <stdint.h>

// THE CLOCK PAGE: the monotonic clock's current conversion, mapped
// read-only into every process so ring 3 reads the time with no syscall
// -- Linux's vDSO data page (vvar), NT's KUSER_SHARED_DATA. SYS_CLOCK_PAGE
// says where it is; a kernel without one answers -ENOSYS, and the caller
// keeps using SYS_MONOTONIC_NS. The kernel reads its clock through the
// same structure (kernel/core/clocksource.c), so the two rings cannot
// compute the time differently.
//
// **A LATCH, NOT A SEQLOCK** (Linux's raw_write_seqcount_latch()): two
// copies, and `seq`'s low bit says which one a reader uses. ONE BUMP PER
// UPDATE: the writer fills the copy no reader is told to use, bumps `seq`
// (readers move onto it), then makes the copy they left the same. Neither
// parity means "busy". A reader never waits for a writer -- it only retries
// when `seq` moved under it -- so an NMI landing inside the writer on the
// same core still reads a whole copy instead of spinning on an odd count.
//
// Reading it:
//
//     for (;;) {
//         uint32_t s = pg->seq;              // then a compiler barrier
//         const struct clock_snap *c = &pg->snap[s & 1];
//         if (c->mode != CLOCK_MODE_TSC) -> use SYS_MONOTONIC_NS
//         uint64_t ns = clock_snap_ns(c, lfence; rdtsc);
//         if (pg->seq == s) return ns;       // after a compiler barrier
//     }

#define CLOCK_PAGE_MAGIC   0x4B434C43u   // 'C' 'L' 'C' 'K'
#define CLOCK_PAGE_VERSION 1u

// How ring 3 reads the raw counter a snapshot was taken against (Linux's
// vdso_clock_mode). Anything but the TSC -- the ACPI PM timer's port, the
// PIT's tick count -- is out of ring 3's reach, so it takes the syscall.
#define CLOCK_MODE_SYSCALL 0u
#define CLOCK_MODE_TSC     1u

struct clock_snap {
    uint64_t base_raw;    // the counter when this copy was taken
    uint64_t base_ns;     // the monotonic time then
    uint64_t base_frac;   // and the part of a nanosecond below it, << shift
    uint64_t mask;        // significant bits of the counter (it may wrap)
    uint64_t max_delta;   // the largest delta the multiply can convert
    uint32_t mult, shift; // ns = (delta * mult + base_frac) >> shift
    uint32_t mode;        // CLOCK_MODE_*
    uint32_t coarse_ns;   // ns per USER_HZ tick, or 0: ticks need the syscall
};

struct clock_page {
    uint32_t magic;       // CLOCK_PAGE_MAGIC
    uint32_t version;     // CLOCK_PAGE_VERSION
    volatile uint32_t seq;
    uint32_t pad;
    struct clock_snap snap[2];
};

// The time `raw` stands for under snapshot `s`.
//
// **A FULL-WIDTH COUNTER BEHIND THE BASE READS AS THE BASE**: a 64-bit
// delta with its top bit set is not a gap of centuries but another core's
// TSC a hair behind the one that took the snapshot, and reading it as a
// near-wrap would jump the clock forward by the mask. The base itself only
// moves forward, so a reader never sees time before it. A NARROW counter
// (the ACPI PM timer's 24 bits) is one global register that cannot be
// behind, and the kernel's idle runs it past half its span, so a delta
// there is always a delta.
static inline int clock_delta_behind(const struct clock_snap *s, uint64_t delta) {
    return s->mask == ~0ull && (delta >> 63) != 0;
}

static inline uint64_t clock_snap_ns(const struct clock_snap *s, uint64_t raw) {
    uint64_t delta = (raw - s->base_raw) & s->mask;
    if (clock_delta_behind(s, delta)) delta = 0;
    else if (delta > s->max_delta) delta = s->max_delta;
    return s->base_ns + ((delta * s->mult + s->base_frac) >> s->shift);
}

#endif
