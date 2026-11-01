// A BENCHMARK, not a test: what a system call, a context switch and a
// read or write of a given size cost. Not in the gate, because a timing
// number varies with the host. tools/sys_bench.py boots a build, runs
// this and collects the rows.
//
//   getpid       SYSCALL in, SYSRET out, a call that does nothing
//   yield        a trip through the scheduler with nobody else to run
//   pipe 1 B     a write then a read of one byte, one process
//   switch       two processes passing a byte back and forth over two
//                pipes: each round trip is two switches and four pipe
//                calls, so `switch` is (round trip - 2 x pipe 1 B) / 2,
//                lmbench's lat_ctx subtraction
//   read/write N one call of N bytes against a /tmp (ramfs) file; the
//                `copy` column is two ring-3 memcpys of N, the floor
//                for a call that copies through a kernel bounce buffer
//
// Cycles come from RDTSC; under TCG they are the emulator's and mean
// little, so read a KVM run or real hardware. The `machine:` line says
// which this was. Rows go to STDERR, which reaches `dmesg` from a
// spawned program as stdout does not. SPAWN it: the switch row forks, and the legacy `run`
// loader has no scheduler slot to fork from.
//
//     spawn /tests/syscall_bench [CALLS [SIZE...]]   (default 1000000;
//                                     SIZE in bytes, up to 1 MiB)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"

#define say(...) fprintf(stderr, __VA_ARGS__)

#define SCRATCH "/tmp/syscall_bench.dat"
#define SIZE_MAX_BYTES (1024 * 1024)

static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t r[4]) {
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

// The CPU's brand string and the hypervisor's signature, so a pasted
// result says what it was measured on.
static void print_machine(void) {
    uint32_t r[4];
    char brand[49] = "unknown CPU";
    cpuid(0x80000000u, r);
    if (r[0] >= 0x80000004u) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, r);
            memcpy(brand + 16 * i, r, 16);
        }
        brand[48] = '\0';
    }
    const char *b = brand;
    while (*b == ' ') b++;
    char hv[13] = "none";
    cpuid(1, r);
    if (r[2] & (1u << 31)) {
        cpuid(0x40000000u, r);
        memcpy(hv, &r[1], 4);
        memcpy(hv + 4, &r[2], 4);
        memcpy(hv + 8, &r[3], 4);
        hv[12] = '\0';
    }
    say("syscall_bench: machine: %s; hypervisor %s\n", b, hv);
}

static uint8_t *g_buf, *g_copy;
static int g_rfd, g_wfd, g_pa[2], g_pb[2];
static size_t g_size;

static void do_getpid(long n) {
    for (long i = 0; i < n; i++) {
        uint64_t r = SYS_GETPID;
        __asm__ volatile ("syscall" : "+a"(r) :: "rcx", "r11", "memory");
    }
}
static void do_yield(long n) { for (long i = 0; i < n; i++) sys_yield(); }
static void do_pipe1(long n) {
    for (long i = 0; i < n; i++) {
        sys_write(g_pa[1], g_buf, 1);
        sys_read(g_pa[0], g_buf, 1);
    }
}
static void do_pingpong(long n) {
    for (long i = 0; i < n; i++) {
        sys_write(g_pa[1], g_buf, 1);
        sys_read(g_pb[0], g_buf, 1);
    }
}
static void do_read(long n) {
    for (long i = 0; i < n; i++) {
        sys_lseek(g_rfd, 0, SYS_SEEK_SET);
        sys_read(g_rfd, g_buf, g_size);
    }
}
static void do_write(long n) {
    for (long i = 0; i < n; i++) {
        sys_lseek(g_wfd, 0, SYS_SEEK_SET);
        sys_write(g_wfd, g_buf, g_size);
    }
}
static void do_copy(long n) {
    for (long i = 0; i < n; i++) {
        memcpy(g_copy, g_buf, g_size);
        memcpy(g_buf, g_copy, g_size);
    }
}

struct cost { double ns, cycles; };

static struct cost measure(void (*loop)(long), long n) {
    loop(n / 10 ? n / 10 : 1);   // warm: the first calls pay for cold caches and TLB
    uint64_t t0 = sys_monotonic_ns(), c0 = tsc();
    loop(n);
    uint64_t c1 = tsc(), t1 = sys_monotonic_ns();
    return (struct cost){ (double)(t1 - t0) / (double)n, (double)(c1 - c0) / (double)n };
}

