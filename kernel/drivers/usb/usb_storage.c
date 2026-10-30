// USB mass storage, bulk-only transport: sticks, card readers, USB disks.
//
// The shape every system shares (docs/usb-storage-design.md): a thin
// transport that carries SCSI command blocks, and the disk logic in
// SCSI (kernel/lib/scsi.c). Bulk-only (the USB-IF's "BBB") is three
// phases on two bulk endpoints -- a 31-byte Command Block Wrapper OUT, the
// data either way, a 13-byte Command Status Wrapper IN -- one command at a
// time per device. Each LUN with a medium becomes a block device,
// `usb<n>`, registered with blk_add_disk() so a stick plugged in after
// boot is named and partitioned but never becomes the root.
//
// THE STALL IS PART OF THE PROTOCOL, not an error. A device stalls a data
// phase it will not complete (a short read, a refused write) and then
// still sends the status -- so this driver recovers its own endpoints
// (xhci_bulk_own_recovery()), clears the device's halt, and reads the
// CSW. A status it cannot read, or one that does not belong to the
// command, is a phase error: Reset Recovery (BOT spec 5.3.4).
//
// REMOVAL (block.h's blk_device_gone()): unbind marks the device
// unplugged FIRST, so a transfer in flight or about to start fails at
// once rather than waiting out a timeout, then tells the block layer.
// THE DEVICE'S STRUCT IS NEVER FREED -- a caller between looking a disk
// up and calling into it holds a pointer into it, and nothing can count
// that window -- only its DMA buffer, once no LUN is held (release()) and
// no transfer is in flight (`busy`). A few hundred bytes a plug.
//
// QUICK REMOVAL is the block layer's (`removable`): every write is
// flushed before the call that made it returns, so this driver adds no
// cache of its own and forwards a flush as SYNCHRONIZE CACHE.
#include "usb.h"
#include "usb_driver.h"
#include "xhci.h"
#include "block.h"
#include "scsi.h"
#include "pmm.h"
#include "heap.h"
#include "kmutex.h"
#include "scheduler.h"
#include "clocksource.h"
#include "barrier.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "driver.h"

DRIVER_DECLARE("usb-storage", "block", "USB mass storage, bulk-only");

#define CBW_SIG 0x43425355u   // "USBC"
#define CSW_SIG 0x53425355u   // "USBS"
// One command's data at most: 1 MiB at SuperSpeed, as Linux's
// usb-storage sets max_sectors 2048 there -- a stick writes a big command
// several times faster than the same bytes in 64 KiB ones -- and 64 KiB
// below it. A bigger buffer that cannot be had falls back to the small.
#define DATA_BYTES_SS (1024u * 1024)
#define DATA_BYTES    (64u * 1024)
#define DMA_PAGES(b)  ((b) / 4096 + 1)  // + a page for the CBW and CSW
#define XFER_NS    (10ull * 1000000000ull)
#define MAX_LUNS   16                     // GET MAX LUN's field is four bits

struct ums;

struct ums_lun {
    struct block_device blk;   // first: release() gets this back
    struct ums *u;
    uint8_t lun;
    uint8_t registered;
    uint32_t block;            // logical block, 512 or 4096
    uint64_t blocks;
    char model[40];
};

struct ums {
    struct ums *next;          // the bound devices, for unbind to find by slot
    usb_dev_t slot;
    uint8_t ifnum, ep_in, ep_out;
    uint8_t dma_pinned;        // a cancel was not confirmed: never free the buffer
    uint16_t mps_in, mps_out;
    volatile int unplugged;
    struct kmutex lock;        // one command at a time
    uint64_t dma;              // data_bytes of data, then the CBW and CSW
    uint32_t data_bytes;       // one command's data at most
    uint8_t *data;
    uint8_t *cbw, *csw;
    uint32_t tag;
    // The transfer in flight, completed from the event drain.
    volatile int done, ok;
    volatile uint32_t bytes;
    int nluns, live;           // LUNs found, and how many the block layer still holds
    int busy;                  // rw()/flush calls inside the driver (atomic)
    struct ums_lun *luns;      // nluns of them
};

static int enter(struct ums *u);
static void leave(struct ums *u);

static void xfer_done(void *ctx, uint64_t phys, uint32_t bytes, int ok) {
    (void)phys;
    struct ums *u = ctx;
    u->bytes = bytes;
    u->ok = ok;
    __atomic_store_n(&u->done, 1, __ATOMIC_RELEASE);
    scheduler_wake(u, 0);
}

