#ifndef UUI_DISKPLAN_H
#define UUI_DISKPLAN_H

// uui_diskplan -- what a disk holds NOW and what it will hold AFTER an
// operation, as two labelled partition maps one above the other: an
// installer's or a partitioner's before-and-after (Calamares' partition
// page, Windows Setup's). The Now row is the disk as lib/udiskscan.h
// read it, gaps of a megabyte or more drawn as free space; the After
// row is the caller's list. Each map is a uui_segbar, so a partition
// looks here as it does in Disks.
//
// Display only: no hit, no keys. The rows are the widget's; the caller
// hands it a disk and the new partitions and redraws.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_segbar.h"
#include "lib/udiskscan.h"

#define UUI_DISKPLAN_NEW_MAX 4

struct uui_diskplan_new {
    uint64_t start, count;     // 512-byte sectors
    int role;                  // UDISK_ROLE_*: firmware partitions are drawn apart
    const char *title;         // "/boot", "SaunaOS"
    const char *detail;        // "FAT32"; the size is added
};

struct uui_diskplan {
    int x, y, w, h;
    // OWNED
    struct uui_segbar now, after;
    struct uui_segbar_seg now_segs[UUI_SEGBAR_MAX], after_segs[UUI_DISKPLAN_NEW_MAX];
    char now_title[UUI_SEGBAR_MAX][32], now_detail[UUI_SEGBAR_MAX][40], now_corner[UUI_SEGBAR_MAX][24];
    char after_title[UUI_DISKPLAN_NEW_MAX][32], after_detail[UUI_DISKPLAN_NEW_MAX][40];
    int label_w;
};

void uui_diskplan_init(struct uui_diskplan *p);
// Show `disk` now (NULL: an empty row) and `after` (n of them, at most
// UUI_DISKPLAN_NEW_MAX). Both are copied.
void uui_diskplan_set(struct uui_diskplan *p, const struct udisk *disk,
                      const struct uui_diskplan_new *after, int n);
// A partition card's colours by what it IS (a UDISK_ROLE_*): firmware
// amber, `strong` (a filesystem) the accent's tint, the rest grey --
// theme roles, shared with Disks so the two maps agree.
void uui_diskplan_colour(struct uui_segbar_seg *s, int role, int strong);

extern const struct uui_widget_ops uui_diskplan_ops;

#endif
