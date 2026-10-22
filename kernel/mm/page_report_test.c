// KTESTs for free page reporting's core (page_report.h), through a fake
// reporter rather than the device: what a walk hands out, that it gives
// every frame back, and that the reported mark is set and cleared at the
// right times. Each test ends with pmm_report_forget_all(): the marks a
// fake report set are cleared, so the real reporter's next walk (after
// its usual gate) re-reports those blocks rather than skipping them.
#include "page_report.h"
#include "pmm.h"
#include "ktest.h"

#define BLOCK_BYTES ((uint64_t)PAGE_REPORT_BLOCK_FRAMES * 4096)
#define MAX_SEEN 512

static struct page_report_run g_seen[MAX_SEEN];
static int g_nseen, g_record, g_bad, g_ret;

static int fake(const struct page_report_run *runs, int n) {
    if (n < 1 || n > PAGE_REPORT_BATCH_BLOCKS) g_bad |= 4;
    for (int i = 0; i < n; i++) {
        if (runs[i].phys % BLOCK_BYTES || !runs[i].len || runs[i].len % BLOCK_BYTES) g_bad |= 1;
        // Isolated while the host has them: used, so nothing can take one.
        for (uint64_t p = runs[i].phys; p < runs[i].phys + runs[i].len; p += 4096)
            if (!pmm_frame_is_managed(p) || !pmm_frame_is_used(p)) { g_bad |= 2; break; }
        if (g_record && g_nseen < MAX_SEEN) g_seen[g_nseen] = runs[i];
        g_nseen++;
    }
    return g_ret;
}

static void reset(int ret, int record) { g_nseen = 0; g_bad = 0; g_ret = ret; g_record = record; }

static int seen_contains(uint64_t phys) {
    for (int i = 0; i < g_nseen && i < MAX_SEEN; i++)
        if (phys >= g_seen[i].phys && phys < g_seen[i].phys + g_seen[i].len) return 1;
    return 0;
}

KTEST("page-report", "a walk hands out isolated whole blocks and gives every frame back") {
    if (!pmm_report_blocks()) KTEST_SKIP("no whole 2 MiB block under management");
    uint64_t held = pmm_alloc_frame(PMM_ZONE_ANY);   // in use: no run may cover it
    KTEST_ASSERT(held != 0);
    pmm_report_forget_all();
    reset(PAGE_REPORT_TAKEN, 1);
    uint64_t before = pmm_free_frames();
    int batches = page_report_walk_for_test(fake);
    uint64_t after = pmm_free_frames();
    int held_seen = seen_contains(held);
    int held_marked = pmm_report_block_reported(held);
    pmm_free_frame(held);
    pmm_report_forget_all();

    KTEST_ASSERT(batches > 0);
    KTEST_ASSERT(g_nseen > 0);
    KTEST_ASSERT(g_bad == 0);
    KTEST_ASSERT(after == before);
    KTEST_ASSERT(!held_seen);
    KTEST_ASSERT(!held_marked);
}

KTEST("page-report", "an allocation in a reported block makes it reportable again") {
    if (!pmm_report_blocks()) KTEST_SKIP("no whole 2 MiB block under management");
    pmm_report_forget_all();
    reset(PAGE_REPORT_TAKEN, 1);
    page_report_walk_for_test(fake);

    // Two blocks' worth of contiguous frames hold one whole aligned
    // block, and that block was wholly free -- so reported -- before.
    uint64_t run = pmm_alloc_contiguous(2 * PAGE_REPORT_BLOCK_FRAMES, PMM_ZONE_ANY);
    uint64_t blk = (run + BLOCK_BYTES - 1) & ~(BLOCK_BYTES - 1);
    int was_reported = run && seen_contains(blk);
    int marked_after_alloc = run ? pmm_report_block_reported(blk) : 1;
    if (run) pmm_free_contiguous(run, 2 * PAGE_REPORT_BLOCK_FRAMES);

    reset(PAGE_REPORT_TAKEN, 1);
    page_report_walk_for_test(fake);
    int reported_again = run && seen_contains(blk);
    pmm_report_forget_all();

    KTEST_ASSERT(run != 0);
    KTEST_ASSERT(was_reported);
    KTEST_ASSERT(!marked_after_alloc);
    KTEST_ASSERT(reported_again);
}

KTEST("page-report", "a refused report gives the frames back unreported") {
    if (!pmm_report_blocks()) KTEST_SKIP("no whole 2 MiB block under management");
    pmm_report_forget_all();
    reset(PAGE_REPORT_REFUSED, 1);
    uint64_t before = pmm_free_frames();
    int batches = page_report_walk_for_test(fake);
    uint64_t after = pmm_free_frames();
    int marked = g_nseen ? pmm_report_block_reported(g_seen[0].phys) : 1;
    pmm_report_forget_all();

    KTEST_ASSERT(batches == 1);   // a refusal ends the walk
    KTEST_ASSERT(g_bad == 0);
    KTEST_ASSERT(after == before);
    KTEST_ASSERT(!marked);
}
