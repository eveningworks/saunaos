# part

**a `/bin` program.**

**Category:** Storage

## Synopsis

    part delete PARTITION confirm

## Commands

- `delete PARTITION confirm` -- remove PARTITION from its disk's table.
  PARTITION is the name `lsblk` prints (`usb1p2`). Without `confirm` it
  only says what would go, and changes nothing.

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

## Examples

    part delete usb1p2
    part: this DELETES usb1p2 (24.0 GiB tfs3 "Photos") on usb1, and whatever is in it.
    part: re-run as `part delete usb1p2 confirm` if that is what you want.

    part delete usb1p2 confirm
    Deleted usb1p2 (24.0 GiB tfs3 "Photos") on usb1. The other partitions keep their names.

## See also

`mkpart`, `parttable`, `lsblk`, `mkfs`; Disks, which does the same from
its Delete command.
