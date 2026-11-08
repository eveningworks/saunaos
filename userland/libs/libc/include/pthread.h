#ifndef PTHREAD_H
#define PTHREAD_H

// POSIX threads, over the kernel's four thread syscalls.
//
// **A THREAD SHARES EVERYTHING EXCEPT ITS STACK AND ITS TLS.** Same
// memory, same file descriptors, same cwd, same pid: getpid() answers
// the same value in every thread, and sys_gettid() (rt/sys.h) is what
// tells them apart.
//
// What this implementation does NOT do, stated rather than discovered:
//
//   - **A THREAD STACK HAS NO GUARD PAGE.** It is ordinary malloc'd
//     memory, so an overrun quietly walks into another allocation
//     instead of faulting. The kernel's own stacks have guard pages;
//     giving one to a thread needs an mprotect this system has no
//     syscall for (docs/roadmap.md).
//   - **A DETACHED THREAD'S STACK IS NOT RECLAIMED** until the process
//     exits -- nothing can safely free memory a dying thread is still
//     standing on, which is what Linux's CLONE_CHILD_CLEARTID futex
//     wake exists to solve. Join a thread if you want its memory back.
//   - **A MUTEX SPINS AND YIELDS** rather than blocking: the futex it
//     could park on exists, and this has not been moved onto it.
//     Correct under this preemptive scheduler, and it costs the waiter
//     its timeslice.
//   - **CANCELLATION IS DEFERRED, AND HONOURED IN THREE PLACES ONLY**:
//     pthread_testcancel(), pthread_cond_wait() and
//     pthread_cond_timedwait(). The blocking calls POSIX also makes
//     cancellation points (read, nanosleep, pthread_join...) are not
//     ones here, and asynchronous cancellation does not exist, so
//     PTHREAD_CANCEL_ASYNCHRONOUS is not defined -- code asking for it
//     fails to compile rather than running uncancellable.
//   - No scheduling attributes, no barriers, and no
//     pthread_mutex_timedlock (so <unistd.h> does not claim
//     _POSIX_TIMEOUTS).
//
// pthread_exit() from main() exits the PROCESS here, not just the
// initial thread -- see SYS_THREAD_EXIT in abi/syscall_abi.h.

#include <stdint.h>
#include <stddef.h>
#include <time.h>   // struct timespec, clockid_t, CLOCK_REALTIME
#include <sched.h>  // sched_yield(): POSIX has <pthread.h> make <sched.h> visible
#ifdef __cplusplus
extern "C" {
#endif

struct pthread;
typedef struct pthread *pthread_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    size_t stacksize;   // 0 means the default
    int    detachstate; // PTHREAD_CREATE_*
} pthread_attr_t;

// A MUTEX'S TYPE IS ITS ATTRIBUTE, and the default is ERROR-CHECKING:
// a relock by the holder answers EDEADLK rather than hanging, which is
// what POSIX allows for PTHREAD_MUTEX_DEFAULT. NORMAL is the same here
// (POSIX would have it deadlock; nothing wants that). A RECURSIVE mutex
// counts its holder's locks; an ERRORCHECK or RECURSIVE one refuses an
// unlock from a thread that does not hold it (EPERM).
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

typedef struct {
    int type;           // PTHREAD_MUTEX_*
} pthread_mutexattr_t;

typedef struct {
    volatile int locked;
    int          owner;  // tid, for the recursion check; 0 when free
    int          type;   // PTHREAD_MUTEX_*
    int          count;  // a RECURSIVE mutex's depth
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, PTHREAD_MUTEX_NORMAL, 0 }
// glibc's name for a statically initialised recursive mutex, which
// libgcc's and libstdc++'s thread layer (gthr-posix.h) use when present.
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, 0, PTHREAD_MUTEX_RECURSIVE, 0 }

typedef struct {
    clockid_t clock;    // what pthread_cond_timedwait()'s deadline is on
} pthread_condattr_t;

typedef struct {
    volatile unsigned generation;
    clockid_t         clock;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0, CLOCK_REALTIME }

typedef struct {
    volatile int done;
} pthread_once_t;

#define PTHREAD_ONCE_INIT { 0 }

