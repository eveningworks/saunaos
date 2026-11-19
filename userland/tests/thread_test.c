// Threads, from ring 3: creation, joining, sharing, and the two things
// that are NOT shared.
//
// **IT MUST BE SPAWNED, NOT `run`.** A thread needs its creator to hold
// a scheduler slot, and the legacy `run` loader has none -- under it
// SYS_THREAD_CREATE answers -EPERM and every check below fails for a
// reason that has nothing to do with threads. kernel/proc/thread_test.c
// spawns it, the same arrangement cputime_test and pipe_test have.
//
// THREE CHECKS ARE LOAD-BEARING, in the sense that a plausible broken
// implementation passes everything else and fails exactly one:
//
//   - **The pointer round trip.** A thread that was really a fork would
//     run, return a value, and join perfectly -- and the parent would
//     never see the write. Writing through a pointer the parent handed
//     over is what proves ONE address space.
//   - **errno after a failure in each.** A shared errno passes every
//     other check here; this is the only one that reads it from two
//     threads and requires two different values. It is the reason TLS
//     had to land in the same change as threads rather than after.
//   - **The critical section under a yield.** A counter incremented in
//     a loop can come out right with no mutex at all, because a
//     preemption has to land in exactly the wrong instruction. Yielding
//     INSIDE the section makes a missing mutex fail every time.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include "rt/sys.h"
#include <unistd.h>       // fork, _exit
#include <sys/mman.h>     // PROT_NONE
#include "query_abi.h"    // QUERY_PROCMAP: where the guard page is
#include "signal_abi.h"   // SIGNAL_EXIT_BASE, SIGSEGV



#include "base/utest.h"

// --- what the threads report back ------------------------------------

struct shared {
    int   ran;
    int   tid;
    int   pid;
    int   errno_seen;
    void *stack_probe;   // the address of one of its locals
};

// A `__thread` variable that is NOT errno: errno could in principle be
// special-cased, a plain one cannot.
static __thread int t_local = 11;
static int t_local_from_thread;

static void *worker(void *arg) {
    struct shared *s = (struct shared *)arg;
    int on_my_stack = 0;

    s->ran = 1;
    s->tid = sys_gettid();
    s->pid = sys_getpid();
    s->stack_probe = &on_my_stack;

    t_local = 22;
    t_local_from_thread = t_local;

    // A call that must fail, so this thread has an errno of its own.
    // A negative fd cannot be valid in any implementation.
    sys_close(-1);
    s->errno_seen = sys_errno();

    return (void *)0xC0FFEE;
}

// --- mutual exclusion -------------------------------------------------

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_in_section;   // whose section is open, by tid
static volatile int g_section_broken;
static volatile int g_section_runs;

static void *section_worker(void *arg) {
    (void)arg;
    int me = sys_gettid();
    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&g_lock);
        g_in_section = me;
        // THE YIELD IS THE TEST. Anything else in the section would be
        // over before a preemption could land in it.
        sys_yield();
        if (g_in_section != me) g_section_broken = 1;
        g_section_runs++;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

// --- a condition variable ---------------------------------------------

static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_handed_over;

static void *cv_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cv_lock);
    g_handed_over = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_cv_lock);
    return NULL;
}

// --- a detached thread -------------------------------------------------

static volatile int g_detached_ran;

static void *detached_worker(void *arg) {
    (void)arg;
    g_detached_ran = 1;
    return NULL;
}

// --- the stack a thread starts on ------------------------------------
//
// **EVERY CHECK ABOVE PASSED WHILE THIS WAS BROKEN**, which is why it
// is its own worker. The kernel handed a new thread a 16-ALIGNED RSP,
// where SysV says a function begins with RSP % 16 == 8 (a `call` has
// just pushed a return address). GCC then emits `movaps` against stack
// slots it believes are aligned and the thread takes a #GP -- but only
// if it uses SSE at all, and none of the workers above do. The first
// GUI client to run a worker crashed on its first snprintf.
//
// So this one deliberately does what ordinary code does: formats into a
// local buffer and does floating-point arithmetic, both of which GCC
// compiles to aligned stack traffic.
static volatile int g_sse_ok;

