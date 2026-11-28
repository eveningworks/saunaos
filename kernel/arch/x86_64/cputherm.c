// The CPU's thermal sensors (Linux's coretemp) and passive cooling's
// frequency limit (cputherm.h).
//
// A SENSOR READS DEGREES BELOW TjMax, per core: IA32_THERM_STATUS[22:16]
// on the core it describes, IA32_PACKAGE_THERM_STATUS[22:16] anywhere in
// the package, both valid only with bit 31 set. TjMax is
// MSR_TEMPERATURE_TARGET[23:16] on the models cpufreq.c's table names,
// else 100 C -- coretemp's fallback.
//
// THE LIMIT is the ratio ceiling, written on each core: IA32_HWP_REQUEST's
// maximum when HWP is on (the CPU picks its own ratio under it), else
// IA32_PERF_CTL's target ratio WITH TURBO DISENGAGED (bit 32) and the
// range lowest..base: any ratio over base is a turbo request, which the
// CPU answers with its highest turbo ratio (measured on the ASUS: 16
// asked, 20 run). Nothing else in SaunaOS writes either register, so the
// first limit saves each core's value and 100% writes it back.
#include "cpuinfo.h"
#include "cputherm.h"
#include "heap.h"
#include "knum.h"
#include "msr.h"
#include "percpu.h"
#include "query.h"
#include "smp.h"
#include "spinlock.h"
#include "string.h"

#define MSR_PLATFORM_INFO          0xCEu
#define MSR_IA32_THERM_STATUS      0x19Cu
#define MSR_IA32_PERF_CTL          0x199u
#define MSR_IA32_MISC_ENABLE       0x1A0u
#define MSR_TEMPERATURE_TARGET     0x1A2u
#define MSR_IA32_PACKAGE_THERM     0x1B1u
#define MSR_IA32_PM_ENABLE         0x770u
#define MSR_IA32_HWP_CAPABILITIES  0x771u
#define MSR_IA32_HWP_REQUEST       0x774u

#define CPUID1_ECX_EIST            (1u << 7)
#define CPUID1_ECX_HYPERVISOR      (1u << 31)
#define CPUID6_EAX_DTS             (1u << 0)
#define CPUID6_EAX_PTM             (1u << 6)
#define CPUID6_EAX_HWP             (1u << 7)
#define MISC_ENABLE_EIST           (1ull << 16)
#define THERM_VALID                (1ull << 31)
#define PERF_CTL_TURBO_OFF         (1ull << 32)
#define TJMAX_FALLBACK_C           100

uint32_t cputherm_plan(const struct cpu_info *ci, uint32_t cpuid1_ecx, uint32_t cpuid6_eax,
                       const char **why) {
    *why = 0;
    if (cpuid1_ecx & CPUID1_ECX_HYPERVISOR) {   // first: the reason that matters in a guest
        *why = "a virtual machine";
        return 0;
    }
    if (k_strcmp(ci->vendor, "GenuineIntel") != 0 || ci->family != 6) {
        *why = "not an Intel family-6 CPU";
        return 0;
    }
    if (ci->max_leaf < 6) {
        *why = "no thermal leaf in CPUID";
        return 0;
    }
    uint32_t have = 0;
    if (cpuid6_eax & CPUID6_EAX_DTS) have |= CPUTHERM_DTS;
    if (cpuid6_eax & CPUID6_EAX_PTM) have |= CPUTHERM_PTM;
    if (cpu_freq_ratio_model(ci->model)) {
        have |= CPUTHERM_TJMAX;
        if (cpuid6_eax & CPUID6_EAX_HWP) have |= CPUTHERM_HWP;
        else if (cpuid1_ecx & CPUID1_ECX_EIST) have |= CPUTHERM_PERFCTL;
    }
    if (!(have & (CPUTHERM_HWP | CPUTHERM_PERFCTL))) *why = "no frequency control this kernel knows";
    return have;
}

static void cpuid_regs(uint32_t leaf, uint32_t *eax, uint32_t *ecx) {
    uint32_t ebx, edx;
    __asm__ volatile ("cpuid" : "=a"(*eax), "=b"(ebx), "=c"(*ecx), "=d"(edx) : "a"(leaf), "c"(0));
}

static struct spinlock g_lock;   // g_have, g_tjmax, the per-core arrays
static int g_planned;
static uint32_t g_have;
static const char *g_why;
static int g_tjmax_c;
static uint16_t g_threads;       // per core, for the core number from an APIC id
static int64_t *g_max;           // per logical core: the highest reading; then the package's
static uint64_t *g_saved;        // per logical core: the register before the first limit
static uint8_t *g_saved_ok;
static int g_limited;