// One bulk transfer, waited for. A scheduled caller sleeps until the
// completion wakes it; where nothing can park it drains the event ring
// itself, which the single-consumer guard in xhci_service() allows.
// 1 done, 0 failed (*stalled says whether the endpoint halted), -1 timed
// out -- after which the endpoint has been cancelled, so the buffer is
// the driver's again.
static int xfer(struct ums *u, uint8_t ep, uint64_t phys, uint32_t len, uint32_t *got,
                int *stalled) {
    *stalled = 0;
    if (u->unplugged) return 0;
    u->done = 0;
    if (xhci_bulk_post(u->slot, ep, phys, len) < 0) {
        *stalled = xhci_bulk_halted(u->slot, ep);
        return 0;
    }
    uint64_t deadline = clocksource_now_ns() + XFER_NS;
    int parked = 1;
    while (!__atomic_load_n(&u->done, __ATOMIC_ACQUIRE)) {
        if (u->unplugged || clocksource_now_ns() >= deadline) {
            xhci_cancel_endpoint(u->slot, ep);
            return u->unplugged ? 0 : -1;
        }
        if (parked) {
            scheduler_wait_arm(u);
            if (__atomic_load_n(&u->done, __ATOMIC_ACQUIRE)) { scheduler_wait_disarm(); break; }
            if (!scheduler_block_kernel_until(u, SCHED_WAIT_DISK, deadline)) {
                scheduler_wait_disarm();
                parked = 0;   // nowhere to park: drain it ourselves
            }
        } else {
            xhci_service();
            cpu_relax();
        }
    }
    if (got) *got = u->bytes;
    if (!u->ok) *stalled = xhci_bulk_halted(u->slot, ep);
    return u->ok ? 1 : 0;
}

static int control(struct ums *u, uint8_t type, uint8_t req, uint16_t value, void *buf,
                   uint16_t len) {
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8), u->ifnum, 0,
                         (uint8_t)len, (uint8_t)(len >> 8) };
    return xhci_control(u->slot, setup, buf, len, (type & 0x80) != 0);
}

// Bulk-Only Mass Storage Reset, then any HALTED endpoint's halt cleared:
// what a phase error or a lost status costs. The reset keeps the device's
// toggles and stalls (BOT spec 3.1), so an endpoint that is not halted is
// left exactly as it is. 0 when the device took it.
static int reset_recovery(struct ums *u) {
    if (u->unplugged) return -1;
    klog_printf("usb-storage: slot %u reset recovery\n", u->slot);
    if (control(u, 0x21, 0xFF, 0, 0, 0) < 0) return -1;
    xhci_bulk_reset(u->slot, u->ep_in);
    xhci_bulk_reset(u->slot, u->ep_out);
    return 0;
}

#define CMD_OK      0
#define CMD_FAILED  1    // the device ran it and said no -- ask for sense
#define CMD_GONE   -1    // transport failure: unplugged, or recovery did not help

// One command, three phases. The data is u->data, `len` bytes either way.
static int command(struct ums *u, uint8_t lun, const uint8_t *cdb, int cdb_len, int in,
                   uint32_t len, uint32_t *got) {
    if (u->unplugged) return CMD_GONE;
    uint32_t tag = ++u->tag;
    uint8_t *w = u->cbw;
    k_memset(w, 0, 31);
    w[0] = (uint8_t)CBW_SIG; w[1] = (uint8_t)(CBW_SIG >> 8);
    w[2] = (uint8_t)(CBW_SIG >> 16); w[3] = (uint8_t)(CBW_SIG >> 24);
    for (int i = 0; i < 4; i++) { w[4 + i] = (uint8_t)(tag >> (8 * i)); w[8 + i] = (uint8_t)(len >> (8 * i)); }
    w[12] = in ? 0x80 : 0x00;
    w[13] = lun;
    w[14] = (uint8_t)cdb_len;
    k_memcpy(w + 15, cdb, (uint32_t)cdb_len);

    uint64_t cbw_phys = u->dma + u->data_bytes, csw_phys = cbw_phys + 64;
    int stalled;
    if (xfer(u, u->ep_out, cbw_phys, 31, 0, &stalled) != 1) {
        reset_recovery(u);
        return CMD_GONE;
    }
    uint32_t moved = 0;
    if (len) {
        int r = xfer(u, in ? u->ep_in : u->ep_out, u->dma, len, &moved, &stalled);
        if (r < 0) { reset_recovery(u); return CMD_GONE; }
        if (r == 0 && stalled) xhci_bulk_reset(u->slot, in ? u->ep_in : u->ep_out);
        else if (r == 0) { reset_recovery(u); return CMD_GONE; }
    }
    // The status, once more after a stall on it (BOT spec 6.7.2).
    for (int attempt = 0;; attempt++) {
        int r = xfer(u, u->ep_in, csw_phys, 13, 0, &stalled);
        if (r == 1) break;
        if (r == 0 && stalled && attempt == 0) { xhci_bulk_reset(u->slot, u->ep_in); continue; }
        reset_recovery(u);
        return CMD_GONE;
    }
    const uint8_t *s = u->csw;
    uint32_t sig = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
    uint32_t stag = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    if (sig != CSW_SIG || stag != tag || s[12] == 2) {   // not ours, or a phase error
        reset_recovery(u);
        return CMD_GONE;
    }
    if (got) *got = moved;
    return s[12] == 0 ? CMD_OK : CMD_FAILED;
}

