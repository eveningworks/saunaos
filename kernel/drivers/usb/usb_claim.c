// A USB DEVICE HELD BY A RING-3 PROCESS -- dev_claim.c's shape, for a
// bus the kernel keeps owning. usb_claim.h says why the two differ.
// driver-none: it drives nothing; it records who holds what
#include "usb_claim.h"
#include "usb.h"
#include "usb_driver.h"
#include "scheduler.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "xhci.h"
#include "pmm.h"
#include "vmm.h"
#include "mmap.h"
#include "futex.h"
#include "devevent.h"
#include "barrier.h"

#define USB_CLASS_HUB 9   // the spec's value for a hub interface

// By slot id, like the device table: a claim needs no free entry, so
// it cannot run out before the controller's slots do. Held while pml4
// is nonzero.
static struct {
    uint64_t pml4;
    int      pid;
} g_claims[USB_SLOT_IDS];

// TWO ISOCHRONOUS ENDPOINTS, AN OUT AND AN IN: a UAC stream and its
// feedback endpoint. Each has its own grant; a holder that needs more
// can ask when there is one.
#define ISOCH_MAX 2
// Received lengths kept for an IN endpoint until reaped -- the transfer
// ring's size, so a holder that reaps as often as it posts never loses one.
#define ISOCH_LENS 256
struct isoch {
    uint8_t  slot, ep, open;
    uint64_t dma_phys, dma_base, dma_pages;
    // Written by the completion callback, which runs in INTERRUPT
    // CONTEXT -- volatile, and read-and-cleared by the status call.
    volatile uint32_t completions;
    // NEVER CLEARED, unlike `completions` -- it is what the close line
    // reports, and a total nobody resets is the one number that says
    // whether the interrupt half ever ran at all.
    volatile uint32_t total;
    volatile uint32_t posted;
    // IN ONLY: each completed TD's received bytes, in completion order,
    // which is post order -- usbfs's iso_frame_desc actual_length.
    uint16_t lens[ISOCH_LENS];
    volatile uint32_t lens_head, lens_tail, lens_lost;
    int pid;
};
static struct isoch g_isoch[ISOCH_MAX];

static void isoch_close(struct isoch *q);

static struct isoch *isoch_find(uint8_t slot, uint8_t ep) {
    for (int i = 0; i < ISOCH_MAX; i++)
        if (g_isoch[i].open && g_isoch[i].slot == slot && g_isoch[i].ep == ep)
            return &g_isoch[i];
    return 0;
}

static void isoch_close_slot(uint8_t slot) {
    for (int i = 0; i < ISOCH_MAX; i++)
        if (g_isoch[i].open && g_isoch[i].slot == slot) isoch_close(&g_isoch[i]);
}

static const struct usb_device_info *dev_by_slot(uint8_t slot) {
    for (int i = 0; i < usb_device_count(); i++) {
        const struct usb_device_info *d = usb_device_at(i);
        if (d && d->in_use && d->slot == slot) return d;
    }
    return 0;
}

static int slot_index(uint8_t slot) {
    return slot && g_claims[slot].pml4 ? slot : -1;
}

int usb_claim_holder_pid(uint8_t slot) {
    int i = slot_index(slot);
    return i < 0 ? 0 : g_claims[i].pid;
}

int usb_claim_check(uint8_t slot, uint64_t pml4) {
    int i = slot_index(slot);
    if (i < 0) return -EACCES;
    return g_claims[i].pml4 == pml4 ? 0 : -EBUSY;
}

// EVERY CLASS DRIVER HERE HAS AN UNBIND, so every enumerated device is
// claimable -- unlike PCI, where a driver without a remove() is the
// thing that makes a device un-claimable. Kept as a function anyway
// because it is the gate a caller asks about, and the day a class
// driver arrives that cannot let go this is the one place to say so.
int usb_claim_claimable(uint8_t slot) {
    const struct usb_device_info *d = dev_by_slot(slot);
    if (!d) return 0;
    // A HUB IS NOT OFFERED. Taking one away would strand every device
    // behind it, which is a claim over things the holder never asked
    // for and cannot give back.
    if (d->dev_class == USB_CLASS_HUB ||
        (d->if_count && d->ifs[0].if_class == USB_CLASS_HUB)) return 0;
    return 1;
}