static void *sse_worker(void *arg) {
    (void)arg;
    char buf[80];
    volatile double d[4] = { 1.5, 2.25, 3.125, 4.0625 };
    double sum = d[0] + d[1] + d[2] + d[3];
    snprintf(buf, sizeof buf, "%d/%d", (int)sum, (int)(sum * 16));
    g_sse_ok = (strcmp(buf, "10/175") == 0);
    return NULL;
}

// What a new thread finds blocked, before it changes anything.
static volatile int g_mask_int = -1;

static void *mask_worker(void *arg) {
    (void)arg;
    sigset_t cur;
    sigemptyset(&cur);
    if (sigprocmask(SIG_BLOCK, 0, &cur) == 0) g_mask_int = sigismember(&cur, SIGINT);
    return 0;
}

// A HANDLER INSTALLED AFTER A THREAD EXISTS, run on THAT thread. Each
// thread kept its own copy of the disposition table from its creation,
// and a frame was refused anywhere but the main stack -- so the worker
// died of SIGUSR1's default action and took the process with it.
static volatile int g_sig_tid, g_worker_tid, g_worker_done;
static volatile uintptr_t g_sig_sp, g_worker_sp;

static void on_usr1(int sig) {
    (void)sig;
    volatile int here = 0;
    g_sig_tid = sys_gettid();
    g_sig_sp = (uintptr_t)&here;
}

static void *sig_worker(void *arg) {
    (void)arg;
    volatile int here = 0;
    g_worker_sp = (uintptr_t)&here;
    g_worker_tid = sys_gettid();
    for (int i = 0; i < 4000 && !g_sig_tid; i++) sys_yield();
    g_worker_done = 1;
    return 0;
}

// A recursive mutex another thread holds: neither takeable nor unlockable.
static pthread_mutex_t *g_rm;
static int g_rm_trylock, g_rm_unlock;
static void *rm_worker(void *arg) {
    (void)arg;
    g_rm_trylock = pthread_mutex_trylock(g_rm);
    g_rm_unlock = pthread_mutex_unlock(g_rm);
    return NULL;
}

// Cancellation, at pthread_testcancel() and inside pthread_cond_wait().
static volatile int g_cancel_cleanups, g_cancel_held_mutex = -1;
static pthread_mutex_t g_cancel_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cancel_cv = PTHREAD_COND_INITIALIZER;
static void cancel_cleanup(void *arg) { (void)arg; g_cancel_cleanups++; }
static void cancel_unlock(void *arg) {
    // POSIX: the waiter holds the mutex again when its handlers run.
    g_cancel_held_mutex = pthread_mutex_unlock((pthread_mutex_t *)arg) == 0;
}
static void *cancel_spinner(void *arg) {
    (void)arg;
    pthread_cleanup_push(cancel_cleanup, NULL);
    for (;;) { pthread_testcancel(); sys_yield(); }
    pthread_cleanup_pop(0);
    return NULL;
}
static void *cancel_waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cancel_mu);
    pthread_cleanup_push(cancel_unlock, &g_cancel_mu);
    for (;;) pthread_cond_wait(&g_cancel_cv, &g_cancel_mu);
    pthread_cleanup_pop(0);
    return NULL;
}

// A read-write lock another thread holds for writing.
static pthread_rwlock_t *g_rw;
static int g_rw_tryrd;
static void *rw_worker(void *arg) {
    (void)arg;
    g_rw_tryrd = pthread_rwlock_tryrdlock(g_rw);
    return NULL;
}

// Thread-specific data: a fresh thread sees NULL, and its own value's
// destructor runs when it returns.
static pthread_key_t g_key;
static int g_key_saw_null, g_key_dtor_runs;
static int g_key_thread_value;
static void *g_key_dtor_arg;
static void key_dtor(void *v) { g_key_dtor_arg = v; g_key_dtor_runs++; }
static void *key_worker(void *arg) {
    (void)arg;
    g_key_saw_null = pthread_getspecific(g_key) == NULL;
    pthread_setspecific(g_key, &g_key_thread_value);
    return NULL;
}

