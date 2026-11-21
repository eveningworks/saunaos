#ifndef ABI_KTRACE_ABI_H
#define ABI_KTRACE_ABI_H

// C++ spells C11's _Static_assert `static_assert` (from C++11; before it
// there is none, and the C build has checked these already). This
// header is in the SDK, so C++ reads it.
#if defined(__cplusplus) && !defined(_Static_assert)
#if __cplusplus >= 201103L
#define _Static_assert static_assert
#else
#define _Static_assert(expr, msg)
#endif
#endif

// The kernel EVENT trace: per-core rings of fixed-size binary records
// written at fixed points (a context switch, a wakeup, an interrupt, a
// page fault, a syscall, a contended spinlock, a kmutex sleep), armed by
// `kernel.trace` and read through QUERY_KTRACE -- the shape of Linux
// ftrace's per-CPU ring buffer and of ETW's per-CPU buffers, decoded in
// ring 3 (`/bin/trace`, `tools/dev/trace_view.py`). docs/trace-design.md,
// "The kernel event ring". Not the strace ring: that is abi/trace_abi.h,
// one process's syscalls in a buffer its tracer owns.
#include <stdint.h>

// One event. `tsc` is the writing core's own TSC; `tid` the thread
// current on that core when it was written (0: the kernel or an idle
// core). A switch's is often 0 -- a thread that parks lets go of the core
// before the switch -- so its arguments name both sides.
struct ktrace_rec {
    uint64_t tsc;
    uint16_t event;   // KTRACE_EV_*
    uint16_t cpu;
    int32_t  tid;
    uint64_t a[4];
};
_Static_assert(sizeof(struct ktrace_rec) == 48, "struct ktrace_rec is ABI: 48 bytes");

// Groups: what `kernel.trace` arms, by name ("switch,syscall", or "all").
#define KTRACE_G_SWITCH  (1u << 0)
#define KTRACE_G_WAKEUP  (1u << 1)
#define KTRACE_G_IRQ     (1u << 2)
#define KTRACE_G_FAULT   (1u << 3)
#define KTRACE_G_SYSCALL (1u << 4)
#define KTRACE_G_SPIN    (1u << 5)
#define KTRACE_G_MUTEX   (1u << 6)
#define KTRACE_G_ALL     0x7Fu
#define KTRACE_G_TEST    (1u << 31)   // the KTESTs' own event; never armed by name

#define KTRACE_GROUPS(X)                 \
    X(KTRACE_G_SWITCH,  "switch")        \
    X(KTRACE_G_WAKEUP,  "wakeup")        \
    X(KTRACE_G_IRQ,     "irq")           \
    X(KTRACE_G_FAULT,   "fault")         \
    X(KTRACE_G_SYSCALL, "syscall")       \
    X(KTRACE_G_SPIN,    "spin")          \
    X(KTRACE_G_MUTEX,   "mutex")

// EVERY EVENT ONCE: X(id, name, group, args). `args` names the four
// arguments and how each prints, "name:kind" separated by spaces --
// d signed decimal, x hex, s a scheduler state letter, n a syscall
// number (its name), t a TSC interval (printed as time), v a vector.
// The kernel's panic tail, /bin/trace and a dump's own event table
// (which trace_view.py reads, so a dump describes itself, as ftrace's
// `events/*/format` files do) all come from this list. Ids are ABI:
// a dump outlives the kernel that wrote it.
#define KTRACE_EVENTS(X)                                                              \
    X(1,  "switch",      KTRACE_G_SWITCH,  "prev:d prev_state:s next:d next_tgid:d")   \
    X(2,  "wakeup",      KTRACE_G_WAKEUP,  "tid:d chan:x reason:d tgid:d")             \
    X(3,  "irq",         KTRACE_G_IRQ,     "vector:v rip:x cs:x")                      \
    X(4,  "irq_exit",    KTRACE_G_IRQ,     "vector:v")                                 \
    X(5,  "fault",       KTRACE_G_FAULT,   "addr:x error:x rip:x")                     \
    X(6,  "sys_enter",   KTRACE_G_SYSCALL, "nr:n a0:x a1:x a2:x")                      \
    X(7,  "sys_exit",    KTRACE_G_SYSCALL, "nr:n ret:d")                               \
    X(8,  "spin_wait",   KTRACE_G_SPIN,    "lock:x wait:t site:x")                     \
    X(9,  "mutex_sleep", KTRACE_G_MUTEX,   "mutex:x owner:d")                          \
    X(10, "mutex_wake",  KTRACE_G_MUTEX,   "mutex:x next:d")                           \
    X(11, "test",        KTRACE_G_TEST,    "a0:d a1:d a2:d a3:d")

enum {
    KTRACE_EV_SWITCH = 1, KTRACE_EV_WAKEUP, KTRACE_EV_IRQ, KTRACE_EV_IRQ_EXIT,
    KTRACE_EV_FAULT, KTRACE_EV_SYS_ENTER, KTRACE_EV_SYS_EXIT, KTRACE_EV_SPIN_WAIT,
    KTRACE_EV_MUTEX_SLEEP, KTRACE_EV_MUTEX_WAKE, KTRACE_EV_TEST,
    KTRACE_EV_COUNT   // one past the last id
};

// A switch's prev_state: the outgoing thread's scheduler state, as `ps`
// letters it. R: preempted, still runnable. S: parked. Z: exited.
#define KTRACE_STATE_UNUSED  0
#define KTRACE_STATE_READY   1
#define KTRACE_STATE_RUNNING 2
#define KTRACE_STATE_ZOMBIE  3
#define KTRACE_STATE_BLOCKED 4

// ---- the dump file `trace dump` writes -------------------------------
//
// Little-endian, in this order: the header; `ncpu` cpu rows; `nevents`
// event rows; `nsyscalls` syscall-name rows; `nnames` thread-name rows;
// then `nrecs` records of `rec_size` bytes, each core's oldest first,
// the cores one after another (a cpu row's `nrecs` says how many).
// A reader skips what it does not know by the sizes given here: a row
// grows append-only, as a query record does.
#define KTRACE_DUMP_MAGIC   "SKTRACE"   // 8 bytes with its NUL
#define KTRACE_DUMP_VERSION 1u

struct ktrace_dump_hdr {
    char     magic[8];
    uint32_t version;
    uint32_t hdr_size;       // sizeof this struct
    uint64_t tsc_hz;         // the calibrated TSC rate: ticks to time
    uint64_t armed_tsc;      // when the trace was started
    uint32_t mask;           // the groups it recorded
    uint32_t ncpu;
    uint32_t nevents, nsyscalls, nnames;
    uint32_t rec_size;       // sizeof(struct ktrace_rec)
    uint32_t cpu_size, event_size, syscall_size, name_size;   // each row's size
    uint64_t nrecs;
};

struct ktrace_dump_cpu {
    uint32_t cpu, pad;
    uint64_t capacity;   // records its ring holds
    uint64_t written;    // records written since the trace started
    uint64_t lost;       // ...overwritten before they were read
    uint64_t nrecs;      // in this file
};

#define KTRACE_NAME_MAX 16
#define KTRACE_ARGS_MAX 64

struct ktrace_dump_event {
    uint32_t id, group;
    char     name[KTRACE_NAME_MAX];
    char     args[KTRACE_ARGS_MAX];   // KTRACE_EVENTS' argument string
};

struct ktrace_dump_syscall {
    uint32_t nr;
    char     name[28];
};

// A thread alive when the dump was taken, so a viewer can name a track.
struct ktrace_dump_name {
    int32_t tid, tgid;
    char    name[24];
};

#endif
