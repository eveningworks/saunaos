# install

**a `/bin` program.**

**Category:** Storage

## Synopsis

    install [--disk NAME] [--esp MIB] [--mbr] [--ask-all] [confirm]
    install --bootloader [confirm]
    install --migrate [confirm]

## Options

- `--disk NAME` -- the target disk, named as `lsblk` names it. The disk
  this machine is running from is refused. Without it, at a terminal,
  `install` asks (below); anywhere else it lists the disks it could use.
- `--esp MIB` -- size of the FAT32 `/boot` partition; 64 by default,
  and anything under 8 MiB is refused.
- `--mbr` -- write an MBR table instead of GPT, for firmware that will
  not boot a GPT disk in legacy/CSM mode. Refused before anything is
  erased on a build that staged no `core-msdos.img`.
- `--ask-all` -- the new machine's First-boot setup asks every page,
  including the ones this session already set (below).
- `--bootloader` -- rewrite this machine's own bootloader in place; see
  "Refreshing the bootloader" below. `--disk` and `--mbr` are refused
  with it, because the target is this machine's disk and the layout is
  read from that disk.
- `--migrate` -- `--bootloader`, but only on a machine whose boot volume
  still has the old `/boot/boot` layout; anywhere else it says so and
  exits 0. See "Moving `/boot/boot` up to `/boot`" below. It takes no
  other flag.
- `confirm` -- go ahead without asking; it ERASES the target disk.
- `-h`, `--help` -- every option on one page.

## Description

`/bin/install` puts the running system onto another disk and makes that disk
boot. The **Install toy-os** window does the same through the same library
(`lib/uinstall.h`), so the two cannot install two different ways. Five steps,
each of which is an existing command's operation done through the same syscall
that command uses:

| step | what it is |
| --- | --- |
| partition | `SYS_MKPART` — the same call `mkpart --disk` makes |
| format | `SYS_MKFS` — twice, FAT32 then TFS3 |
| mount | `SYS_MOUNT` — the target's root at `/mnt`, its ESP at `/mnt/boot` |
| copy | `ufileop`, the library `cp -r` and the File Manager share |
| bootloader | `SYS_INSTALL_BOOT` — the boot sector and GRUB's core image |

It prints each step as it starts, and a meter over the copy when it is
writing to a terminal:

    [1/5] Partition   GPT, 3 partitions, root 445.9 MiB
    [2/5] Format      FAT32 on ata0p2, TFS3 on ata0p3
    [3/5] Mount       ata0p3 at /mnt, ata0p2 at /mnt/boot
    [4/5] Copy files  1225 files, 130.6 MiB
    [===========================================>] 100% 130.6M/130.6M 3.4M/s in 38s
    [5/5] Bootloader  the boot sector and /install/core.img
    install: done in 0:40. Boot ata0 on its own and it should come up.

The four files it writes onto the target's boot partitions come from
**`/install`** — `kernel.bin`, `grub.cfg`, `boot.img`, `core.img` — which every
toy-os filesystem carries.

Nothing here is a special path into the kernel. An installer that needed one
would be an installer whose steps could not be checked by hand — every one of
these can be typed at a shell and watched.

The layout it writes:

| | size | type | holds |
| --- | --- | --- | --- |
| `p1` | 1 MiB | BIOS boot | GRUB's `core.img`, no filesystem |
| `p2` | `--esp` (64 MiB) | ESP, FAT32 | the kernel, `grub.cfg`, GRUB's modules |
| `p3` | the rest | TFS3 | the root |

## Asking at a terminal

Run bare at a terminal, `install` asks -- Alpine's `setup-alpine` shape:

    /$ install
    Install toy-os onto which disk?
         ram0      171.0 MiB  live image                   This system is running from it
      1) ata0      512.0 MiB  QEMU HARDDISK                (empty)
    Disk [1]:
    Partition table -- GPT, or MBR for firmware that won't boot GPT [GPT]:
    This ERASES ata0:
      (no partitions -- nothing on it is lost)
    Type ata0 to go on: ata0
    [1/5] Partition   GPT, 3 partitions, root 445.9 MiB
    ...

