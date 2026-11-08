#ifndef ULIB_SYS_RESOURCE_H
#define ULIB_SYS_RESOURCE_H

// Per-process resource limits. **RLIMIT_DATA IS THE ONE ENFORCED** --
// the heap plus private writable mmap()s, Linux's meaning since 4.7;
// past it malloc() returns NULL. Every other limit reads back as
// RLIM_INFINITY and setrlimit() refuses to lower it rather than
// accepting a number it will not enforce.
//
// That refusal is the interesting half. A setrlimit() that returned 0
// and did nothing would let a shell's `ulimit -f 100` print success and
// then write a 4 GB file -- the caller is told the limit is in force
// and has no way to discover otherwise. Answering EPERM is the same
// rule <termios.h> follows for VMIN/VTIME and <locale.h> for a locale
// that is not "C": refuse what cannot be honoured.

#include <sys/types.h>
#include <stdint.h>
#include <sys/time.h>   // struct timeval, for struct rusage
#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rlim_t;

#define RLIM_INFINITY  ((rlim_t)-1)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

struct rlimit {
    rlim_t rlim_cur;  // the soft limit
    rlim_t rlim_max;  // the ceiling the soft limit may be raised to
};

// The resources POSIX names. All but RLIMIT_DATA read RLIM_INFINITY;
// they exist so that code naming one compiles, and so a future limit
// has a number already allocated rather than shifting everything below
// it. The numbers are Linux's.
// setpriority()/getpriority(): nice, -20..19, LOWER gets MORE of the
// CPU -- a weight within SCHED_OTHER, never a rank (that is <sched.h>).
// Anyone may raise its own; only init may lower one, or touch another
// process (EPERM).
#define PRIO_PROCESS 0

int setpriority(int which, int who, int value);
// A NEGATIVE RESULT IS LEGITIMATE, so a caller that cares clears errno
// first and checks it -- POSIX's own rule for this call.
int getpriority(int which, int who);

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_AS      6
#define RLIMIT_NPROC   7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_NLIMITS 9

// Fills *rlp: RLIMIT_DATA's limits, RLIM_INFINITY for any other. Returns
// 0, or -1 with EINVAL for a resource number this system does not name.
int getrlimit(int resource, struct rlimit *rlp);

// RLIMIT_DATA: anyone may lower either limit, and raise `rlim_cur` up
// to `rlim_max`; raising `rlim_max` is init's alone (EPERM), and a
// `rlim_cur` above `rlim_max` is EINVAL. Inherited by spawn, fork and
// exec. Any other resource: 0 if the request leaves both fields
// RLIM_INFINITY -- setting a limit to the value it already has is not a
// change -- otherwise -1 with EPERM: this system cannot enforce it and
// will not say it has.
int setrlimit(int resource, const struct rlimit *rlp);

// getrusage(): the CALLER's processor time, in ru_utime. The kernel
// counts one CPU time per process, not a user/system split, so
// ru_stime is zero -- as is every field POSIX leaves to the
// implementation; they exist so code naming them compiles (GCC's
// -ftime-report reads ru_utime). RUSAGE_CHILDREN is REFUSED with
// EINVAL: a reaped child's time is not added to its parent here
// (<sys/times.h>'s tms_cutime), and zeroes would claim it used none --
// the reason wait3()/wait4() refuse their rusage too.
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN  (-1)
int getrusage(int who, struct rusage *ru);

#ifdef __cplusplus
}
#endif

#endif
