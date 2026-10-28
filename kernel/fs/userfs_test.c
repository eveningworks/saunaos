// userfs's handle table: grows past the 16 it was fixed at, and its
// RAM-sized ceiling refuses with one log line. Through the test seam, so
// no share is mounted and no daemon is needed.
#include "ktest.h"
#include "userfs.h"
#include "klimit.h"
#include "klog.h"

KTEST("userfs", "the handle table grows past 16 shares and refuses at its ceiling, logged") {
    int ceiling = (int)klimit_get(&userfs_limit);
    int got = userfs_test_take_slots(30);

    uint32_t was = klimit_test_set(&userfs_limit, 20);
    uint64_t mark = klog_total_bytes();
    int capped = userfs_test_take_slots(100);
    int logged = klimit_test_logged(&userfs_limit, mark);
    klimit_test_set(&userfs_limit, was);

    KTEST_ASSERT(ceiling > 16);
    KTEST_ASSERT_EQ(got, 30);   // past 16; a 256 MiB guest allows 31
    // The table already grew past 20 above, so its existing free slots
    // are all taken before the ceiling stops a further growth.
    KTEST_ASSERT(capped < 100);
    KTEST_ASSERT(logged);
}
