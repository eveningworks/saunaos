#ifndef ISO9660_H
#define ISO9660_H

// ISO 9660 and Joliet, read-only, from an image FILE (kernel/fs/iso9660.c).
// Nothing probes a device for it: it is mounted only by
// iso9660_mount_image(), which SYS_MOUNT's SYS_MNT_IMAGE reaches.

struct fs_ops;
extern const struct fs_ops iso9660_ops;

// Mounts the image at absolute, normalized `image` at `point`,
// read-only. 1, or 0 and *why.
int iso9660_mount_image(const char *image, const char *point, const char **why);

// Opens and checks an image without mounting it: 1 with *out (close it
// with iso9660_close()) and its volume label in `label` (Joliet's when
// it has one; may be NULL), or 0 and *why. For a test and the mount.
struct iso_vol;
int iso9660_open(struct iso_vol **out, const char *image, char *label, int label_cap, const char **why);
void iso9660_close(struct iso_vol *v);

#endif
