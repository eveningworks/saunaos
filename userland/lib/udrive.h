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
    char name[64];      // its label, else the stick's model
    int labeled;        // `name` is the volume's own label
    char model[48];
    char fstype[16];    // "" when nothing this system can mount is on it
    char point[64];     // where it is mounted, "" when it is not
    uint64_t total, used;   // bytes, while mounted
    uint64_t size;          // bytes of the volume, mounted or not
    uint64_t disk_size;     // bytes of the whole stick it is on
    int mountable;
    int no_automount;       // the disk says not to mount it unasked (a test's)
    // A DISC IMAGE mounted from a file (lib/uiso.h): `disk` and `dev` are
    // "", `fstype` "iso9660", and Eject unmounts it (udrive_eject_image()).
    int image;
    char source[64];        // its file, as QUERY_FSINFO gives it ("..."-ended when longer)
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
// ejects it -- a disc image whose FILE is on it is unmounted first. 0, or
// -1 and errno -- EBUSY naming nothing: a file is open.
int udrive_eject(const char *disk);

// ---- disc images: a file mounted as a CD (kernel/fs/iso9660.c) -----------

// Mounts the image at `path` read-only at /media/<its label>, or answers
// where it is mounted already -- one image, one drive. 0 with the point,
// or -1 and errno (ENODEV: no disc image).
int udrive_mount_image(const char *path, char *point_out, size_t cap);
// Where the image at `path` is mounted, into `point`: 1, or 0 when it is not.
int udrive_image_point(const char *path, char *point, size_t cap);
// Unmounts the image mounted at `point` and removes the /media folder made
// for it; 0 when nothing is mounted there any more, EINVAL when what is
// mounted there is no disc image.
int udrive_eject_image(const char *point);
// Every disc image whose FILE is under `point` (a stick about to go).
int udrive_eject_images_under(const char *point);

// The line under a drive's name: "32.0 GiB FAT32 \xb7 25.3 GiB free" while
// mounted, its size and filesystem otherwise ("nothing readable" when
// this system cannot mount it).
void udrive_describe(const struct udrive *d, char *out, size_t cap);
// The same without the filesystem, for a line that names it already
// ("330 MiB \xb7 328 MiB free" under "FAT32 volume").
void udrive_describe_space(const struct udrive *d, char *out, size_t cap);
// The filesystem as it is written: "FAT32", "exFAT", "TFS3".
const char *udrive_fs_name(const struct udrive *d);

// The disk a volume is on ("usb0p1" -> "usb0"), from the kernel's table.
// 0, or -1 when `dev` is no block device.
int udrive_disk_of(const char *dev, char *disk, size_t cap);

// ---- asking the service (automountd's channel, "drives") ----------------

#define UDRIVE_SERVICE "drives"
#define UDRIVE_EJECT 1
#define UDRIVE_MOUNT 2
#define UDRIVE_MOUNT_IMAGE 3    // the image is the PATH sent before it
#define UDRIVE_EJECT_IMAGE 4    // the image's mount point is the PATH sent before it
#define UDRIVE_PATH_PART 5      // a struct udrive_path_part

// ONE MESSAGE IS A uchan SLOT, 64 bytes (lib/uchan_page.h), and a reply
// 32 -- so a path travels as PATH_PART pieces the service joins for that
// client, and a reply never carries one: where an image went is read
// from the mount table (udrive_image_point()).
struct udrive_msg {
    uint32_t verb;      // UDRIVE_*
    int32_t result;     // in a reply: 0, or a negative errno
    char dev[16];       // the disk (EJECT) or the volume (MOUNT)
};

#define UDRIVE_PART_BYTES 56
struct udrive_path_part {
    uint32_t verb;      // UDRIVE_PATH_PART
    uint32_t offset;    // where these bytes go; 0 starts a new path
    char part[UDRIVE_PART_BYTES];   // NUL-padded; the path ends at the first NUL
};

// Never waits: success shows in what the desktop already watches -- the
// mount list and the device events. 0 when the request was queued, -1
// when there is no service.
int udrive_ask_eject(const char *disk);
int udrive_ask_mount(const char *dev);
int udrive_ask_mount_image(const char *path);
int udrive_ask_eject_image(const char *point);
// For a PROGRAM, never the compositor: asks the service to mount the image
// and waits up to `timeout_ms` for where. 0 with the point, or -1 and errno
// (ETIMEDOUT, or ENOSYS when no service runs).
int udrive_mount_image_wait(const char *path, char *point, size_t cap, int timeout_ms);
// The service's next answer, if one has come: 1 with it in `out`, else 0.
// How a refusal ("still in use") reaches the person who asked.
int udrive_take_reply(struct udrive_msg *out);

#endif
