#ifndef KMUTEX_H
#define KMUTEX_H

#include "spinlock.h"

// A SLEEPING LOCK: a contender that can sleep yields the CPU instead of
// holding the machine still, which is the whole point of it existing.
//
// It replaces `vfs.c`'s blanket preemption guard, and the difference is
// what `docs/blocking-design.md` is about: the guard stopped EVERY
// context for the length of a backend call, so a compositor waiting on
// nothing at all still waited. A lock stops only the contexts that
// actually want the same thing.
//
// **IT STARTED AS ONE LOCK OVER A NON-RE-ENTRANT SUBSYSTEM, Linux's
// Big Kernel Lock** -- which took from 2.0 to 2.6.39 to remove. It is
// one per MOUNT now (docs/fslock-design.md, stage 2), and one per
// volume is still a big lock over that volume.
//
// **ONE THING FROM THE BKL IS DELIBERATELY NOT COPIED: it was dropped
// when its holder slept and retaken on return.** Here the lock must be
// HELD across a disk wait, or the serialisation the scratch buffers
// depend on evaporates at exactly the moment it is needed.
struct kmutex {
    int depth;   // 0 = free. RECURSIVE: see below
    int owner;   // pid of the holder, 0 for the kernel context. Only
                 // meaningful while depth > 0
    int owned;   // whether `owner` means anything -- pid 0 is the
                 // KERNEL context, a real owner, so it cannot double as
                 // "nobody"
    int handed;  // an unlock GAVE it to `owner`, which is still parked
                 // and has not resumed to claim it -- see kmutex_unlock()
    struct spinlock wait_lock;   // guards the four above; zero is free
};

// Blocks until the lock is held. A caller with a scheduler slot and no
// preemption guard raised SLEEPS; anything else spins (see kmutex.c on
// why the spin must not disable preemption).
//
// **RECURSIVE, BECAUSE WHAT IT REPLACED WAS A COUNTER.**
// scheduler_preempt_disable() nests, so an FS_OP() reached from inside
// another one was merely depth 2 and worked. A plain mutex turns every
// one of those into an instant self-deadlock -- which is what it did:
// the boot hung at `init: target graphical` with no output at all. The
// BKL was recursive for the same reason and it is the same trade: it
// does NOT make recursion safe (a backend re-entered through an
// fs_list() callback still walks tfs3.c's scratch twice, which vfs.c's
// comment has always refused), it just keeps a shape that already
// worked from becoming a hang.
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);

// THE SAME LOCK, ANNOTATED FOR LOCKDEP (lockdep.h) -- both are
// kmutex_lock() without `option lockdep`. _nested() takes it as
// `subclass` of its class, for two of one class held in a fixed order
// (a parent mount's, then a child's: Linux's mutex_lock_nested()).
// _nest_lock() takes one of many of its class under `outer`, which
// serialises every such holder (Linux's mutex_lock_nest_lock()).
#ifdef SAUNAOS_LOCKDEP
void kmutex_lock_nested(struct kmutex *m, int subclass);
void kmutex_lock_nest_lock(struct kmutex *m, const struct kmutex *outer);
#else
#define kmutex_lock_nested(m, subclass) ((void)(subclass), kmutex_lock(m))
#define kmutex_lock_nest_lock(m, outer) ((void)(outer), kmutex_lock(m))
#endif

// Its lockdep class, for a kmutex inside a struct: every lock given one
// name is one class. Where it is set up (a zeroed struct is unlocked);
// a file-scope static needs none.
#define kmutex_name(m, name) lockdep_name_kmutex((m), (name))

// Takes it only if that needs no wait (free, or already ours). 1 if
// held now. For a caller that has somewhere better to be -- an idle
// write-back -- and never blocks, so it is safe from any context.
int kmutex_trylock(struct kmutex *m);
// ...as `subclass` of its class (kmutex_lock_nested()'s), for a lock taken
// both ways: the mount locks.
#ifdef SAUNAOS_LOCKDEP
int kmutex_trylock_nested(struct kmutex *m, int subclass);
#else
#define kmutex_trylock_nested(m, subclass) ((void)(subclass), kmutex_trylock(m))
#endif

// How many kmutex_lock() calls came from a context that could neither
// sleep nor be rotated away (kmutex.c's might_sleep()). Each distinct
// call site is also logged once. For a test; the log is the diagnostic.
unsigned kmutex_atomic_takes(void);

// For a diagnostic, and for an assertion in a test. Never a decision.
int kmutex_held(const struct kmutex *m);
int kmutex_owner(const struct kmutex *m);

#endif