// Once, on first use: the CPU cannot change, and cpu_info_get() is cheap
// but CPUID is a VM exit.
static void plan(void) {
    if (__atomic_load_n(&g_planned, __ATOMIC_ACQUIRE)) return;
    static struct cpu_info ci;   // 300+ bytes: not a stack local
    cpu_info_get(&ci);
    uint32_t eax1, ecx1, eax6 = 0, ecx6 = 0;
    cpuid_regs(1, &eax1, &ecx1);
    if (ci.max_leaf >= 6) cpuid_regs(6, &eax6, &ecx6);
    const char *why;
    uint32_t have = cputherm_plan(&ci, ecx1, eax6, &why);
    int tj = TJMAX_FALLBACK_C;
    if (have & CPUTHERM_TJMAX) {
        int t = (int)((rdmsr(MSR_TEMPERATURE_TARGET) >> 16) & 0xFF);
        if (t >= 50 && t <= 130) tj = t;   // a zero or nonsense byte: coretemp's fallback
    }
    if ((have & CPUTHERM_HWP) && !(rdmsr(MSR_IA32_PM_ENABLE) & 1)) {
        // HWP the firmware left off: the legacy ratio request still rules.
        have &= ~CPUTHERM_HWP;
        have |= CPUTHERM_PERFCTL;
    }
    if ((have & CPUTHERM_PERFCTL) && !(rdmsr(MSR_IA32_MISC_ENABLE) & MISC_ENABLE_EIST)) {
        have &= ~CPUTHERM_PERFCTL;
        why = "SpeedStep is off in the firmware";
    }
    int n = smp_cpu_count();
    if (n < 1) n = 1;
    int64_t *mx = kzalloc(sizeof *mx * (unsigned)(n + 1));
    for (int i = 0; mx && i <= n; i++) mx[i] = QUERY_THERMAL_NONE;
    uint64_t f = spin_lock_irqsave(&g_lock);
    if (g_planned) {   // a second first caller (kacpid and a syscall): the first one's stands
        spin_unlock_irqrestore(&g_lock, f);
        kfree(mx);
        return;
    }
    g_have = have;
    g_why = why;
    g_tjmax_c = tj;
    g_threads = ci.threads_per_core ? ci.threads_per_core : 1;
    g_max = mx;
    __atomic_store_n(&g_planned, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_lock, f);
}

// --- the sensors ------------------------------------------------------------

static int core_shift(void) {
    int s = 0;
    while ((1u << s) < g_threads) s++;
    return s;
}

// The logical index of each core's first thread, in order: a core is
// read once however many threads it has.
static int nth_core(int nth, uint32_t *core_no) {
    int shift = core_shift(), seen = 0;
    if (smp_cpu_count() <= 1) {   // nosmp, or one core: no table to walk
        *core_no = 0;
        return nth == 0 ? 0 : -1;
    }
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (!c || !smp_apic_online(c->apic_id)) continue;
        uint32_t core = c->apic_id >> shift;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) {
            const struct cpu *d = smp_cpu_at(j);
            dup = d && smp_apic_online(d->apic_id) && (d->apic_id >> shift) == core;
        }
        if (dup) continue;
        if (seen++ == nth) {
            *core_no = core;
            return i;
        }
    }
    return -1;
}

int cputherm_count(void) {
    plan();
    if (!(g_have & CPUTHERM_DTS)) return 0;
    int n = 0;
    uint32_t core;
    while (nth_core(n, &core) >= 0) n++;
    return n + ((g_have & CPUTHERM_PTM) ? 1 : 0);
}

struct therm_msr { uint32_t msr; uint64_t value; };
static void read_msr_here(void *arg) {
    struct therm_msr *m = arg;
    m->value = rdmsr(m->msr);
}

int cputherm_read(int index, struct query_thermal *q) {
    plan();
    if (!(g_have & CPUTHERM_DTS) || index < 0) return 0;
    k_memset(q, 0, sizeof *q);
    q->critical_mc = (int64_t)g_tjmax_c * 1000;
    q->hot_mc = q->passive_mc = QUERY_THERMAL_NONE;
    q->temp_mc = QUERY_THERMAL_NONE;
    q->perf_pct = 100;
    int pkg = (g_have & CPUTHERM_PTM) ? 1 : 0;
    struct therm_msr m = { MSR_IA32_THERM_STATUS, 0 };
    int logical, slot;
    if (pkg && index == 0) {
        q->kind = QUERY_THERMAL_CPU_PACKAGE;
        k_strlcpy(q->name, "Package", sizeof q->name);
        m.msr = MSR_IA32_PACKAGE_THERM;
        logical = (int)spin_cpu_index();   // any core of the (one) package
        slot = smp_cpu_count();
    } else {
        uint32_t core;
        logical = nth_core(index - pkg, &core);
        if (logical < 0) return 0;
        q->kind = QUERY_THERMAL_CPU_CORE;
        q->cpu = core;
        k_strlcpy(q->name, "Core ", sizeof q->name);
        k_itoa(core, q->name + 5, sizeof q->name - 5);
        slot = logical;
    }
    if (!smp_call(logical, read_msr_here, &m, 1)) return 1;   // the row, unread
    q->readings = 1;
    if (m.value & THERM_VALID) {
        q->temp_mc = ((int64_t)g_tjmax_c - (int64_t)((m.value >> 16) & 0x7F)) * 1000;
        q->flags |= QUERY_THERMAL_VALID;
        if (q->temp_mc + QUERY_THERMAL_WARN_MC >= q->critical_mc) q->flags |= QUERY_THERMAL_HOT_SOON;
    }
    uint64_t f = spin_lock_irqsave(&g_lock);
    if (g_max && (q->flags & QUERY_THERMAL_VALID) && q->temp_mc > g_max[slot]) g_max[slot] = q->temp_mc;
    q->max_mc = g_max ? g_max[slot] : QUERY_THERMAL_NONE;
    spin_unlock_irqrestore(&g_lock, f);
    return 1;
}

