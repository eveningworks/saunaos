// read() and write() without the kernel lock (docs/smp-design.md, stage 7):
// a pipe answered, parked and woken by MPSAFE handlers on any core, and a
// ramfs file read.
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
//
// And a ramfs file (/tmp) read the same way, trylocking the position and
// mount locks (fs_read_range_lockless()):
//   - the read row counts a /tmp file's reads as LOCKLESS, and a disk
//     file's (/var/tmp) and a read past the interrupts-off bound as TAKES;
//   - two readers sharing one description each move the position under
//     its lock: threads read one numbered word at a time from ONE fd, and
//     every word must arrive exactly once, whole (no position lock: a word
//     read twice, or torn);
//   - a mount and unmount beside the file while threads read it: every
//     read answers its whole word (the walk inside the mount table's
//     sequence count; a torn walk answers 0, which reads as EOF);
//   - and written: counted the same way, read back word for word;
//     writers sharing one description each take the next position, and
//     appenders with a description each land at the end -- every word
//     once, each writer's in its order (no position lock: words lost).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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

// --- a ramfs file -----------------------------------------------------------

#define RAM_FILE "/tmp/lockless_rw.dat"
#define DISK_FILE "/var/tmp/lockless_rw.dat"
#define CHURN_POINT "/tmp/lockless_rw.mnt"
#define WORDS 16384u          // 64 KiB of numbered words: word i holds i
#define BIG_READ (128u * 1024u)   // past the lock-free bound (fs_syscalls.c)
#define READERS 4

static int make_file(const char *path) {
    static uint32_t w[WORDS];
    for (uint32_t i = 0; i < WORDS; i++) w[i] = i;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    ssize_t n = write(fd, w, sizeof w);
    close(fd);
    return n == (ssize_t)sizeof w;
}

// CALLS 64-byte reads at numbered offsets, each checked: the count is
// only worth anything if the bytes are right too. A few may decline --
// another process holding /tmp's lock for a moment -- so the lock-free
// count allows a tenth to take the locked path. **The takes are NOT
// bounded**: the row counts every process's calls, and under the TCG gate
// other processes' writes added 1395 takes in one 5000-write window.
static int read_rounds(int fd, uint32_t len) {
    static char buf[BIG_READ];
    int bad = 0;
    for (int i = 0; i < CALLS; i++) {
        uint32_t word = (uint32_t)(i * 16) % WORDS;
        lseek(fd, (off_t)word * 4, SEEK_SET);
        ssize_t n = read(fd, buf, len);
        uint32_t got;
        memcpy(&got, buf, 4);
        if (n <= 0 || got != word) bad++;
    }
    return bad;
}

static void count_reads(const char *what, const char *path, uint32_t len, int want_lockless) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { utest_checkf(0, "%s opens", path); return; }
    uint64_t t0, l0, t1, l1;
    char d[128];
    syscall_row(SYS_READ, &t0, &l0);
    int bad = read_rounds(fd, len);
    syscall_row(SYS_READ, &t1, &l1);
    close(fd);
    snprintf(d, sizeof d, "read lockless +%llu takes +%llu, %d wrong",
             (unsigned long long)(l1 - l0), (unsigned long long)(t1 - t0), bad);
    if (want_lockless)
        utest_check_detail(!bad && l1 - l0 >= CALLS - CALLS / 10, what, d);
    else
        utest_check_detail(!bad && t1 - t0 >= CALLS, what, d);
}

// Whether `path` is on a ramfs: the longest mount point it is under. A
// machine with no disk has /var/tmp in RAM, and its "disk file" checks
// would then be testing nothing they name.
static int on_ramfs(const char *path) {
    struct query_fsinfo q;
    size_t best = 0;
    int ram = 0;
    QUERY_FOREACH(QUERY_FSINFO, q, i) {
        size_t n = strlen(q.point);
        int under = n == 1 || (strncmp(path, q.point, n) == 0 && (path[n] == '/' || !path[n]));
        if (!(q.flags & QUERY_FS_MOUNTED) || !under || n < best) continue;
        best = n;
        ram = strcmp(q.name, "ramfs") == 0;
    }
    return ram;
}

static void check_file_counted(void) {
    if (!make_file(RAM_FILE) || !make_file(DISK_FILE)) {
        utest_check(0, "the test files are written");
        return;
    }
    count_reads("a /tmp file is read without the kernel lock", RAM_FILE, 64, 1);
    int disk = !on_ramfs(DISK_FILE);
    if (disk) count_reads("a /var/tmp (disk) file's read takes the kernel lock", DISK_FILE, 64, 0);
    count_reads("a read past the lock-free bound takes the kernel lock", RAM_FILE, BIG_READ, 0);
    tunable_set(TUNABLE, "off");
    count_reads("turned off, a /tmp read takes the kernel lock again", RAM_FILE, 64, 0);
    tunable_set(TUNABLE, "on");
    unlink(DISK_FILE);
}

