// RAM-sized ceilings: the arithmetic, and the one log line.
#include "ktest.h"
#include "klimit.h"
#include "klog.h"

KTEST("klimit", "a ceiling is RAM / fraction / entry, clamped to its floor and ceiling") {
    const uint64_t gib2 = 2ull << 30;
    KTEST_ASSERT_EQ(klimit_from_ram(gib2, 16, 4096, 1024, 1u << 20), 32768u);
    KTEST_ASSERT_EQ(klimit_from_ram(gib2, 16, 4096, 1024, 1000), 1000u);    // capped
    KTEST_ASSERT_EQ(klimit_from_ram(1u << 20, 16, 4096, 64, 1u << 20), 64u); // floored
    KTEST_ASSERT_EQ(klimit_from_ram(gib2, 0, 4096, 77, 77), 77u);           // fixed

    static struct klimit fixed = KLIMIT_FIXED("klimit ktest: fixed", 1234);
    KTEST_ASSERT_EQ(klimit_get(&fixed), 1234u);
    static struct klimit ram = KLIMIT("klimit ktest: ram", 64, 4096, 8, 1u << 30);
    KTEST_ASSERT(klimit_get(&ram) > 8);   // every machine this boots on has > 2 MiB
}

KTEST("klimit", "reaching a ceiling logs once, and a second hit is silent") {
    static struct klimit l = KLIMIT("klimit ktest: once", 64, 4096, 8, 1u << 30);
    klimit_test_set(&l, 5);
    uint64_t a = klog_total_bytes();
    klimit_hit(&l);
    int first = klimit_test_logged(&l, a);
    uint64_t b = klog_total_bytes();
    klimit_hit(&l);
    int second = klimit_test_logged(&l, b);
    KTEST_ASSERT(first);
    KTEST_ASSERT(!second);
}
