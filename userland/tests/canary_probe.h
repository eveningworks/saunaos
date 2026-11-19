// What one process sees of its stack-protector canary, written as one
// line for canary_test to compare across processes: the TLS copy GCC's
// default guard reads (%fs:0x28), the global the `global` guard reads
// (__stack_chk_guard), a second thread's %fs:0x28, and the canary the
// kernel's AT_RANDOM bytes make. Shared by canary_test (static) and
// canary_dyn_test (dynamic), which differ only in how they are linked.
#ifndef CANARY_PROBE_H
#define CANARY_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/auxv.h>

extern uintptr_t __stack_chk_guard;

static uint64_t probe_fs28(void) {
    uint64_t v;
    __asm__ volatile ("mov %%fs:0x28, %0" : "=r"(v));
    return v;
}

static void *probe_thread(void *out) {
    *(uint64_t *)out = probe_fs28();
    return 0;
}

// AT_RANDOM's first 8 bytes, low byte zeroed (glibc's rule); 0 if absent.
// Through getauxval() and its own byte arithmetic, not abi/auxv.h's
// auxv_canary() the runtimes share -- the check must not be the code.
static uint64_t probe_auxv_canary(void) {
    const uint8_t *b = (const uint8_t *)(uintptr_t)getauxval(AT_RANDOM);
    if (!b) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 1; i--) v = (v << 8) | b[i];
    return v << 8;
}

static int probe_print(void) {
    uint64_t thr = 0;
    pthread_t t;
    if (pthread_create(&t, 0, probe_thread, &thr) != 0 || pthread_join(t, 0) != 0) thr = 1;
    printf("%016llx %016llx %016llx %016llx\n",
           (unsigned long long)probe_fs28(), (unsigned long long)__stack_chk_guard,
           (unsigned long long)thr, (unsigned long long)probe_auxv_canary());
    return 0;
}

#endif
