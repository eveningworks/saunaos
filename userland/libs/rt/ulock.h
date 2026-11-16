// ulock -- ring 3's one internal lock: a word that parks its waiters on
// a futex. Every lock in libc (mutex, rwlock, once, stdio, malloc) and
// the toolkit's own small ones are this, so none of them spins.
//
// Drepper's "mutex3" ("Futexes Are Tricky"), glibc's and musl's shape:
// 0 free, 1 held, 2 held and somebody may be parked. An uncontended
// take is one compare-and-swap and its drop one exchange; only a drop
// that finds 2 makes a syscall.
//
// NO SPIN BEFORE PARKING: ring 3 runs on one core, so the holder cannot
// run while a waiter spins. A yield loop instead kept every waiter
// READY and charged it CPU for as long as the holder slept.
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