int usb_claim_take(uint8_t slot, uint64_t pml4, int pid) {
    if (!pml4) return -EPERM;
    const struct usb_device_info *d = dev_by_slot(slot);
    if (!d) return -EINVAL;
    int i = slot_index(slot);
    if (i >= 0) return g_claims[i].pml4 == pml4 ? 0 : -EBUSY;
    if (!usb_claim_claimable(slot)) return -EPERM;

    // THE UNBIND COMES FIRST, and it is what the claim is FOR: the
    // class driver has the device's endpoints configured, and two
    // owners posting to one transfer ring is not a thing to recover
    // from. Every interface's driver lets go, and `bound`/`driver` go
    // with them, so lsusb no longer reports the device as driven.
    usb_unbind_drivers((struct usb_device_info *)d);

    g_claims[slot].pml4 = pml4;
    g_claims[slot].pid  = pid;
    klog_printf("usb: pid %d claimed slot %u (%04x:%04x)\n",
                pid, slot, d->vendor_id, d->product_id);
    char id[24];
    usb_device_id(d, id, sizeof id);
    devevent_add(QUERY_DEVEV_CLAIMED, id, "", "Taken by pid %d", pid);
    return 0;
}

int usb_claim_drop(uint8_t slot, uint64_t pml4, int rebind) {
    int i = slot_index(slot);
    if (i < 0) return -EACCES;
    if (pml4 && g_claims[i].pml4 != pml4) return -EACCES;

    int pid = g_claims[i].pid;
    // THE ENDPOINT GOES WITH THE CLAIM. A holder that drops the device
    // while TDs are posted would otherwise leave the controller
    // fetching from frames the allocator has handed to somebody else.
    isoch_close_slot(slot);
    k_memset(&g_claims[i], 0, sizeof g_claims[i]);
    klog_printf("usb: pid %d released slot %u%s\n", pid, slot,
                rebind ? " (rebinding)" : "");
    const struct usb_device_info *gone = dev_by_slot(slot);
    if (gone) {
        char id[24];
        usb_device_id(gone, id, sizeof id);
        devevent_add(QUERY_DEVEV_RETURNED, id, "", rebind ? "Given back by pid %d"
                     : "Given back by pid %d, left without a driver", pid);
    }

    // THE DEVICE IS LEFT AS THE HOLDER LEFT IT unless a rebind is
    // asked for -- its alternate setting, its clock, its endpoint
    // state. A class driver binding on top of that is entitled to
    // configure what it needs, which is what bind already does.
    if (rebind) {
        struct usb_device_info *d = (struct usb_device_info *)dev_by_slot(slot);
        if (d && d->cfg && d->cfg_len) usb_bind_drivers(d, d->cfg, d->cfg_len);
    }
    return 0;
}

// A TD GROUP FINISHED. From the event drain, so it does the two cheap
// things and nothing else -- the holder does the refilling, which is
// the whole point of it being in ring 3.
static void isoch_done(void *ctx, uint32_t bytes) {
    struct isoch *q = ctx;
    if (!q->open) return;
    if (q->ep & 0x80) {
        uint32_t h = q->lens_head;
        if (h - q->lens_tail < ISOCH_LENS) {
            q->lens[h % ISOCH_LENS] = (uint16_t)bytes;
            kbarrier();   // the length before the index that publishes it
            q->lens_head = h + 1;
        } else {
            q->lens_lost++;
        }
    }
    q->completions++;
    q->total++;
    futex_note_ready(q->pid);
}

