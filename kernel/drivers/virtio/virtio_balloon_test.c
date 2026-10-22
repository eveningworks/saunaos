// KTEST for virtio-balloon through the LIVE driver. The gate scans
// pci.c's own table, so a driver that failed to bind FAILS rather than
// skips (virtio_rng_test.c's rule).
#include "virtio.h"
#include "virtio_balloon.h"
#include "page_report.h"
#include "pci.h"
#include "string.h"
#include "ktest.h"

static int balloon_on_bus(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->vendor_id == VIRTIO_PCI_VENDOR &&
            (d->device_id == 0x1045 || d->device_id == 0x1002)) return 1;
    }
    return 0;
}

KTEST("virtio-balloon", "the device on the bus is the registered page reporter") {
    if (!balloon_on_bus()) KTEST_SKIP("no virtio-balloon on this machine");
    KTEST_ASSERT(virtio_balloon_present());
    const char *name = page_report_name();
    KTEST_ASSERT(name && k_strcmp(name, "virtio-balloon") == 0);
}
