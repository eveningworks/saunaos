# eject

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    eject DISK

## Options

- `-h`, `--help` -- print the usage and exit.

## Description

Makes a USB stick (or any removable disk) safe to pull out: everything
mounted from DISK or a partition of it is unmounted first, then the disk
is told -- a stick gets its cache synced and SCSI START STOP UNIT -- and
it goes away, as if it had been pulled, but recorded as **ejected**.
DISK is the name `lsblk` prints (`usb0`), never a partition.

A disk that is still in use -- a file open on it, a shell standing in it
-- is not ejected, and says so. A folder `automountd` made under `/media`
for it is removed. A disk that is not
removable is refused, and so is the disk the system runs from. To use an ejected stick again, unplug it and plug
it back in.

Pulling a stick out WITHOUT ejecting loses nothing a finished write put
on it: a removable disk gets quick removal (every write is on it before
the call that made it returns). What it can lose is a copy still running.

## Examples

    eject usb0
    eject: usb0 can be pulled out

## See also

`lsblk`, `mount`, `umount`; `docs/usb-storage-design.md`.
