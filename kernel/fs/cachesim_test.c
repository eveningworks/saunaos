// Tests for the ghost cache's LRU (cachesim.h). A ghost cache that
// reports the wrong hits is a measurement that decides the wrong design,
// so these pin the three things its numbers rest on: a repeat hits, the
// LEAST-recent key is the one evicted, and a changed generation misses.
#include "ktest.h"
#include "cachesim.h"

#define CAP 4
static struct cachesim_entry g_e[CAP];
static uint32_t g_b[CAP];
static struct cachesim_lru g_c;

static void fresh(void) { cachesim_lru_init(&g_c, g_e, g_b, CAP); }

KTEST("cachesim", "a key misses once, then hits") {
    fresh();
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 7, 0, 1), 0);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 7, 0, 1), 1);
    KTEST_ASSERT_EQ(g_c.lookups, 2);
    KTEST_ASSERT_EQ(g_c.hits, 1);
}

KTEST("cachesim", "the least-recently used key is the one evicted") {
    fresh();
    for (uint64_t k = 1; k <= CAP; k++) cachesim_lru_touch(&g_c, k, 0, 1);
    cachesim_lru_touch(&g_c, 1, 0, 1);          // 1 is now the most recent
    cachesim_lru_touch(&g_c, 100, 0, 1);        // evicts 2, the least recent
    KTEST_ASSERT_EQ(g_c.used, CAP);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 1, 0, 1), 1);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 100, 0, 1), 1);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 2, 0, 1), 0);
}

KTEST("cachesim", "keys sharing a bucket are told apart and evicted cleanly") {
    fresh();
    // CAP buckets: keys CAP apart land in one chain.
    for (uint64_t i = 0; i < CAP; i++) cachesim_lru_touch(&g_c, 3 + i * CAP, 0, 1);
    for (uint64_t i = 0; i < CAP; i++)
        KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 3 + i * CAP, 0, 1), 1);
    // A full cycle of new keys through the same chain evicts every old one.
    for (uint64_t i = 0; i < CAP; i++) cachesim_lru_touch(&g_c, 1000 * CAP + 3 + i * CAP, 0, 1);
    for (uint64_t i = 0; i < CAP; i++)
        KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 3 + i * CAP, 0, 0), 0);
}

KTEST("cachesim", "a changed generation is a miss, and the new one then hits") {
    fresh();
    cachesim_lru_touch(&g_c, 9, 0, 1);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 9, 1, 1), 0);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 9, 1, 1), 1);
}

KTEST("cachesim", "an uncounted touch (a write) keeps a key without counting a lookup") {
    fresh();
    cachesim_lru_touch(&g_c, 5, 0, 0);
    KTEST_ASSERT_EQ(g_c.lookups, 0);
    KTEST_ASSERT_EQ(cachesim_lru_touch(&g_c, 5, 0, 1), 1);
    KTEST_ASSERT_EQ(g_c.hits, 1);
}
