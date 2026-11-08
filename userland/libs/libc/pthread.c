// POSIX threads over SYS_THREAD_*. See <pthread.h> for what this does
// not do; this file is the mechanism.
//
// THE DIVISION OF LABOUR IS THE POINT: the kernel starts a thread on a
// stack it is handed and tells you when one has died, and everything
// with a malloc in it lives here -- the stack, the TLS block, the
// return value, the descriptor. That is why the kernel's half is four
// small syscalls rather than a threading library in ring 0.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"

struct pthread {
    int    tid;
    void *(*start)(void *);
    void  *arg;
    void  *retval;
    void  *stack;    // the malloc'd block, NULL for the initial thread
    void  *tls;      // ditto
    int    detached;
    void **tsd;      // pthread_setspecific()'s values, indexed by key
    unsigned ntsd;
    volatile int cancel_pending;
    int cancel_disabled;
    struct __pthread_cleanup *cleanup;   // newest first
};

// WHICH THREAD AM I -- the first real use of `__thread` in this system
// beyond errno, and the reason pthread_self() and pthread_exit() need no
// table lookup. NULL in the initial thread, which never went through
// thread_entry().
static __thread struct pthread *g_self;

// The initial thread's descriptor. Static because it must exist before
// any allocation can, and because there is exactly one.
static struct pthread g_main;

static void tsd_run_destructors(struct pthread *t);

static void thread_entry(void *p) {
    struct pthread *t = (struct pthread *)p;
    g_self = t;
    t->retval = t->start(t->arg);
    tsd_run_destructors(t);
    // The exit CODE is not the return value: a void* does not fit an
    // int, so the value goes in the descriptor and pthread_join() reads
    // it from there. The kernel only ever carries "it finished".
    sys_thread_exit(0);
}

// A block of `size` bytes aligned to 16, plus the pointer to free.
// malloc's own alignment is not part of its contract here, and a TLS
// block placed at an odd address puts every `__thread` variable at one.
static void *aligned_block(size_t size, void **out_raw) {
    char *raw = (char *)malloc(size + 16);
    if (!raw) return NULL;
    *out_raw = raw;
    uintptr_t a = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    return (void *)a;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (!out || !start) return EINVAL;

    size_t stacksize = PTHREAD_STACK_DEFAULT;
    int detached = PTHREAD_CREATE_JOINABLE;
    if (attr) {
        if (attr->stacksize) stacksize = attr->stacksize;
        detached = attr->detachstate;
    }
    if (stacksize < PTHREAD_STACK_MIN) stacksize = PTHREAD_STACK_MIN;

    struct pthread *t = (struct pthread *)malloc(sizeof *t);
    if (!t) return EAGAIN;
    memset(t, 0, sizeof *t);
    t->start = start;
    t->arg = arg;
    t->detached = detached == PTHREAD_CREATE_DETACHED;

    void *stack_raw = NULL;
    char *stack = (char *)aligned_block(stacksize, &stack_raw);
    if (!stack) { free(t); return EAGAIN; }
    t->stack = stack_raw;

    void *tls_raw = NULL;
    void *tls_mem = aligned_block((size_t)rt_tls_size(), &tls_raw);
    if (!tls_mem) { free(stack_raw); free(t); return EAGAIN; }
    t->tls = tls_raw;
    // EVERY THREAD GETS ITS OWN COPY OF EVERY `__thread` VARIABLE,
    // initialised from the program's template -- which is what makes
    // errno per thread and g_self above possible at all.
    void *tp = rt_tls_install(tls_mem);

    // The stack grows DOWN from the top of the block.
    int tid = sys_thread_create(thread_entry, stack + stacksize, t, tp,
                                t->detached, (unsigned)stacksize);
    if (tid < 0) {
        free(tls_raw); free(stack_raw); free(t);
        return EAGAIN;
    }
    t->tid = tid;
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **retval) {
    if (!t || t == &g_main) return EINVAL;
    if (t->detached) return EINVAL;
    if (sys_thread_join(t->tid) < 0) return ESRCH;
    if (retval) *retval = t->retval;
    // Safe HERE and nowhere earlier: the kernel has reported the thread
    // reaped, so nothing is standing on this stack any more.
    free(t->tsd);
    free(t->tls);
    free(t->stack);
    free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    if (!t || t == &g_main || t->detached) return EINVAL;
    if (sys_thread_detach(t->tid) < 0) return ESRCH;
    t->detached = 1;
    // The stack, the TLS and this descriptor are deliberately LEAKED --
    // see <pthread.h>. Nothing here can know when the thread has
    // stopped using its own stack.
    return 0;
}

