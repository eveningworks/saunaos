#ifndef KERNEL_MSR_H
#define KERNEL_MSR_H

// Reading and writing a model-specific register. Ring 0 only; an MSR
// the CPU does not implement is a #GP, so a caller checks CPUID first
// for anything that is not architectural on every x86-64.
#include <stdint.h>

#define MSR_EFER   0xC0000080u   // bit 0 SCE: SYSCALL/SYSRET enabled
#define MSR_STAR   0xC0000081u   // the selectors SYSCALL and SYSRET load
#define MSR_LSTAR  0xC0000082u   // SYSCALL's 64-bit entry point
#define MSR_FMASK  0xC0000084u   // RFLAGS bits SYSCALL clears

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                                 "d"((uint32_t)(val >> 32)));
}

#endif