static void isoch_close(struct isoch *q) {
    if (!q->open) return;
    // ONE LINE, AT CLOSE. A probe per completion would outrun the klog
    // ring at an endpoint's service rate; this is the whole question --
    // did the controller ever come back -- asked once.
    // UNDERRUNS ARE THE DISCRIMINATOR: a ring that ran dry says the
    // refill was late, and one that did not says the descriptors were
    // there and something else dropped them.
    klog_printf("usb: isoch ep 0x%x closing -- %u posted, %u completed, "
                "%u underrun(s), %u refused\n",
                q->ep, (unsigned)q->posted, (unsigned)q->total,
                (unsigned)xhci_isoch_underruns(q->slot, q->ep),
                (unsigned)xhci_isoch_refused(q->slot, q->ep));
    // THE MAPPING GOES BEFORE THE FRAMES. They are borrowed, so no
    // teardown disposes of them -- freeing first would hand the
    // allocator pages the holder still has a live writable PTE for.
    if (q->dma_base)
        mmap_drop_dma_region(q->dma_base, q->dma_pages);
    // And the frames only once the endpoint has let go of its TDs.
    if (q->dma_phys && xhci_cancel_endpoint(q->slot, q->ep) == 0)
        pmm_free_contiguous(q->dma_phys, q->dma_pages);
    k_memset(q, 0, sizeof *q);
}

