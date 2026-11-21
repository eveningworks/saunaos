// ulock -- ring 3's one internal lock: a word that parks its waiters on
// a futex. libc's mutex, stdio and malloc locks, the guard on a rwlock's
// counts and the toolkit's small locks are this, so none of them spins;
// a rwlock's, pthread_once's and a condition variable's WAITERS park on
// a word of their own and are woken all at once (pthread.c).
//
// Drepper's "mutex3" ("Futexes Are Tricky"), glibc's and musl's shape:
// 0 free, 1 held, 2 held and somebody may be parked. An uncontended
// take is one compare-and-swap and its drop one exchange; only a drop
// that finds 2 makes a syscall.
//
// NO SPIN BEFORE PARKING, though a program's threads run on several
// cores since smp=run became the default: glibc's default mutex does not
// spin either (that is the opt-in PTHREAD_MUTEX_ADAPTIVE_NP), and nothing
// here has measured a spin winning. A yield loop, the shape before this,
// kept every waiter READY and charged it CPU while the holder slept.
//
// THE TRAP: a taker that saw 2 stores 2, never 1 -- it cannot know it
// was the last waiter, and a 1 would let the next drop skip its wake
// and strand whoever is still parked.
#ifndef USERLAND_RT_ULOCK_H
#define USERLAND_RT_ULOCK_H

#include "rt/sys.h"

static inline int ulock_try(volatile int *w) {
    int c = 0;
    return __atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void ulock_take(volatile int *w) {
    int c = 0;
    if (__atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2) c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        // -EAGAIN when the word is no longer 2, and a signal may end the
        // wait early: both go round and try again.
        sys_futex_wait((volatile uint32_t *)w, 2, 0);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
}

static inline void ulock_drop(volatile int *w) {
    if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2)
        sys_futex_wake((volatile uint32_t *)w, 1);
}

#endif