// The sense behind a CMD_FAILED. 0 if even that could not be read.
static int sense(struct ums *u, uint8_t lun, struct scsi_sense *out) {
    uint8_t cdb[16];
    int n = scsi_cdb_request_sense(cdb, SCSI_SENSE_LEN);
    uint32_t got = 0;
    if (command(u, lun, cdb, n, 1, SCSI_SENSE_LEN, &got) != CMD_OK) return 0;
    return scsi_parse_sense(u->data, got, out);
}

// A command, retried once after a UNIT ATTENTION -- what any disk says
// first after a reset or a medium change, and which means "tell me again".
static int command_retry(struct ums *u, uint8_t lun, const uint8_t *cdb, int cdb_len, int in,
                         uint32_t len, uint32_t *got, struct scsi_sense *why) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int r = command(u, lun, cdb, cdb_len, in, len, got);
        if (r != CMD_FAILED) return r;
        struct scsi_sense s = { 0, 0, 0 };
        sense(u, lun, &s);
        if (why) *why = s;
        if (s.key != SCSI_KEY_UNIT_ATTENTION) return CMD_FAILED;
    }
    return CMD_FAILED;
}

// ---- the block device ---------------------------------------------------

static struct ums_lun *lun_of(const struct block_device *dev) { return (struct ums_lun *)dev; }

static uint64_t ums_count(const struct block_device *dev) {
    struct ums_lun *l = lun_of(dev);
    return l->blocks * (l->block / 512);
}

static int ums_xfer_max(const struct block_device *dev) { return (int)(lun_of(dev)->u->data_bytes / 512); }

static int rw(const struct block_device *dev, uint64_t lba, int count, void *buf, int write) {
    struct ums_lun *l = lun_of(dev);
    struct ums *u = l->u;
    uint32_t spb = l->block / 512;
    if (count <= 0 || ((lba | (uint64_t)count) & (spb - 1))) return 0;   // the block layer refuses these too
    if (lba + (uint64_t)count > ums_count(dev)) return 0;
    if (!enter(u)) return 0;
    kmutex_lock(&u->lock);
    int ok = 1;
    uint8_t *p = buf;
    while (count > 0 && ok) {
        uint32_t n = (uint32_t)count < u->data_bytes / 512 ? (uint32_t)count : u->data_bytes / 512;
        uint32_t bytes = n * 512;
        uint8_t cdb[16];
        int clen = scsi_cdb_rw(cdb, write, lba / spb, n / spb);
        if (write) k_memcpy(u->data, p, bytes);
        uint32_t got = 0;
        struct scsi_sense why = { 0, 0, 0 };
        int r = command_retry(u, l->lun, cdb, clen, !write, bytes, &got, &why);
        if (r != CMD_OK || (!write && got != bytes)) {
            if (r == CMD_FAILED)
                klog_printf(KLOG_ERR "usb-storage: %s %s at %llu failed: %s (%02x/%02x)\n",
                            blk_device_name(dev), write ? "write" : "read",
                            (unsigned long long)lba, scsi_key_name(why.key), why.asc, why.ascq);
            ok = 0;
            break;
        }
        if (!write) k_memcpy(p, u->data, bytes);
        p += bytes; lba += n; count -= (int)n;
    }
    kmutex_unlock(&u->lock);
    leave(u);
    return ok;
}

