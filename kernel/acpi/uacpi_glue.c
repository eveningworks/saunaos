// uACPI's kernel API (uacpi/kernel_api.h), implemented on SaunaOS's
// primitives -- the table in docs/aml-design.md, stage 4, is the map.
//
// WHERE THIS RUNS. In kacpid (uacpi_boot.c), a kernel thread: it may
// park, but it runs with interrupts off, so it is never preempted. And
// in a KTEST, from the kernel context, which may not park at all. So
// every wait here tries to park and, told there is nowhere to
// (scheduler_block_kernel*() returning 0), polls instead.
//
// THE SCI IS HELD, NOT INSTALLED, until stage 4c: acpi_sci.c owns the
// line and the power button on it, and calls uACPI's handler after it --
// for the global lock's release, the one event uACPI enables itself. Its
// GPE handling stays dark (no uacpi_finalize_gpe_initialization(), so no
// GPE is enabled).
#include <uacpi/kernel_api.h>
#include <uacpi/status.h>

#include "acpi.h"
#include "barrier.h"      // cpu_relax()
#include "clocksource.h"
#include "heap.h"
#include "io.h"
#include "irqflags.h"
#include "kfmt.h"
#include "klog.h"
#include "multiboot.h"     // multiboot_mmap_foreach()
#include "paging.h"       // paging_identity_limit(), paging_map_device()
#include "pci_internal.h"
#include "ratelimit.h"
#include "scheduler.h"
#include "spinlock.h"
#include "uacpi_host.h"

// --- tables and memory ------------------------------------------------

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    uint64_t p = acpi_get_state()->rsdp_phys;
    if (!p) return UACPI_STATUS_NOT_FOUND;
    *out = p;
    return UACPI_STATUS_OK;
}

// Is [lo, hi) inside the identity map ABOVE 4 GiB? That map is SPARSE:
// paging_extend_identity_map() covers only the whole 2 MiB slots of
// usable RAM, so ACPI-reclaim or NVS memory there, and the holes between
// RAM ranges, are not mapped although they sit below its end.
static uint64_t g_q_lo, g_q_hi;
static int g_q_in;

static void ram_slot_cb(const struct multiboot_mmap_region *r) {
    if (r->type != 1) return;
    uint64_t start = (r->base + 0x1FFFFF) & ~0x1FFFFFull;
    uint64_t end = (r->base + r->length) & ~0x1FFFFFull;
    if (g_q_lo >= start && g_q_hi <= end) g_q_in = 1;
}

static int identity_mapped_high(uint64_t lo, uint64_t hi) {
    if (hi > paging_identity_limit()) return 0;
    static struct spinlock q_lock;   // the callback has no context argument
    uint64_t f = spin_lock_irqsave(&q_lock);
    g_q_lo = lo;
    g_q_hi = hi;
    g_q_in = 0;
    multiboot_mmap_foreach(ram_slot_cb);
    int in = g_q_in;
    spin_unlock_irqrestore(&q_lock, f);
    return in;
}

// Under 4 GiB an address IS its kernel pointer (tables, RAM and MMIO --
// paging_map_device() answers the same). Above it, usable RAM is in the
// identity map and anything else gets an uncached device mapping, which
// paging_map_device() never takes down: unmap is a no-op, and a table
// or OperationRegion up there costs its mapping for the boot.
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    const uint64_t four_gib = 0x100000000ull;
    if (len == 0) len = 1;
    if (addr + len < addr) return UACPI_MAP_FAILED;
    if (addr + len <= four_gib) return (void *)(uintptr_t)addr;
    if (addr >= four_gib && identity_mapped_high(addr, addr + len)) return (void *)(uintptr_t)addr;
    volatile void *v = paging_map_device(addr, len);
    return v ? (void *)v : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size); }
void *uacpi_kernel_alloc_zeroed(uacpi_size size) { return kzalloc(size); }
void uacpi_kernel_free(void *mem) { if (mem) kfree(mem); }

// --- the log ----------------------------------------------------------