static void row(const char *name, struct cost c) {
    say("syscall_bench: %-12s %10.1f ns %10.0f cycles\n", name, c.ns, c.cycles);
}

// The child of the switch row: echo every byte back until the parent
// closes its end.
static void echo_child(void) {
    sys_close(g_pa[1]);
    sys_close(g_pb[0]);
    uint8_t b;
    while (sys_read(g_pa[0], &b, 1) == 1) sys_write(g_pb[1], &b, 1);
    sys_exit(0);
}

static int switch_rows(long n, struct cost pipe1) {
    if (sys_pipe(g_pb) < 0) { say("syscall_bench: pipe failed\n"); return 1; }
    int pid = sys_fork();
    if (pid < 0) { say("syscall_bench: fork failed -- SPAWN this, not run\n"); return 1; }
    if (pid == 0) echo_child();
    sys_close(g_pb[1]);
    struct cost rt = measure(do_pingpong, n);
    sys_close(g_pa[1]);
    sys_waitpid(pid, NULL);
    row("round trip", rt);
    row("switch", (struct cost){ (rt.ns - 2 * pipe1.ns) / 2, (rt.cycles - 2 * pipe1.cycles) / 2 });
    return 0;
}

static void size_rows(const size_t *sizes, unsigned count) {
    say("syscall_bench: %-12s %10s %10s %10s %10s\n", "size", "read ns", "write ns", "copy ns",
           "read GB/s");
    for (unsigned k = 0; k < count; k++) {
        g_size = sizes[k];
        // ~64 MiB a row, so a 64-byte call and a 1 MiB one both outlast the clock's grain.
        long reps = (long)((64u << 20) / g_size);
        if (reps > 100000) reps = 100000;
        struct cost r = measure(do_read, reps), w = measure(do_write, reps), c = measure(do_copy, reps);
        char label[16];
        snprintf(label, sizeof label, "%zu", g_size);
        say("syscall_bench: %-12s %10.0f %10.0f %10.0f %10.2f\n", label, r.ns, w.ns, c.ns,
               r.ns > 0 ? (double)g_size / r.ns : 0);
    }
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    if (n < 1000) n = 1000;
    print_machine();
    g_buf = malloc(SIZE_MAX_BYTES);
    g_copy = malloc(SIZE_MAX_BYTES);
    if (!g_buf || !g_copy) { say("syscall_bench: out of memory\n"); return 1; }
    memset(g_buf, 0x5A, SIZE_MAX_BYTES);
    memset(g_copy, 0, SIZE_MAX_BYTES);
    say("syscall_bench: %ld calls a row\n", n);

    row("getpid", measure(do_getpid, n));
    row("yield", measure(do_yield, n / 10));
    if (sys_pipe(g_pa) < 0) { say("syscall_bench: pipe failed\n"); return 1; }
    struct cost pipe1 = measure(do_pipe1, n / 10);
    row("pipe 1 B", pipe1);
    int rc = switch_rows(n / 10, pipe1);

    g_wfd = sys_open(SCRATCH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (g_wfd < 0) { say("syscall_bench: cannot create " SCRATCH "\n"); return 1; }
    g_size = SIZE_MAX_BYTES;
    sys_write(g_wfd, g_buf, g_size);
    g_rfd = sys_open(SCRATCH, 0);
    if (g_rfd < 0) { say("syscall_bench: cannot open " SCRATCH "\n"); return 1; }
    static const size_t SIZES[] = { 64, 4096, 65536, 262144, SIZE_MAX_BYTES };
    size_t picked[16];
    unsigned count = 0;
    for (int i = 2; i < argc && count < sizeof picked / sizeof picked[0]; i++) {
        long v = atol(argv[i]);
        if (v > 0 && v <= SIZE_MAX_BYTES) picked[count++] = (size_t)v;
    }
    if (count) size_rows(picked, count);
    else size_rows(SIZES, sizeof SIZES / sizeof SIZES[0]);
    sys_close(g_rfd);
    sys_close(g_wfd);
    sys_unlink(SCRATCH);
    say("syscall_bench: done\n");
    return rc;
}