static int ums_read(const struct block_device *dev, uint64_t lba, int count, void *buf) {
    return rw(dev, lba, count, buf, 0);
}

static int ums_write(const struct block_device *dev, uint64_t lba, int count, const void *buf) {
    return rw(dev, lba, count, (void *)buf, 1);
}

// SYNCHRONIZE CACHE. A stick that refuses it (many have no cache to sync
// and say ILLEGAL REQUEST) has nothing to lose, so that is success.
static int ums_flush(const struct block_device *dev) {
    struct ums_lun *l = lun_of(dev);
    struct ums *u = l->u;
    if (!enter(u)) return 0;
    kmutex_lock(&u->lock);
    uint8_t cdb[16];
    int n = scsi_cdb_sync_cache(cdb);
    struct scsi_sense why = { 0, 0, 0 };
    int r = command_retry(u, l->lun, cdb, n, 0, 0, 0, &why);
    kmutex_unlock(&u->lock);
    leave(u);
    return r == CMD_OK || (r == CMD_FAILED && why.key == SCSI_KEY_ILLEGAL_REQUEST);
}

static struct ums *g_ums;

// The DMA buffer goes once nothing can touch it: pulled, no LUN held, no
// call inside. Any of the three may be the last to change, so each calls
// this. A buffer the controller may still DMA into is leaked instead: a
// late write into a frame that has become a page table is an unlogged
// reset (xhci_cancel_endpoint()).
static void maybe_free_dma(struct ums *u) {
    if (!__atomic_load_n(&u->unplugged, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&u->live, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&u->busy, __ATOMIC_SEQ_CST))
        return;
    uint64_t dma = __atomic_exchange_n(&u->dma, 0, __ATOMIC_SEQ_CST);
    if (dma && !u->dma_pinned) pmm_free_contiguous(dma, DMA_PAGES(u->data_bytes));
}

