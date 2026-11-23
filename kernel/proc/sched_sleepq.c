// The wait channels, hashed onto chains with a lock each -- FreeBSD's
// sleepqueue chains (sched_internal.h). A slot is on its channel's chain
// from an arm (scheduler_wait_arm()) or a park until it is disarmed,
// woken or leaves BLOCKED, so a wake walks the few slots that asked for
// that channel, and one that finds nobody parked takes no other lock.
// **THE CHAIN COMES FIRST** in the scheduler's order (sched_internal.h):
// a wake holds it while it takes each sleeper's pi_lock and queue, and a
// park holds it from "was I woken?" until BLOCKED.
//
// The chain count is a hash width, not a capacity -- any number of slots
// waits on one chain.
#include "sched_internal.h"
#include "irqflags.h"
#include "spinlock.h"

#define SQ_CHAINS 64   // a power of two

struct sq_chain {
    struct spinlock lock;
    int head, tail;   // slots, -1 for none
};
// **EMPTY FROM THE FIRST INSTRUCTION, NOT FROM sleepq_init()**: USB's
// port scans unlock a contended kmutex before scheduler_init() runs, and
// a zeroed head names "slot 0" of a table that does not exist yet -- a
// walk round zeroed memory forever, interrupts off (a hard lockup at
// boot on a machine with a USB device plugged in).
static struct sq_chain g_chains[SQ_CHAINS] = {
    [0 ... SQ_CHAINS - 1] = { .head = -1, .tail = -1 },
};

static struct sq_chain *chain_of(const void *chan) {
    uint64_t a = (uint64_t)(uintptr_t)chan;
    return &g_chains[(a * 0x9E3779B97F4A7C15ull) >> (64 - 6)];
}
_Static_assert(SQ_CHAINS == 1 << 6, "chain_of() shifts for 64 chains");

// Names the locks for lockdep, nothing else: see g_chains.
void sleepq_init(void) {
    for (int i = 0; i < SQ_CHAINS; i++) spin_lock_name(&g_chains[i].lock, "sleepq");
}

uint64_t sq_lock(const void *chan) {
    uint64_t f = irq_save();
    spin_lock_noguard(&chain_of(chan)->lock);
    return f;
}

void sq_unlock(const void *chan, uint64_t f) {
    spin_unlock_noguard(&chain_of(chan)->lock);
    irq_restore(f);
}

void sq_lock_raw(const void *chan) { spin_lock_noguard(&chain_of(chan)->lock); }
void sq_unlock_raw(const void *chan) { spin_unlock_noguard(&chain_of(chan)->lock); }

int sq_first(const void *chan) { return chain_of(chan)->head; }

void sq_link(int s, const void *chan) {
    struct sq_chain *c = chain_of(chan);
    struct sched_process *p = &procs[s];
    p->sq_chan = chan;
    p->sq_next = -1;
    p->sq_prev = c->tail;
    if (c->tail >= 0) procs[c->tail].sq_next = s;
    else c->head = s;
    c->tail = s;
    __atomic_store_n(&p->sq_on, 1, __ATOMIC_RELEASE);
}

void sq_unlink(int s) {
    struct sched_process *p = &procs[s];
    struct sq_chain *c = chain_of(p->sq_chan);
    if (p->sq_prev >= 0) procs[p->sq_prev].sq_next = p->sq_next;
    else c->head = p->sq_next;
    if (p->sq_next >= 0) procs[p->sq_next].sq_prev = p->sq_prev;
    else c->tail = p->sq_prev;
    __atomic_store_n(&p->sq_on, 0, __ATOMIC_RELEASE);
}

// The channel is read before its lock is held, so it is checked again
// under it: only the slot itself re-arms, and it is not running when
// this is called for it -- but a stale read costs one more turn, not a
// wrong unlink.
void sq_unlink_any(int s) {
    struct sched_process *p = &procs[s];
    for (;;) {
        if (!__atomic_load_n(&p->sq_on, __ATOMIC_ACQUIRE)) return;
        const void *chan = p->sq_chan;
        uint64_t f = sq_lock(chan);
        int mine = p->sq_on && p->sq_chan == chan;
        if (mine) {
            sq_unlink(s);
            p->arm_chan = 0;
            p->armed_woken = 0;
        }
        sq_unlock(chan, f);
        if (mine) return;
    }
}

// A park's first half, holding nothing yet: an arm on another channel is
// over, then `chan`'s chain lock -- before the parker's pi_lock and queue. 1 = a wake already reached
// the arm -- consumed, nothing parked, the lock let go; 0 = park, with
// the chain lock HELD (*q) until BLOCKED, so "was I woken?" and "I am
// asleep" are one step to every waker.
int sq_park_begin(int s, const void *chan, uint64_t *q) {
    struct sched_process *p = &procs[s];
    if (p->sq_on && p->sq_chan != chan) sq_unlink_any(s);
    *q = sq_lock(chan);
    if (p->armed_woken && p->arm_chan == chan) {
        p->armed_woken = 0;
        p->arm_chan = 0;
        if (p->sq_on) sq_unlink(s);
        sq_unlock(chan, *q);
        return 1;
    }
    return 0;
}

// Parking: on the chain, the arm spent. Chain lock held.
void sq_park_commit(int s, const void *chan) {
    procs[s].arm_chan = 0;
    if (!procs[s].sq_on) sq_link(s, chan);
}

// Not parking after all (a signal): the arm spent, off the chain.
void sq_park_cancel(int s, const void *chan, uint64_t q) {
    procs[s].arm_chan = 0;
    if (procs[s].sq_on) sq_unlink(s);
    sq_unlock(chan, q);
}

// Not parking HERE (a lock-free park declining): nothing written, the
// arm left for the path that parks it.
void sq_park_abandon(const void *chan, uint64_t q) { sq_unlock(chan, q); }
