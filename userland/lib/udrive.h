#ifndef ULIB_UDRIVE_H
#define ULIB_UDRIVE_H
// Removable drives -- USB sticks -- as the desktop shows them: each
// volume on a removable disk, its name, what it holds and where it is
// mounted; mounting one at /media/<name>; ejecting a disk; and asking the
// auto-mount service to do either (docs/usb-storage-design.md, stage 5).
//
// THE SERVICE OWNS THE DISK WORK. Mounting and ejecting wait on the
// disk, and the compositor must never block, so the tray and the File
// Manager ASK automountd (udrive_ask_*()) rather than doing it
// themselves; automountd and `eject` call the doing half directly.
#include <stddef.h>
#include <stdint.h>

// One volume on a removable disk: a partition, or the disk itself when
// it has no table. `name` is what a person reads: its label, else the
// disk's model, else "USB drive".
struct udrive {
    char disk[16];      // "usb0" -- what Eject takes
    char dev[16];       // "usb0p1", or "usb0" for a whole-disk volume
    char name[64];
    char model[48];
    char fstype[16];    // "" when nothing this system can mount is on it
    char point[64];     // where it is mounted, "" when it is not
    uint64_t total, used;   // bytes, while mounted
    uint64_t size;          // bytes of the volume, mounted or not
    int mountable;
    int no_automount;       // the disk says not to mount it unasked (a test's)
};

// Every removable volume, freshly read: an array the caller frees, and
// its count (0 and *out NULL when there is none, -1 on failure).
int udrive_list(struct udrive **out);

// "/media/<name>" with what a path cannot hold made safe, and a number
// added when another mount already has it. Into `out`; 0 on success.
int udrive_media_point(const struct udrive *d, char *out, size_t cap);

// Mounts `d` at a fresh /media point, made for it. 0, or -1 and errno.
int udrive_mount(const struct udrive *d, char *point_out, size_t cap);

// Unmounts everything on `disk` (removing /media folders it made) and
// ejects it. 0, or -1 and errno -- EBUSY naming nothing: a file is open.
int udrive_eject(const char *disk);

// The line under a drive's name: "32.0 GiB FAT32 \xb7 25.3 GiB free" while
// mounted, its size and filesystem otherwise ("nothing readable" when
// this system cannot mount it).
void udrive_describe(const struct udrive *d, char *out, size_t cap);

// The disk a volume is on ("usb0p1" -> "usb0"), from the kernel's table.
// 0, or -1 when `dev` is no block device.
int udrive_disk_of(const char *dev, char *disk, size_t cap);

// ---- asking the service (automountd's channel, "drives") ----------------

#define UDRIVE_SERVICE "drives"
#define UDRIVE_EJECT 1
#define UDRIVE_MOUNT 2

struct udrive_msg {
    uint32_t verb;      // UDRIVE_*
    int32_t result;     // in a reply: 0, or a negative errno
    char dev[16];       // the disk (EJECT) or the volume (MOUNT)
};

// Never waits: success shows in what the desktop already watches -- the
// mount list and the device events. 0 when the request was queued, -1
// when there is no service.
int udrive_ask_eject(const char *disk);
int udrive_ask_mount(const char *dev);
// The service's next answer, if one has come: 1 with it in `out`, else 0.
// How a refusal ("still in use") reaches the person who asked.
int udrive_take_reply(struct udrive_msg *out);

#endif