// One description, READERS threads: each read takes the next word.
static int g_shared_fd;
static unsigned char g_seen[WORDS];
static volatile unsigned g_torn;

static void *shared_reader(void *arg) {
    (void)arg;
    uint32_t w;
    for (;;) {
        ssize_t n = read(g_shared_fd, &w, 4);
        if (n != 4) return 0;
        if (w >= WORDS) { __atomic_fetch_add(&g_torn, 1, __ATOMIC_RELAXED); continue; }
        __atomic_fetch_add(&g_seen[w], 1, __ATOMIC_RELAXED);
        g_progress++;
    }
}

static void check_shared_position(void) {
    g_phase = "readers sharing one description";
    unsigned twice = 0, missed = 0;
    for (int round = 0; round < 4; round++) {
        memset(g_seen, 0, sizeof g_seen);
        g_shared_fd = open(RAM_FILE, O_RDONLY);
        if (g_shared_fd < 0) { utest_check(0, "the shared file opens"); return; }
        pthread_t t[READERS];
        for (int i = 0; i < READERS; i++) pthread_create(&t[i], 0, shared_reader, 0);
        for (int i = 0; i < READERS; i++) pthread_join(t[i], 0);
        close(g_shared_fd);
        for (uint32_t i = 0; i < WORDS; i++) {
            if (g_seen[i] > 1) twice++;
            if (g_seen[i] == 0) missed++;
        }
    }
    utest_checkf(!twice && !missed && !g_torn,
                 "readers sharing one description each get the next word: "
                 "%u read twice, %u missed, %u torn", twice, missed, g_torn);
}

// --- writes ------------------------------------------------------------------

#define W_FILE "/tmp/lockless_rw.w"
#define W_EACH 4000u   // words per writer

static void count_writes(const char *what, const char *path, int want_lockless) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { utest_checkf(0, "%s opens for writing", path); return; }
    uint64_t t0, l0, t1, l1;
    char d[128];
    int bad = 0;
    syscall_row(SYS_WRITE, &t0, &l0);
    for (uint32_t i = 0; i < CALLS; i++)
        if (write(fd, &i, 4) != 4) bad++;
    syscall_row(SYS_WRITE, &t1, &l1);
    close(fd);
    // Read back through the other path: every word where it was written.
    fd = open(path, O_RDONLY);
    for (uint32_t i = 0, w; fd >= 0 && i < CALLS; i++)
        if (read(fd, &w, 4) != 4 || w != i) bad++;
    if (fd >= 0) close(fd);
    snprintf(d, sizeof d, "write lockless +%llu takes +%llu, %d wrong",
             (unsigned long long)(l1 - l0), (unsigned long long)(t1 - t0), bad);
    if (want_lockless)
        utest_check_detail(!bad && l1 - l0 >= CALLS - CALLS / 10, what, d);
    else
        utest_check_detail(!bad && t1 - t0 >= CALLS, what, d);
    unlink(path);
}

