// `lscpu` -- what the CPU is, what it supports, and what this kernel
// has actually turned on.
//
// Named after Linux's lscpu, and a sibling of this project's own lspci:
// a real ring-3 ELF in /bin driven by a syscall (SYS_CPU_INFO), not a
// shell builtin.
//
// **The `enabled` column is the part Linux's lscpu doesn't have**, and
// it's the reason this is more than a CPUID dump. CPUID answers "can
// this silicon do X"; it says nothing about whether the OS switched X
// on. Those are different questions with different answers -- SSE2 is
// supported by every x86-64 CPU ever built and was NOT enabled in this
// kernel until CR4.OSFXSR got set. So a supported feature that needs an
// OS opt-in prints as `sse2*` with the star explained in the legend,
// and the "Enabled by this kernel" block below lists each control-
// register bit by name and source.
//
// Why a syscall at all, when CPUID is unprivileged and this program
// could just execute it: CR0/CR4/EFER are privileged and ring 3 cannot
// read them, so the `enabled` half has to come from the kernel
// regardless. Given that, taking the whole struct in one call keeps the
// decoding (family/model combining, leaf-4 cache arithmetic) in one
// place. See api/cpuinfo.h.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h> // strlen, memcpy
#include "knum.h"       // k_utoa/k_htoa -- fixed-width hex has no kfmt
                         // conversion (its printf has no `*` width)
#include "cpuinfo.h"
#include "cpu_features.h" // the (word,bit) -> name table, shared with the Control Panel
#include "lib/uargs.h"

// The sink. Everything below formats into a buffer and hands it here;
// the conversions themselves are the toolkit's, not this file's.
static void put(const char *s) {
    sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)s, strlen(s));
}

static void put_udec(uint32_t v) {
    char buf[21]; // knum.h documents 21 as always sufficient
    k_utoa(v, buf, sizeof buf);
    put(buf);
}

// Fixed width, no "0x" prefix -- see knum.h on why k_htoa leaves the
// prefix to the caller.
static void put_hex(uint32_t v, int digits) {
    char buf[17];
    k_htoa(v, buf, sizeof buf, (unsigned)digits);
    put(buf);
}

// Left-justified label, so the values line up in a column. Still a pad
// loop rather than a format string: kfmt's printf has zero-pad widths
// for numbers only -- no left-justify and no width on %s.
static void put_label(const char *s) {
    put(s);
    put(":");
    int pad = 22 - (int)strlen(s) - 1;
    for (int i = 0; i < pad; i++) put(" ");
}

// A cache size reads better as "32 KiB" or "16 MiB" than "16384 KiB".
static void put_cache_size(uint32_t kb) {
    if (kb >= 1024 && (kb % 1024) == 0) {
        put_udec(kb / 1024);
        put(" MiB");
    } else {
        put_udec(kb);
        put(" KiB");
    }
}

static struct cpu_info g_ci; // a global, not a stack local: 300+ bytes,
                              // and the user stack here is a single page
static struct cpu_info g_ci2; // the second APERF/MPERF sample

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// "22 (2200 MHz)" -- a ratio in units of the 100 MHz bus clock.
static void put_ratio(uint32_t r) {
    put_udec(r);
    put(" (");
    put_udec(r * CPU_FREQ_BCLK_MHZ);
    put(" MHz)");
}

