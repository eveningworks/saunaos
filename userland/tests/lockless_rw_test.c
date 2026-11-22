// read() and write() without the kernel lock (docs/smp-design.md, stage 7):
// a pipe answered, parked and woken by MPSAFE handlers on any core.
//
// What a broken version would still pass, and the check that catches it:
//   - the rows flagged but the fast path never taken: QUERY_BKLSTAT's
//     read and write rows must count a pipe's calls as LOCKLESS, and with
//     kernel.lockless_syscalls off as TAKES (in the same boot);
//   - a lost wake between a waiter's test and its park, on either end:
//     two threads ping-pong numbered bytes through two pipes, each end
//     parking on every round, and a lost wake is a hang -- bounded here by
//     a watchdog thread, so it fails a named check rather than the
//     runner's timeout;
//   - two writers interleaving in the buffer (no pipe lock): writers on
//     several cores send whole tagged records through one pipe, and the
//     reader checks each record is one writer's, in that writer's order;
//   - a lock-free park left parked after its other end went away: a
//     reader parked on an empty pipe must see EOF when the last writer
//     closes, and a writer parked on a full one EPIPE when the reader does.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "syscall_abi.h"
#include "base/utest.h"
#include "base/tunable.h"

#define TUNABLE "kernel.lockless_syscalls"
#define CALLS 5000
#define STALL_MS 20000

// Every check bumps g_progress as it goes; a stall past STALL_MS is a lost
// wake (both ends parked for good), named and ended here.
static volatile unsigned g_progress;
static const char *volatile g_phase = "start";
static volatile int g_all_done;

// Cores scheduling processes: a park goes lock-free only on an AP.
static int running_cores(void) {
    struct query_cpustate q;
    int n = 0;
    QUERY_FOREACH(QUERY_CPUSTATE, q, i) n += q.state == QUERY_CPUSTATE_RUNNING;
    return n;
}

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static void *watchdog(void *arg) {
    (void)arg;
    unsigned seen = g_progress;
    unsigned long long since = now_ms();
    while (!g_all_done) {
        sys_sleep_ms(100);
        if (g_progress != seen) { seen = g_progress; since = now_ms(); continue; }
        if (now_ms() - since < STALL_MS) continue;
        utest_checkf(0, "%s makes progress: stuck %llu ms (a lost wake?)", g_phase, now_ms() - since);
        exit(utest_end());
    }
    return 0;
}

static void syscall_row(uint64_t nr, uint64_t *takes, uint64_t *lockless) {
    struct query_bklstat r;
    *takes = *lockless = 0;
    QUERY_FOREACH(QUERY_BKLSTAT, r, i) {
        if (r.kind == QUERY_BKLSTAT_SYSCALL && r.id == nr) {
            *takes = r.takes;
            *lockless = r.lockless;
            return;
        }
    }
}

// One byte through one pipe, CALLS times: neither end ever waits.
static void pipe_rounds(int p[2]) {
    char b = 'x';
    for (int i = 0; i < CALLS; i++) {
        (void)sys_write(p[1], &b, 1);
        (void)sys_read(p[0], &b, 1);
    }
}

static void check_counted(void) {
    int p[2];
    if (pipe(p) != 0) { utest_check(0, "a pipe opens"); return; }
    uint64_t rt0, rl0, wt0, wl0, rt1, rl1, wt1, wl1;
    char d[128];

    utest_check(tunable_set(TUNABLE, "on"), "kernel.lockless_syscalls can be turned on");
    syscall_row(SYS_READ, &rt0, &rl0);
    syscall_row(SYS_WRITE, &wt0, &wl0);
    pipe_rounds(p);
    syscall_row(SYS_READ, &rt1, &rl1);
    syscall_row(SYS_WRITE, &wt1, &wl1);
    snprintf(d, sizeof d, "read lockless +%llu takes +%llu, write lockless +%llu takes +%llu",
             (unsigned long long)(rl1 - rl0), (unsigned long long)(rt1 - rt0),
             (unsigned long long)(wl1 - wl0), (unsigned long long)(wt1 - wt0));
    utest_check_detail(rl1 - rl0 >= CALLS && wl1 - wl0 >= CALLS,
                       "a pipe is read and written without the kernel lock", d);
    utest_check_detail(rt1 - rt0 < CALLS / 10 && wt1 - wt0 < CALLS / 10,
                       "...and neither call takes it", d);

    // The positive control: the same calls through the lock.
    utest_check(tunable_set(TUNABLE, "off"), "kernel.lockless_syscalls can be turned off");
    syscall_row(SYS_READ, &rt0, &rl0);
    syscall_row(SYS_WRITE, &wt0, &wl0);
    pipe_rounds(p);
    syscall_row(SYS_READ, &rt1, &rl1);
    syscall_row(SYS_WRITE, &wt1, &wl1);
    tunable_set(TUNABLE, "on");
    snprintf(d, sizeof d, "read lockless +%llu takes +%llu, write lockless +%llu takes +%llu",
             (unsigned long long)(rl1 - rl0), (unsigned long long)(rt1 - rt0),
             (unsigned long long)(wl1 - wl0), (unsigned long long)(wt1 - wt0));
    utest_check_detail(rt1 - rt0 >= CALLS && wt1 - wt0 >= CALLS,
                       "turned off, both take the kernel lock again", d);
    close(p[0]);
    close(p[1]);
}