void pthread_exit(void *retval) {
    struct pthread *self = pthread_self();
    // Cleanup handlers first, then thread-specific data, as POSIX orders
    // them. Each is unlinked before it runs, so one that exits again
    // does not run itself twice.
    while (self->cleanup) {
        struct __pthread_cleanup *c = self->cleanup;
        self->cleanup = c->prev;
        c->fn(c->arg);
    }
    if (g_self) g_self->retval = retval;
    tsd_run_destructors(self);
    sys_thread_exit(0); // from the initial thread this exits the process
    for (;;) { }        // unreachable; keeps the noreturn promise visible
}

pthread_t pthread_self(void) {
    return g_self ? g_self : &g_main;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_attr_init(pthread_attr_t *a) {
    if (!a) return EINVAL;
    a->stacksize = 0;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a) { return a ? 0 : EINVAL; }

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (!a || size < PTHREAD_STACK_MIN) return EINVAL;
    a->stacksize = size;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *out) {
    if (!a || !out) return EINVAL;
    *out = a->stacksize ? a->stacksize : PTHREAD_STACK_DEFAULT;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    if (!a || (state != PTHREAD_CREATE_JOINABLE &&
               state != PTHREAD_CREATE_DETACHED)) return EINVAL;
    a->detachstate = state;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *out) {
    if (!a || !out) return EINVAL;
    *out = a->detachstate;
    return 0;
}

// --- mutexes ---------------------------------------------------------
//
// A TEST-AND-SET THAT YIELDS, not a futex: `SYS_FUTEX_WAIT` exists
// (kernel/proc/futex.c) and this has not been moved onto it yet
// (docs/roadmap.md). Correct under a preemptive
// round-robin scheduler -- the holder is always eventually run, so a
// waiter always eventually gets in -- and the cost is that a waiter
// spends its slice asking. The acquire/release ordering is real work
// even so: x86-64 is TSO, but the COMPILER reorders freely, and the
// builtins are what stop it hoisting the critical section out.

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
    if (!a) return EINVAL;
    a->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { return a ? 0 : EINVAL; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if (!a || (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
               type != PTHREAD_MUTEX_ERRORCHECK)) return EINVAL;
    a->type = type;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) {
    if (!a || !type) return EINVAL;
    *type = a->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner = 0;
    m->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
    m->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->locked) return EBUSY;
    return 0;
}

// `owner == me` is read without the lock, and that is sound: only this
// thread ever stores its own tid there, so it cannot see itself by
// accident -- at worst it sees another thread's tid or 0.
int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    int me = sys_gettid();
    if (m->locked && m->owner == me) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) { m->count++; return 0; }
        // Not recursive: a relock says so rather than deadlocking.
        return EDEADLK;
    }
    while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE))
        sys_yield();
    m->owner = me;
    m->count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    int me = sys_gettid();
    if (m->locked && m->owner == me && m->type == PTHREAD_MUTEX_RECURSIVE) {
        m->count++;
        return 0;
    }
    if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) return EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (!m->locked) return EPERM;
    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner != sys_gettid()) return EPERM;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) return 0;
    m->owner = 0;
    m->count = 0;
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

// --- condition variables ---------------------------------------------
//
// A GENERATION COUNTER, and reading it BEFORE the unlock is what makes
// the wait race-free: a signal that lands in the window between
// unlocking and looping has already bumped the counter the waiter
// sampled, so the waiter sees it and does not sleep through it. That is
// the same lost-wakeup argument the kernel's own wait channels make.

int pthread_condattr_init(pthread_condattr_t *a) {
    if (!a) return EINVAL;
    a->clock = CLOCK_REALTIME;
    return 0;
}
int pthread_condattr_destroy(pthread_condattr_t *a) { return a ? 0 : EINVAL; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock) {
    if (!a || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) return EINVAL;
    a->clock = clock;
    return 0;
}
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock) {
    if (!a || !clock) return EINVAL;
    *clock = a->clock;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    if (!c) return EINVAL;
    c->generation = 0;
    c->clock = attr ? attr->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { return c ? 0 : EINVAL; }

// A cancellation point: a cancel that arrives while waiting takes the
// mutex back FIRST, as POSIX requires, so cleanup handlers run holding it.
static int cancel_now(void) {
    struct pthread *self = pthread_self();
    return __atomic_load_n(&self->cancel_pending, __ATOMIC_ACQUIRE) && !self->cancel_disabled;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) return EINVAL;
    unsigned seen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
    int rc = pthread_mutex_unlock(m);
    if (rc) return rc;
    while (__atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) == seen) {
        if (cancel_now()) { pthread_mutex_lock(m); pthread_testcancel(); }
        sys_yield();
    }
    return pthread_mutex_lock(m);
}

