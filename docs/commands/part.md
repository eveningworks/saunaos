# part

**a `/bin` program.**

**Category:** Storage

## Synopsis

    part delete PARTITION confirm | resize PARTITION SIZE confirm

## Commands

- `delete PARTITION confirm` -- remove PARTITION from its disk's table.
  PARTITION is the name `lsblk` prints (`usb1p2`). Without `confirm` it
  only says what would go, and changes nothing.
- `resize PARTITION SIZE confirm` -- make PARTITION SIZE long (40G, +10G
  or max), its filesystem with it. SIZE is a length -- `40G`, `512M`,
  `40GiB` -- or `+10G` to grow it by that much, or `max` for all the
  free space after it; a bare number is sectors, as `mkpart` reads one.
  Without `confirm` it only says what would change.

## Options

- `-h`, `--help` -- print the usage and exit.

## Description

Changes ONE partition of a disk, editing the table in place: every other
partition keeps its bytes -- its GUIDs, its name, its attribute bits --
its NUMBER, and so its device name. Deleting `usb1p1` leaves `usb1p2`
called `usb1p2`, as Linux and Windows leave it. That is the difference
from `mkpart`, which writes a whole new table.

The disk is re-read at once: the deleted partition's device is gone from
`lsblk` without a reboot, and a partition still mounted beside it is not
disturbed.

Refused, with the reason:

- a partition that is mounted -- `umount` it first;
- a partition on the disk the system runs from, whose table is what the
  next boot finds the root by;
- a whole disk (`usb1` rather than `usb1p2`).

Nothing is erased: the data stays on the disk until something is written
over it, but nothing in SaunaOS can find it again.

**A resize moves the end** -- the start stays -- and the filesystem
inside follows, in the safe order: growing, the table first and then
the filesystem (`SYS_FS_RESIZE`), sfdisk-then-resize2fs's order. A grow
cut off between the two leaves room the filesystem does not use yet; it
mounts as it was, and the same command again finishes it. What can be
resized today:

- **tfs3 grows**, into the free space right after the partition. It
  cannot shrink yet, and it grows to 512 GiB at most from a smaller
  size (past that its descriptor table would grow, which moves every
  group); a volume of tfs3 format version 1 or 2 does not grow.
- **FAT32 grows**, to FAT32's cluster limit or 2 TiB. When its FATs
  already have entries for the new clusters (a FAT is whole sectors, so
  there is usually a little room) only its size changes. Otherwise the
  FATs grow and the data after them MOVES -- only the clusters in use,
  so the time is the used data, not the partition -- and **FAT32 keeps
  no journal: a grow interrupted while its data moves (a power cut, the
  disk pulled out) loses the volume.** `part` says so before `confirm`
  and while it runs; back the volume up first. A volume marked dirty
  (not unmounted cleanly), one whose FATs are not mirrored, and a move
  over clusters marked bad are refused.
- **FAT32 shrinks** to what its files need, and never under 65525
  clusters for a volume that has that many (fewer is FAT16 by
  Microsoft's rule, which every system's mount follows) -- `part` names
  the smallest size when asked for less. The filesystem goes first, then
  the table: every cluster in use past the new end moves below it, and
  the FAT links, folder entries and `.`/`..` naming it follow. **The same
  warning holds: no journal, so an interrupted shrink loses the volume.**
- **exFAT** is not resized yet, and a filesystem SaunaOS cannot mount
  (NTFS, ext4) is refused -- moving the table under it would cut it
  short.
- **An empty partition** -- nothing recognised in it -- grows and
  shrinks freely: only the table changes.

Each refusal prints the kernel's reason.

## Examples

    part delete usb1p2
    part: this DELETES usb1p2 (24.0 GiB tfs3 "Photos") on usb1, and whatever is in it.
    part: re-run as `part delete usb1p2 confirm` if that is what you want.

    part delete usb1p2 confirm
    Deleted usb1p2 (24.0 GiB tfs3 "Photos") on usb1. The other partitions keep their names.

    part resize usb1p2 36G
    part: this resizes usb1p2 from 24.0 GiB to 36.0 GiB; its tfs3 filesystem follows.
    part: re-run as `part resize usb1p2 36G confirm` if that is what you want.

    part resize usb1p2 36G confirm
    Resized usb1p2 from 24.0 GiB to 36.0 GiB; its tfs3 filesystem is 36.0 GiB now.

    part resize usb1p1 +1G
    part: this resizes usb1p1 from 15.0 GiB to 16.0 GiB; its fat32 filesystem follows.
    part: FAT32 keeps no journal -- if this is interrupted while its data moves (a power cut,
          the disk pulled out), the volume is lost. Back it up first.
    part: re-run as `part resize usb1p1 +1G confirm` if that is what you want.

    part resize usb1p1 +1G confirm
    part: growing its FAT32 filesystem -- do not power off or pull the disk until this ends.
    Resized usb1p1 from 15.0 GiB to 16.0 GiB; its fat32 filesystem is 16.0 GiB now.

    part resize usb1p3 +1G confirm
    part: usb1p3 cannot be resized -- exFAT volumes cannot be resized yet

## See also

`mkpart`, `parttable`, `lsblk`, `mkfs`, `fsck`; Disks, which does the
same from its Delete and Resize... commands.
