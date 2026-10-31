// KTESTs for CPU identification (see api/cpuinfo.h).
//
// These assert on INVARIANTS rather than on specific values, because
// the values legitimately differ per CPU model -- the same kernel image
// boots on `qemu64` (AuthenticAMD, no CPUID leaf 4) and on
// `Skylake-Client` (GenuineIntel, leaf 4 populated), and a test that
// hardcoded either one's answers would fail on the other. Run the same
// suite against a second model with:
//
//     python3 tools/vm.py --cpu Skylake-Client run "ktest cpuid"
#include "cpuinfo.h"
#include "cpu_features.h"
#include "ktest.h"
#include "string.h"

KTEST("cpuid", "vendor and leaf limits look sane") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    // Exactly 12 characters, NUL-terminated. A wrong register order
    // here (EBX,ECX,EDX instead of EBX,EDX,ECX) still produces 12
    // printable characters -- "GenuintelineI" -- so length alone can't
    // catch it; the vendor check below is what does.
    KTEST_ASSERT_EQ((int)k_strlen(ci.vendor), 12);

    int known = (k_strcmp(ci.vendor, "GenuineIntel") == 0) ||
                (k_strcmp(ci.vendor, "AuthenticAMD") == 0);
    KTEST_ASSERT(known);

    // Any CPU that reached long mode answers leaf 1 and the extended
    // leaves at least as far as 80000001H (that's where the long-mode
    // bit itself lives).
    KTEST_ASSERT(ci.max_leaf >= 1);
    KTEST_ASSERT(ci.max_ext_leaf >= 0x80000001u);
}

KTEST("cpuid", "reports the features this kernel depends on") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    // Not arbitrary picks: the kernel is already running because each
    // of these works. If CPUID says otherwise, the decoding is wrong,
    // not the CPU.
    KTEST_ASSERT(cpu_has_feature(&ci, CPU_WORD_EXT1_EDX, 29)); // lm -- we're in long mode
    KTEST_ASSERT(cpu_has_feature(&ci, CPU_WORD_1_EDX, 6));      // pae -- paging is on
    KTEST_ASSERT(cpu_has_feature(&ci, CPU_WORD_1_EDX, 4));      // tsc -- calibration used it
    KTEST_ASSERT(cpu_has_feature(&ci, CPU_WORD_1_EDX, 26));     // sse2 -- mandatory on x86-64
}

KTEST("cpuid", "enabled bits agree with the running kernel") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    // These are observably true: this code is executing.
    KTEST_ASSERT(ci.enabled & CPU_EN_PAGING);
    KTEST_ASSERT(ci.enabled & CPU_EN_LONG_MODE);
    KTEST_ASSERT(ci.enabled & CPU_EN_PAE);
    // Set by fpu_init() at boot -- the cross-check that this file and
    // fpu.c agree about what CR4.OSFXSR says.
    KTEST_ASSERT(ci.enabled & CPU_EN_SSE);
    KTEST_ASSERT(ci.enabled & CPU_EN_NX);
}

KTEST("cpuid", "no control bit is enabled for an unsupported feature") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    // The invariant that actually holds: the kernel must never have set
    // a control-register bit for something the CPU doesn't implement.
    //
    // Note what this does NOT say, because the first version of this
    // test got it wrong and failed: it is NOT "every feature sharing a
    // gate is supported whenever that gate is on". CPU_EN_SSE is one
    // switch (CR4.OSFXSR) that a dozen features in the table hang off,
    // so it's on while plenty of them -- sse4_1 on the default qemu64
    // model -- are absent. The gate answers "is the SSE machinery
    // usable", not "does this specific instruction set exist".
    static const struct { uint32_t en; int word; int bit; } PREREQ[] = {
        { CPU_EN_SSE,       CPU_WORD_1_EDX,    24 }, // OSFXSR needs fxsr
        { CPU_EN_SSE,       CPU_WORD_1_EDX,    25 }, // ...and sse
        { CPU_EN_PAE,       CPU_WORD_1_EDX,     6 },
        { CPU_EN_PGE,       CPU_WORD_1_EDX,    13 },
        { CPU_EN_OSXSAVE,   CPU_WORD_1_ECX,    26 }, // xsave
        { CPU_EN_SMEP,      CPU_WORD_7_0_EBX,   7 },
        { CPU_EN_SMAP,      CPU_WORD_7_0_EBX,  20 },
        { CPU_EN_UMIP,      CPU_WORD_7_0_ECX,   2 },
        { CPU_EN_NX,        CPU_WORD_EXT1_EDX, 20 },
        { CPU_EN_LONG_MODE, CPU_WORD_EXT1_EDX, 29 },
        { CPU_EN_SYSCALL,   CPU_WORD_EXT1_EDX, 11 },
    };
    for (unsigned i = 0; i < sizeof(PREREQ) / sizeof(PREREQ[0]); i++) {
        if (ci.enabled & PREREQ[i].en) {
            KTEST_ASSERT(cpu_has_feature(&ci, PREREQ[i].word, PREREQ[i].bit));
        }
    }
}

KTEST("cpuid", "cache entries are self-consistent") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    KTEST_ASSERT(ci.cache_count <= CPU_MAX_CACHES);
    for (int i = 0; i < ci.cache_count; i++) {
        const struct cpu_cache *c = &ci.cache[i];
        KTEST_ASSERT(c->level >= 1 && c->level <= 4);
        KTEST_ASSERT(c->type != CPU_CACHE_NONE);
        KTEST_ASSERT(c->size_kb > 0);
        // A cache line is a power of two between 16 and 256 bytes on
        // anything real; catches an off-by-one in leaf 4's (value - 1)
        // encoding, which is the easiest mistake to make there.
        KTEST_ASSERT(c->line_size >= 16 && c->line_size <= 256);
        KTEST_ASSERT((c->line_size & (c->line_size - 1)) == 0);
    }
}

