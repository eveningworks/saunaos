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
//   runq    spinning threads, two a core, spend READY time, and runq_ns grows
//   alone   spinning with nothing else to run is not: under a quarter of
//           the wall time waits for the CPU (the idle kernel once took
//           half, handing the CPU back to itself)
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
#define ALONE_MS 500

static struct proc_info self(void) {
    struct proc_info pi;
    memset(&pi, 0, sizeof pi);
    sys_proc_info(PROC_INFO_SELF, &pi);
    return pi;
}

static unsigned long long ms(uint64_t ns) { return (unsigned long long)(ns / 1000000ull); }

static volatile int g_spin_stop;
// Spins until told, then leaves the READY time it gathered meanwhile in *arg.
static void *spin(void *arg) {
    uint64_t r0 = self().delay_runq_ns;
    while (!g_spin_stop) {}
    *(uint64_t *)arg = self().delay_runq_ns - r0;
    return 0;
}

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

    // runq: more spinners than cores, TWO A CORE, for HOLD_MS -- every
    // core has one waiting, so the threads' own READY time grows. Asked of
    // the spinners, not of a thread yielding to one: with a run queue per
    // core a yielder can be the only thing on its core's queue, and then
    // its yield waits for nothing (a delay of 0 on four cores).
    long nspin = 2 * sysconf(_SC_NPROCESSORS_ONLN);
    if (nspin < 2) nspin = 2;
    pthread_t t[nspin];
    uint64_t spun_waits[nspin];
    long started = 0;
    while (started < nspin && pthread_create(&t[started], NULL, spin, &spun_waits[started]) == 0)
        started++;
    sys_sleep_ms(HOLD_MS);
    g_spin_stop = 1;
    uint64_t runq_sum = 0;
    for (long k = 0; k < started; k++) {
        pthread_join(t[k], NULL);
        runq_sum += spun_waits[k];
    }
    utest_checkf(started == nspin && runq_sum > 0,
                 "runq: %ld spinners on %ld cores waited for the CPU (+%llu ms)",
                 started, nspin / 2, ms(runq_sum));

    // alone: half a second of spinning, this thread the only one busy.
    struct proc_info s0 = self();
    uint64_t w0 = sys_monotonic_ns();
    while (sys_monotonic_ns() - w0 < ALONE_MS * 1000000ull) {}
    struct proc_info s1 = self();
    uint64_t wall = sys_monotonic_ns() - w0;
    uint64_t runq = s1.delay_runq_ns - s0.delay_runq_ns;
    utest_checkf(runq < wall / 4, "alone: %llu ms of %llu spent waiting for the CPU, %llu running (under a quarter)",
                 ms(runq), ms(wall), ms(s1.cpu_ns - s0.cpu_ns));

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