// --- ping-pong: every round parks one end or the other ----------------

#define PING_ROUNDS 20000
static int g_ab[2], g_ba[2];
static volatile int g_echo_wrong;

static void *echo(void *arg) {
    (void)arg;
    for (int i = 0; i < PING_ROUNDS; i++) {
        uint8_t b;
        if (read(g_ab[0], &b, 1) != 1) { g_echo_wrong = 1; break; }
        if (b != (uint8_t)i) g_echo_wrong = 1;
        b = (uint8_t)(b + 1);
        if (write(g_ba[1], &b, 1) != 1) { g_echo_wrong = 1; break; }
    }
    return 0;
}

static void check_pingpong(void) {
    if (pipe(g_ab) != 0 || pipe(g_ba) != 0) { utest_check(0, "two pipes open"); return; }
    uint64_t t0, l0, t1, l1;
    syscall_row(SYS_READ, &t0, &l0);
    pthread_t th;
    if (pthread_create(&th, 0, echo, 0) != 0) { utest_check(0, "the echo thread starts"); return; }
    g_phase = "ping-pong";
    int wrong = 0, done = 0;
    for (int i = 0; i < PING_ROUNDS; i++) {
        uint8_t b = (uint8_t)i;
        if (write(g_ab[1], &b, 1) != 1) { wrong++; break; }
        if (read(g_ba[0], &b, 1) != 1) { wrong++; break; }
        if (b != (uint8_t)(i + 1)) wrong++;
        done++;
        g_progress++;
    }
    pthread_join(th, 0);
    syscall_row(SYS_READ, &t1, &l1);
    utest_checkf(done == PING_ROUNDS && !wrong && !g_echo_wrong,
                 "%d of %d ping-pong rounds came back, each byte the one sent (+1)", done, PING_ROUNDS);
    // Most rounds' reads park (a woken one asks again, so lock-free calls
    // outnumber reads). A park that declines takes the kernel lock -- about
    // one a round -- and a park on the boot core always does, so on one
    // core there is nothing to count.
    int cores = running_cores();
    if (cores > 1)
        utest_checkf(t1 - t0 < PING_ROUNDS / 2,
                     "on %d cores the reads parked lock-free too: %llu took the kernel lock, %llu did not",
                     cores, (unsigned long long)(t1 - t0), (unsigned long long)(l1 - l0));
    else
        utest_notef("one core: a park goes the ordinary way, so the lock-free count is not checked");
    for (int i = 0; i < 2; i++) { close(g_ab[i]); close(g_ba[i]); }
}

// --- several writers into one pipe ---------------------------------------

#define WRITERS 3
#define RECORDS 4000
#define RECORD 64   // under PIPE_BUF: each write is one atomic record
static int g_many[2];

struct record { uint32_t writer, seq; uint8_t fill[RECORD - 8]; };

static void *writer(void *arg) {
    uint32_t w = (uint32_t)(uintptr_t)arg;
    struct record r;
    for (uint32_t s = 0; s < RECORDS; s++) {
        r.writer = w;
        r.seq = s;
        memset(r.fill, (int)(w * 37 + s), sizeof r.fill);
        if (write(g_many[1], &r, sizeof r) != (ssize_t)sizeof r) break;
    }
    return 0;
}

