# USB mass storage

A staged plan for USB sticks, card readers and USB disks: both
transports, a disk that can disappear, quick removal, exFAT, auto-mount
with Eject, and a root filesystem on the stick.

Written 2026-10-22. **Stages 1, 2, 4 and 5 are BUILT** (the same day;
exFAT was taken before UAS, which no stick speaks); the others are not. The stage markers go on the list below as they land.

## The finding that shapes the plan

**The transport is the small part. A disk that can DISAPPEAR is the
large one.** Bulk-only transport is a 31-byte command wrapper, a data
phase and a 13-byte status on two bulk endpoints the xHCI driver already
drives for Ethernet. What nothing in SaunaOS can do today is lose a disk:

- the block registry has `blk_untrack()` for a partition re-read and
  nothing for a whole disk going away; its table is a fixed 32 entries;
- a mount holds a `const struct block_device *` for its whole life, and
  so do partition windows (`block_part.c`) and the disk cache's key;
- nothing tells ring 3 a device came or went -- `devevent` is a log a
  reader asks afterwards ("nothing listens for a device event yet"), and
  Disks and the File Manager refresh on F5 or a timer;
- the mount table is a fixed 16.

AHCI hot-plug is parked on the same missing piece
(`docs/roadmap-details.md`, "Hot-plug and surprise removal"). Building it
once, for USB, unparks that too.

## What real systems do

| | transport | above it | a stick appears | pulled while mounted |
|---|---|---|---|---|
| **Linux** | `usb-storage` (BOT), `uas` | SCSI midlayer -> `sd` -> block device | uevent -> udev -> udisks2 mounts at `/run/media/$USER/LABEL`, the desktop notifies | SCSI device goes offline; I/O fails `EIO`; the mount stays until unmounted (lazily) |
| **Windows** | `USBSTOR.SYS`, `UASPStor.sys` | `disk.sys`, a storage class driver | PnP -> a drive letter, AutoPlay | "Quick removal" (default since 1809): no write caching, so pulling it is safe; "Better performance" needs Safely Remove |
| **macOS** | IOUSBMassStorageDriver / UAS | IOSCSI -> IOMedia | DiskArbitration mounts at `/Volumes/LABEL` | "Disk Not Ejected Properly" |

Every one puts **SCSI between the transport and the disk**: both BOT and
UAS carry SCSI command blocks, so the disk logic (INQUIRY, READ
CAPACITY, READ/WRITE(10/16), SYNCHRONIZE CACHE, sense data) is written
once and the transports are thin. SaunaOS copies that shape and not the
size: no SCSI midlayer with hosts, targets and queues -- one helper
library that builds CDBs and decodes sense, and a per-transport
"run this CDB" callback.

## What exists, measured (2026-10-22)

- **xHCI**: bulk endpoints with driver-owned buffers and a completion
  callback from the event drain (`xhci_add_bulk`, `xhci_bulk_post`,
  `xhci_bulk_halted`), used by `rtl_usb.c` and `net_usb_ecm.c`. Halt
  recovery on a stall. **No streams** -- UAS on a SuperSpeed device
  needs them.
- **USB core**: hot-plug, hubs (USB 2), `USB_DRIVER()` binding by
  interface class with bind/unbind, interface claiming.
- **Block layer**: a registry naming `<driver><n>` and `<disk>p<n>`,
  partition windows, a write-through disk cache keyed by whole disk,
  4 KiB-sector support, flush/trim capabilities with an honesty check.
- **Filesystems**: TFS3 and FAT32 mount from any block device; Disks
  names exFAT and NTFS but cannot open them (`docs/roadmap.md`).
- **Booting from a stick**: `make usb-image` boots, but the root is the
  live image in RAM (`docs/rootfs-design.md`) -- the kernel cannot read
  the stick it booted from.

## Decided (2026-10-22, the maintainer)

| question | answer |
|---|---|
| scope | everything below, including a root filesystem on the stick |
| transports | bulk-only AND UAS |
| pulled without Eject | **quick removal**: a removable disk gets no write-back |
| filesystems | FAT32 and TFS3 as today, **plus exFAT read/write** |
| where the driver runs | the kernel, beside the other `USB_DRIVER()` classes |

