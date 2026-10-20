# lsblk

**a `/bin` program.**

**Category:** System information

## Synopsis

    lsblk [-f]

## Options

| Option | Meaning |
|---|---|
| `-f`, `--fs` | show each device's filesystem and label instead of its geometry |
| `-h`, `--help` | print the usage and exit |

## Description

`/bin/lsblk` — every block device this boot found: each disk a driver
enumerated, each partition the kernel named, which one carries the root,
and whether it survives a power cycle.

**The names it prints are the names `root=` and `mount` take**, and that
is most of the reason to print them. `mount ahci0p1 /mnt` is not
something anyone can guess, and before this there was no way to find out
what the second disk was called — or that it existed.

A name is `<driver><index>` for a disk and `<disk>p<n>` for a partition:
`ata0`, `ahci0`, `virtio0`, `ram0`, `ahci0p1`. The partition number is
the **partition table's**, so it matches what `parttable` prints — the
third entry is `p3` even when the two ahead of it are the firmware's and
were skipped.

## What it is not

**Not `parttable`.** That reads a table off a *disk* and reports what is
written there; this reports what the *kernel* found and named. They can
disagree, usefully: a partition whose window the kernel refused (one
extending past the disk's end, say) is in `parttable`'s output and not
in this one, and that difference is the diagnosis.

**Not `df`.** `df` lists what is *mounted*. Most devices here are not,
and that is the normal state — a second drive is enumerated so that it
*can* be mounted.

## Output

    NAME        SIZE      SECTOR TYPE   ROOT  WHERE
    ata0        9.0G      512    disk         disk
    ata0p1      1.0M      512    part         ata0 at lba 2048
    ata0p2      64.0M     512    part         ata0 at lba 4096
    ata0p3      8.9G      512    part   yes   ata0 at lba 135168
    virtio0     1.0G      4096   disk         disk

`SECTOR` is the device's logical block: 512, or 4096 on a 4K-sector disk
(a virtio or NVMe drive can be either). The `lba` in `WHERE` is always in
512-byte units whatever it says -- the kernel counts in those everywhere,
as Linux's sysfs does -- so a partition on a 4K disk starts at a multiple
of 8.

`WHERE` is the parent disk and starting LBA for a partition — the number
that lines a row up against `parttable`. For a whole disk it says
`disk`, or `memory (not persistent)` for a RAM-backed live image, since
"this will not survive a reboot" is the one property a person most needs
not to have to infer.

### `-f`: what each device holds

    NAME        SIZE      FSTYPE   LABEL            MOUNTED
    ahci0       7.3T
    ahci0p1     7.3T      ntfs     Backup           (cannot mount)
    nvme0       238.4G
    nvme0p1     64.0M     fat32                     /boot
    nvme0p2     238.4G    tfs3                      /

util-linux's `lsblk -f`, and the same answer Disks shows. `FSTYPE` is
read from the volume's **signature**, mounted or not, by the kernel
(`kernel/fs/fs_ident.c`) -- ring 3 has no raw access to a disk, which is
where libblkid reads it on Linux. It names filesystems this system cannot
mount too: NTFS, exFAT, FAT12/16/32, ext2/3/4, Btrfs, XFS, Linux swap,
ISO 9660, HFS+, APFS, LUKS, BitLocker, ReFS, and tfs3. A disk that
carries a partition table shows nothing; its partitions answer. An empty
`FSTYPE` means no signature was recognised -- a blank volume, or one
this list does not know.

`MOUNTED` is the mount point, or `(cannot mount)` for a filesystem the
kernel has no driver for.

A machine with no disk at all prints `no block devices` rather than a
bare header: that is a real supported state (the root is ramfs then),
and it should not read like the command failed.

## See also

`parttable` for a disk's own table, `mount` for attaching one of these,
`df` for what is mounted, and `docs/boot-flags.md` for `root=`.
