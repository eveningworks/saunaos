#ifndef KERNEL_LOCKDEP_H
#define KERNEL_LOCKDEP_H

// Lock-order checking, Linux's lockdep in miniature (docs/lockdep-design.md):
// every spinlock and kmutex belongs to a CLASS, the order classes are
// taken in is learned as a graph, and the first acquisition that closes a
// cycle in it -- A then B somewhere, B then A here -- is reported once,
// naming both classes and where each edge was first seen. It also
// reports a class taken in a hardware interrupt and elsewhere with
// interrupts on, a kmutex taken holding a spinlock (a sleep under it), a
// kmutex in an interrupt, the kernel lock taken holding a spinlock, and
// a kmutex still held on the way back to ring 3. It never panics.
//
// `option lockdep = yes` in build.conf (ON in configs/debug.conf);
// without it every hook here is an empty inline and the lock paths
// compile exactly as before.
//
// **A FILE-SCOPE STATIC LOCK IS ITS OWN CLASS; A LOCK INSIDE A STRUCT
// IS NAMED WHERE IT IS SET UP** (spin_lock_name(), kmutex_name()), and
// every lock given one name is one class -- all mount locks are "mount".
// An unnamed lock that is not in the kernel image is not checked; its
// first take is reported once ("unnamed"), which is how a missing name
// shows. Linux's "non-static key" warning.
//
// **TWO LOCKS OF ONE CLASS HELD TOGETHER ARE REPORTED** unless the
// second is taken with kmutex_lock_nested() (a SUBCLASS: a different
// graph node, so parent-before-child is an edge, not a recursion) or
// kmutex_lock_nest_lock() (every one of them under one outer lock --
// fs_exclusive_begin()'s "every mount, holding g_excl").
#include <stdint.h>

struct spinlock;
struct kmutex;

#define LOCKDEP_SUBCLASSES 8   // kmutex_lock_nested()'s subclasses 0..7; more clamps to 7

// What a report is about; query_abi.h's QUERY_LOCKDEP_* mirror these.
enum lockdep_kind {
    LOCKDEP_STATS = 0,          // not a report: the query's summary row
    LOCKDEP_INVERSION = 1,      // a cycle in the order graph
    LOCKDEP_RECURSION = 2,      // two locks of one class held, no annotation
    LOCKDEP_IRQ_INCONSISTENT = 3,  // taken in an interrupt, and with interrupts on
    LOCKDEP_IRQ_UNSAFE_ORDER = 4,  // an irq-safe class held while taking an irq-unsafe one
    LOCKDEP_SLEEP_IN_SPIN = 5,  // a kmutex taken holding a spinlock
    LOCKDEP_KMUTEX_IN_IRQ = 6,  // a kmutex taken in a hardware interrupt
    LOCKDEP_BKL_IN_SPIN = 7,    // the kernel lock taken holding a spinlock
    LOCKDEP_HELD_TO_USER = 8,   // back to ring 3 still holding a kmutex
    LOCKDEP_UNNAMED = 9,        // an embedded lock with no class: not checked
    LOCKDEP_KINDS
};

#define LOCKDEP_KMUTEX_INNER 0x80000000u   // spinlock.ld_class: a kmutex's own wait_lock
#define LOCKDEP_UNNAMED_CLASS 0x7FFFFFFFu  // ...seen unnamed and reported: never checked

#ifdef SAUNAOS_LOCKDEP
#define LOCKDEP_ENABLED 1

struct lockdep_held {
    uint16_t cls;        // the graph node: a class, or one of its subclasses
    uint16_t count;      // > 1: several of the class under one nest lock
    uint32_t pad;
    const void *lock;    // the instance, to match its release
    const void *nest;    // kmutex_lock_nest_lock()'s outer lock, or 0
    uintptr_t site;      // where it was taken
};

// HELD LOCKS ARE TRACKED WHERE THEY CAN LIVE: a spinlock raises the
// preemption guard, so it stays on its core (struct cpu); a kmutex is
// held across sleeps and migrations, so it belongs to the context
// (struct sched_process, or the core's for slot -1 -- the BSP's kernel
// context, an AP's idle loop). So does the interrupt depth, which a
// switch from inside a tick must not hand to the context it resumes.
#define LOCKDEP_TASK_HELD 12
#define LOCKDEP_CPU_HELD 8
struct lockdep_task {
    int hardirq;         // depth inside hardware interrupt handlers
    int n;
    struct lockdep_held h[LOCKDEP_TASK_HELD];
};
struct lockdep_cpu {
    int busy;            // inside lockdep: a lock taken by its own report is not checked
    int n;
    uint64_t checked;    // acquisitions checked here
    struct lockdep_held h[LOCKDEP_CPU_HELD];
    struct lockdep_task kernel_task;   // slot -1's kmutexes on this core
};

// spinlock.c's hooks. `site` is the caller's call site.
void lockdep_spin_acquired(struct spinlock *l, uintptr_t site, int trylock);
void lockdep_spin_acquired_nested(struct spinlock *l, uintptr_t site, int trylock, int sub);
void lockdep_spin_release(struct spinlock *l);
void lockdep_bkl_acquire(uintptr_t site);   // a FRESH take of the kernel lock
// kmutex.c's. prepare() marks the wait_lock before it is first taken.
void lockdep_kmutex_prepare(struct kmutex *m);
void lockdep_kmutex_acquired(struct kmutex *m, int sub, const struct kmutex *nest,
                             uintptr_t site, int trylock);
void lockdep_kmutex_release(struct kmutex *m);
// isr_dispatch(): around a hardware interrupt's HANDLER -- not the way
// back to ring 3 after it, where a signal or an exit runs. Interrupts off.
int  lockdep_hardirq_enter(void);
void lockdep_hardirq_exit(int was);
int  lockdep_hardirq_depth(void);
void lockdep_user_return(void);   // ...and on the way back to ring 3
void lockdep_task_reset(struct lockdep_task *t);   // a context that has never run

void lockdep_name_spin(struct spinlock *l, const char *name);
void lockdep_name_kmutex(struct kmutex *m, const char *name);

// For the query and the KTESTs. A report's names are its classes'.
#define LOCKDEP_PATH_MAX 6
struct lockdep_report {
    uint16_t kind, a, b, path_len;   // a, b: class ids (0: none); path_len: edges
    uint16_t path[LOCKDEP_PATH_MAX]; // an inversion's old path, b ... a, as nodes
    uintptr_t site[4];   // now: held lock's site, new lock's; then the old path's first edge's
    int printed;
};
struct lockdep_stats {
    uint32_t classes, classes_max, edges, edges_max, reports, reports_max;
    uint32_t reports_lost, edges_unsited, max_spin_held, max_task_held, unmatched;
    uint64_t checked;    // acquisitions checked
};
void lockdep_get_stats(struct lockdep_stats *out);
int  lockdep_report_count(void);
int  lockdep_report_get(int i, struct lockdep_report *out);
const char *lockdep_class_name(int cls, char *buf, int cap);   // "mount/2", "heap"
// A KTEST's classes leave nothing behind: their edges, usage and
// reports go; the class ids stay (locks still name them).
void lockdep_forget(const char *name);
#else
#define LOCKDEP_ENABLED 0
static inline void lockdep_name_spin(struct spinlock *l, const char *name) { (void)l; (void)name; }
static inline void lockdep_name_kmutex(struct kmutex *m, const char *name) { (void)m; (void)name; }
#endif

#endif