KTEST("cpuid", "clock was calibrated at boot") {
    struct cpu_info ci;
    cpu_info_get(&ci);

    // Guards the deadlock fix: if calibration ever moves back to first
    // use, it can't complete inside a syscall (interrupts are off) and
    // this reports UNKNOWN. See cpuid.c's cpu_info_init().
    KTEST_ASSERT(ci.mhz_source != CPU_MHZ_UNKNOWN);
    KTEST_ASSERT(ci.mhz > 0);
}

KTEST("cpuid", "feature table is well-formed") {
    // Pure table validation -- no CPU involved. A typo'd word index
    // would otherwise silently read the wrong register and mislabel
    // every flag from that entry onward.
    for (int i = 0; i < CPU_FEATURE_COUNT; i++) {
        const struct cpu_feature_desc *f = &CPU_FEATURES[i];
        KTEST_ASSERT(f->name != 0);
        KTEST_ASSERT(f->name[0] != '\0');
        KTEST_ASSERT(f->word < CPU_WORD_COUNT);
        KTEST_ASSERT(f->bit < 32);
    }
    for (int i = 0; i < CPU_ENABLE_COUNT; i++) {
        KTEST_ASSERT(CPU_ENABLES[i].name != 0);
        KTEST_ASSERT(CPU_ENABLES[i].source != 0);
        KTEST_ASSERT(CPU_ENABLES[i].bit != 0);
    }
}

// Topology is consistent or absent: threads per core divides the
// package's logical count, and neither is reported without the other.
KTEST("cpuid", "the package topology is consistent") {
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (!ci.cores) KTEST_SKIP("this CPU reports no topology");
    KTEST_ASSERT(ci.threads_per_core >= 1);
    KTEST_ASSERT(ci.cores >= 1);
    KTEST_ASSERT(ci.cores * ci.threads_per_core <= 256);
}

// The frequency MSRs' gating, fed synthetic CPUID answers: a wrong gate
// is a #GP in ring 0 on the machine it misjudges, so each refusal is
// asserted, not only the read.
static void freq_cpu(struct cpu_info *ci, const char *vendor, uint16_t family,
                     uint16_t model) {
    k_memset(ci, 0, sizeof *ci);
    k_strlcpy(ci->vendor, vendor, sizeof ci->vendor);
    ci->family = family;
    ci->model = model;
    ci->max_leaf = 0xD;
}

KTEST("cpuid", "the frequency MSRs are read only where they exist") {
    uint32_t skip;
    const uint32_t eist = 1u << 7, hv = 1u << 31;
    const uint32_t turbo = 1u << 1, hwp = 1u << 7, aperf = 1u << 0;

    // The ASUS's Broadwell-Y: everything but HWP.
    struct cpu_info bdw;
    freq_cpu(&bdw, "GenuineIntel", 6, 0x3D);
    uint32_t have = cpu_freq_plan(&bdw, eist, turbo, aperf, &skip);
    KTEST_ASSERT_EQ((int)skip, CPU_FREQ_READ);
    KTEST_ASSERT_EQ((int)have, (int)(CPU_FREQ_HAVE_MISC | CPU_FREQ_HAVE_PERF |
                                     CPU_FREQ_HAVE_RATIOS | CPU_FREQ_HAVE_APERF |
                                     CPU_FREQ_HAVE_TURBO));

    // A guest reads nothing, whatever its CPUID claims.
    have = cpu_freq_plan(&bdw, eist | hv, turbo | hwp, aperf, &skip);
    KTEST_ASSERT_EQ((int)skip, CPU_FREQ_SKIP_HYPERVISOR);
    KTEST_ASSERT_EQ((int)have, 0);

    struct cpu_info amd;
    freq_cpu(&amd, "AuthenticAMD", 0x17, 0x71);
    have = cpu_freq_plan(&amd, eist, turbo, aperf, &skip);
    KTEST_ASSERT_EQ((int)skip, CPU_FREQ_SKIP_NOT_INTEL);
    KTEST_ASSERT_EQ((int)have, 0);

    // An Atom (Goldmont): no model-table MSRs, and no PERF without EIST.
    struct cpu_info atom;
    freq_cpu(&atom, "GenuineIntel", 6, 0x5C);
    have = cpu_freq_plan(&atom, 0, 0, 0, &skip);
    KTEST_ASSERT_EQ((int)have, (int)CPU_FREQ_HAVE_MISC);

    // CPUID leaf 6 is not asked past max_leaf.
    struct cpu_info old;
    freq_cpu(&old, "GenuineIntel", 6, 0x2A);
    old.max_leaf = 5;
    have = cpu_freq_plan(&old, eist, turbo | hwp, aperf, &skip);
    KTEST_ASSERT_EQ((int)have, (int)(CPU_FREQ_HAVE_MISC | CPU_FREQ_HAVE_PERF |
                                     CPU_FREQ_HAVE_RATIOS));
}

// Under QEMU (every KTEST run) the syscall's path must refuse, reading
// no MSR -- the one gate this suite can exercise for real.
KTEST("cpuid", "a guest reads no frequency MSR") {
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (!cpu_has_feature(&ci, CPU_WORD_1_ECX, 31)) KTEST_SKIP("not under a hypervisor");
    cpu_freq_get(&ci);
    KTEST_ASSERT_EQ((int)ci.freq_have, 0);
    KTEST_ASSERT(ci.freq_skip != CPU_FREQ_READ);
    KTEST_ASSERT(ci.perf_status == 0 && ci.aperf == 0);
}