// --- the limit ------------------------------------------------------------

struct limit_arg {
    uint32_t msr;
    int hwp;             // IA32_HWP_REQUEST: its minimum and desired come down with the maximum
    uint64_t set;        // bits to set beside the ratio (PERF_CTL's turbo off)
    uint64_t *saved;     // this core's slot
    uint8_t *saved_ok;
    int restore;         // write *saved back
    uint32_t ratio;      // else: bits 15:8 of the saved value become this
};

static void limit_here(void *arg) {
    struct limit_arg *a = arg;
    if (!*a->saved_ok) {
        *a->saved = rdmsr(a->msr);
        *a->saved_ok = 1;
    }
    if (a->restore) {
        wrmsr(a->msr, *a->saved);
        return;
    }
    uint64_t v = (*a->saved & ~0xFF00ull) | ((uint64_t)(a->ratio & 0xFF) << 8) | a->set;
    if (a->hwp) {
        // A maximum under the minimum is undefined (SDM 15.4.4): a firmware
        // "performance" profile sets min = max = highest, and the ceiling
        // would do nothing. intel_pstate lowers them together.
        if ((v & 0xFF) > a->ratio) v = (v & ~0xFFull) | a->ratio;
        uint64_t desired = (v >> 16) & 0xFF;
        if (desired > a->ratio) v = (v & ~0xFF0000ull) | ((uint64_t)a->ratio << 16);
    }
    wrmsr(a->msr, v);
}

// The ceiling for `pct` between the CPU's lowest and highest ratio.
static uint32_t ratio_for(int pct, uint32_t lo, uint32_t hi) {
    if (hi < lo) hi = lo;
    return lo + (uint32_t)(((uint64_t)(hi - lo) * (uint64_t)pct + 50) / 100);
}

const char *cputherm_limit(int pct) {
    plan();
    if (!(g_have & (CPUTHERM_HWP | CPUTHERM_PERFCTL))) return g_why ? g_why : "no frequency control";
    if (pct >= 100 && !g_limited) return 0;
    int n = smp_cpu_count();
    if (n < 1) n = 1;
    if (!g_saved) {
        g_saved = kzalloc(sizeof *g_saved * (unsigned)n);
        g_saved_ok = kzalloc((unsigned)n);
        if (!g_saved || !g_saved_ok) {
            kfree(g_saved);
            kfree(g_saved_ok);
            g_saved = 0;
            g_saved_ok = 0;
            return "no memory for the saved ratios";
        }
    }
    struct limit_arg a = { 0, 0, 0, 0, 0, pct >= 100, 0 };
    if (g_have & CPUTHERM_HWP) {
        uint64_t caps = rdmsr(MSR_IA32_HWP_CAPABILITIES);
        a.msr = MSR_IA32_HWP_REQUEST;
        a.hwp = 1;
        a.ratio = ratio_for(pct, (uint32_t)((caps >> 24) & 0xFF), (uint32_t)(caps & 0xFF));
    } else {
        uint64_t pi = rdmsr(MSR_PLATFORM_INFO);
        uint32_t lo = (uint32_t)((pi >> 40) & 0xFF), hi = (uint32_t)((pi >> 8) & 0xFF);
        a.msr = MSR_IA32_PERF_CTL;
        a.ratio = ratio_for(pct, lo, hi);
        a.set = PERF_CTL_TURBO_OFF;
    }
    int done = 0, want = 0;
    for (int i = 0; i < n; i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (n > 1 && (!c || !smp_apic_online(c->apic_id))) continue;
        want++;
        a.saved = &g_saved[i];
        a.saved_ok = &g_saved_ok[i];
        done += smp_call(i, limit_here, &a, 1);
    }
    g_limited = pct < 100;
    return done == want ? 0 : "a core did not take the limit";
}