// The clock block. Read-only: SaunaOS writes no ratio, so what this
// prints is what the firmware left the CPU running at.
static void put_frequency(void) {
    if (g_ci.freq_skip != CPU_FREQ_READ) {
        put_label("Frequency");
        put(g_ci.freq_skip == CPU_FREQ_SKIP_HYPERVISOR
             ? "not read -- a guest's frequency MSRs describe no real clock\n"
             : "not read -- only Intel's MSR layout is decoded\n");
        return;
    }
    uint32_t have = g_ci.freq_have;

    if (have & CPU_FREQ_HAVE_PERF) {
        uint32_t now = (uint32_t)(g_ci.perf_status >> 8) & 0xFF;
        uint32_t asked = (uint32_t)(g_ci.perf_ctl >> 8) & 0xFF;
        put_label("Ratio now");
        put_ratio(now);
        put("  (IA32_PERF_STATUS)\n");
        put_label("Ratio requested");
        if (asked) put_ratio(asked);
        else put("none written");
        put("  (IA32_PERF_CTL, as the firmware left it)\n");
    }

    // Busy clock: TSC rate * dAPERF/dMPERF over a spin of ~100 ms --
    // turbostat's Bzy_MHz. Both count only outside idle, so this is the
    // clock work gets, not an average with the halted time.
    if ((have & CPU_FREQ_HAVE_APERF) && g_ci.mhz) {
        uint64_t span = (uint64_t)g_ci.mhz * 100000u;
        while (rdtsc() - g_ci.tsc < span) __asm__ volatile ("pause");
        if (sys_call(SYS_CPU_INFO, (uint64_t)(uintptr_t)&g_ci2, 0, 0) == 0) {
            uint64_t da = g_ci2.aperf - g_ci.aperf;
            uint64_t dm = g_ci2.mperf - g_ci.mperf;
            uint64_t dt = g_ci2.tsc - g_ci.tsc;
            put_label("Busy clock");
            if (dm) {
                put_udec((uint32_t)((uint64_t)g_ci.mhz * da / dm));
                put(" MHz over ");
                put_udec((uint32_t)(dt / ((uint64_t)g_ci.mhz * 1000u)));
                put(" ms of load  (APERF/MPERF)\n");
            } else {
                put("unknown -- MPERF did not count\n");
            }
        }
    }

    if (have & CPU_FREQ_HAVE_RATIOS) {
        put_label("Ratio range");
        put("lowest ");
        put_ratio((uint32_t)(g_ci.platform_info >> 40) & 0xFF);
        put(", base ");
        put_ratio((uint32_t)(g_ci.platform_info >> 8) & 0xFF);
        put("  (MSR_PLATFORM_INFO)\n");

        // One byte per busy-core count; a part with fewer cores leaves
        // the rest 0 or repeats the last.
        uint32_t n = g_ci.cores ? g_ci.cores : 1;
        if (n > 8) n = 8;
        put_label("Turbo ratios");
        for (uint32_t i = 0; i < n; i++) {
            if (i) put(", ");
            put_udec((uint32_t)(g_ci.turbo_ratio_limit >> (8 * i)) & 0xFF);
            put(" with ");
            put_udec(i + 1);
        }
        put(" core(s) busy  (MSR_TURBO_RATIO_LIMIT)\n");
    }

    if (have & CPU_FREQ_HAVE_MISC) {
        int turbo_off = (int)((g_ci.misc_enable >> 38) & 1);
        int ida_off = (have & CPU_FREQ_HAVE_PERF) && ((g_ci.perf_ctl >> 32) & 1);
        put_label("Turbo");
        if (turbo_off)      put("disabled by firmware  (IA32_MISC_ENABLE bit 38)\n");
        else if (ida_off)   put("disengaged  (IA32_PERF_CTL bit 32)\n");
        else if (have & CPU_FREQ_HAVE_TURBO) put("available  (CPUID leaf 06H)\n");
        else                put("not available\n");
        put_label("SpeedStep");
        put(((g_ci.misc_enable >> 16) & 1)
             ? "enabled -- the CPU obeys a ratio written to IA32_PERF_CTL\n"
             : "disabled -- IA32_MISC_ENABLE bit 16 clear, so no ratio can be set\n");
    }

    put_label("HWP");
    if (!(have & CPU_FREQ_HAVE_HWP))  put("not supported  (Skylake and later have it)\n");
    else if (g_ci.pm_enable & 1)      put("on -- the CPU picks its own ratio\n");
    else                              put("supported, off  (IA32_PM_ENABLE)\n");
}

static const struct uargs_opt OPTS[] = { { 0 } };