// A WAITER ON A HELD LOCK IS PARKED: blocked on a futex, and charged
// next to no CPU while the holder sleeps. A lock that spins on
// sys_yield() passes every other check in this file -- it is correct,
// only never asleep, and it took a third of the CPU from an idle desktop.
static volatile int g_park_tid, g_park_got;
static void *park_mutex(void *arg) {
    g_park_tid = sys_gettid();
    pthread_mutex_lock((pthread_mutex_t *)arg);
    g_park_got = 1;
    pthread_mutex_unlock((pthread_mutex_t *)arg);
    return NULL;
}
static void *park_wrlock(void *arg) {
    g_park_tid = sys_gettid();
    pthread_rwlock_wrlock((pthread_rwlock_t *)arg);
    g_park_got = 1;
    pthread_rwlock_unlock((pthread_rwlock_t *)arg);
    return NULL;
}
static void *park_stream(void *arg) {
    g_park_tid = sys_gettid();
    flockfile((FILE *)arg);
    g_park_got = 1;
    funlockfile((FILE *)arg);
    return NULL;
}

static int slot_of(int tid, struct proc_info *out) {
    for (int i = 0; sys_proc_info(i, out) == 0; i++)
        if (out->pid == tid) return i;
    return -1;
}

#define PARK_WINDOW_MS 300
#define PARK_CPU_MAX_NS (30ull * 1000 * 1000)   // a tenth of the window

// Starts `fn` on a lock the caller holds, and reports whether it sat
// parked through the window. The caller then releases and joins.
static int parks(void *(*fn)(void *), void *arg, pthread_t *t, const char *what) {
    g_park_tid = 0;
    g_park_got = 0;
    if (pthread_create(t, NULL, fn, arg) != 0) {
        utest_checkf(0, "a waiter on a held %s started", what);
        return 0;
    }
    while (!g_park_tid) sys_sleep_ms(1);
    sys_sleep_ms(50);                       // into the lock and parked
    struct proc_info a, b;
    int slot = slot_of(g_park_tid, &a);
    sys_sleep_ms(PARK_WINDOW_MS);
    int ok = slot >= 0 && sys_proc_info(slot, &b) == 0 && b.pid == g_park_tid;
    uint64_t used = ok ? b.cpu_ns - a.cpu_ns : 0;
    utest_checkf(ok && b.state == PROC_STATE_BLOCKED && b.wait_reason == PROC_WAIT_FUTEX &&
                 !g_park_got && used < PARK_CPU_MAX_NS,
                 "a waiter on a held %s is parked on a futex (state %u, wait %u, "
                 "%llu us of CPU in %d ms)", what, ok ? b.state : 0u, ok ? b.wait_reason : 0u,
                 (unsigned long long)(used / 1000), PARK_WINDOW_MS);
    return 1;
}

static void *forever(void *arg) {
    (void)arg;
    for (;;) sys_yield();
}

#define MANY 6

// --- the stack's guard page -------------------------------------------
//
// Two halves, because each alone passes a broken version: a stack in the
// heap ALSO dies of SIGSEGV eventually (the recursion walks down until it
// reaches something unmapped), so the map is read too -- a PROT_NONE page
// directly below the region holding the thread's stack.
static uint64_t anon_region_of(uint64_t va, uint64_t *base, uint64_t *prot) {
    struct query_procmap q;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_PROCMAP, i, &q, sizeof q) < (int)sizeof q) return 0;
        if ((int)q.pid == sys_getpid() && q.kind == QUERY_PROCMAP_ANON &&
            va >= q.base && va < q.base + q.bytes) {
            *base = q.base;
            *prot = q.prot;
            return q.bytes;
        }
    }
}

// Holds its stack until told: a joined thread's stack is unmapped.
static volatile uint64_t g_stack_va;
static volatile int g_stack_release;
static void *stack_reporter(void *arg) {
    (void)arg;
    volatile int here = 0;
    g_stack_va = (uint64_t)(uintptr_t)&here;
    while (!g_stack_release) sys_yield();
    return NULL;
}

