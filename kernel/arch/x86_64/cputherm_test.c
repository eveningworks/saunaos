// cputherm_plan(): which sensors and which frequency knob a CPU gets,
// from CPUID answers alone -- the gate that keeps a #GP away.
#include "cpuinfo.h"
#include "cputherm.h"
#include "ktest.h"
#include "string.h"

static void cpu(struct cpu_info *ci, const char *vendor, uint16_t family, uint16_t model) {
    k_memset(ci, 0, sizeof *ci);
    k_strlcpy(ci->vendor, vendor, sizeof ci->vendor);
    ci->family = family;
    ci->model = model;
    ci->max_leaf = 0xD;
}

KTEST("cputherm", "sensors and the frequency knob only where the MSRs exist") {
    const uint32_t eist = 1u << 7, hv = 1u << 31;
    const uint32_t dts = 1u << 0, ptm = 1u << 6, hwp = 1u << 7;
    const char *why;

    // The ASUS's Broadwell-Y: both sensors, TjMax, PERF_CTL.
    struct cpu_info bdw;
    cpu(&bdw, "GenuineIntel", 6, 0x3D);
    KTEST_ASSERT_EQ((int)cputherm_plan(&bdw, eist, dts | ptm, &why),
                    (int)(CPUTHERM_DTS | CPUTHERM_PTM | CPUTHERM_TJMAX | CPUTHERM_PERFCTL));
    KTEST_ASSERT(why == 0);

    // Kaby Lake with HWP: the HWP request, never PERF_CTL as well.
    struct cpu_info kbl;
    cpu(&kbl, "GenuineIntel", 6, 0x9E);
    KTEST_ASSERT_EQ((int)cputherm_plan(&kbl, eist, dts | ptm | hwp, &why),
                    (int)(CPUTHERM_DTS | CPUTHERM_PTM | CPUTHERM_TJMAX | CPUTHERM_HWP));

    // A guest: nothing, whatever CPUID claims.
    KTEST_ASSERT_EQ((int)cputherm_plan(&bdw, eist | hv, dts | ptm, &why), 0);
    KTEST_ASSERT(why != 0);

    struct cpu_info amd;
    cpu(&amd, "AuthenticAMD", 0x17, 0x71);
    KTEST_ASSERT_EQ((int)cputherm_plan(&amd, eist, dts, &why), 0);

    // An Atom: sensors, but no model-table MSRs -- so no TjMax and no knob.
    struct cpu_info atom;
    cpu(&atom, "GenuineIntel", 6, 0x5C);
    KTEST_ASSERT_EQ((int)cputherm_plan(&atom, eist, dts | ptm, &why), (int)(CPUTHERM_DTS | CPUTHERM_PTM));
    KTEST_ASSERT(why != 0);

    // No leaf 6: nothing is asked of it.
    bdw.max_leaf = 5;
    KTEST_ASSERT_EQ((int)cputherm_plan(&bdw, eist, dts | ptm, &why), 0);
}