// A word written 128 MiB in: its chunk table would grow past what a
// lock-free write may grow with interrupts off, so ramfs vetoes it and the
// locked path writes it (ramfs_lockless_write_ok()).
static void check_far_write(void) {
    int fd = open(W_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { utest_check(0, "the far write's file opens"); return; }
    uint64_t t0, l0, t1, l1;
    uint32_t w = 0x5aa5f00d, back = 0;
    lseek(fd, 128ll << 20, SEEK_SET);
    syscall_row(SYS_WRITE, &t0, &l0);
    ssize_t n = write(fd, &w, 4);
    syscall_row(SYS_WRITE, &t1, &l1);
    lseek(fd, 128ll << 20, SEEK_SET);
    ssize_t r = read(fd, &back, 4);
    close(fd);
    unlink(W_FILE);
    utest_checkf(n == 4 && r == 4 && back == w && t1 - t0 >= 1 && l1 == l0,
                 "a write 128 MiB in takes the kernel lock, and lands: wrote %d, read %d, "
                 "lockless +%llu takes +%llu", (int)n, (int)r,
                 (unsigned long long)(l1 - l0), (unsigned long long)(t1 - t0));
}

static void check_write_counted(void) {
    count_writes("a /tmp file is written without the kernel lock", W_FILE, 1);
    check_far_write();
    if (!on_ramfs(DISK_FILE))
        count_writes("a /var/tmp (disk) file's write takes the kernel lock", DISK_FILE, 0);
    tunable_set(TUNABLE, "off");
    count_writes("turned off, a /tmp write takes the kernel lock again", W_FILE, 0);
    tunable_set(TUNABLE, "on");
}

// READERS writers, each word (writer << 24 | n). Shared: one description,
// so each write takes the next position. Appending: a description each,
// O_APPEND, so each write lands at the end under the mount's lock.
static int g_w_fd;
static int g_w_append;

static void *word_writer(void *arg) {
    uint32_t me = (uint32_t)(uintptr_t)arg;
    int fd = g_w_append ? open(W_FILE, O_WRONLY | O_APPEND) : g_w_fd;
    if (fd < 0) return 0;
    for (uint32_t n = 0; n < W_EACH; n++) {
        uint32_t w = me << 24 | n;
        if (write(fd, &w, 4) != 4) break;
        g_progress++;
    }
    if (g_w_append) close(fd);
    return 0;
}

static void check_writers(int append) {
    g_phase = append ? "appenders, a description each" : "writers sharing one description";
    g_w_append = append;
    g_w_fd = open(W_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_w_fd < 0) { utest_check(0, "the writers' file opens"); return; }
    pthread_t t[READERS];
    for (int i = 0; i < READERS; i++) pthread_create(&t[i], 0, word_writer, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < READERS; i++) pthread_join(t[i], 0);
    close(g_w_fd);

    static uint32_t next[READERS + 1];
    memset(next, 0, sizeof next);
    unsigned words = 0, wrong = 0, order = 0;
    int fd = open(W_FILE, O_RDONLY);
    uint32_t w;
    while (fd >= 0 && read(fd, &w, 4) == 4) {
        words++;
        uint32_t who = w >> 24, n = w & 0xffffff;
        if (who < 1 || who > READERS || n >= W_EACH) { wrong++; continue; }
        if (n != next[who]) order++;
        next[who] = n + 1;
    }
    if (fd >= 0) close(fd);
    unlink(W_FILE);
    utest_checkf(words == READERS * W_EACH && !wrong && !order,
                 "%s: every word once, each writer's in order: %u of %u words, %u wrong, %u out of order",
                 append ? "appenders with a description each" : "writers sharing one description",
                 words, READERS * W_EACH, wrong, order);
}

// Mounts and unmounts a ramfs beside RAM_FILE until told to stop.
static volatile int g_churn_stop;
static volatile unsigned g_churns, g_churn_fail;

static void *churner(void *arg) {
    (void)arg;
    struct mount_request req;
    memset(&req, 0, sizeof req);
    // An empty source: ramfs has no volume (mount's `none`).
    strcpy(req.fstype, "ramfs");
    strcpy(req.point, CHURN_POINT);
    while (!g_churn_stop) {
        if (sys_mount(&req) < 0 || sys_umount(CHURN_POINT) < 0) g_churn_fail++;
        g_churns++;
    }
    return 0;
}

static volatile unsigned g_churn_bad;

static void *churn_reader(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
    int fd = open(RAM_FILE, O_RDONLY);
    if (fd < 0) { g_churn_bad++; return 0; }
    while (!g_churn_stop) {
        seed = seed * 1103515245u + 12345u;
        uint32_t word = (seed >> 8) % WORDS, w = ~0u;
        lseek(fd, (off_t)word * 4, SEEK_SET);
        if (read(fd, &w, 4) != 4 || w != word) g_churn_bad++;
        g_progress++;
    }
    close(fd);
    return 0;
}

static void check_mount_churn(void) {
    g_phase = "reads beside a mount and unmount";
    mkdir(CHURN_POINT, 0755);
    pthread_t c, r[READERS - 1];
    for (int i = 0; i < READERS - 1; i++) pthread_create(&r[i], 0, churn_reader, (void *)(uintptr_t)(i + 1));
    pthread_create(&c, 0, churner, 0);
    unsigned long long until = now_ms() + 3000;
    while (now_ms() < until && g_churns < 200) sys_sleep_ms(50);
    g_churn_stop = 1;
    pthread_join(c, 0);
    for (int i = 0; i < READERS - 1; i++) pthread_join(r[i], 0);
    rmdir(CHURN_POINT);
    utest_checkf(g_churns > 10 && !g_churn_fail, "a ramfs mounts and unmounts beside the file: %u times, %u failed",
                 g_churns, g_churn_fail);
    utest_checkf(!g_churn_bad, "...and every read beside it answers its whole word: %u wrong", g_churn_bad);
}

int main(void) {
    utest_begin("lockless_rw_test", "the lock-free read/write: pipe.c, syscall_fd.c, a ramfs file",
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
    check_file_counted();
    check_shared_position();
    check_mount_churn();
    check_write_counted();
    check_writers(0);
    check_writers(1);
    unlink(RAM_FILE);
    g_all_done = 1;
    pthread_join(dog, 0);
    tunable_set(TUNABLE, was);
    return utest_end();
}