Every disk is listed; the ones it cannot use are not numbered and say why (the
disk it runs from, a live image, sectors that are not 512 bytes, too small for
the system). `--disk` without `confirm` at a terminal prints the plan and the
partitions it would erase, then asks for the disk's name the same way.

**Flags and `confirm` skip every question, and a program that is not a
terminal is never asked anything**: off a terminal, bare `install` lists the
disks and exits, and `--disk` without `confirm` prints the plan and exits, as
they always did. A script or a test drives it exactly as before.

## The installer window

**Install toy-os** (`/bin/wm/system/installer`, System in the Start menu) is
the same install on a step rail -- Welcome, Disk, Ready, Installing, Finished,
First-boot setup's frame. Disk shows the disks it can use and what each holds
now and after, Ready says what is erased and what goes with the install, and
nothing is written until a confirm naming the partitions that are lost. The
rail and the buttons are held while the copy runs, and closing the window then
is refused.

On an installed machine the window installs onto ANOTHER disk, and its
"keep trying" buttons say Close and Quit instead.

**A live session offers it unasked**: `/etc/services.d/installer` runs
`installer --boot` after the desktop starts, which in a live session puts an
**Install toy-os** launcher on the desktop and a notice in the corner ("This is
a live session", with Install toy-os) that stays until it is closed or the
installer opens. An installed machine gets neither. The live ISO's boot menu
also has an **Install toy-os** entry, which opens the window full screen at
once (`installer=on`) with the launcher already on the desktop behind it, so
"Keep trying toy-os" leaves a way back; `installer=off` offers nothing at all
(`docs/boot-flags.md`).

## What a live session's settings do

**They go with the install.** The copy takes `/etc`, so what was set while
trying the live session -- the keyboard, the region, the computer's name --
is what the new machine starts with. It also writes
`/var/lib/fbs/answered` on the target, naming each First-boot setup page one of
whose settings the session set, and the new machine's First-boot setup skips
those pages and asks the rest. `--ask-all` (or the window's "Ask every
First-boot setup question again") writes no such list, so setup asks
everything. A live session itself never shows First-boot setup
(`docs/commands/fbs.md`).

What the session's desktop held goes too -- except its **Install toy-os**
launcher, which the install leaves behind.

## Refreshing the bootloader

    install --bootloader confirm

`grub-install` on a machine that is already installed: it writes the boot
sector and GRUB's core image, adds `bootpart=` to `grub.cfg`, and does
**nothing else** — no partition table, no format, no other file touched. It is
the only way an installed machine can gain a bootloader capability it was
installed without.

    install: this rewrites ahci0's bootloader in place (GPT layout):
      boot sector    LBA 0, keeping this disk's partition table
      core image     /install/core.img
      grub.cfg       bootpart= added to 2 entries (a .bak is kept)

**`bootpart=` is the newest such capability.** It tells the kernel which disk
GRUB booted from, so a second disk cannot take the root
(`docs/boot-flags.md`). It needs BOTH halves: a core image with GRUB's `probe`
module, and grub.cfg lines that use it — and System Update writes neither, so
a machine installed before it gets it from this refresh. **grub.cfg is EDITED,
not replaced**: each toy-os (`multiboot2`) entry that can be edited as words
gains the two probe lines above its boot line and `bootpart=$bootpart` on it,
exactly as `bootcfg` would edit it; a `linux` entry, an entry already carrying
the word, and anything else in the file are left as they are, and the old file
is kept as `grub.cfg.bak`. Running it again adds nothing.

That is not hypothetical. The bare-metal laptop was installed before `gzio`
joined `install_grub.py`'s `CORE_MODULES`, so when it was later flashed with a
gzipped kernel its GRUB read the gzip bytes as raw ones:

    error: ... grub_multiboot2_load: no multiboot header found
    error: ... grub_loader_boot: you need to load the kernel first

The default menu entry was dead until the rescue entry was picked by hand. The
same goes for `loadenv`, which `reboot --entry` needs: a machine installed
before it joined gets it from this refresh, and until then `reboot --entry`
refuses by name rather than boot the default. The
full installer cannot help there — it ERASES its target and refuses the disk the
machine runs from.

**The running disk is the only target**, which is what makes the layout
knowable: `QUERY_PARTTABLE` answers for this machine's disk, and the prefix
baked into each staged core image (`(hd0,gpt2)` against `(hd0,msdos1)`) must
match it or GRUB comes up at a rescue prompt having found no config. Another
disk is the full installer's job.

**The core image is read back before the boot sector is written**
(`sys_install_boot`), so a short or failed write leaves the machine booting the
bootloader it already has. The boot sector — the one write that decides what
runs — is last, and the disk's own partition table bytes at 0x1b8 are preserved
through it.

It records what it installed in **`/etc/grub-core.modules`**, one line of GRUB
module names. That stamp is the only readable record of what the installed
bootloader can do: `core.img` lives in raw sectors nothing can open and is
lzma-compressed besides. `tools/remote.py flash` reads it to decide whether this
machine can be sent a compressed kernel.

## Moving `/boot/boot` up to `/boot`

    install --migrate confirm

The boot volume holds `kernel.bin` and `grub/` at its ROOT, which toy-os
shows as `/boot/kernel.bin` and `/boot/grub/` — a Linux `/boot` partition's
shape. Disks written before 2026-10-11 kept both one level down, so toy-os
showed `/boot/boot/...`, and their GRUB core image has `/boot/grub` baked into
its prefix: the files cannot simply be moved. So the move is three steps around
the one write that changes what GRUB reads (`lib/ubootlayout.h`):

| step | the machine boots |
| --- | --- |
| copy `/boot/boot/*` up to `/boot`; grub.cfg's `/boot/kernel.bin` becomes `/kernel.bin` | the old tree, through the old core |
| write the payload's core image, whose prefix is `/grub` | the new tree, through the new core |
| delete `/boot/boot` | the new tree |

A failed core-image write deletes the copies again, so the machine is as it
was. `kernel.old`, `grubenv`, `grub.cfg.bak` and anything else in `/boot/boot`
come along; the machine's own grub.cfg edits are kept.

**It runs by itself, once**: `/etc/services.d/bootmigrate` runs `install
--migrate confirm` at every boot, which is a no-op on a machine already moved.
It is a boot service rather than a step of System Update because the update that
brings it runs the PREVIOUS updater, and the payload whose core image reads
`/grub` only lands at the reboot after it — so an old machine moves on the
first boot of the update and reads the flat layout from the boot after that.
`/install/core.prefix` must say the staged images read `/grub`, or nothing is
written.

## Installing from live media

**This is the path a real machine uses**, and it is why `/install` exists. Write
`toy-os-live.iso` to a stick or a CD, boot it, and the root is a filesystem
image GRUB loaded into RAM — the machine's own disks are enumerated but nothing
is mounted from them, so `install --disk <name>` can erase and rewrite the one
you point it at.

`/boot` is **empty** in a live session: there is no ESP, because nothing drives
the boot medium once GRUB has loaded the kernel and the image (toy-os has no
USB mass-storage driver). That is exactly why the payload travels in the root
rather than being read out of `/boot` — one path on every medium.

## What it deliberately does not do

- **It does not install onto the disk this machine is running from,** and
  refuses by name. There is no reading of "reinstall over myself" that ends
  with a working machine.
- **It asks only for the disk, the table and a typed confirm**, and only at a
  terminal. No partitioner, no package selection, no hostname prompt: the
  settings are the live session's, and First-boot setup asks the rest.
- **It does not resize or preserve anything on the target.** The whole disk
  becomes the layout above.
- **It does not generate a new `core.img`.** See the trap.
- **It does not copy GRUB's module directory** (305 files, ~4 MB). `core.img`
  already contains every module `grub.cfg`'s `insmod` asks for, so the target
  boots without it; what that target cannot do is have a *host* `grub-install`
  run against it later without re-copying them.

## The trap

**The layout is not a preference, it is a constraint.** `core.img` carries its
prefix baked in at the host's `grub-mkimage` time — `(hd0,gpt2)/grub` — so
the copy this installs only finds its config if the ESP is **partition 2** on
the target as well. Anaconda and the Debian installer generate a fresh core
image per target instead; toy-os has no `grub-mkimage` of its own, so it
reproduces the layout that the image it copies expects. Changing the partition
order here without rebuilding `core.img` produces a disk that reaches GRUB and
stops at a rescue prompt.

**`/install/grub.cfg` is written verbatim, so any `KCMDLINE` baked into this
build is baked into the target too.** A system built with `make iso
KCMDLINE="root=ata0p3"` installs a target that also insists on `ata0p3` — which
is right if the target's root really is its third partition (it is) and wrong
the moment that disk is attached as something other than `ata0`.

**`confirm` is a speed bump, not a permission check** — the same caveat
`mkpart` and `mkfs` carry, and for the same reason: this OS has no privilege
model.

## `--mbr`, for firmware that will not boot a GPT disk

The default layout is GPT with a BIOS boot partition. **That combination —
a legacy BIOS booting a GPT disk — is the one consumer firmware most often
refuses**: the machine can be in CSM/legacy mode, with the disk in its boot
order, and simply not touch it. A Lenovo Yoga 500-15IBD did exactly that.

`--mbr` writes the other layout:

| | Default (GPT) | `--mbr` |
|---|---|---|
| `core.img` | p1, a 1 MiB BIOS boot partition | the gap at sectors 1–2047 |
| `/boot` | p2, FAT32 | **p1**, FAT32, marked **active** |
| root | p3, TFS3 | **p2**, TFS3 |

Everything shifts down one, because MBR has no BIOS-boot partition type and
GRUB's core image goes in the gap before the first partition instead.

**It is not a runtime choice in GRUB's eyes.** A core image carries its
prefix — the table format *and* the partition number — baked in at
`grub-mkimage` time, and the target has no `grub-mkimage` of its own. So the
build stages **two** core images (`core.img` and `core-msdos.img`,
`tools/install_grub.py`) and this picks between them. A build staged without
the second one refuses `--mbr` before erasing anything.

Use it when the default install produced a disk the firmware will not boot.
There is no reason to prefer it otherwise, and no reason to switch the
machine to UEFI — **nothing here installs a UEFI bootloader**, so a UEFI-only
machine cannot boot a toy-os install at all.

## Limits

- **The target must be a whole disk**, not a partition.
- **A build made without GRUB's BIOS target carries no `/install`**, and says so
  before erasing anything. Such a build boots perfectly; it just cannot install
  itself.
- **`/tmp` is not copied**, and `/boot` and `/mnt` are recreated as empty mount
  points rather than copied as directories — `/boot` because it is the
  *source's* ESP and goes to the target's separately, `/mnt` because it is
  where the target itself is mounted.
- **A file that fails to copy does not stop the rest**, but the install as a
  whole then reports failure and says the target is in an unknown state.

## Typical use

    install                            # at a terminal: asks, then installs
    install --disk virtio0             # off one: prints the plan, changes nothing
    install --disk virtio0 confirm     # ~40 seconds
    poweroff

If that disk then does not boot, the firmware may be refusing GPT in legacy
mode; re-run with `--mbr`.

Then boot that disk on its own. From live media it is the same three lines,
with the machine's own disk as the target.

## See also

`mkpart` (partition a disk), `mkfs` (put a filesystem on one partition),
`mount`, `lsblk` (what disks exist), `fsformat` (reformat the RUNNING root).