// A load prints a dozen lines at once, so the first 128 go straight
// through and only then is it one a second -- a chatty firmware loop
// must not scroll the boot out of the klog ring.
#define UACPI_LOG_BURST 128

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg) {
    static unsigned sent;
    static struct ratelimit rl;
    unsigned held = 0;
    if (sent >= UACPI_LOG_BURST && !ratelimit_ok(&rl, &held)) return;
    sent++;
    const char *tag = level == UACPI_LOG_ERROR ? "error: " :
                      level == UACPI_LOG_WARN  ? "warning: " : "";
    if (held) klog_printf("uacpi: (%u lines held back)\n", held);
    klog_printf("uacpi: %s%s", tag, msg);   // msg ends in UACPI_END_OF_LOG_MSG, "\n"
}

// --- PCI config space -------------------------------------------------
//
// A handle is a struct pci_device naming only the address: firmware
// may name a function enumeration never recorded (a hidden bridge), and
// pci_config_*() read nothing else of the struct. Segment 0 and the
// first 256 bytes only -- there is no ECAM here. A function this cannot
// reach answers NOT_FOUND, which uACPI turns into reads of 0xFF: AML's
// own way to probe for a device.

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    if (a.segment != 0 || a.device > 31 || a.function > 7) return UACPI_STATUS_NOT_FOUND;
    struct pci_device *d = kzalloc(sizeof *d);
    if (!d) return UACPI_STATUS_OUT_OF_MEMORY;
    d->bus = a.bus;
    d->device = a.device;
    d->function = a.function;
    *out = d;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle h) { kfree(h); }

#define PCI_OFF_OK(off, w) ((off) + (w) <= 256 && ((off) & ((w) - 1)) == 0)

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    if (!PCI_OFF_OK(off, 1)) return UACPI_STATUS_INVALID_ARGUMENT;
    *v = pci_config_read8(h, (uint8_t)off);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    if (!PCI_OFF_OK(off, 2)) return UACPI_STATUS_INVALID_ARGUMENT;
    *v = pci_config_read16(h, (uint8_t)off);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    if (!PCI_OFF_OK(off, 4)) return UACPI_STATUS_INVALID_ARGUMENT;
    *v = pci_config_read32(h, (uint8_t)off);
    return UACPI_STATUS_OK;
}

// CONFIG_DATA takes a whole dword, so a byte write is a read-modify-write
// of the dword it sits in -- pci.c's config_write16() shape.
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    if (!PCI_OFF_OK(off, 1)) return UACPI_STATUS_INVALID_ARGUMENT;
    uint8_t at = (uint8_t)(off & ~3u);
    unsigned shift = (unsigned)(off & 3) * 8;
    uint32_t dw = pci_config_read32(h, at);
    dw = (dw & ~(0xFFu << shift)) | ((uint32_t)v << shift);
    pci_config_write32(h, at, dw);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    if (!PCI_OFF_OK(off, 2)) return UACPI_STATUS_INVALID_ARGUMENT;
    pci_config_write16(h, (uint8_t)off, v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    if (!PCI_OFF_OK(off, 4)) return UACPI_STATUS_INVALID_ARGUMENT;
    pci_config_write32(h, (uint8_t)off, v);
    return UACPI_STATUS_OK;
}

// --- port I/O ---------------------------------------------------------
//
// A handle is the base port with bit 32 set, so port 0 is not NULL.
// uACPI bounds an access by the length it mapped; the map refuses a
// range past the 64 KiB port space.

#define IO_TAG (1ull << 32)

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    if (base > 0xFFFF || len > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)(IO_TAG | base);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle h) { (void)h; }

