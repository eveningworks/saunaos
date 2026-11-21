#ifndef KERNEL_LOCKSTAT_H
#define KERNEL_LOCKSTAT_H

// Per-lock contention and hold times, Linux's lock_stat
// (/proc/lock_stat) for the few spinlocks worth naming: the kernel lock,
// the heap, the frame allocator. What SMP stage 7 splits next is chosen
// from these numbers (docs/smp-design.md), and `lockstat` prints them
// over QUERY_LOCKSTAT.
//
// **EVERY FIELD IS WRITTEN BY THE LOCK'S HOLDER, UNDER THE LOCK**, so the
// counters need no atomics. A reader takes them unlocked: a busy lock's
// fields may be a moment apart, which a diagnostic accepts. Times are
// TSC ticks; the query converts.
//
// **HOLD TIMES ARE ARMED, THE REST IS ALWAYS ON** (`kernel.lockstat`,
// `lockstat track on`), the shape of kernel.syscall_stall: a hold costs
// two TSC reads on every take, measured at +25% on a getpid. Takes, waits
// and wait times cost no clock read on a take that did not wait -- one
// that did was spinning anyway.
#include <stdint.h>
#include "spinlock.h"

struct lockstat {
    const char *name;        // as `lockstat` prints it
    uint64_t acquired;       // takes, contended or not
    uint64_t contended;      // ...of which had to wait
    uint64_t wait_tsc, wait_max_tsc;
    uint64_t hold_tsc, hold_max_tsc;
    uint64_t held_since;     // the current holder's acquire time
    uint64_t last_wait_tsc;  // ...and how long it waited, 0 for not at all
};

// A file-scope spinlock with its counters, registered for QUERY_LOCKSTAT
// by being declared -- a pointer in .lockstats, which linker.ld keeps,
// as INITCALL() does. Counted from its first acquisition, boot included.
#define SPINLOCK_COUNTED(lock_, name_)                                        \
    static struct lockstat lock_##_stat = { .name = (name_) };                \
    static struct lockstat *const lock_##_stat_ptr                            \
        __attribute__((used, section(".lockstats"))) = &lock_##_stat;         \
    static struct spinlock lock_ = { .stat = &lock_##_stat }

// Armed: every take after this is timed until disarmed. Arming zeroes
// the hold totals, and a hold is billed only if it BEGAN after the arm
// (g_lockstat_armed_tsc), so one taken before, or in an earlier armed
// stretch, adds nothing to the new totals. Refused (0) without a
// calibrated TSC rate, which could not convert them.
//
// **THE CALLER HOLDS THE KERNEL LOCK** (a setting's apply runs in a
// syscall): the zeroing writes every core's kernel-lock row, which only
// the lock's holder may.
int lockstat_timing_set(int on);
uint64_t lockstat_timing_since_ns(void);   // when it was armed; 0 while off
extern volatile int g_lockstat_timing;
extern volatile uint64_t g_lockstat_armed_tsc;
static inline int lockstat_timing(void) { return g_lockstat_timing; }

// Ends the holder's timed hold now and returns it in ticks (0: not
// timed), so a caller can bill it elsewhere before the unlock -- which
// then adds nothing. Under the lock.
uint64_t lockstat_hold_end(struct lockstat *s);

int lockstat_count(void);
const struct lockstat *lockstat_at(int i);

#endif