The visible parts -- the plug notice, Eject, how a stick shows in the
File Manager and Disks, the mount point's name -- get a mockup canvas
before any of their code.

## The removal model

**A gone device stays a valid object until its last user lets go.**

1. The transport driver notices the unplug (unbind) and sets its own
   `unplugged` flag, so every later transfer fails at once instead of
   timing out; then calls `blk_device_gone(disk)`.
2. The block layer marks the disk and every partition window on it
   GONE, forgets the disk cache's lines for it, and records a
   `QUERY_DEVEV_REMOVED` event.
3. The mount layer marks every mount on those devices GONE: each later
   backend call on it fails without reaching the backend (Linux's
   offline SCSI device, one layer up), so nothing waits on a dead
   device -- but `umount` still works and is what releases it.
4. When nothing is mounted from a gone device any more, the block layer
   drops its entries and calls the driver's `release()`. Before that its
   name is kept and shown as removed, so it cannot be reused under a mount
   still holding it. Even after it, the driver keeps the struct the block
   layer knew -- a raw caller between looking it up and calling into it
   holds nothing anyone can count -- and frees only its DMA buffer, once no
   call is inside.

The device table and the mount table become allocated and growing
(`docs/conventions/kernel.md`, "Size it to the data").

## Quick removal

A block device says `removable`. For a mount on one, every backend call
made by path that changed anything ends with the backend's `sync` and a
device flush before it returns; what a program writes through an open
file is settled when it closes the file, or by a write a second after
the change it owes (Linux's vfat `flush`) -- so a stick pulled after a
copy finished, or between two path calls, loses nothing either
completed. A file still open and idle lands at its close. FAT32's dirty
bit is set on mount and cleared on unmount, as Windows does, so a stick
pulled while mounted is checked next time. Eject is unmount, then
SCSI START STOP UNIT (stop, eject), then the port is told it may go.

## The stages

Each stage is gated, committed and usable on its own.

1. **A disk that can disappear.** BUILT 2026-10-22. The registry and mount tables grow;
   `blk_device_gone()` and `release()` as above; GONE mounts; a
   `removable` flag and quick removal in `vfs.c`; a device-change
   generation a process can subscribe to, bumping its WAKEWORD (Linux's
   uevent socket, Windows' `WM_DEVICECHANGE`) -- the wakeword because it
   already is the one word a process waits on for everything, with the
   timeouts and lost-wake handling a new blocking call would have to
   re-earn. Tested with a RAM disk a KTEST yanks.
2. **Bulk-only transport.** BUILT 2026-10-22. Verified the same day on
   three real sticks passed through to QEMU -- two Kingston DataTraveler
   3.0 (SuperSpeed) and a Silicon Motion 090c:1000 (high speed): all
   enumerated, a Windows installer's FAT32, a Windows-formatted exFAT and
   a SaunaOS TFS3 stick mounted, and files written to the exFAT and FAT32
   ones -- one written just before the stick was detached -- read back on
   Linux with matching sha256s, `fsck.exfat -n` calling the exFAT stick
   clean. Not yet on the laptops' own xHCI. `kernel/lib/scsi.c` (CDBs, sense; shared,
   with host checks) and `kernel/drivers/usb/usb_storage.c`: Reset
   Recovery, Get Max LUN, CBW/CSW, READ CAPACITY(16) falling back to
   (10), READ/WRITE(10/16), SYNCHRONIZE CACHE, TEST UNIT READY with
   sense-driven retries (a card reader with no card is a disk with no
   medium, not an error). Registers `usbN`; partitions are found as for
   any disk. QEMU `usb-storage`, plugged and yanked over QMP
   (`device_add` / `device_del`).
3. **UAS.** xHCI streams (Configure Endpoint with a stream context
   array) and `usb_uas.c` on the same SCSI helper: command, status,
   data-in and data-out pipes, tags. BOT stays the fallback a UAS
   device also offers (alternate setting 0). QEMU `usb-uas`.
4. **exFAT.** BUILT 2026-10-22 (`kernel/fs/exfat*.c`, `mkfs -t exfat`;
   Disks' format dialog waits for stage 5's mockups). A backend beside FAT32, read and write: the boot region
   and its checksum, the allocation bitmap, the up-case table, directory
   entry sets with their checksum, contiguous (`NoFatChain`) and FAT-
   chained files. Host-checked against Linux's `mkfs.exfat` and the
   kernel's exfat driver on images, before it touches a real stick.
   Formatting from Disks follows.
5. **Auto-mount, Eject, the notice.** BUILT 2026-10-22, from mockups
   (N2, F1, R1). `/bin/automountd` (a service, udisks2's job) waits on
   the device wakeword and mounts each readable volume on a removable
   disk at `/media/<label>` -- `-2`, `-3` on a clash -- once per plug, so
   a manual unmount is respected; it tidies a pulled stick's GONE mount.
   `SYS_EJECT` (`blk_eject()`: flush, SCSI SYNCHRONIZE CACHE and START
   STOP UNIT, then the disk goes as `QUERY_DEVEV_EJECTED`) behind
   `/bin/eject` and `lib/udrive.h`. The desktop never touches the disk:
   it ASKS automountd over its channel (`drives`). The tray's USB item
   (`userland/wm/drives_popup.c`, KDE's Disks & Devices) opens itself
   for a few seconds when a stick is mounted, with Open, Eject, Eject
   all and Disks, drawn by the shared `ui/uui_drive.h` row (a stick with
   several volumes: a stick row with the one Eject, a volume row under
   it per volume with its Open; the header counts sticks); the File
   Manager's tree has a Removable heading and a per-row Eject
   (`uui_tree_node.action`). From the kernel's device events it says
   "<name> can be pulled out" after an Eject from anywhere, and "<name>
   was pulled out" -- held until read -- when a stick with a mounted
   volume went without one. `tools/usb_desktop_test.py`.
6. **The root on a stick.** `rootwait` (Linux's word): the boot waits,
   bounded, for the named root device, since USB enumeration finishes
   after the disks a PCI probe finds; `install` and `make usb-image`
   produce a stick that runs from itself, persistently.

## Testing

- QEMU: `-device usb-storage,drive=...,bus=xhci.0` (BOT) and
  `-device usb-uas,id=uas -device scsi-hd,bus=uas.0,drive=...` (UAS),
  hot-plugged and removed with QMP mid-copy -- the oracle for removal is
  the host image file, read back after the guest is gone.
- Real sticks passed through from the dev host (`vm.py --usb-host
  vid:pid`), then on the laptops: a stick's quirks (a wrong capacity, a
  stall on an unsupported command, a slow TEST UNIT READY) are what
  QEMU never shows.
- **A test that yanks must check the DATA, not that nothing crashed**:
  what a completed write left on the stick, read back from the host.

## The case against

- **Quick removal is slow on purpose.** Every file's metadata lands
  before its close returns, and a cheap stick's small random write is
  tens of milliseconds (flash rewrites a whole erase block). Eject-free
  safety is worth it on a disk people pull. Measured on the ASUS's
  Kingston DT 3.0: a big file, in 1 MiB calls and 1 MiB SuperSpeed
  commands, at ~7 MB/s -- about the stick's own speed (2026-10-30;
  ~128 KB/s before those two and FAT32's run I/O). `cp -r /bin` (124
  files) ~37 s to FAT32 and ~40 s to TFS3 settling per call, ~31 s and
  ~38 s settling per file at close, ~25 s to FAT32 once its directory
  sector waits for the settle and a new file's first cluster is one
  write (2026-10-31): what is left is the WRITE count, 5 a file on FAT32
  and 14 on TFS3, at ~30 ms each.
- **exFAT doubles the round.** It is a second FAT-family backend to keep
  correct, and a corrupted stick is somebody's photos. The host checks
  against Linux's implementation are the mitigation, not a nicety.
- **UAS needs streams**, an xHCI feature nothing else here uses, and a
  stick never speaks UAS -- only SSD enclosures do. It is in because it
  was asked for, and it is the stage to cut if one has to go.
