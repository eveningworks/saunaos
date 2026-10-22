// virtio-balloon: free memory back to the host, by free page reporting.
//
// Of the device's four jobs this does one. The reporting queue
// (VIRTIO_BALLOON_F_REPORTING, spec 5.5.6.7) takes device-WRITABLE
// buffers naming free guest memory; QEMU discards each with
// madvise(DONTNEED) and hands the buffer back, and the guest frees the
// frames again -- page_report.c decides which and when. Classic
// inflate/deflate (the host asking for a size through `num_pages`), the
// stats queue and free page hinting are not driven: a host `balloon`
// command is simply never met.
//
// THE REPORTING QUEUE'S INDEX DEPENDS ON WHAT WAS NEGOTIATED. The spec
// numbers queues densely, skipping an absent one; QEMU creates the stats
// queue whether or not it is negotiated. Taking STATS_VQ (QEMU always
// offers it) makes both say index 3. QEMU's `free-page-hint=on` would
// add a queue before it and is not supported.
//
// It needs `-device virtio-balloon-pci,free-page-reporting=on`; without
// the property the device is declined, since there is nothing else here
// for it to do.
#include "virtio.h"
#include "virtio_balloon.h"
#include "page_report.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "pci_driver.h"

DRIVER_DECLARE("virtio-balloon", "memory", "virtio free page reporting");

#define VIRTIO_BALLOON_F_STATS_VQ  (1ull << 1)
#define VIRTIO_BALLOON_F_REPORTING (1ull << 5)

static struct virtio_device g_dev;
static struct virtqueue g_vq;
static int g_present;
static uint32_t g_reports;

int virtio_balloon_present(void) { return g_present; }
uint32_t virtio_balloon_reports(void) { return g_reports; }

static int report(const struct page_report_run *runs, int n) {
    struct virtio_sg in[PAGE_REPORT_BATCH_BLOCKS];
    if (n > PAGE_REPORT_BATCH_BLOCKS) return PAGE_REPORT_REFUSED;
    for (int i = 0; i < n; i++) {
        in[i].phys = runs[i].phys;
        in[i].len = (uint32_t)runs[i].len;  // at most a batch, 64 MiB
    }
    int head = virtqueue_submit(&g_vq, 0, 0, in, n);
    if (head < 0) return PAGE_REPORT_REFUSED;
    virtqueue_kick(&g_vq);
    if (!virtqueue_poll(&g_vq, head, 0)) return PAGE_REPORT_LOST;  // poll logged it
    g_reports++;
    return PAGE_REPORT_TAKEN;
}

static const struct pci_match virtio_balloon_matches[] = {
    VIRTIO_PCI_MATCH_MODERN(VIRTIO_ID_BALLOON), PCI_MATCH_ID(VIRTIO_PCI_VENDOR, 0x1002),
};

static int virtio_balloon_probe(const struct pci_device *pci) {
    if (g_dev.pci) return pci_probe_decline(pci, "a second virtio-balloon; one is driven");
    g_dev.name = "virtio-balloon";
    if (!virtio_pci_attach(pci, VIRTIO_ID_BALLOON, &g_dev))
        return pci_probe_decline(pci, "the virtio transport did not attach");
    if (!virtio_begin(&g_dev, VIRTIO_BALLOON_F_STATS_VQ | VIRTIO_BALLOON_F_REPORTING))
        return pci_probe_decline(pci, "feature negotiation failed");
    if (!virtio_has_feature(&g_dev, VIRTIO_BALLOON_F_REPORTING)) {
        virtio_fail(&g_dev);
        return pci_probe_decline(pci, "the host offers no free page reporting "
                                      "(QEMU: free-page-reporting=on)");
    }
    uint16_t q = virtio_has_feature(&g_dev, VIRTIO_BALLOON_F_STATS_VQ) ? 3 : 2;
    if (!virtqueue_setup(&g_dev, q, &g_vq)) {
        virtio_fail(&g_dev);
        return pci_probe_decline(pci, "could not set up its reporting queue");
    }
    virtio_driver_ok(&g_dev);
    g_present = 1;
    page_report_register("virtio-balloon", report);
    return 0;
}
PCI_DRIVER("virtio-balloon", virtio_balloon_matches, virtio_balloon_probe);
