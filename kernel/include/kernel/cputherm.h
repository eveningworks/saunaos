#ifndef KERNEL_CPUTHERM_H
#define KERNEL_CPUTHERM_H

// The CPU's own thermal sensors and the one knob passive cooling turns
// (kernel/arch/x86_64/cputherm.c): Linux's coretemp and the cpufreq
// limit its thermal governor sets, Intel family 6 only, never under a
// hypervisor -- cpufreq.c's gating, for the same #GP reason.

#include <stdint.h>

struct cpu_info;
struct query_thermal;

#define CPUTHERM_DTS     (1u << 0)   // a digital sensor per core (CPUID.06H:EAX[0])
#define CPUTHERM_PTM     (1u << 1)   // ...and one for the package (CPUID.06H:EAX[6])
#define CPUTHERM_TJMAX   (1u << 2)   // MSR_TEMPERATURE_TARGET names TjMax (model table)
#define CPUTHERM_HWP     (1u << 3)   // HWP: the limit goes in IA32_HWP_REQUEST
#define CPUTHERM_PERFCTL (1u << 4)   // EIST + the ratio MSRs: the limit goes in IA32_PERF_CTL

// What this CPU offers, from CPUID alone (pure; cputherm_test.c). `why`
// says what is missing when nothing is: a fixed string.
uint32_t cputherm_plan(const struct cpu_info *ci, uint32_t cpuid1_ecx, uint32_t cpuid6_eax,
                       const char **why);

// QUERY_THERMAL's CPU rows: the package sensor, then one per core. Each
// read is an MSR on THAT core (smp_call()), so from a syscall, never with
// a spinlock held.
int cputherm_count(void);
int cputherm_read(int index, struct query_thermal *out);

// Passive cooling's limit, 1..100% of the CPU's range, on every online
// core; 100 puts back what was there before the first limit. 0 when it
// reached every core, else why not (a fixed string). kacpid only.
const char *cputherm_limit(int pct);

#endif