static void check_many_writers(void) {
    if (pipe(g_many) != 0) { utest_check(0, "a pipe opens"); return; }
    pthread_t th[WRITERS];
    int made = 0;
    for (int i = 0; i < WRITERS; i++)
        made += pthread_create(&th[i], 0, writer, (void *)(uintptr_t)i) == 0;
    uint32_t next[WRITERS] = {0};
    int bad = 0, got = 0;
    g_phase = "several writers into one pipe";
    while (got < made * RECORDS) {
        struct record r;
        size_t have = 0;
        // A record is written whole; a read may still split it, so gather.
        while (have < sizeof r) {
            ssize_t n = read(g_many[0], (char *)&r + have, sizeof r - have);
            if (n <= 0) break;
            have += (size_t)n;
        }
        if (have != sizeof r) { bad++; break; }
        got++;
        g_progress++;
        if (r.writer >= WRITERS || r.seq != next[r.writer]) { bad++; continue; }
        next[r.writer]++;
        for (size_t k = 0; k < sizeof r.fill; k++)
            if (r.fill[k] != (uint8_t)(r.writer * 37 + r.seq)) { bad++; break; }
    }
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);
    utest_checkf(made == WRITERS && got == WRITERS * RECORDS,
                 "%d writers sent %d records through one pipe", made, got);
    utest_checkf(bad == 0, "every record whole, one writer's, in its order: %d were not", bad);
    close(g_many[0]);
    close(g_many[1]);
}

// --- a parked end, and the other end closing -------------------------------

static int g_eof[2];
static volatile ssize_t g_eof_got = -2;

static void *eof_reader(void *arg) {
    (void)arg;
    char b;
    g_eof_got = read(g_eof[0], &b, 1);
    return 0;
}

static int g_full[2];
static volatile ssize_t g_full_got = -2;
static volatile int g_full_errno;

static void *full_writer(void *arg) {
    (void)arg;
    static char chunk[512];
    for (;;) {
        ssize_t n = write(g_full[1], chunk, sizeof chunk);
        if (n < 0) { g_full_errno = errno; g_full_got = n; return 0; }
    }
}

static void wait_parked(void) {
    // Long enough for the other thread to reach its park; a thread that is
    // still running when the close comes tests the ordinary path instead.
    for (int i = 0; i < 50; i++) sys_yield();
    usleep(50000);
}

static void check_close_wakes(void) {
    if (pipe(g_eof) != 0 || pipe(g_full) != 0) { utest_check(0, "two pipes open"); return; }
    pthread_t a, b;
    g_phase = "a parked reader told of EOF";
    if (pthread_create(&a, 0, eof_reader, 0) != 0) { utest_check(0, "a reader starts"); return; }
    wait_parked();
    close(g_eof[1]);
    pthread_join(a, 0);
    utest_checkf(g_eof_got == 0, "a reader parked on an empty pipe reads EOF when the writer closes: %d",
                 (int)g_eof_got);
    g_progress++;
    g_phase = "a parked writer told of EPIPE";
    signal(SIGPIPE, SIG_IGN);
    if (pthread_create(&b, 0, full_writer, 0) != 0) { utest_check(0, "a writer starts"); return; }
    wait_parked();
    close(g_full[0]);
    pthread_join(b, 0);
    g_progress++;
    utest_checkf(g_full_got < 0 && g_full_errno == EPIPE,
                 "a writer parked on a full pipe fails EPIPE when the reader closes: %d, errno %d",
                 (int)g_full_got, g_full_errno);
    close(g_eof[0]);
    close(g_full[1]);
}

int main(void) {
    utest_begin("lockless_rw_test", "kernel/proc/pipe.c and syscall_fd.c's lock-free read/write",
                UTEST_VERDICT_FILE);
    char was[16] = "on";
    tunable_get(TUNABLE, was, sizeof was);
    pthread_t dog;
    utest_check(pthread_create(&dog, 0, watchdog, 0) == 0, "the watchdog starts");
    check_counted();
    tunable_set(TUNABLE, "on");
    check_pingpong();
    check_many_writers();
    check_close_wakes();
    g_all_done = 1;
    pthread_join(dog, 0);
    tunable_set(TUNABLE, was);
    return utest_end();
}