int sys_usb_isoch_open(struct syscall_ctx *c) {
    struct usb_isoch_msg m;
    int64_t ret;
    struct isoch *q = 0;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    if (!m.dma_bytes || m.dma_bytes > 64 * 1024 || !m.mps) { ret = -EINVAL; goto out; }
    if (isoch_find((uint8_t)m.slot, (uint8_t)m.ep)) { ret = -EBUSY; goto out; }
    // One of each direction: a second OUT is a second stream, which
    // nothing asks for.
    for (int i = 0; i < ISOCH_MAX; i++) {
        if (g_isoch[i].open && ((g_isoch[i].ep ^ m.ep) & 0x80) == 0) { ret = -EBUSY; goto out; }
        if (!g_isoch[i].open && !q) q = &g_isoch[i];
    }
    if (!q) { ret = -EBUSY; goto out; }

    uint64_t npages = (m.dma_bytes + 4095) / 4096;
    uint64_t base = mmap_dma_reserve(npages);
    if (!base) { ret = -ENOMEM; goto out; }
    uint64_t phys = pmm_alloc_contiguous(npages, PMM_ZONE_DMA32);
    if (!phys) { ret = -ENOMEM; goto out; }
    // **WRITE-BACK, NOT UC, AND IT IS THE DIFFERENCE BETWEEN MUSIC AND
    // CRACKLE.** This is ordinary RAM the controller DMAs out of, not a
    // register file -- and x86 DMA snoops the caches, which is exactly
    // why hda.c has to CLEAR NoSnoop rather than map anything uncached
    // (docs/decisions/drivers.md). The driver fills it with 3-byte
    // stores, so an uncached mapping makes every sample a separate bus
    // transaction: ~1150 of them per wakeup at group 32, which is what
    // made a bigger group SLOWER (5803 packets/s at 32, 393 at 64) and
    // why KVM did not help. The kernel's own usb-audio driver writes
    // its packet buffer through a normal write-back mapping.
    if (!mmap_map_dma(c->pml4, base, phys, npages, 1, VMM_MT_NORMAL)) {
        pmm_free_contiguous(phys, npages);
        ret = -ENOMEM;
        goto out;
    }

    q->slot = (uint8_t)m.slot;
    q->ep   = (uint8_t)m.ep;
    q->dma_phys = phys;
    q->dma_base = base;
    q->dma_pages = npages;
    q->pid = scheduler_current_pid();
    q->open = 1;

    int r = (m.ep & 0x80)
        ? xhci_add_isoch_in((uint8_t)m.slot, (uint8_t)m.ep, (uint16_t)m.mps,
                            (uint8_t)m.interval, isoch_done, q)
        : xhci_add_isoch_out((uint8_t)m.slot, (uint8_t)m.ep, (uint16_t)m.mps,
                             (uint8_t)m.interval, isoch_done, q);
    if (r < 0) {
        isoch_close(q);
        ret = -EIO;
        goto out;
    }

    m.addr = base;
    m.phys = phys;
    if (!vmm_copy_to_user(c->pml4, c->a0, &m, sizeof m)) {
        isoch_close(q);
        ret = -EFAULT;
        goto out;
    }
    klog_printf("usb: pid %d opened isoch ep 0x%x on slot %u, %llu page(s) at %llx\n",
                q->pid, m.ep, (unsigned)m.slot,
                (unsigned long long)npages, (unsigned long long)phys);
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_isoch_post(struct syscall_ctx *c) {
    struct usb_isoch_post_msg m;
    uint16_t lens[256];
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    struct isoch *q = isoch_find((uint8_t)m.slot, (uint8_t)m.ep);
    if (!q) { ret = -EINVAL; goto out; }
    uint32_t count = m.count ? m.count : 1;
    if (count > 256) { ret = -EINVAL; goto out; }
    // PER-PACKET LENGTHS, when given: a stream following its device's
    // clock carries a frame more or less now and then.
    uint32_t biggest = m.len;
    if (m.lens) {
        if (!vmm_copy_from_user(c->pml4, lens, m.lens, count * sizeof lens[0])) {
            ret = -EFAULT;
            goto out;
        }
        biggest = 0;
        for (uint32_t i = 0; i < count; i++) if (lens[i] > biggest) biggest = lens[i];
    }
    uint32_t stride = m.stride ? m.stride : biggest;
    // THE RANGE IS CHECKED FOR THE WHOLE GROUP, not trusted: each of
    // these becomes a PHYSICAL address in a descriptor the controller
    // will read, so one past the grant would point the hardware at
    // somebody else's page. Checked on the LAST, which bounds them all
    // -- and no packet may be longer than the stride between them.
    uint64_t bytes = q->dma_pages * 4096;
    uint64_t last = (uint64_t)m.offset + (uint64_t)(count - 1) * stride;
    if (!biggest || biggest > stride || last + biggest > bytes) {
        ret = -EINVAL;
        goto out;
    }

    // A COMPLETION ON THE LAST ONLY, which is what `IOC every N` means:
    // an isochronous endpoint completes in order and an error reports
    // itself regardless, so one event per group is all the driver needs.
    // An IN endpoint asks for EVERY one, because each TD's length is
    // the data.
    int in = (m.ep & 0x80) != 0;
    uint32_t done = 0;
    int r = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = m.lens ? lens[i] : m.len;
        r = xhci_isoch_post((uint8_t)m.slot, (uint8_t)m.ep,
                            q->dma_phys + m.offset + (uint64_t)i * stride, len,
                            (in || (m.ioc && i + 1 == count)) ? 1 : 0);
        if (r != 0) break;
        done++;
    }
    q->posted += done;
    ret = done ? (int64_t)done : r == -2 ? -ENOSPC : -EINVAL;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_isoch_status(struct syscall_ctx *c) {
    int64_t ret;
    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if ((ret = usb_claim_check((uint8_t)c->a0, c->pml4)) != 0) goto out;
    struct isoch *q = isoch_find((uint8_t)c->a0, (uint8_t)c->a1);
    if (!q) { ret = -EINVAL; goto out; }
    // READ AND CLEAR, so a caller that misses a wakeup still learns how
    // many landed rather than one.
    ret = (int64_t)q->completions;
    q->completions = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_isoch_reap(struct syscall_ctx *c) {
    struct usb_isoch_reap_msg m;
    uint16_t out_lens[64];
    int64_t ret;
    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    struct isoch *q = isoch_find((uint8_t)m.slot, (uint8_t)m.ep);
    if (!q || !(q->ep & 0x80)) { ret = -EINVAL; goto out; }
    uint32_t max = m.max < 64 ? m.max : 64, n = 0;
    while (n < max && q->lens_tail != q->lens_head) {
        kbarrier();
        out_lens[n++] = q->lens[q->lens_tail % ISOCH_LENS];
        q->lens_tail++;
    }
    if (n && !vmm_copy_to_user(c->pml4, m.lens, out_lens, n * sizeof out_lens[0])) {
        ret = -EFAULT;
        goto out;
    }
    ret = (int64_t)n;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- the syscalls ------------------------------------------------------

int sys_usb_claim(struct syscall_ctx *c) {
    int64_t ret = scheduler_current_mm()
        ? usb_claim_take((uint8_t)c->a0, c->pml4, scheduler_current_pid())
        : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// ONE BOUNCE PAGE, and it is shared.
//
// xhci_control() takes a KERNEL buffer and uses its virtual address as
// the physical one, so neither a user pointer nor a kernel stack (which
// has a guard page and is not identity-mapped) can be handed to it.
//
// A MODULE-LEVEL BUFFER REACHED FROM A SYSCALL is what
// docs/smp-design.md says to read before adding, and the reason is
// here: a ring-3 process is preemptible inside a syscall, so two
// holders of two different devices would otherwise interleave through
// this one page. The transfer runs under the same preemption guard
// vfs.c's FS_OP() uses, for the same reason.
static uint64_t g_bounce;

int sys_usb_control(struct syscall_ctx *c) {
    struct usb_control_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    if (m.len > USB_CONTROL_MAX || (m.len && !m.buf)) { ret = -EINVAL; goto out; }

    // The two that would desync the kernel from the controller.
    // bmRequestType 0 = host-to-device, standard, device.
    if (m.setup[0] == 0x00 && (m.setup[1] == 5 || m.setup[1] == 9)) {
        klog_printf("usb: pid %d asked for %s on slot %u -- refused\n",
                    scheduler_current_pid(),
                    m.setup[1] == 5 ? "SET_ADDRESS" : "SET_CONFIGURATION",
                    (unsigned)m.slot);
        ret = -EINVAL;
        goto out;
    }

    if (!g_bounce) {
        g_bounce = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
        if (!g_bounce) { ret = -ENOMEM; goto out; }
    }
    uint8_t *buf = (uint8_t *)(uintptr_t)g_bounce;  // identity-mapped

    if (m.len && !m.in &&
        !vmm_copy_from_user(c->pml4, buf, m.buf, m.len)) { ret = -EFAULT; goto out; }

    scheduler_preempt_disable();
    int r = xhci_control((uint8_t)m.slot, m.setup, m.len ? buf : 0,
                         (uint16_t)m.len, m.in ? 1 : 0);
    scheduler_preempt_enable();

    if (r < 0) { ret = -EIO; goto out; }
    if (m.len && m.in &&
        !vmm_copy_to_user(c->pml4, m.buf, buf, m.len)) { ret = -EFAULT; goto out; }
    ret = (int64_t)m.len;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_release(struct syscall_ctx *c) {
    int64_t ret = scheduler_current_mm()
        ? usb_claim_drop((uint8_t)c->a0, c->pml4,
                         (c->a1 & USB_RELEASE_REBIND) ? 1 : 0)
        : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// THE CLAIM DIES WITH THE ADDRESS SPACE, and does NOT rebind -- the
// PCI path's rule, for its reason: a driver under supervision is
// restarted and has to find its device free.
void usb_claim_space_gone(uint64_t pml4) {
    if (!pml4) return;
    for (int i = 1; i < USB_SLOT_IDS; i++)
        if (g_claims[i].pml4 == pml4)
            usb_claim_drop((uint8_t)i, 0, 0);
}
