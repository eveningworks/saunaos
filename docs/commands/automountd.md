# automountd

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    automountd

## Description

`/bin/automountd` mounts USB sticks as they arrive. init starts it from
`/etc/services.d/automountd`; it takes no arguments.

**Arriving.** Each volume on a removable disk that this system can read
-- FAT32, exFAT, TFS3 -- is mounted at `/media/<name>`: its label, else
the stick's model, with `/` and spaces made `-` so a shell can type it,
and `-2`, `-3`... when two sticks share a name. Only a volume it has not
seen is mounted: a stick unmounted by hand stays unmounted until it is
plugged in again.

**Leaving.** A stick pulled out while mounted leaves its mount read-only
and marked `removed` (`mount`); automountd unmounts it and removes its
`/media` folder.

**Eject.** The desktop's Eject -- the USB tray flyout, the File
Manager's Removable group -- asks automountd over its channel (`drives`,
`lib/udrive.h`), because unmounting and ejecting wait on the disk and
the compositor must never block. `eject` does the same work directly.

It wakes on device changes through its wakeword (`SYS_DEV_WATCH`), not
by polling. What it did goes to its standard output -- the system log.

## See also

`eject`, `mount`, `lsblk`; `docs/usb-storage-design.md`.
