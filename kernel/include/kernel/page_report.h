#ifndef PAGE_REPORT_H
#define PAGE_REPORT_H

// Free page reporting: telling a hypervisor which guest memory is free,
// so the host can take those pages back and the VM costs it only what
// the guest is using.
//
// Linux's mm/page_reporting.c in shape: a generic core walks the frame
// allocator for wholly free 2 MiB blocks, ISOLATES them (marks them used
// so nothing allocates one mid-report), hands them to the one registered
// reporter -- virtio-balloon's reporting queue -- and frees them again.
// A block is reported once and stays marked until something allocates
// from it.
//
// A REPORTED FRAME MAY COME BACK ZERO-FILLED. The host discards what it
// is told about, and the next touch gets a fresh zero page. Nothing may
// rely on a freed frame keeping its contents -- true before this
// existed, enforced by nothing until now.
//
// It runs from scheduler_idle(), never from a free: a report is a device
// round trip, and pmm_free_frame() is called with interrupts off from
// paths that must stay short.
#include <stdint.h>

// One reported block is 2 MiB: Linux's pageblock order, and the host's
// transparent huge page, which a smaller discard would split.
#define PAGE_REPORT_BLOCK_FRAMES 512
// At most this many blocks are isolated at once, and a batch is at most
// this many runs -- Linux's PAGE_REPORTING_CAPACITY, and the size of
// QEMU's reporting queue.
#define PAGE_REPORT_BATCH_BLOCKS 32

// A run of whole, adjacent blocks.
struct page_report_run {
    uint64_t phys;
    uint64_t len;  // bytes, a multiple of the block size
};

// What a reporter returns.
#define PAGE_REPORT_TAKEN    1   // the host has them; mark them reported
#define PAGE_REPORT_REFUSED  0   // not sent; the frames go back unreported
#define PAGE_REPORT_LOST    -1   // sent, and the device stopped answering

// Hands `n` isolated runs to the host and waits for it to finish with
// them. On PAGE_REPORT_LOST the core never frees those frames again --
// the device may still discard them -- and stops reporting.
typedef int (*page_report_fn)(const struct page_report_run *runs, int n);

// One reporter; a second registration is refused. `max_runs` is how
// many runs it can take at once (a virtqueue's descriptors), so a batch
// never asks for more than fits. The first walk is due a moment after
// this, and reports everything free since boot.
int page_report_register(const char *name, page_report_fn fn, int max_runs);

// scheduler_idle()'s hook. One compare when nothing is registered.
void page_report_idle(void);

const char *page_report_name(void);       // NULL when nothing registered
uint64_t page_report_reported_bytes(void); // cumulative, a block counted each time

// FOR THE KTEST ONLY: one whole walk through `fn` instead of the
// registered reporter, run now. Returns the number of batches, or -1
// when a walk was already in progress.
int page_report_walk_for_test(page_report_fn fn);

// --- pmm.c's half: the books only it can touch -----------------------

// Isolates up to PAGE_REPORT_BATCH_BLOCKS wholly free, unreported blocks
// from block *cursor on, never taking a zone below `keep_free` free
// frames (DMA32 also keeps its reserve), and coalesces adjacent ones in
// one zone into at most `max_runs` runs.
// Returns the run count, 0 only when the walk reached the end; *cursor
// ends past the last block examined.
int pmm_report_isolate(uint64_t *cursor, struct page_report_run *runs, int max_runs,
                       uint64_t keep_free);
// Frees isolated runs again, marking each block reported when `reported`.
void pmm_report_release(const struct page_report_run *runs, int n, int reported);
uint64_t pmm_report_blocks(void);         // blocks the walk covers
uint64_t pmm_report_freed(void);          // frames freed since the last reset
void pmm_report_reset_freed(void);
int pmm_report_block_reported(uint64_t phys);
void pmm_report_forget_all(void);         // KTEST cleanup: every block reportable again

#endif
