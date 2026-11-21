#ifndef KERNEL_KTRACE_H
#define KERNEL_KTRACE_H

// The kernel event trace: a ring of binary records per core, written at
// fixed points and read in ring 3 -- Linux ftrace's per-CPU ring buffer,
// ETW's per-CPU buffers (docs/trace-design.md, "The kernel event ring").
// The records, events and groups are abi/ktrace_abi.h's.
//
// **ARMED, OFF BY DEFAULT** (`kernel.trace`, `trace start`): disarmed, a
// trace point is one load of g_ktrace_mask and a branch predicted not
// taken, and its arguments are never evaluated. Armed, a write is
// interrupts off, a TSC read and 48 bytes into this core's own ring.
//
// **A WRITER TAKES NO LOCK.** Each core writes only its own ring (struct
// cpu's `ktrace`, read in one %gs load with interrupts off, so a
// migration cannot hand it another core's), so the only writer a ring
// can race is an NMI landing mid-record on the same core -- one record
// garbled or lost, never a fault. A full ring overwrites its oldest
// records, and the trace's stop logs how many each core lost.
#include <stdint.h>
#include "ktrace_abi.h"

extern volatile uint32_t g_ktrace_mask;   // KTRACE_G_* armed now

void ktrace_write(uint32_t event, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

static inline int ktrace_on(uint32_t group) {
    return __builtin_expect((g_ktrace_mask & group) != 0, 0);
}

// A trace point. The arguments are evaluated only while `group` is armed.
#define KTRACE(group, event, a0, a1, a2, a3)                                  \
    do {                                                                      \
        if (ktrace_on(group))                                                 \
            ktrace_write((event), (uint64_t)(a0), (uint64_t)(a1),             \
                         (uint64_t)(a2), (uint64_t)(a3));                     \
    } while (0)

// Records per core when `kernel.trace_size` names none: 8192, 384 KiB.
#define KTRACE_DEFAULT_RECORDS 8192u

// Starts (or, running, re-arms with) the groups in `mask`. From stopped
// it empties every ring first, allocating them at the size
// kernel.trace_size asks the first time or when that changed. 0 when
// refused: no calibrated TSC rate, or no memory -- logged.
// **THE CALLER HOLDS THE KERNEL LOCK** (a setting's apply, a KTEST): the
// rings change only under it, which is what keeps a reader's query safe.
int ktrace_start(uint32_t mask);
// Disarms and keeps the rings for reading; logs each core that lost
// records to a full ring. Under the kernel lock.
void ktrace_stop(void);
uint32_t ktrace_mask(void);

// Records per core for the next start from stopped: rounded up to a
// power of two. 0 for a size out of range (64 .. a 32nd of RAM in all).
int ktrace_set_size(uint64_t records);
uint64_t ktrace_size(void);

// What a core's ring holds: 0 for a core with none.
struct query_ktrace_cpu;
int ktrace_cpu_info(int cpu, struct query_ktrace_cpu *out);
// Up to `n` of core `cpu`'s records from number `seq` on, skipping any
// overwritten before or while they were copied: how many were copied,
// and the first one's number in *first.
int ktrace_read(int cpu, uint64_t seq, struct ktrace_rec *out, int n, uint64_t *first);

// THE PANIC TAIL: the newest records of each core as text, for the panic
// record (panic_store.c). No lock, no heap; it disarms the trace first.
// ktrace_panic_reserve() is what to leave for it -- 0 when the trace was
// not running -- and ktrace_panic_tail() writes at most `cap`, returning
// the length.
uint32_t ktrace_panic_reserve(void);
uint32_t ktrace_panic_tail(char *out, uint32_t cap);

#endif