// NOT INLINED: GCC folds a few levels into one frame otherwise, and a
// frame past 4 KiB steps over the guard instead of onto it.
static volatile int g_depth_cap = 1 << 30;   // never reached; quiets -Winfinite-recursion
__attribute__((noinline)) static int recurse(int n) {
    volatile char frame[512];
    frame[0] = (char)n;
    if (n >= g_depth_cap) return n;
    return recurse(n + 1) + frame[0];
}
static void *overflower(void *arg) { (void)arg; return (void *)(intptr_t)recurse(0); }

static void guard_checks(void) {
    pthread_t t;
    int made = pthread_create(&t, NULL, stack_reporter, NULL) == 0;
    while (made && !g_stack_va) sys_yield();
    uint64_t sb = 0, sp = 0, gb = 0, gp = 99;
    uint64_t sbytes = g_stack_va ? anon_region_of(g_stack_va, &sb, &sp) : 0;
    uint64_t gbytes = sbytes ? anon_region_of(sb - 4096, &gb, &gp) : 0;
    g_stack_release = 1;
    if (made) pthread_join(t, NULL);
    uint64_t gone_b, gone_p;
    utest_check(made && !anon_region_of(g_stack_va, &gone_b, &gone_p),
                "joined, the thread's stack mapping is gone");
    utest_check(sbytes && sp == (PROT_READ | PROT_WRITE),
                "a thread's stack is an RW anonymous mapping of its own");
    utest_check(gbytes == 4096 && gb == sb - 4096 && gp == PROT_NONE,
                "...with one PROT_NONE guard page directly below it");

    int pid = fork();
    if (pid == 0) {
        pthread_t o;
        if (pthread_create(&o, NULL, overflower, NULL) == 0) pthread_join(o, NULL);
        _exit(0);
    }
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    utest_checkf(code == SIGNAL_EXIT_BASE + SIGSEGV,
                 "a thread that overflows its stack faults on the guard (SIGSEGV; exit %d)", code);
}