// A call entering the driver: counted BEFORE the unplugged check, so
// maybe_free_dma() -- which reads them the other way round -- can never
// free a buffer a call has just decided to use.
static int enter(struct ums *u) {
    __atomic_add_fetch(&u->busy, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&u->unplugged, __ATOMIC_SEQ_CST)) return 1;
    __atomic_sub_fetch(&u->busy, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void leave(struct ums *u) {
    if (__atomic_sub_fetch(&u->busy, 1, __ATOMIC_SEQ_CST) == 0) maybe_free_dma(u);
}

// EJECT (blk_eject() calls it, after the flush): the cache synced once
// more, then START STOP UNIT -- stop, eject. A stick with neither says
// ILLEGAL REQUEST, and it has nothing to lose, so that is success too:
// what makes it safe to pull is that nothing is mounted and every write
// already reached it.
static int ums_eject(const struct block_device *dev) {
    struct ums_lun *l = lun_of(dev);
    struct ums *u = l->u;
    if (!enter(u)) return 0;
    kmutex_lock(&u->lock);
    uint8_t cdb[16];
    struct scsi_sense why = { 0, 0, 0 };
    int n = scsi_cdb_sync_cache(cdb);
    int r = command_retry(u, l->lun, cdb, n, 0, 0, 0, &why);
    int ok = r == CMD_OK || (r == CMD_FAILED && why.key == SCSI_KEY_ILLEGAL_REQUEST);
    if (ok) {
        n = scsi_cdb_start_stop(cdb, 0, 1);
        r = command_retry(u, l->lun, cdb, n, 0, 0, 0, &why);
        ok = r == CMD_OK || (r == CMD_FAILED && why.key == SCSI_KEY_ILLEGAL_REQUEST);
    }
    kmutex_unlock(&u->lock);
    leave(u);
    return ok;
}

// The block layer is done with one LUN.
static void ums_release(const struct block_device *dev) {
    struct ums *u = lun_of(dev)->u;
    __atomic_sub_fetch(&u->live, 1, __ATOMIC_SEQ_CST);
    maybe_free_dma(u);
}

// ---- finding the disks ----------------------------------------------------

// A LUN with a medium, sized; 0 for one that is not a disk or is empty.
static int probe_lun(struct ums *u, struct ums_lun *l) {
    uint8_t cdb[16];
    uint32_t got = 0;
    int n = scsi_cdb_inquiry(cdb, SCSI_INQUIRY_LEN);
    struct scsi_inquiry q;
    if (command_retry(u, l->lun, cdb, n, 1, SCSI_INQUIRY_LEN, &got, 0) != CMD_OK ||
        !scsi_parse_inquiry(u->data, got, &q))
        return 0;
    if (q.type != 0) {
        klog_printf("usb-storage: slot %u lun %u is SCSI type %u, not a disk -- skipped\n",
                    u->slot, l->lun, q.type);
        return 0;
    }
    k_snprintf(l->model, sizeof l->model, "%s %s", q.vendor, q.product);

    // TEST UNIT READY until the medium answers: a stick spinning up its
    // controller says NOT READY for a moment, a card slot with no card
    // says it for good.
    struct scsi_sense why = { 0, 0, 0 };
    int ready = 0;
    for (int i = 0; i < 20 && !ready; i++) {
        n = scsi_cdb_test_unit_ready(cdb);
        int r = command_retry(u, l->lun, cdb, n, 0, 0, 0, &why);
        if (r == CMD_GONE) return 0;
        ready = r == CMD_OK;
        if (!ready && scsi_sense_no_medium(&why)) {
            klog_printf("usb-storage: %s (lun %u) has no medium\n", l->model, l->lun);
            return 0;
        }
        if (!ready) xhci_delay_ms(150);
    }
    if (!ready) {
        klog_printf("usb-storage: %s (lun %u) never became ready: %s\n", l->model, l->lun,
                    scsi_key_name(why.key));
        return 0;
    }

    uint64_t last = 0;
    uint32_t block = 0;
    n = scsi_cdb_read_capacity10(cdb);
    int ok = command_retry(u, l->lun, cdb, n, 1, 8, &got, 0) == CMD_OK &&
             scsi_parse_capacity10(u->data, got, &last, &block);
    if (ok && last == 0xFFFFFFFFull) {   // past 2 TiB: ask the long form
        n = scsi_cdb_read_capacity16(cdb, 32);
        ok = command_retry(u, l->lun, cdb, n, 1, 32, &got, 0) == CMD_OK &&
             scsi_parse_capacity16(u->data, got, &last, &block);
    }
    if (!ok) {
        klog_printf("usb-storage: %s (lun %u): no usable capacity\n", l->model, l->lun);
        return 0;
    }
    l->block = block;
    l->blocks = last + 1;
    return 1;
}

// The interface's two bulk endpoints, from the configuration descriptor.
static int find_endpoints(struct ums *u, const uint8_t *cfg, uint32_t total) {
    int in_if = 0;
    for (uint32_t at = 0; at + 2 <= total && cfg[at]; at += cfg[at]) {
        const uint8_t *d = cfg + at;
        if (at + d[0] > total) break;
        if (d[1] == 4 && d[0] >= 9) in_if = d[2] == u->ifnum && d[3] == 0;   // alt 0 of ours
        else if (d[1] == 5 && d[0] >= 7 && in_if && (d[3] & 3) == 2) {
            uint16_t mps = (uint16_t)(d[4] | d[5] << 8) & 0x7FF;
            if (d[2] & 0x80) { u->ep_in = d[2]; u->mps_in = mps; }
            else { u->ep_out = d[2]; u->mps_out = mps; }
        }
    }
    return u->ep_in && u->ep_out;
}

static int ums_bind(struct usb_device_info *d, int iface, const uint8_t *cfg, uint32_t total) {
    struct ums *u = kzalloc(sizeof *u);
    if (!u) return 0;
    u->slot = d->slot;
    u->ifnum = d->ifs[iface].ifnum;
    if (!find_endpoints(u, cfg, total)) {
        klog_printf("usb-storage: slot %u has no bulk IN/OUT pair -- declined\n", d->slot);
        kfree(u);
        return 0;
    }
    u->data_bytes = d->speed >= XHCI_SPEED_SUPER ? DATA_BYTES_SS : DATA_BYTES;
    u->dma = pmm_alloc_contiguous(DMA_PAGES(u->data_bytes), PMM_ZONE_DMA32);
    if (!u->dma && u->data_bytes != DATA_BYTES) {
        u->data_bytes = DATA_BYTES;
        u->dma = pmm_alloc_contiguous(DMA_PAGES(u->data_bytes), PMM_ZONE_DMA32);
    }
    if (!u->dma) { kfree(u); return 0; }   // nothing has seen it yet: a plain free
    u->data = (uint8_t *)(uintptr_t)u->dma;
    u->cbw = u->data + u->data_bytes;
    u->csw = u->cbw + 64;
    if (xhci_add_bulk(u->slot, u->ep_in, u->mps_in, xfer_done, u) < 0 ||
        xhci_add_bulk(u->slot, u->ep_out, u->mps_out, xfer_done, u) < 0) {
        klog_printf("usb-storage: slot %u: could not configure its endpoints\n", d->slot);
        pmm_free_contiguous(u->dma, DMA_PAGES(u->data_bytes));
        kfree(u);
        return 0;
    }
    xhci_bulk_own_recovery(u->slot, u->ep_in);
    xhci_bulk_own_recovery(u->slot, u->ep_out);

    // GET MAX LUN. A device with one LUN may stall it (BOT spec 3.2).
    uint8_t max_lun = 0;
    if (control(u, 0xA1, 0xFE, 0, &max_lun, 1) < 0) max_lun = 0;
    if (max_lun >= MAX_LUNS) max_lun = MAX_LUNS - 1;
    u->luns = kzalloc((size_t)(max_lun + 1) * sizeof *u->luns);
    if (!u->luns) {
        xhci_cancel_endpoint(u->slot, u->ep_in);
        xhci_cancel_endpoint(u->slot, u->ep_out);
        u->unplugged = 1;   // never registered anything: the buffer goes now
        maybe_free_dma(u);
        return 0;
    }
    u->nluns = max_lun + 1;

    for (int i = 0; i < u->nluns; i++) {
        struct ums_lun *l = &u->luns[i];
        l->u = u;
        l->lun = (uint8_t)i;
        if (!probe_lun(u, l)) continue;
        l->blk.name = "usb";
        l->blk.driver = "usb-storage";
        l->blk.model = l->model;
        l->blk.sector_count = ums_count;
        l->blk.read_sectors = ums_read;
        l->blk.write_sectors = ums_write;
        l->blk.max_sectors_per_xfer = ums_xfer_max;
        l->blk.persistent = 1;
        l->blk.removable = 1;
        l->blk.caps = BLK_CAP_FLUSH;
        l->blk.flush = ums_flush;
        l->blk.block_size = l->block;
        l->blk.release = ums_release;
        l->blk.eject = ums_eject;
        __atomic_add_fetch(&u->live, 1, __ATOMIC_SEQ_CST);
        if (!blk_add_disk(&l->blk)) { __atomic_sub_fetch(&u->live, 1, __ATOMIC_SEQ_CST); continue; }
        l->registered = 1;
        klog_printf("usb-storage: %s, %llu MiB in %u-byte blocks, as %s\n", l->model,
                    (unsigned long long)(l->blocks * l->block >> 20), l->block,
                    blk_device_name(&l->blk));
    }
    // Bound even with no disk yet: a card reader with every slot empty is
    // still this driver's, and holding it keeps the endpoints configured.
    u->next = g_ums;
    g_ums = u;
    return 1;
}

static void ums_unbind(struct usb_device_info *d, int iface) {
    struct ums *u = 0;
    for (struct ums **pp = &g_ums; *pp; pp = &(*pp)->next) {
        if ((*pp)->slot != d->slot || (*pp)->ifnum != d->ifs[iface].ifnum) continue;
        u = *pp;
        *pp = u->next;
        break;
    }
    if (!u) return;
    __atomic_store_n(&u->unplugged, 1, __ATOMIC_SEQ_CST);
    scheduler_wake(u, 0);   // a command waiting on a transfer sees it at once
    if (xhci_cancel_endpoint(u->slot, u->ep_in) < 0 ||
        xhci_cancel_endpoint(u->slot, u->ep_out) < 0)
        u->dma_pinned = 1;
    for (int i = 0; i < u->nluns; i++)
        if (u->luns[i].registered) blk_device_gone(&u->luns[i].blk);   // may release at once
    maybe_free_dma(u);   // `u` itself stays: see the top of this file
}

// SCSI transparent command set over bulk-only: every stick and card reader.
static const struct usb_match ums_matches[] = { USB_MATCH_IF(0x08, 0x06, 0x50) };
USB_DRIVER("usb-storage", ums_matches, ums_bind, ums_unbind);
