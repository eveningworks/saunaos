// The CPU's clock, read from its frequency MSRs: the ratio it runs at,
// the ratio asked for, the limits, and the APERF/MPERF pair a caller
// samples twice for the clock actually delivered (turbostat's Bzy_MHz).
// READ-ONLY: nothing here writes IA32_PERF_CTL -- whatever ratio the
// firmware left is what the machine runs at.
//
// THE TRAP: an MSR the CPU does not implement is a #GP, and this kernel
// has no rdmsr_safe() to survive one, so every read is gated --
//   - Intel family 6 only; AMD's P-state MSRs are another layout;
//   - never under a hypervisor (CPUID.01H:ECX bit 31): QEMU and KVM
//     emulate some of these and fault on the rest, and none of them
//     describes a real clock;
//   - PERF_STATUS/PERF_CTL behind EIST, APERF/MPERF behind CPUID.06H,
//     IA32_PM_ENABLE behind HWP;
//   - PLATFORM_INFO and TURBO_RATIO_LIMIT are not architectural at all,
//     so behind a model table (intel_pstate's and turbostat's shape).
#include "cpuinfo.h"
#include "msr.h"
#include "string.h"

#define MSR_PLATFORM_INFO       0xCEu
#define MSR_IA32_MPERF          0xE7u
#define MSR_IA32_APERF          0xE8u
#define MSR_IA32_PERF_STATUS    0x198u
#define MSR_IA32_PERF_CTL       0x199u
#define MSR_IA32_MISC_ENABLE    0x1A0u
#define MSR_TURBO_RATIO_LIMIT   0x1ADu
#define MSR_IA32_PM_ENABLE      0x770u

#define CPUID1_ECX_EIST         (1u << 7)
#define CPUID1_ECX_HYPERVISOR   (1u << 31)
#define CPUID6_EAX_TURBO        (1u << 1)
#define CPUID6_EAX_HWP          (1u << 7)
#define CPUID6_ECX_APERFMPERF   (1u << 0)

// Family-6 big cores, Sandy Bridge on, whose PLATFORM_INFO and
// TURBO_RATIO_LIMIT have the client layout on a 100 MHz bus clock. Left
// out on purpose: Atoms (another layout), Nehalem/Westmere (133 MHz),
// and Skylake-SP/Ice Lake-SP, whose turbo bytes are per core GROUP.
static const uint8_t RATIO_MODELS[] = {
    0x2A, 0x2D,             // Sandy Bridge
    0x3A, 0x3E,             // Ivy Bridge
    0x3C, 0x3F, 0x45, 0x46, // Haswell
    0x3D, 0x47, 0x4F, 0x56, // Broadwell
    0x4E, 0x5E,             // Skylake
    0x8E, 0x9E,             // Kaby Lake / Coffee Lake
    0xA5, 0xA6,             // Comet Lake
    0x7D, 0x7E,             // Ice Lake client
    0x8C, 0x8D,             // Tiger Lake
    0xA7,                   // Rocket Lake
    0x97, 0x9A,             // Alder Lake
    0xB7, 0xBA, 0xBF,       // Raptor Lake
};

int cpu_freq_ratio_model(uint16_t model) {
    for (unsigned i = 0; i < sizeof RATIO_MODELS; i++)
        if (RATIO_MODELS[i] == model) return 1;
    return 0;
}

uint32_t cpu_freq_plan(const struct cpu_info *ci, uint32_t cpuid1_ecx,
                       uint32_t cpuid6_eax, uint32_t cpuid6_ecx,
                       uint32_t *skip) {
    *skip = CPU_FREQ_READ;
    if (k_strcmp(ci->vendor, "GenuineIntel") != 0 || ci->family != 6) {
        *skip = CPU_FREQ_SKIP_NOT_INTEL;
        return 0;
    }
    if (cpuid1_ecx & CPUID1_ECX_HYPERVISOR) {
        *skip = CPU_FREQ_SKIP_HYPERVISOR;
        return 0;
    }
    uint32_t have = CPU_FREQ_HAVE_MISC;  // IA32_MISC_ENABLE: every family-6 part
    if (cpuid1_ecx & CPUID1_ECX_EIST) have |= CPU_FREQ_HAVE_PERF;
    if (ci->max_leaf >= 6) {
        if (cpuid6_ecx & CPUID6_ECX_APERFMPERF) have |= CPU_FREQ_HAVE_APERF;
        if (cpuid6_eax & CPUID6_EAX_HWP)        have |= CPU_FREQ_HAVE_HWP;
        if (cpuid6_eax & CPUID6_EAX_TURBO)      have |= CPU_FREQ_HAVE_TURBO;
    }
    if (cpu_freq_ratio_model(ci->model)) have |= CPU_FREQ_HAVE_RATIOS;
    return have;
}

static void cpuid_regs(uint32_t leaf, uint32_t *eax, uint32_t *ecx) {
    uint32_t ebx, edx;
    __asm__ volatile ("cpuid" : "=a"(*eax), "=b"(ebx), "=c"(*ecx), "=d"(edx)
                              : "a"(leaf), "c"(0));
}

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cpu_freq_get(struct cpu_info *ci) {
    uint32_t eax1, ecx1, eax6 = 0, ecx6 = 0;
    cpuid_regs(1, &eax1, &ecx1);
    if (ci->max_leaf >= 6) cpuid_regs(6, &eax6, &ecx6);

    uint32_t skip;
    uint32_t have = cpu_freq_plan(ci, ecx1, eax6, ecx6, &skip);
    ci->freq_have = have;
    ci->freq_skip = skip;

    if (have & CPU_FREQ_HAVE_MISC)   ci->misc_enable = rdmsr(MSR_IA32_MISC_ENABLE);
    if (have & CPU_FREQ_HAVE_PERF) {
        ci->perf_status = rdmsr(MSR_IA32_PERF_STATUS);
        ci->perf_ctl    = rdmsr(MSR_IA32_PERF_CTL);
    }
    if (have & CPU_FREQ_HAVE_RATIOS) {
        ci->platform_info     = rdmsr(MSR_PLATFORM_INFO);
        ci->turbo_ratio_limit = rdmsr(MSR_TURBO_RATIO_LIMIT);
    }
    if (have & CPU_FREQ_HAVE_HWP)    ci->pm_enable = rdmsr(MSR_IA32_PM_ENABLE);
    if (have & CPU_FREQ_HAVE_APERF) {
        // Back to back, so a caller's two samples bracket the same span.
        ci->mperf = rdmsr(MSR_IA32_MPERF);
        ci->aperf = rdmsr(MSR_IA32_APERF);
        ci->tsc   = read_tsc();
    }
}
