// thermal_policy.c: the unit conversion and passive cooling's step, with
// the ASUS UX305FA's constants (_TC1 2, _TC2 10).
#include "thermal_policy.h"
#include "ktest.h"

#define C(x) ((int64_t)(x) * 1000)

KTEST("thermal", "deci-kelvin reads as millidegrees; placeholders read as none") {
    KTEST_ASSERT_EQ(thermal_dk_to_mc(3232), 50050);          // KELV(50) = 0x0AAC + 500
    KTEST_ASSERT_EQ(thermal_dk_to_mc(2732 + 1000), 100050);
    KTEST_ASSERT_EQ(thermal_dk_to_mc(0), THERMAL_NONE);
    KTEST_ASSERT_EQ(thermal_dk_to_mc(2732), THERMAL_NONE);
    KTEST_ASSERT_EQ(thermal_dk_to_mc(0xFFFFFFFFull), THERMAL_NONE);
}

KTEST("thermal", "below the passive trip nothing is slowed") {
    struct thermal_passive s = { 0, 100, 0 };
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(80), C(95), 2, 10), 100);
    KTEST_ASSERT_EQ(s.active, 0);
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(90), THERMAL_NONE, 2, 10), 100);
}

KTEST("thermal", "over the trip the limit falls by _TC2 per kelvin, faster while heating") {
    struct thermal_passive s = { 0, 100, 0 };
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(96), C(95), 2, 10), 90);     // 10 * 1 K
    KTEST_ASSERT_EQ(s.active, 1);
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(98), C(95), 2, 10), 56);     // 2*2 + 10*3 = 34
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(98), C(95), 2, 10), 26);     // steady: 30
}

KTEST("thermal", "cooling below the trip gives speed back and ends the spell at 100") {
    struct thermal_passive s = { 1, 40, C(96) };
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(93), C(95), 2, 10), 66);     // -6 - 20 = -26
    KTEST_ASSERT_EQ(s.active, 1);
    KTEST_ASSERT_EQ(thermal_passive_step(&s, C(90), C(95), 2, 10), 100);    // -6 - 50, clamped
    KTEST_ASSERT_EQ(s.active, 0);
}

KTEST("thermal", "the limit never reaches 0, and a small excess still moves it") {
    struct thermal_passive s = { 0, 100, 0 };
    for (int i = 0; i < 40; i++) thermal_passive_step(&s, C(110), C(95), 2, 10);
    KTEST_ASSERT_EQ(s.perf_pct, THERMAL_PERF_MIN);
    struct thermal_passive t = { 0, 100, 0 };
    KTEST_ASSERT_EQ(thermal_passive_step(&t, C(95) + 60, C(95), 2, 10), 99);   // 0.6% rounds to 1
}