static int ts_reached(const struct timespec *now, const struct timespec *t) {
    return now->tv_sec > t->tv_sec ||
           (now->tv_sec == t->tv_sec && now->tv_nsec >= t->tv_nsec);
}

// pthread_cond_wait() with a deadline, checked between yields. The mutex
// is held again on BOTH returns, as POSIX requires -- a caller retests
// its predicate either way.
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime) {
    if (!c || !m || !abstime || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)
        return EINVAL;
    unsigned seen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
    int rc = pthread_mutex_unlock(m);
    if (rc) return rc;
    int timed_out = 0;
    while (__atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) == seen) {
        if (cancel_now()) { pthread_mutex_lock(m); pthread_testcancel(); }
        struct timespec now;
        if (clock_gettime(c->clock, &now) == 0 && ts_reached(&now, abstime)) {
            timed_out = 1;
            break;
        }
        sys_yield();
    }
    rc = pthread_mutex_lock(m);
    return rc ? rc : timed_out ? ETIMEDOUT : 0;
}

// SIGNAL AND BROADCAST ARE THE SAME CALL, and that is allowed: POSIX
// lets an implementation wake more waiters than asked, which is why
// every correct use of a condition variable re-tests its predicate in a
// loop. Waking exactly one needs per-waiter state, which this would
// have to grow before a futex could wake a chosen waiter.
int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) { return pthread_cond_signal(c); }

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (!once || !fn) return EINVAL;
    // 0 = not run, 1 = running, 2 = done.
    int expected = 0;
    if (__atomic_compare_exchange_n(&once->done, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) != 2) sys_yield();
    return 0;
}

// --- cancellation ------------------------------------------------------

int pthread_cancel(pthread_t t) {
    if (!t) return ESRCH;
    __atomic_store_n(&t->cancel_pending, 1, __ATOMIC_RELEASE);
    return 0;
}

int pthread_setcancelstate(int state, int *oldstate) {
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    struct pthread *self = pthread_self();
    if (oldstate) *oldstate = self->cancel_disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
    self->cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
    return 0;
}

int pthread_setcanceltype(int type, int *oldtype) {
    if (type != PTHREAD_CANCEL_DEFERRED) return EINVAL;
    if (oldtype) *oldtype = PTHREAD_CANCEL_DEFERRED;
    return 0;
}

void pthread_testcancel(void) {
    if (cancel_now()) pthread_exit(PTHREAD_CANCELED);
}

void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg) {
    struct pthread *self = pthread_self();
    c->fn = fn;
    c->arg = arg;
    c->prev = self->cleanup;
    self->cleanup = c;
}

void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute) {
    struct pthread *self = pthread_self();
    self->cleanup = c->prev;
    if (execute) c->fn(c->arg);
}

// --- read-write locks ------------------------------------------------
//
// The counts change only under `lock`, a test-and-set taken for a few
// instructions; waiting happens OUTSIDE it, yielding between looks.

static void rw_take(pthread_rwlock_t *l) {
    while (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE)) sys_yield();
}
static void rw_drop(pthread_rwlock_t *l) { __atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE); }

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) { return a ? 0 : EINVAL; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { return a ? 0 : EINVAL; }

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!l) return EINVAL;
    l->lock = 0;
    l->readers = l->writer = l->writers_waiting = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    return l->readers || l->writer ? EBUSY : 0;
}

// One attempt at the read lock: 0, or EBUSY while a writer holds or waits.
static int rd_try(pthread_rwlock_t *l) {
    rw_take(l);
    int ok = !l->writer && !l->writers_waiting;
    if (ok) l->readers++;
    rw_drop(l);
    return ok ? 0 : EBUSY;
}

static int wr_try(pthread_rwlock_t *l, int me) {
    rw_take(l);
    int ok = !l->writer && !l->readers;
    if (ok) l->writer = me;
    rw_drop(l);
    return ok ? 0 : EBUSY;
}

// `abstime` NULL: wait for ever.
static int rd_wait(pthread_rwlock_t *l, const struct timespec *abstime) {
    if (!l) return EINVAL;
    if (l->writer == sys_gettid()) return EDEADLK;
    while (rd_try(l) != 0) {
        struct timespec now;
        if (abstime && clock_gettime(CLOCK_REALTIME, &now) == 0 && ts_reached(&now, abstime))
            return ETIMEDOUT;
        sys_yield();
    }
    return 0;
}

