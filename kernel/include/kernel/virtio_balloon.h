#ifndef VIRTIO_BALLOON_H
#define VIRTIO_BALLOON_H

// virtio-balloon, free page reporting only: the reporter behind
// page_report.h. Nothing to call -- it registers itself at probe.
#include <stdint.h>

int virtio_balloon_present(void);  // bound, and the host offered reporting
uint32_t virtio_balloon_reports(void); // batches the host acknowledged

#endif