static uint16_t io_port(uacpi_handle h, uacpi_size off) {
    return (uint16_t)(((uintptr_t)h & 0xFFFF) + off);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    *v = inb(io_port(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    *v = inw(io_port(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    *v = inl(io_port(h, off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    outb(io_port(h, off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    outw(io_port(h, off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    outl(io_port(h, off), v);
    return UACPI_STATUS_OK;
}

// --- time -------------------------------------------------------------

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return clocksource_now_ns(); }

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t until = clocksource_now_ns() + (uint64_t)usec * 1000;
    while (clocksource_now_ns() < until) cpu_relax();
}

// AML's Sleep(): parked where there is a thread to park, a busy wait in
// the kernel context. Woken early by nothing -- the channel is private.
void uacpi_kernel_sleep(uacpi_u64 msec) {
    static char sleep_chan;
    uint64_t until = clocksource_now_ns() + msec * 1000000ull;
    while (clocksource_now_ns() < until) {
        if (!scheduler_block_kernel_until(&sleep_chan, SCHED_WAIT_TIMER, until)) cpu_relax();
    }
}

// --- semaphores: a count under a spinlock, waited on by address -------

struct uacpi_sem {
    struct spinlock lock;
    uint32_t count;
};

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial) {
    struct uacpi_sem *s = kzalloc(sizeof *s);
    if (s) s->count = initial;
    return s;
}

void uacpi_kernel_free_semaphore(uacpi_handle h) { kfree(h); }

static int sem_take(struct uacpi_sem *s) {
    uint64_t f = spin_lock_irqsave(&s->lock);
    int got = s->count > 0;
    if (got) s->count--;
    spin_unlock_irqrestore(&s->lock, f);
    return got;
}

// `timeout` is in milliseconds; 0xFFFF waits for ever, 0 only tries.
// Armed before the test, so a signal between the test and the park is
// not lost (scheduler.h, "prepare_to_wait").
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16 timeout) {
    struct uacpi_sem *s = h;
    uint64_t until = timeout == 0xFFFF ? 0 : clocksource_now_ns() + (uint64_t)timeout * 1000000ull;
    for (;;) {
        scheduler_wait_arm(s);
        if (sem_take(s)) {
            scheduler_wait_disarm();
            return UACPI_STATUS_OK;
        }
        if (timeout == 0 || (until && clocksource_now_ns() >= until)) {
            scheduler_wait_disarm();
            return UACPI_STATUS_TIMEOUT;
        }
        if (!scheduler_block_kernel_until(s, SCHED_WAIT_LOCK, until)) {
            scheduler_wait_disarm();
            cpu_relax();
        }
    }
}

void uacpi_kernel_signal_semaphore(uacpi_handle h) {
    struct uacpi_sem *s = h;
    uint64_t f = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, f);
    scheduler_wake(s, 0);
}

// --- threads, interrupts, spinlocks -----------------------------------

// pid + 1: the kernel context is pid 0, and NULL must not be an id.
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)(uintptr_t)(scheduler_current_pid() + 1);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { irq_restore(s); }

uacpi_handle uacpi_kernel_create_spinlock(void) { return kzalloc(sizeof(struct spinlock)); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    return (uacpi_cpu_flags)spin_lock_irqsave(h);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f) {
    spin_unlock_irqrestore(h, f);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    if (r->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        klog_printf("uacpi: firmware Fatal(type 0x%x, code 0x%x, arg 0x%llx)\n",
                    (unsigned)r->fatal.type, (unsigned)r->fatal.code,
                    (unsigned long long)r->fatal.arg);
    return UACPI_STATUS_OK;
}

// --- the SCI: held, not installed (the top of this file) ---------------
//
// acpi_sci.c's handler takes the power button and then calls the held
// one (uacpi_host_sci_dispatch()), under g_sci_lock -- which the
// uninstall takes too, so a teardown never frees a context an interrupt
// on another core is still in.

static struct spinlock g_sci_lock;
static struct {
    uacpi_interrupt_handler fn;
    uacpi_handle ctx;
    uint32_t irq;
} g_sci;

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn,
                                                    uacpi_handle ctx, uacpi_handle *out) {
    uint64_t f = spin_lock_irqsave(&g_sci_lock);
    int busy = g_sci.fn != 0;
    if (!busy) {
        g_sci.fn = fn;
        g_sci.ctx = ctx;
        g_sci.irq = irq;
    }
    spin_unlock_irqrestore(&g_sci_lock, f);
    if (busy) return UACPI_STATUS_ALREADY_EXISTS;
    *out = &g_sci;
    klog_printf("uacpi: SCI handler for line %u chained behind acpi_sci.c's\n", (unsigned)irq);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h) {
    uint64_t f = spin_lock_irqsave(&g_sci_lock);
    int ok = h == &g_sci && g_sci.fn == fn;
    if (ok) g_sci.fn = 0;
    spin_unlock_irqrestore(&g_sci_lock, f);
    return ok ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

int uacpi_host_sci_held(uint32_t *irq) {
    if (!g_sci.fn) return 0;
    if (irq) *irq = g_sci.irq;
    return 1;
}

uint32_t uacpi_host_sci_dispatch(void) {
    uint64_t f = spin_lock_irqsave(&g_sci_lock);
    uint32_t r = g_sci.fn ? g_sci.fn(g_sci.ctx) : 0;
    spin_unlock_irqrestore(&g_sci_lock, f);
    return r;
}

// --- deferred work: kacpid's queue -------------------------------------
//
// Linux's kacpid shape: GPE methods and Notify() handlers run later, in
// a thread, never inside the interrupt or the method that raised them.
// An item is queued at most once; uACPI owns its lifetime.

struct uacpi_work {
    uacpi_work_handler fn;
    uacpi_handle ctx;
    struct uacpi_work *next;
    int queued;
};

static struct spinlock g_wq_lock;
static struct uacpi_work *g_wq_head, *g_wq_tail;
static int g_wq_running;        // items popped and still running
static char g_wq_chan;          // kacpid waits here for work
static char g_wq_idle_chan;     // uacpi_kernel_wait_for_work_completion() waits here
volatile int g_uacpi_kacpid_pid;

uacpi_handle uacpi_kernel_create_work_item(void) { return kzalloc(sizeof(struct uacpi_work)); }

void uacpi_kernel_free_work_item(uacpi_handle h) { kfree(h); }

void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item,
                                uacpi_work_handler fn, uacpi_handle ctx) {
    (void)type;   // GPE work wants CPU 0; kacpid is not pinned yet (stage 4b)
    struct uacpi_work *w = item;
    uint64_t f = spin_lock_irqsave(&g_wq_lock);
    if (!w->queued) {
        w->fn = fn;
        w->ctx = ctx;
        w->next = 0;
        w->queued = 1;
        if (g_wq_tail) g_wq_tail->next = w;
        else g_wq_head = w;
        g_wq_tail = w;
    }
    spin_unlock_irqrestore(&g_wq_lock, f);
    scheduler_wake(&g_wq_chan, 0);
}

static int wq_idle(void) {
    uint64_t f = spin_lock_irqsave(&g_wq_lock);
    int idle = !g_wq_head && !g_wq_running;
    spin_unlock_irqrestore(&g_wq_lock, f);
    return idle;
}

void uacpi_host_drain_work(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&g_wq_lock);
        struct uacpi_work *w = g_wq_head;
        if (w) {
            g_wq_head = w->next;
            if (!g_wq_head) g_wq_tail = 0;
            w->queued = 0;
            g_wq_running++;
        }
        spin_unlock_irqrestore(&g_wq_lock, f);
        if (!w) break;
        w->fn(w->ctx);
        f = spin_lock_irqsave(&g_wq_lock);
        g_wq_running--;
        spin_unlock_irqrestore(&g_wq_lock, f);
    }
    scheduler_wake(&g_wq_idle_chan, 0);
}

void uacpi_host_wait_for_work(void) {
    scheduler_wait_arm(&g_wq_chan);
    uint64_t f = spin_lock_irqsave(&g_wq_lock);
    int any = g_wq_head != 0;
    spin_unlock_irqrestore(&g_wq_lock, f);
    if (any) scheduler_wait_disarm();
    else scheduler_block_kernel(&g_wq_chan, SCHED_WAIT_EVENT);
}

// From kacpid itself, or with no kacpid to wait for, the caller drains.
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    int me = scheduler_current_pid();
    if (!g_uacpi_kacpid_pid || me == g_uacpi_kacpid_pid) {
        uacpi_host_drain_work();
        return UACPI_STATUS_OK;
    }
    for (;;) {
        scheduler_wait_arm(&g_wq_idle_chan);
        if (wq_idle()) {
            scheduler_wait_disarm();
            return UACPI_STATUS_OK;
        }
        if (!scheduler_block_kernel(&g_wq_idle_chan, SCHED_WAIT_EVENT)) {
            scheduler_wait_disarm();
            cpu_relax();
        }
    }
}

void uacpi_host_reset(void) {
    uint64_t f = spin_lock_irqsave(&g_wq_lock);
    g_wq_head = g_wq_tail = 0;
    g_wq_running = 0;
    spin_unlock_irqrestore(&g_wq_lock, f);
    f = spin_lock_irqsave(&g_sci_lock);
    g_sci.fn = 0;
    spin_unlock_irqrestore(&g_sci_lock, f);
}