static int wr_wait(pthread_rwlock_t *l, const struct timespec *abstime) {
    if (!l) return EINVAL;
    int me = sys_gettid();
    if (l->writer == me) return EDEADLK;
    rw_take(l);
    l->writers_waiting++;
    rw_drop(l);
    int rc = 0;
    while (wr_try(l, me) != 0) {
        struct timespec now;
        if (abstime && clock_gettime(CLOCK_REALTIME, &now) == 0 && ts_reached(&now, abstime)) {
            rc = ETIMEDOUT;
            break;
        }
        sys_yield();
    }
    rw_take(l);
    l->writers_waiting--;
    rw_drop(l);
    return rc;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) { return rd_wait(l, NULL); }
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { return l ? rd_try(l) : EINVAL; }
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *abstime) {
    return abstime ? rd_wait(l, abstime) : EINVAL;
}
int pthread_rwlock_wrlock(pthread_rwlock_t *l) { return wr_wait(l, NULL); }
int pthread_rwlock_trywrlock(pthread_rwlock_t *l) { return l ? wr_try(l, sys_gettid()) : EINVAL; }
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *abstime) {
    return abstime ? wr_wait(l, abstime) : EINVAL;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    if (!l) return EINVAL;
    int rc = 0;
    rw_take(l);
    if (l->writer && l->writer == sys_gettid()) l->writer = 0;
    else if (l->readers > 0) l->readers--;
    else rc = EPERM;
    rw_drop(l);
    return rc;
}

// --- thread-specific data --------------------------------------------
//
// The key table grows and its slots are never reused (see <pthread.h>);
// each thread's value array grows on its first pthread_setspecific()
// past the end. The table's lock is the same spin-and-yield as a mutex.

struct tsd_key {
    int used;
    void (*dtor)(void *);
};

static struct tsd_key *g_keys;
static unsigned g_nkeys, g_keycap;
static volatile int g_keylock;

static void key_lock(void) {
    while (__atomic_exchange_n(&g_keylock, 1, __ATOMIC_ACQUIRE)) sys_yield();
}
static void key_unlock(void) { __atomic_store_n(&g_keylock, 0, __ATOMIC_RELEASE); }

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    key_lock();
    if (g_nkeys == g_keycap) {
        unsigned cap = g_keycap ? g_keycap * 2 : 16;
        struct tsd_key *k = realloc(g_keys, cap * sizeof *k);
        if (!k) { key_unlock(); return EAGAIN; }
        g_keys = k;
        g_keycap = cap;
    }
    g_keys[g_nkeys].used = 1;
    g_keys[g_nkeys].dtor = destructor;
    *key = g_nkeys++;
    key_unlock();
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    key_lock();
    int ok = key < g_nkeys && g_keys[key].used;
    if (ok) g_keys[key].used = 0;
    key_unlock();
    return ok ? 0 : EINVAL;
}

void *pthread_getspecific(pthread_key_t key) {
    struct pthread *t = pthread_self();
    return key < t->ntsd ? t->tsd[key] : NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    key_lock();
    int ok = key < g_nkeys && g_keys[key].used;
    key_unlock();
    if (!ok) return EINVAL;
    struct pthread *t = pthread_self();
    if (key >= t->ntsd) {
        unsigned n = key + 1 > t->ntsd * 2 ? key + 1 : t->ntsd * 2;
        void **v = realloc(t->tsd, n * sizeof *v);
        if (!v) return ENOMEM;
        for (unsigned i = t->ntsd; i < n; i++) v[i] = NULL;
        t->tsd = v;
        t->ntsd = n;
    }
    t->tsd[key] = (void *)value;
    return 0;
}

// POSIX's loop: a destructor may set another value, so repeat while any
// ran, a bounded number of times. Each value is cleared BEFORE its
// destructor runs.
static void tsd_run_destructors(struct pthread *t) {
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        int ran = 0;
        for (unsigned k = 0; k < t->ntsd; k++) {
            void *v = t->tsd[k];
            if (!v) continue;
            key_lock();
            void (*dtor)(void *) = k < g_nkeys && g_keys[k].used ? g_keys[k].dtor : NULL;
            key_unlock();
            t->tsd[k] = NULL;
            if (dtor) { dtor(v); ran = 1; }
        }
        if (!ran) break;
    }
}
