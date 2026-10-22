// Free page reporting: the policy half (page_report.h). pmm.c keeps the
// books; this decides WHEN to walk them and hands each batch to the one
// registered reporter.
//
// WHEN: a walk is due 2 s after a block's worth of frames has been freed
// (Linux's PAGE_REPORTING_DELAY), so memory freed and promptly reused is
// not reported for nothing, and it runs a few batches per idle pass so a
// first walk over a large machine does not hold the idle loop. Due work
// asks for its wake, since a tickless idle may otherwise sleep through it.
#include "page_report.h"
#include "clocksource.h"
#include "clockevent.h"
#include "klog.h"
#include "kfmt.h"

#define DELAY_NS          2000000000ull
#define BATCHES_PER_PASS  8
// A zone is never isolated below this many free frames (32 MiB, plus
// DMA32's reserve there), so a report in flight is not what fails a
// single-frame allocation. A large contiguous run can still find a free
// stretch split by isolated blocks for the moment a batch is out.
#define KEEP_FREE_FRAMES  8192

static page_report_fn g_fn;
static const char *g_name;
static int g_max_runs;     // the reporter's capacity, at most a batch
static int g_busy;         // a walk is running; refuses a nested one
static int g_dead;         // the device stopped answering
static int g_first_done;
static uint64_t g_due_ns;  // 0: no walk pending
static uint64_t g_cursor;  // the block the current walk resumes at
static uint64_t g_reported_bytes;
static int g_refusals_logged;

int page_report_register(const char *name, page_report_fn fn, int max_runs) {
    if (g_fn || !fn || max_runs < 1) return 0;
    g_name = name;
    g_fn = fn;
    g_max_runs = max_runs < PAGE_REPORT_BATCH_BLOCKS ? max_runs : PAGE_REPORT_BATCH_BLOCKS;
    g_due_ns = clocksource_now_ns() + DELAY_NS;
    klog_printf("page-report: %s takes free 2 MiB blocks back for the host\n", name);
    return 1;
}

const char *page_report_name(void) { return g_fn ? g_name : 0; }
uint64_t page_report_reported_bytes(void) { return g_reported_bytes; }

// Up to `batches` batches from *cursor, in runs of at most `max_runs`.
// Returns 1 when the walk is over -- it reached the end, or the reporter
// refused or was lost. `live` is 0 for the KTEST's fake reporter, which
// must not move the gate, the totals or the device's state.
static int walk(page_report_fn fn, uint64_t *cursor, int max_runs, int live, int batches,
                int *done_batches) {
    if (live && *cursor == 0) pmm_report_reset_freed(); // a free after this starts the next walk
    for (int i = 0; i < batches; i++) {
        struct page_report_run runs[PAGE_REPORT_BATCH_BLOCKS];
        int n = pmm_report_isolate(cursor, runs, max_runs, KEEP_FREE_FRAMES);
        if (n == 0) return 1;
        int r = fn(runs, n);
        if (done_batches) (*done_batches)++;
        if (r == PAGE_REPORT_LOST && live) {
            // Leaked on purpose: a device that never acknowledged may
            // still discard them, under whoever allocated them next.
            g_dead = 1;
            klog_write(KLOG_ERR "page-report: the reporter stopped answering -- reporting off, "
                       "the frames in flight are never reused\n");
            return 1;
        }
        pmm_report_release(runs, n, r == PAGE_REPORT_TAKEN);
        if (r != PAGE_REPORT_TAKEN) {
            // Within the declared capacity a refusal means the reporter is
            // short of something; the next walk tries again.
            if (live && g_refusals_logged < 3) {
                g_refusals_logged++;
                klog_printf(KLOG_WARN "page-report: %s refused a batch of %d runs\n", g_name, n);
            }
            return 1;
        }
        for (int k = 0; live && k < n; k++) g_reported_bytes += runs[k].len;
    }
    return 0;
}

void page_report_idle(void) {
    if (!g_fn || g_dead || g_busy) return;
    uint64_t now = clocksource_now_ns();
    if (!g_due_ns) {
        if (pmm_report_freed() < PAGE_REPORT_BLOCK_FRAMES) return;
        g_due_ns = now + DELAY_NS;
    }
    if (now < g_due_ns) {
        clockevent_idle_wake_by(g_due_ns);
        return;
    }
    g_busy = 1;
    if (walk(g_fn, &g_cursor, g_max_runs, 1, BATCHES_PER_PASS, 0)) {
        g_cursor = 0;
        g_due_ns = 0;
        if (!g_first_done && !g_dead) {
            g_first_done = 1;
            klog_printf("page-report: first walk done, %lu MiB reported\n",
                        g_reported_bytes >> 20);
        }
    } else {
        clockevent_idle_wake_by(now);  // more to walk: the next idle pass
    }
    g_busy = 0;
}

int page_report_walk_for_test(page_report_fn fn) {
    if (g_busy) return -1;
    g_busy = 1;
    uint64_t cursor = 0;
    int batches = 0;
    while (!walk(fn, &cursor, PAGE_REPORT_BATCH_BLOCKS, 0, BATCHES_PER_PASS, &batches)) {}
    g_busy = 0;
    return batches;
}
