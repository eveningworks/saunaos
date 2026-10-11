#ifndef ULIB_UBOOTLAYOUT_H
#define ULIB_UBOOTLAYOUT_H

// Where GRUB's files are on the boot volume, and the one-time move from
// the layout before this one.
//
// The boot volume (FAT32, mounted at /boot) holds kernel.bin and grub/
// at its ROOT -- /boot/kernel.bin, /boot/grub/grub.cfg -- the shape of a
// Linux /boot partition. Disks written before that kept everything one
// level down, so toy-os showed /boot/boot/... ; GRUB's core image on
// those machines has `/boot/grub` baked into its prefix.
//
// THE MOVE IS TWO HALVES AROUND A CORE-IMAGE WRITE, and only `install
// --bootloader` does the write: copy_up() makes the flat tree beside the
// old one (the old core keeps booting the old tree), the caller writes a
// core image whose prefix is the flat one, then drop_legacy() removes
// /boot/boot. A failed write is undone with drop_flat(). Every step
// leaves a disk GRUB can boot.

#define UBOOTLAYOUT_DIR     "/boot"
#define UBOOTLAYOUT_LEGACY  "/boot/boot"

// 1 when /boot still has the old layout (/boot/boot/grub/grub.cfg) --
// including halfway through the move, before drop_legacy().
int ubootlayout_legacy(void);

// The directory kernel.bin and grub/ are in on THIS machine: /boot, or
// /boot/boot on a machine not yet moved. For the code that must keep
// working either way -- System Update most of all.
const char *ubootlayout_dir(void);

// Copy /boot/boot's contents up to /boot and point grub.cfg's kernel
// paths at the volume's root (`/boot/kernel.bin` -> `/kernel.bin`).
// /boot must be writable. 0, or -1 with `*why`.
int ubootlayout_copy_up(const char **why);

// Remove what copy_up() made, for a bootloader write that failed.
void ubootlayout_drop_flat(void);

// Remove /boot/boot, once the core image reading /boot/grub is in place.
// 0, or -1 with `*why`.
int ubootlayout_drop_legacy(const char **why);

#endif
