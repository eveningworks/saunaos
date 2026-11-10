// Ring 3's half of the shared allocator -- see api/heap_os.h.
//
// TWO SOURCES, glibc's shape: SYS_SBRK for the heap proper, and a
// private mmap of its own for a region of MMAP_THRESHOLD or more -- a
// big block, which goes back to the kernel (munmap) once it is wholly
// free. sbrk only moves up, so what it gave is never returned: freed
// small blocks go back on this process's own free list.
//
// **Small regions stay on sbrk ON PURPOSE.** A process has at most
// MMAP_MAX_REGIONS mappings, each fault scans them, and a heap of 64 KiB
// regions as mappings would reach that at 64 MiB; big blocks are few. A
// refused mmap (that ceiling, RLIMIT_DATA) falls back to sbrk, so this
// adds no way for malloc() to fail.
//
// **The pages are not real yet.** sbrk reserves address space; the
// frame arrives on first touch. The allocator writes a block header
// into a fresh region immediately, so the first page of every region
// faults in here -- and the rest arrive as the program actually uses
// them. A process that reserves far more than the machine has finds out
// at the page it cannot be given, not at the malloc().
#include "heap_os.h"
#include "rt/sys.h"

// glibc's M_MMAP_THRESHOLD default.
#define MMAP_THRESHOLD (128u * 1024u)

// What sbrk has handed out: [g_brk_lo, g_brk_hi). Everything else this
// heap holds came from mmap.
static uintptr_t g_brk_lo, g_brk_hi;

void *heap_os_alloc(uint64_t bytes) {
    if (bytes >= MMAP_THRESHOLD) {
        void *m = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                           SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
        if (m != (void *)-1) return m;
    }
    // sbrk hands back the OLD break, which is the start of what was
    // just claimed -- exactly the region the allocator wants.
    void *p = sys_sbrk((int64_t)bytes);
    if (p == (void *)-1) return 0;
    if (!g_brk_lo) g_brk_lo = (uintptr_t)p;
    g_brk_hi = (uintptr_t)p + bytes;
    return p;
}

int heap_os_region_returnable(const void *region) {
    uintptr_t r = (uintptr_t)region;
    return !(g_brk_lo && r >= g_brk_lo && r < g_brk_hi);
}

void heap_os_free(void *region, uint64_t bytes) { sys_munmap(region, bytes); }

// stderr, not stdout: a GUI client's stdout goes nowhere useful and a
// spawned process's goes into its parent's pipe, while stderr reaches
// the kernel log and `dmesg` where a test can read it. Same reasoning
// as every other ring-3 diagnostic here.
void heap_os_report(const char *msg) { sys_eprint(msg); }

// No injector in ring 3 yet. The kernel's tests cover the allocator's
// own out-of-memory path, and it is the same code.
int heap_os_should_fail_alloc(void) { return 0; }

// THE FREE LIST, AND WHY RING 3 NEEDS A LOCK WHERE THE KERNEL DOES NOT.
//
// A process can have threads now, and they are preempted at any
// instruction -- so two of them inside kmalloc() walk and rewrite one
// list. heap_core.c's own top comment predicted exactly this ("if a
// future caller ever needs kmalloc from ... a genuinely preemptible
// kernel thread, this needs a lock first"); threads are that caller,
// arriving on the ring-3 side.
//
// A SPIN THAT YIELDS, not a futex: `SYS_FUTEX_WAIT` exists and this has
// not been moved onto it (docs/roadmap.md). Correct under a preemptive round-robin scheduler,
// because the holder is always eventually run, and cheap in the case
// that matters -- an uncontended acquire is one atomic exchange, which
// is what every single-threaded program in this system pays.
//
// The acquire/release ordering is not decoration on x86-64's TSO: the
// COMPILER is what would otherwise hoist a list read out of the
// critical section.
static volatile int g_heap_lock;

void heap_os_lock(void) {
    while (__atomic_exchange_n(&g_heap_lock, 1, __ATOMIC_ACQUIRE))
        sys_yield();
}

void heap_os_unlock(void) {
    __atomic_store_n(&g_heap_lock, 0, __ATOMIC_RELEASE);
}
