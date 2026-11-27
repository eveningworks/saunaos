// M16 scheduler demo (see kernel/proc/scheduler.c). A tiny freestanding
// ring-3 program that proves preemptive, non-cooperative scheduling:
// prints 'A' a number of times (20, or its argument), each followed by
// a long busy-spin (spanning several 100Hz timer ticks), then exits. Run alongside
// counter_b.c (which does the same with 'B') via the shell's `schedtest`
// command. There is no yield syscall anywhere in this project -- if the
// output interleaves on screen instead of printing all the As followed
// by all the Bs, the ONLY possible explanation is the timer preempting
// one process mid-spin and handing the CPU to the other.
#include <stdint.h>
#include "rt/sys.h"







// A global (not a stack local) so it's definitely inside a PT_LOAD
// segment elf.c mapped -- same reasoning as write_test.c's approach.
static char out_char = 'A';

#define ITERATIONS  20
#define ITERATIONS_MAX 600 // spin_test.c's bound for the same loop
#define SPIN_ITERS  30000000u // long enough to span several 10ms slices

// How many to print is an optional argument: the default is `schedtest`'s,
// done in well under a second on a fast host, and a test that needs the
// job ALIVE across a wait of its own (tools/tests/sweep/debug_tty_test.py)
// asks for more. Anything but a number is the default, as in spin_test.c.
static int parse_count(const char *s) {
    if (!s || !*s) return ITERATIONS;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return ITERATIONS;
        v = v * 10 + (*s - '0');
        if (v > ITERATIONS_MAX) return ITERATIONS_MAX;
    }
    return v > 0 ? v : ITERATIONS;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? parse_count(argv[1]) : ITERATIONS;
    for (int i = 0; i < n; i++) {
        sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)&out_char, 1);
        for (uint32_t j = 0; j < SPIN_ITERS; j++) __asm__ volatile("" : "+r"(j)); // in a register: spin_test.c
    }
    sys_exit(0);
}