// READ-WRITE LOCKS, WRITER-PREFERRING: once a writer waits, new readers
// wait behind it, so a stream of readers cannot starve it. A thread
// that read-locks again while a writer waits therefore waits on itself
// -- POSIX leaves that case to the implementation, and glibc's
// writer-preferring kind does the same. Spin-and-yield, like a mutex.
typedef struct {
    volatile int lock;      // guards the three counts below
    int readers;            // holding a read lock
    int writer;             // the tid holding the write lock, or 0
    int writers_waiting;
} pthread_rwlock_t;

typedef struct { int unused; } pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, 0, 0 }

// THREAD-SPECIFIC DATA. A key is an index that is never reused, so a
// value set under a deleted key can never be read back under a new one;
// each thread's values grow to fit the keys it has set. At a thread's
// exit each non-NULL value's destructor runs, up to
// PTHREAD_DESTRUCTOR_ITERATIONS rounds while destructors set more.
// The initial thread's run only through pthread_exit(), as POSIX says.
typedef unsigned pthread_key_t;
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

// The default stack for a thread, in bytes. Fixed and ungrowable, where
// the initial thread's grows on fault -- so a thread that recurses
// deeply needs pthread_attr_setstacksize().
#define PTHREAD_STACK_DEFAULT (64 * 1024)
#define PTHREAD_STACK_MIN     (16 * 1024)

int  pthread_create(pthread_t *out, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg);
int  pthread_join(pthread_t t, void **retval);
int  pthread_detach(pthread_t t);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);

int  pthread_attr_init(pthread_attr_t *a);
int  pthread_attr_destroy(pthread_attr_t *a);
int  pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int  pthread_attr_getstacksize(const pthread_attr_t *a, size_t *out);
int  pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int  pthread_attr_getdetachstate(const pthread_attr_t *a, int *out);

int  pthread_mutexattr_init(pthread_mutexattr_t *a);
int  pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int  pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int  pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);

int  pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int  pthread_mutex_destroy(pthread_mutex_t *m);
int  pthread_mutex_lock(pthread_mutex_t *m);
int  pthread_mutex_trylock(pthread_mutex_t *m);
int  pthread_mutex_unlock(pthread_mutex_t *m);

int  pthread_condattr_init(pthread_condattr_t *a);
int  pthread_condattr_destroy(pthread_condattr_t *a);
int  pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock);
int  pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock);

int  pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int  pthread_cond_destroy(pthread_cond_t *c);
int  pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
// ETIMEDOUT once `abstime`, on the condition's clock (CLOCK_REALTIME
// unless its attribute said otherwise), has passed.
int  pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime);
int  pthread_cond_signal(pthread_cond_t *c);
int  pthread_cond_broadcast(pthread_cond_t *c);

int  pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int  pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);
int  pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr);
int  pthread_rwlock_destroy(pthread_rwlock_t *l);
int  pthread_rwlock_rdlock(pthread_rwlock_t *l);
int  pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int  pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *abstime);
int  pthread_rwlock_wrlock(pthread_rwlock_t *l);
int  pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int  pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *abstime);
int  pthread_rwlock_unlock(pthread_rwlock_t *l);

// --- cancellation (see the top of this file for where it is honoured)
#define PTHREAD_CANCELED        ((void *)-1)
#define PTHREAD_CANCEL_ENABLE   0
#define PTHREAD_CANCEL_DISABLE  1
#define PTHREAD_CANCEL_DEFERRED 0

int  pthread_cancel(pthread_t t);
int  pthread_setcancelstate(int state, int *oldstate);
int  pthread_setcanceltype(int type, int *oldtype);   // DEFERRED only
void pthread_testcancel(void);

// Cleanup handlers, run newest first by pthread_exit() -- and so by a
// cancellation -- or by pthread_cleanup_pop(1). The two macros open
// and close one block, as POSIX allows, so they must pair in one scope.
struct __pthread_cleanup {
    void (*fn)(void *);
    void *arg;
    struct __pthread_cleanup *prev;
};
void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute);
#define pthread_cleanup_push(fn, arg) \
    do { struct __pthread_cleanup __pthread_c; __pthread_cleanup_push(&__pthread_c, (fn), (arg));
#define pthread_cleanup_pop(execute) \
    __pthread_cleanup_pop(&__pthread_c, (execute)); } while (0)

int  pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int  pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int  pthread_setspecific(pthread_key_t key, const void *value);

int  pthread_once(pthread_once_t *once, void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif
