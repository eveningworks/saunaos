#ifndef CPUINFO_H
#define CPUINFO_H

#include <stdint.h>

// What the CPU says about itself (CPUID), and what this kernel has
// actually turned on (CR0/CR4/EFER). Filled by
// kernel/arch/x86_64/cpuid.c, printed by /bin/lscpu and the Control
// Panel's System Info applet.
//
// **The supported/enabled split is the point of this header**, not a
// detail. Those two things come from different places and only one of
// them is reachable from ring 3:
//
//   supported -- CPUID. An UNPRIVILEGED instruction: any ring-3 program
//                can execute it directly, no syscall needed.
//   enabled   -- CR0/CR4/EFER. Privileged. `mov %cr4, %rax` faults in
//                ring 3, so this half can only come from the kernel.
//
// That asymmetry is why there's a syscall here at all (SYS_CPU_INFO):
// not because ring 3 can't identify the CPU, but because it can't see
// which of those capabilities the OS has switched on. It's also why the
// distinction is worth showing rather than collapsing -- SSE2 is
// supported by every x86-64 chip ever made and was NOT enabled in this
// kernel until CR4.OSFXSR got set (see fpu.h); SMEP may well be
// supported and still off. "Supported" is a fact about the silicon,
// "enabled" is a fact about this OS.
//
// The feature NAME table lives in a separate header, api/cpu_features.h,
// deliberately not included from kapi.h -- see that file's top comment.

#define CPU_VENDOR_LEN 13 // 12 chars + NUL
#define CPU_BRAND_LEN  49 // 48 chars + NUL

// Feature words, in the order struct cpu_info::feature[] holds them.
// Each is one 32-bit register from one CPUID leaf; api/cpu_features.h
// maps (word, bit) pairs to names.
enum cpu_feature_word {
    CPU_WORD_1_EDX = 0,   // CPUID.01H:EDX -- the original Pentium-era set
    CPU_WORD_1_ECX,       // CPUID.01H:ECX -- SSE3 onward
    CPU_WORD_7_0_EBX,     // CPUID.07H:0:EBX -- BMI/AVX2/SMEP/SMAP...
    CPU_WORD_7_0_ECX,     // CPUID.07H:0:ECX -- UMIP/PKU/...
    CPU_WORD_7_0_EDX,     // CPUID.07H:0:EDX -- mitigations, AVX-512 tail
    CPU_WORD_EXT1_EDX,    // CPUID.80000001H:EDX -- NX, 1G pages, long mode
    CPU_WORD_EXT1_ECX,    // CPUID.80000001H:ECX -- LAHF, ABM, ...
    CPU_WORD_COUNT
};

// Bits in struct cpu_info::enabled -- what this kernel actually switched
// on, read from the control registers. A feature can be supported and
// not enabled; the reverse is impossible.
#define CPU_EN_FPU        (1u << 0)  // CR0.EM clear + CR0.MP -- FP executes rather than #UD
#define CPU_EN_SSE        (1u << 1)  // CR4.OSFXSR -- FXSAVE/FXRSTOR exist, SSE usable
#define CPU_EN_XMMEXCPT   (1u << 2)  // CR4.OSXMMEXCPT -- SIMD FP exceptions arrive as #XF
#define CPU_EN_NX         (1u << 3)  // EFER.NXE -- the PTE no-execute bit is honored
#define CPU_EN_PAE        (1u << 4)  // CR4.PAE
#define CPU_EN_PGE        (1u << 5)  // CR4.PGE -- global pages
#define CPU_EN_OSXSAVE    (1u << 6)  // CR4.OSXSAVE -- XSAVE/XGETBV usable (AVX needs this)
#define CPU_EN_SMEP       (1u << 7)  // CR4.SMEP -- ring 0 can't execute user pages
#define CPU_EN_SMAP       (1u << 8)  // CR4.SMAP -- ring 0 can't read user pages unguarded
#define CPU_EN_UMIP       (1u << 9)  // CR4.UMIP -- SGDT/SIDT/etc blocked in ring 3
#define CPU_EN_LONG_MODE  (1u << 10) // EFER.LMA -- 64-bit mode active (always, here)
#define CPU_EN_PAGING     (1u << 11) // CR0.PG
#define CPU_EN_WP         (1u << 12) // CR0.WP -- ring 0 honors read-only pages
#define CPU_EN_SYSCALL    (1u << 13) // EFER.SCE -- ring 3 enters through SYSCALL

// One level of the cache hierarchy, from CPUID leaf 4.
enum cpu_cache_type {
    CPU_CACHE_NONE = 0,
    CPU_CACHE_DATA = 1,
    CPU_CACHE_INSTRUCTION = 2,
    CPU_CACHE_UNIFIED = 3,
};

struct cpu_cache {
    uint8_t  level;       // 1, 2, 3...
    uint8_t  type;        // enum cpu_cache_type
    uint16_t line_size;   // bytes per line
    uint16_t ways;        // associativity; 0xFFFF for fully associative
    uint32_t sets;
    uint32_t size_kb;     // ways * partitions * line_size * sets, in KiB
};

#define CPU_MAX_CACHES 8

// How struct cpu_info::mhz was arrived at -- worth reporting rather
// than presenting one number as if all sources were equal.
enum cpu_mhz_source {
    CPU_MHZ_UNKNOWN = 0,
    CPU_MHZ_CPUID_16H,   // CPUID leaf 16H told us directly (Skylake and later)
    CPU_MHZ_MEASURED,    // RDTSC calibrated against the PIT -- see cpuid.c
};

// Fixed layout on purpose: this struct crosses the syscall boundary
// (SYS_CPU_INFO copies one straight into a ring-3 buffer), so it is an
// ABI, not just a struct. Adding a field at the END is safe; reordering
// or resizing anything is not.
struct cpu_info {
    char vendor[CPU_VENDOR_LEN];  // "GenuineIntel", "AuthenticAMD", ...
    char brand[CPU_BRAND_LEN];    // the marketing string, leaves 80000002-4
    uint32_t max_leaf;            // highest basic CPUID leaf
    uint32_t max_ext_leaf;        // highest 8000000xH leaf

    uint16_t family;              // already combined with the extended fields
    uint16_t model;               // likewise -- these are the real values,
                                   // not the raw bitfields
    uint8_t  stepping;
    uint8_t  mhz_source;          // enum cpu_mhz_source
    uint8_t  cache_count;
    uint8_t  reserved;

    uint32_t mhz;
    uint32_t enabled;             // CPU_EN_* -- what this kernel turned on
    uint32_t feature[CPU_WORD_COUNT];

    struct cpu_cache cache[CPU_MAX_CACHES];

    // Topology of ONE package, from CPUID leaf 0BH (leaf 1 + leaf 4 on
    // parts without it): cores per package and hardware threads per
    // core. 0 when the CPU does not say. The number of PACKAGES is not
    // CPUID's to know -- divide the MADT's logical count by
    // cores * threads for that (lscpu.c, about.c).
    uint16_t cores;
    uint16_t threads_per_core;

    // The clock, from the frequency MSRs (cpu_freq_get(), cpufreq.c):
    // filled by SYS_CPU_INFO, NOT by cpu_info_get(). A field is 0 unless
    // its CPU_FREQ_HAVE_* bit is set; freq_skip says why none were read.
    uint32_t freq_have;          // CPU_FREQ_HAVE_*
    uint32_t freq_skip;          // enum cpu_freq_skip
    uint64_t perf_status;        // IA32_PERF_STATUS: 15:8 the ratio running now
    uint64_t perf_ctl;           // IA32_PERF_CTL: 15:8 the ratio asked for, bit 32 turbo off
    uint64_t misc_enable;        // IA32_MISC_ENABLE: bit 16 SpeedStep on, bit 38 turbo off
    uint64_t platform_info;      // MSR_PLATFORM_INFO: 15:8 base ratio, 47:40 lowest
    uint64_t turbo_ratio_limit;  // byte N: the turbo ratio with N+1 cores busy
    uint64_t pm_enable;          // IA32_PM_ENABLE: bit 0 HWP on
    uint64_t aperf, mperf, tsc;  // read back to back; two samples give the busy clock
};

#define CPU_FREQ_HAVE_MISC   (1u << 0)  // misc_enable
#define CPU_FREQ_HAVE_PERF   (1u << 1)  // perf_status, perf_ctl (CPUID EIST)
#define CPU_FREQ_HAVE_RATIOS (1u << 2)  // platform_info, turbo_ratio_limit (model table)
#define CPU_FREQ_HAVE_HWP    (1u << 3)  // pm_enable; the CPU can pick its own ratio
#define CPU_FREQ_HAVE_APERF  (1u << 4)  // aperf, mperf, tsc
#define CPU_FREQ_HAVE_TURBO  (1u << 5)  // CPUID.06H says turbo is available (and not disabled)

#define CPU_FREQ_BCLK_MHZ 100           // the ratio's unit on every RATIO_MODELS part

enum cpu_freq_skip {
    CPU_FREQ_READ = 0,
    CPU_FREQ_SKIP_NOT_INTEL,     // not Intel family 6: another MSR layout, unread
    CPU_FREQ_SKIP_HYPERVISOR,    // a guest: the MSRs describe no real clock
};

// Fills the freq_* block of `ci`, which cpu_info_get() has filled first.
// Reads MSRs, so ring 0 with the gating in cpufreq.c; a syscall only.
void cpu_freq_get(struct cpu_info *ci);

// Which CPU_FREQ_HAVE_* a CPU gets, from CPUID alone -- pure, so the
// gating is testable with any CPU's answers (cpuid_test.c).
uint32_t cpu_freq_plan(const struct cpu_info *ci, uint32_t cpuid1_ecx,
                       uint32_t cpuid6_eax, uint32_t cpuid6_ecx,
                       uint32_t *skip);
int cpu_freq_ratio_model(uint16_t model);

// One-time setup: works out the clock speed and caches it. Call once
// from kernel_main(), AFTER idt_init() (it needs the PIT ticking).
//
// This exists as a separate init rather than being folded into the
// first cpu_info_get() because a lazy calibration deadlocks: it spins
// waiting for coarse_ticks() to advance, and every interrupt gate here --
// and the syscall entry -- clears IF, so a calibration
// reached through SYS_CPU_INFO waits forever for a tick that can't
// arrive. See cpuid.c.
void cpu_info_init(void);

// Fills `out` with everything above. Cheap (CPUID is a few hundred
// cycles, the clock speed is already cached) and safe to call from
// anywhere, including a syscall handler with interrupts off. Reports
// CPU_MHZ_UNKNOWN if cpu_info_init() never ran.
void cpu_info_get(struct cpu_info *out);

// The two accessors below are `static inline` HERE rather than
// implemented in cpuid.c, and that's deliberate: /bin/lscpu and the
// About window are ring-3 ELFs, which link libuapp and libc but no
// kernel object, so anything they call has to be either a syscall or
// inline. A kernel function declared above would compile fine there and
// fail at link with an undefined symbol.

// 1 if the (word, bit) feature is supported by the CPU. Bounds-checked:
// an out-of-range word returns 0 rather than reading past the array.
static inline int cpu_has_feature(const struct cpu_info *ci, int word, int bit) {
    if (!ci || word < 0 || word >= CPU_WORD_COUNT || bit < 0 || bit > 31) return 0;
    return (int)((ci->feature[word] >> bit) & 1u);
}

// Human-readable name for enum cpu_cache_type, e.g. "data". Never NULL.
static inline const char *cpu_cache_type_name(int type) {
    switch (type) {
        case CPU_CACHE_DATA:        return "data";
        case CPU_CACHE_INSTRUCTION: return "instruction";
        case CPU_CACHE_UNIFIED:     return "unified";
        default:                    return "unknown";
    }
}

#endif