int main(void) {
    utest_begin("thread_test", "threads, TLS and mutual exclusion", UTEST_VERDICT_FILE);

    // --- one thread, everything it shares and everything it does not --
    struct shared s;
    memset(&s, 0, sizeof s);
    int main_tid = sys_gettid();
    int main_pid = sys_getpid();

    // Give the main thread an errno FIRST, so a shared one would be
    // overwritten by the thread's failure below.
    sys_close(-2);
    int main_errno_before = sys_errno();

    pthread_t t;
    int rc = pthread_create(&t, NULL, worker, &s);
    utest_check(rc == 0, "pthread_create() started a thread");

    void *ret = NULL;
    utest_check(rc == 0 && pthread_join(t, &ret) == 0, "pthread_join() collected it");
    utest_check(ret == (void *)0xC0FFEE, "the thread's return value survived the join");
    utest_check(s.ran == 1, "the thread ran");
    // THE ONE A FORK WOULD FAIL: `s` lives on main's stack and the
    // thread wrote through a pointer to it.
    utest_check(s.stack_probe != NULL, "it wrote through the parent's pointer -- ONE address space");
    utest_check(s.tid != 0 && s.tid != main_tid, "gettid() differs between the two threads");
    utest_check(s.pid == main_pid, "getpid() is the SAME in both -- a thread is not a process");
    utest_check(s.stack_probe != (void *)&s, "its locals are on a different stack");

    utest_check(t_local == 11, "the main thread's __thread variable is untouched");
    utest_check(t_local_from_thread == 22, "the thread saw its OWN copy of it");
    utest_check(s.errno_seen != 0, "the thread's failing call set an errno");
    // THE TLS CHECK THAT MATTERS: two threads, two failures, two
    // reasons, neither overwritten by the other.
    utest_check(sys_errno() == main_errno_before,
          "the main thread's errno survived a failure in another thread");

    // --- several threads at once ---------------------------------------
    struct shared many[MANY];
    pthread_t ts[MANY];
    memset(many, 0, sizeof many);
    int started = 0;
    for (int i = 0; i < MANY; i++)
        if (pthread_create(&ts[i], NULL, worker, &many[i]) == 0) started++;
    utest_check(started == MANY, "six more threads started at once");

    int joined = 0, all_ran = 1, distinct_stacks = 1;
    for (int i = 0; i < started; i++) {
        void *r = NULL;
        if (pthread_join(ts[i], &r) == 0) joined++;
        if (!many[i].ran) all_ran = 0;
        for (int j = 0; j < i; j++)
            if (many[i].stack_probe == many[j].stack_probe) distinct_stacks = 0;
    }
    utest_check(joined == MANY, "all six joined");
    utest_check(all_ran, "all six ran");
    utest_check(distinct_stacks, "each got a stack of its own");

    // --- mutual exclusion -----------------------------------------------
    pthread_t locks[3];
    int lockers = 0;
    for (int i = 0; i < 3; i++)
        if (pthread_create(&locks[i], NULL, section_worker, NULL) == 0) lockers++;
    for (int i = 0; i < lockers; i++) pthread_join(locks[i], NULL);
    utest_check(lockers == 3 && g_section_runs == 3 * 20, "three threads ran 60 critical sections");
    utest_check(!g_section_broken, "no thread entered a section another held ACROSS A YIELD");

    // --- a condition variable --------------------------------------------
    pthread_t cvt;
    if (pthread_create(&cvt, NULL, cv_worker, NULL) == 0) {
        pthread_mutex_lock(&g_cv_lock);
        while (!g_handed_over) pthread_cond_wait(&g_cv, &g_cv_lock);
        pthread_mutex_unlock(&g_cv_lock);
        pthread_join(cvt, NULL);
        utest_check(g_handed_over, "a condition variable carried a handover");
    } else {
        utest_check(0, "a condition variable carried a handover");
    }

    // --- detaching ---------------------------------------------------------
    pthread_t det;
    if (pthread_create(&det, NULL, detached_worker, NULL) == 0) {
        utest_check(pthread_detach(det) == 0, "a running thread can be detached");
        utest_check(pthread_join(det, NULL) != 0, "a detached thread refuses to be joined");
        for (int spins = 0; spins < 1000 && !g_detached_ran; spins++) sys_yield();
        utest_check(g_detached_ran, "the detached thread still ran");
    } else {
        utest_check(0, "a running thread can be detached");
    }

    pthread_t sse;
    if (pthread_create(&sse, NULL, sse_worker, NULL) == 0) {
        pthread_join(sse, NULL);
        // Reaching this at all is most of the assertion: a misaligned
        // stack faults rather than computing the wrong answer, and the
        // fault kills the whole process.
        utest_check(g_sse_ok, "a worker can use SSE -- its stack is aligned as SysV requires");
    } else {
        utest_check(0, "a worker can use SSE -- its stack is aligned as SysV requires");
    }

    utest_check(pthread_equal(pthread_self(), pthread_self()), "pthread_self() is stable");

    // POSIX: a thread starts with its creator's signal mask. Blocking a
    // signal around the create is how a program keeps it off a worker,
    // and a kernel that started every thread unmasked let it through.
    sigset_t intr, old;
    sigemptyset(&intr);
    sigaddset(&intr, SIGINT);
    sigprocmask(SIG_BLOCK, &intr, &old);
    pthread_t mt;
    int made = pthread_create(&mt, NULL, mask_worker, NULL) == 0;
    sigprocmask(SIG_SETMASK, &old, 0);
    if (made) pthread_join(mt, NULL);
    utest_check(made && g_mask_int == 1, "a thread starts with its creator's signal mask");

    {
        pthread_t st;
        int ok = pthread_create(&st, NULL, sig_worker, NULL) == 0;
        for (int i = 0; ok && i < 4000 && !g_worker_tid; i++) sys_yield();
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, 0);          // AFTER the worker exists
        if (ok && g_worker_tid) kill(g_worker_tid, SIGUSR1);
        if (ok) pthread_join(st, NULL);
        utest_check(ok && g_worker_done && g_sig_tid == g_worker_tid,
                    "a handler set after a thread started runs on that thread");
        uintptr_t d = g_sig_sp > g_worker_sp ? g_sig_sp - g_worker_sp : g_worker_sp - g_sig_sp;
        utest_check(g_sig_sp && d < 64 * 1024,
                    "...on the thread's own stack, not the main one");
        signal(SIGUSR1, SIG_DFL);
    }

    // --- what libstdc++'s thread layer (gthr-posix.h) needs ---------
    {
        pthread_mutexattr_t ma;
        pthread_mutex_t rm;
        pthread_mutexattr_init(&ma);
        utest_check(pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE) == 0 &&
                    pthread_mutex_init(&rm, &ma) == 0, "a RECURSIVE mutex from an attribute");
        int twice = pthread_mutex_lock(&rm) == 0 && pthread_mutex_lock(&rm) == 0 &&
                    pthread_mutex_trylock(&rm) == 0;
        utest_check(twice, "...its holder locks it three times");
        g_rm = &rm;
        pthread_t rt;
        int made_rt = pthread_create(&rt, NULL, rm_worker, NULL) == 0;
        if (made_rt) pthread_join(rt, NULL);
        utest_check(made_rt && g_rm_trylock == EBUSY && g_rm_unlock == EPERM,
                    "...another thread can neither take it nor unlock it");
        // Held after two unlocks of three: the other thread still cannot take it.
        int undone = pthread_mutex_unlock(&rm) == 0 && pthread_mutex_unlock(&rm) == 0;
        made_rt = undone && pthread_create(&rt, NULL, rm_worker, NULL) == 0;
        if (made_rt) pthread_join(rt, NULL);
        utest_check(made_rt && g_rm_trylock == EBUSY, "...two unlocks of three leave it held");
        utest_check(pthread_mutex_unlock(&rm) == 0 && pthread_mutex_unlock(&rm) == EPERM,
                    "...and the third frees it");
        static pthread_mutex_t srm = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
        utest_check(pthread_mutex_lock(&srm) == 0 && pthread_mutex_lock(&srm) == 0 &&
                    pthread_mutex_unlock(&srm) == 0 && pthread_mutex_unlock(&srm) == 0,
                    "PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP is recursive");
        pthread_mutex_t dm = PTHREAD_MUTEX_INITIALIZER;
        utest_check(pthread_mutex_lock(&dm) == 0 && pthread_mutex_lock(&dm) == EDEADLK &&
                    pthread_mutex_unlock(&dm) == 0, "the default mutex still refuses a relock");
    }
    {
        utest_check(pthread_key_create(&g_key, key_dtor) == 0, "pthread_key_create");
        static int mine = 1;
        utest_check(pthread_getspecific(g_key) == NULL && pthread_setspecific(g_key, &mine) == 0 &&
                    pthread_getspecific(g_key) == &mine, "a value set reads back in this thread");
        pthread_t kt;
        int made_kt = pthread_create(&kt, NULL, key_worker, NULL) == 0;
        if (made_kt) pthread_join(kt, NULL);
        utest_check(made_kt && g_key_saw_null, "...and not in another, which starts at NULL");
        utest_check(g_key_dtor_arg == &g_key_thread_value && g_key_dtor_runs == 1,
                    "the destructor runs once, with the thread's value, when the thread ends");
        utest_check(pthread_getspecific(g_key) == &mine, "...leaving this thread's value alone");
        pthread_key_t k2;
        utest_check(pthread_key_delete(g_key) == 0 && pthread_key_create(&k2, NULL) == 0 &&
                    k2 != g_key && pthread_getspecific(k2) == NULL,
                    "a deleted key's slot is not handed out again");
    }
    {
        pthread_t ct;
        void *ret = NULL;
        int made_ct = pthread_create(&ct, NULL, cancel_spinner, NULL) == 0;
        utest_check(made_ct && pthread_cancel(ct) == 0 && pthread_join(ct, &ret) == 0 &&
                    ret == PTHREAD_CANCELED, "pthread_cancel ends a thread at pthread_testcancel()");
        utest_check(g_cancel_cleanups == 1, "...running its cleanup handler once");
        made_ct = pthread_create(&ct, NULL, cancel_waiter, NULL) == 0;
        for (int i = 0; made_ct && i < 50; i++) sys_yield();   // let it reach the wait
        ret = NULL;
        utest_check(made_ct && pthread_cancel(ct) == 0 && pthread_join(ct, &ret) == 0 &&
                    ret == PTHREAD_CANCELED, "...and one waiting in pthread_cond_wait()");
        utest_check(g_cancel_held_mutex == 1, "...which holds the mutex again for its handler");
    }
    {
        pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
        utest_check(pthread_rwlock_rdlock(&rw) == 0 && pthread_rwlock_tryrdlock(&rw) == 0,
                    "a read lock is shared");
        utest_check(pthread_rwlock_trywrlock(&rw) == EBUSY, "...and keeps a writer out");
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 20 * 1000 * 1000;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        utest_check(pthread_rwlock_timedwrlock(&rw, &dl) == ETIMEDOUT,
                    "...for as long as a timed writer is willing to wait");
        utest_check(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == 0 &&
                    pthread_rwlock_wrlock(&rw) == 0, "the write lock once both readers leave");
        g_rw = &rw;
        pthread_t wt;
        int made_wt = pthread_create(&wt, NULL, rw_worker, NULL) == 0;
        if (made_wt) pthread_join(wt, NULL);
        utest_check(made_wt && g_rw_tryrd == EBUSY, "...which keeps readers out");
        utest_check(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == EPERM,
                    "...until it is unlocked, once");
    }
    {
        pthread_mutex_t tm = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t tc = PTHREAD_COND_INITIALIZER;
        struct timespec t0, dl, t1;
        clock_gettime(CLOCK_REALTIME, &t0);
        dl = t0;
        dl.tv_nsec += 50 * 1000 * 1000;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&tm);
        int r = pthread_cond_timedwait(&tc, &tm, &dl);
        clock_gettime(CLOCK_REALTIME, &t1);
        int64_t waited = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
        utest_checkf(r == ETIMEDOUT && waited >= 50 * 1000 * 1000,
                     "pthread_cond_timedwait times out after its deadline (%d, %lld ms)",
                     r, (long long)(waited / 1000000));
        utest_check(pthread_mutex_unlock(&tm) == 0,
                    "...holding the mutex again");
    }

    {
        pthread_t t;
        pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m);
        if (parks(park_mutex, &m, &t, "mutex")) {
            pthread_mutex_unlock(&m);
            pthread_join(t, NULL);
            utest_check(g_park_got, "...and takes the mutex once it is released");
        }

        pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
        pthread_rwlock_rdlock(&rw);
        if (parks(park_wrlock, &rw, &t, "read-write lock")) {
            pthread_rwlock_unlock(&rw);
            pthread_join(t, NULL);
            utest_check(g_park_got, "...and the writer gets in once the reader leaves");
        }

        flockfile(stdout);
        if (parks(park_stream, stdout, &t, "stdio stream")) {
            funlockfile(stdout);
            pthread_join(t, NULL);
            utest_check(g_park_got, "...and locks the stream once it is released");
        }
    }

    guard_checks();

    // A THREAD LEFT RUNNING AT EXIT, on purpose and never joined.
    // Without it nothing here reaches the group teardown at all: every
    // other thread above is joined or detached, so it is already gone
    // by the time this process dies, and removing the teardown
    // entirely changes no result. (Measured -- it reddened nothing.)
    // kernel/proc/thread_test.c is what looks, from outside, for the
    // slot this must not leave behind.
    pthread_t spinner;
    utest_check(pthread_create(&spinner, NULL, forever, NULL) == 0,
          "a thread the process will not wait for is running at exit");

    return utest_end();
}
