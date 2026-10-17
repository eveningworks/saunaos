// A BENCHMARK, not a test: what one system call costs -- SYSCALL in,
// SYSRET out (kernel/arch/x86_64/syscall_entry.asm). Not in the gate,
// because a timing number varies with the host. (It compared `int
// $0x80` too, until that entry was retired.)
//
// The call is SYS_GETPID, which does nothing, so the figure is the
// entry and exit alone.
// Cycles come from RDTSC; under TCG they are the emulator's and mean
// little, so read a KVM run (`tools/vm.py --kvm`) or real hardware.
//
//     spawn /tests/syscall_bench [CALLS]       (default 1000000)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"

static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void via_syscall(long n) {
    for (long i = 0; i < n; i++) {
        uint64_t r = SYS_GETPID;
        __asm__ volatile ("syscall" : "+a"(r) :: "rcx", "r11", "memory");
    }
}

static void row(const char *name, void (*loop)(long), long n, double *ns_out) {
    loop(n / 10);   // warm: the first calls pay for cold caches and TLB
    uint64_t t0 = sys_monotonic_ns(), c0 = tsc();
    loop(n);
    uint64_t c1 = tsc(), t1 = sys_monotonic_ns();
    double ns = (double)(t1 - t0) / (double)n;
    printf("  %-10s %8.1f ns/call %8.1f cycles/call\n", name, ns,
           (double)(c1 - c0) / (double)n);
    *ns_out = ns;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    if (n < 1000) n = 1000;
    printf("syscall_bench: %ld x getpid\n", n);
    double ns = 0;
    row("syscall", via_syscall, n, &ns);
    return 0;
}