static const struct uargs_prog PROG = {
    .name = "lscpu",
    .summary = "Show the CPU: identity, clock, caches, what this kernel enabled, flags.",
    .opts = OPTS,
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc) return uargs_error(&PROG, "takes no arguments");

    if (sys_call(SYS_CPU_INFO, (uint64_t)(uintptr_t)&g_ci, 0, 0) != 0) {
        put("lscpu: SYS_CPU_INFO failed\n");
        return 1;
    }

    // ---- identity ------------------------------------------------------
    put_label("Vendor");        put(g_ci.vendor);  put("\n");
    if (g_ci.brand[0]) {
        // The brand string is right-padded with spaces by many CPUs
        // (it's a fixed 48-byte field), so trim before printing.
        char trimmed[CPU_BRAND_LEN];
        int n = (int)strlen(g_ci.brand);
        while (n > 0 && g_ci.brand[n - 1] == ' ') n--;
        memcpy(trimmed, g_ci.brand, (size_t)n);
        trimmed[n] = '\0';
        // Leading spaces happen too, on Intel parts especially.
        const char *p = trimmed;
        while (*p == ' ') p++;
        put_label("Model name"); put(p); put("\n");
    }
    put_label("Family");        put_udec(g_ci.family);   put("\n");
    put_label("Model");         put_udec(g_ci.model);    put("\n");
    put_label("Stepping");      put_udec(g_ci.stepping); put("\n");

    put_label("CPU MHz");
    if (g_ci.mhz_source == CPU_MHZ_UNKNOWN) {
        put("unknown\n");
    } else {
        put_udec(g_ci.mhz);
        put(g_ci.mhz_source == CPU_MHZ_CPUID_16H
             ? "  (CPUID leaf 16H, base frequency)\n"
             : "  (measured: RDTSC against the PIT)\n");
    }

    put_frequency();

    put_label("Max CPUID leaf"); put("0x"); put_hex(g_ci.max_leaf, 8);
    put("  extended 0x"); put_hex(g_ci.max_ext_leaf, 8); put("\n");

    // ---- what the firmware says exists ---------------------------------
    //
    // CPUID describes THE CORE THIS CODE IS RUNNING ON and cannot count
    // the others; the ACPI MADT is the only thing that can, which is
    // why this comes from a different source than everything above.
    // `online` is always no -- this kernel schedules on one core.
    {
        int cores = 0, enabled = 0;
        struct query_cpu c;
        QUERY_FOREACH(QUERY_CPUS, c, i) {
            cores++;
            if (c.flags & QUERY_CPU_ENABLED) enabled++;
        }
        put_label("Logical CPUs");
        if (cores == 0) {
            put("1  (no ACPI MADT -- nothing counted the others)\n");
        } else {
            put_udec((uint32_t)cores);
            put("  (");
            put_udec((uint32_t)enabled);
            put(" enabled by firmware, 1 online -- this kernel is single-core)\n");
        }
    }

    if (g_ci.cores) {
        put_label("Topology");
        put_udec(g_ci.cores); put(" core(s) per package, ");
        put_udec(g_ci.threads_per_core); put(" thread(s) per core  (CPUID leaf 0BH)\n");
    }

    // ---- caches --------------------------------------------------------
    put("\nCaches:\n");
    if (g_ci.cache_count == 0) {
        put("  (CPUID leaf 4 reported none)\n");
    }
    for (int i = 0; i < g_ci.cache_count; i++) {
        const struct cpu_cache *c = &g_ci.cache[i];
        put("  L");
        put_udec(c->level);
        put(" ");
        put(cpu_cache_type_name(c->type));
        // Pad the type name out to the width of "instruction".
        for (int p = (int)strlen(cpu_cache_type_name(c->type)); p < 12; p++) put(" ");
        put_cache_size(c->size_kb);
        put(", ");
        if (c->ways == 0xFFFF) {
            put("fully associative");
        } else {
            put_udec(c->ways);
            put("-way");
        }
        put(", ");
        put_udec(c->line_size);
        put(" B lines, ");
        put_udec(c->sets);
        put(" sets\n");
    }

    // ---- what the kernel switched on -----------------------------------
    put("\nEnabled by this kernel:\n");
    for (int i = 0; i < CPU_ENABLE_COUNT; i++) {
        put("  ");
        put((g_ci.enabled & CPU_ENABLES[i].bit) ? "[on ] " : "[off] ");
        put(CPU_ENABLES[i].name);
        for (int p = (int)strlen(CPU_ENABLES[i].name); p < 16; p++) put(" ");
        put(CPU_ENABLES[i].source);
        put("\n");
    }

    // ---- supported features --------------------------------------------
    put("\nFlags (* = supported but NOT enabled by this kernel):\n ");
    int col = 1;
    int gated_off = 0;
    for (int i = 0; i < CPU_FEATURE_COUNT; i++) {
        const struct cpu_feature_desc *f = &CPU_FEATURES[i];
        if (!cpu_has_feature(&g_ci, f->word, f->bit)) continue;

        int needs = f->enable != 0;
        int off = needs && !(g_ci.enabled & f->enable);
        if (off) gated_off++;

        int len = (int)strlen(f->name) + (off ? 1 : 0) + 1;
        if (col + len > 78) { put("\n "); col = 1; }
        put(" ");
        put(f->name);
        if (off) put("*");
        col += len;
    }
    put("\n");

    if (gated_off == 0) {
        put("\nEvery gated feature the CPU supports is enabled.\n");
    } else {
        put("\n");
        put_udec((uint32_t)gated_off);
        put(" supported feature(s) are not enabled -- see the block above\n");
        put("for which control-register bit each one waits on.\n");
    }
    return 0;
}
