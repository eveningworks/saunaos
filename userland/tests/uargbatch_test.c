// lib/uargbatch: a long argument list split across spawns, each argv
// within SPAWN_ARGS_MAX. The "spawn" here records what it was handed,
// so the test reads every batch back rather than trusting a count.
#include <string.h>
#include "syscall_abi.h"
#include "lib/uargbatch.h"
#include "lib/uktest.h"

#define ITEMS 300
static char g_item[ITEMS][24];
static char *g_items[ITEMS];

struct seen {
    int runs, items, next, bad_head, too_long;
};

static void record(char *const *argv, void *ctx) {
    struct seen *s = ctx;
    s->runs++;
    size_t len = 0;
    for (int i = 0; argv[i]; i++) len += strlen(argv[i]) + 1;
    if (len > SPAWN_ARGS_MAX) s->too_long++;
    if (strcmp(argv[0], "/bin/trash") || strcmp(argv[1], "put")) s->bad_head++;
    // Every item once, in order, across the batches.
    for (int i = 2; argv[i]; i++, s->items++)
        if (argv[i] != g_items[s->next++]) s->bad_head++;
}

static char *const HEAD[] = { "/bin/trash", "put" };

static void setup(void) {
    for (int i = 0; i < ITEMS; i++) {
        // "/home/Desktop/file-NNN": 22 bytes and a NUL, so a batch holds
        // about forty and 300 need several.
        memcpy(g_item[i], "/home/Desktop/file-000", 23);
        g_item[i][19] = (char)('0' + i / 100);
        g_item[i][20] = (char)('0' + i / 10 % 10);
        g_item[i][21] = (char)('0' + i % 10);
        g_items[i] = g_item[i];
    }
}

static void case_split(void) {
    setup();
    struct seen s = { 0 };
    int runs = uargbatch(HEAD, 2, g_items, ITEMS, record, &s);
    KTEST_ASSERT(runs > 1);
    KTEST_ASSERT_EQ(runs, s.runs);
    KTEST_ASSERT_EQ(s.items, ITEMS);
    KTEST_ASSERT_EQ(s.bad_head, 0);
    KTEST_ASSERT_EQ(s.too_long, 0);
}

static void case_fits_once(void) {
    setup();
    struct seen s = { 0 };
    KTEST_ASSERT_EQ(uargbatch(HEAD, 2, g_items, 3, record, &s), 1);
    KTEST_ASSERT_EQ(s.items, 3);
    struct seen none = { 0 };
    KTEST_ASSERT_EQ(uargbatch(HEAD, 2, g_items, 0, record, &none), 0);
    KTEST_ASSERT_EQ(none.runs, 0);
}

static void case_never_fits(void) {
    setup();
    static char huge[SPAWN_ARGS_MAX + 8];
    memset(huge, 'x', sizeof huge - 1);
    char *items[3] = { g_items[0], huge, g_items[1] };
    struct seen s = { 0 };
    KTEST_ASSERT_EQ(uargbatch(HEAD, 2, items, 3, record, &s), -1);
    KTEST_ASSERT_EQ(s.runs, 1);
    KTEST_ASSERT_EQ(s.too_long, 0);
}

static const struct uktest_case CASES[] = {
    { "300 paths split into batches, every one within SPAWN_ARGS_MAX, each path once in order", case_split },
    { "a list that fits is one spawn, and an empty one none", case_fits_once },
    { "an item too long for any spawn is skipped and reported, the rest still run", case_never_fits },
};

int main(void) {
    utest_begin("uargbatch_test", "an argument list split to fit a spawn", 0);
    uktest_run(CASES, (int)(sizeof CASES / sizeof CASES[0]));
    return utest_end();
}
