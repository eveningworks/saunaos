// A THREAD'S DELAY TOTALS SAY WHERE ITS WAITING WENT -- Linux's delayacct.
//
// abi/proc_info.h's delay_disk_ns, delay_lock_ns and delay_runq_ns:
// time parked mid-call on a disk transfer, on a kernel lock, and READY
// with the CPU elsewhere. A wait the program chose -- a sleep, a record
// lock it asked to wait for -- counts in none of them. Read through
// SYS_PROC_INFO's PROC_INFO_SELF.
//
//   self    the call answers for this thread
//   disk    writes and an fsync park on the disk, and disk_ns grows
//   runq    yielding to a spinning thread is READY time, and runq_ns grows
//   sleep   300 ms asleep grows none of the three by more than a little
//   flock   300 ms waiting on another process's record lock is not a
//           kernel lock: lock_ns does not grow by it
//
// A KERNEL lock wait (a mount's) is not forced here -- no fixture holds
// one for a known time -- so lock_ns's positive case is a NOTE, not a
// check.
//
// It must be SPAWNED, not `run` -- it runs a thread and a child.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "base/utest.h"

#define SELF "/tests/delay_test"
#define LOCKFILE "/var/tmp/delay_test.lock"
#define DATAFILE "/var/tmp/delay_test.data"
#define HOLD_MS 300
#define SLACK_MS 50   // what a 300 ms wait by choice may still add, at most

static struct proc_info self(void) {
    struct proc_info pi;
    memset(&pi, 0, sizeof pi);
    sys_proc_info(PROC_INFO_SELF, &pi);
    return pi;
}

static unsigned long long ms(uint64_t ns) { return (unsigned long long)(ns / 1000000ull); }

static volatile int g_spin_stop;
static void *spin(void *arg) { (void)arg; while (!g_spin_stop) {} return 0; }

// The child: holds a write lock on LOCKFILE for HOLD_MS, then exits.
static int lock_holder(void) {
    int fd = open(LOCKFILE, O_RDWR | O_CREAT);
    struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    if (fd < 0 || fcntl(fd, F_SETLK, &fl) != 0) return 1;
    sys_sleep_ms(HOLD_MS);
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--hold-lock")) return lock_holder();
    utest_begin("delay_test", "delay totals say where waiting went", UTEST_VERDICT_FILE);

    struct proc_info a = self();
    utest_checkf(a.pid == sys_gettid() && !strcmp(a.name, "delay_test"),
                 "PROC_INFO_SELF answers for this thread (pid %d, '%s')", (int)a.pid, a.name);

    // disk: 2 MiB written and made durable.
    static char block[64 * 1024];
    memset(block, 'd', sizeof block);
    int fd = open(DATAFILE, O_WRONLY | O_CREAT | O_TRUNC);
    int wrote = fd >= 0;
    for (int i = 0; wrote && i < 32; i++) wrote = write(fd, block, sizeof block) == (long)sizeof block;
    if (fd >= 0) { wrote = wrote && fsync(fd) == 0; close(fd); }
    unlink(DATAFILE);
    struct proc_info b = self();
    utest_check(wrote, "wrote and synced 2 MiB");
    utest_checkf(b.delay_disk_ns > a.delay_disk_ns, "disk: the writes parked on the disk "
                 "(+%llu ms)", ms(b.delay_disk_ns - a.delay_disk_ns));

    // runq: give the CPU to a spinner and wait for it to come back.
    pthread_t t;
    int spinning = pthread_create(&t, NULL, spin, NULL) == 0;
    struct proc_info c0 = self();
    for (int i = 0; spinning && i < 20; i++) sys_yield();
    struct proc_info c1 = self();
    g_spin_stop = 1;
    if (spinning) pthread_join(t, NULL);
    utest_checkf(spinning && c1.delay_runq_ns > c0.delay_runq_ns,
                 "runq: yielding to a spinning thread is time waiting for the CPU (+%llu ms)",
                 ms(c1.delay_runq_ns - c0.delay_runq_ns));

    // sleep: a wait by choice.
    struct proc_info d0 = self();
    sys_sleep_ms(HOLD_MS);
    struct proc_info d1 = self();
    uint64_t grew = (d1.delay_disk_ns - d0.delay_disk_ns) + (d1.delay_lock_ns - d0.delay_lock_ns) +
                    (d1.delay_runq_ns - d0.delay_runq_ns);
    utest_checkf(ms(grew) < SLACK_MS, "sleep: %d ms asleep adds %llu ms of delay (under %d)",
                 HOLD_MS, ms(grew), SLACK_MS);

    // flock: wait on another process's record lock.
    int holder = sys_spawn(SELF, "--hold-lock", -1);
    int lfd = open(LOCKFILE, O_RDWR | O_CREAT);
    struct flock probe = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    int held = 0;
    for (int i = 0; holder > 0 && lfd >= 0 && i < 200 && !held; i++) {   // until the child has it
        probe.l_type = F_WRLCK;
        held = fcntl(lfd, F_GETLK, &probe) == 0 && probe.l_type != F_UNLCK;
        if (!held) sys_sleep_ms(5);
    }
    struct proc_info e0 = self();
    uint64_t t0 = sys_monotonic_ns();
    struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    int got = held && fcntl(lfd, F_SETLKW, &fl) == 0;
    uint64_t waited = sys_monotonic_ns() - t0;
    struct proc_info e1 = self();
    int code = -1;
    if (holder > 0) sys_waitpid(holder, &code);
    if (lfd >= 0) close(lfd);
    unlink(LOCKFILE);
    utest_checkf(got && ms(waited) >= HOLD_MS / 2,
                 "flock: waited %llu ms for the child's record lock", ms(waited));
    utest_checkf(ms(e1.delay_lock_ns - e0.delay_lock_ns) < SLACK_MS,
                 "flock: a record lock is not a kernel lock (+%llu ms of lock delay)",
                 ms(e1.delay_lock_ns - e0.delay_lock_ns));

    struct proc_info z = self();
    utest_notef("totals: disk %llu ms, lock %llu ms, waiting for the CPU %llu ms",
                ms(z.delay_disk_ns), ms(z.delay_lock_ns), ms(z.delay_runq_ns));
    return utest_end();
}
