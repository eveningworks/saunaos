// The kernel's half of the shared allocator -- see api/heap_os.h.
//
// Memory comes from the physical frame allocator directly, with no
// mapping step: every managed frame is identity-mapped (boot.asm below
// 4 GiB, paging_extend_identity_map() above), so a frame
// pmm_alloc_contiguous() hands back is already a valid kernel pointer.
// That is why the kernel heap needs no vmm involvement at all.
//
// PMM_ZONE_ANY, so on a big machine the heap lives above 4 GiB and the
// low zone is left for DMA. THE TRAP: kmalloc memory is therefore NOT
// a DMA target for a 32-bit engine -- a driver with a 32-bit address
// register takes DMA32 frames from pmm, never a kmalloc buffer.
#include "heap_os.h"
#include "pmm.h"
#include "klog.h"
#include "fault_inject.h"
#include "lockstat.h"   // heap_os_lock()

#define HEAP_PAGE_SIZE 4096

void *heap_os_alloc(uint64_t bytes) {
    uint64_t pages = (bytes + HEAP_PAGE_SIZE - 1) / HEAP_PAGE_SIZE;
    // CONTIGUOUS, not a page at a time: the allocator hands out blocks
    // that span pages, so a region has to be one run.
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    return phys ? (void *)(uintptr_t)phys : 0;
}

int heap_os_region_returnable(const void *region) { (void)region; return 1; }

void heap_os_free(void *region, uint64_t bytes) {
    pmm_free_contiguous((uint64_t)(uintptr_t)region, bytes / HEAP_PAGE_SIZE);
}

void heap_os_report(const char *msg) { klog_write(msg); }

int heap_os_should_fail_alloc(void) { return fault_should_fail_alloc(); }

// ITS OWN SPINLOCK, TAKEN WITH INTERRUPTS OFF (SMP stage 7). Every
// caller holds the kernel lock today, so this excludes nothing the
// kernel lock does not -- it is what lets a path that runs without it
// allocate. Linux's slab and zone locks are irqsave for the reason this
// is: an interrupt handler that allocates must not land on its own
// core's holder, and the old guard ("no IRQ handler allocates",
// measured once) could not see one that started to.
//
// Lock order: heap, then the frame allocator (heap_os_alloc() grows the
// heap under it). **NOT RE-ENTRANT, where the guard it replaced nested**:
// nothing the heap calls under it may allocate -- the frame allocator,
// klog, a KASAN report and fault injection's log line do not -- and a
// second take panics as one (spinlock.c's take()). So may no cross-core
// call function: one answered from inside this lock's wait (smp_poll_ipis())
// would queue behind its own core's ticket.
//
// Balanced by construction: heap_core.c's kmalloc/kfree are thin
// wrappers around unlocked inner functions precisely so no early
// return can skip the unlock. The flags live beside the lock, written
// and read only by its holder.
SPINLOCK_COUNTED(g_heap_lock, "heap");
static uint64_t g_heap_flags;

void heap_os_lock(void) {
    uint64_t f = spin_lock_irqsave(&g_heap_lock);
    g_heap_flags = f;
}
void heap_os_unlock(void) { spin_unlock_irqrestore(&g_heap_lock, g_heap_flags); }
