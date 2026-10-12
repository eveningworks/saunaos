# fbs

**a `/bin` program.**

**Category:** System administration

## Synopsis

    fbs [status] | again | now

## Options

- `status` (or nothing) -- whether First-boot setup has run (and when, and
  whether it was finished or skipped), and what this boot's command line
  says about it.
- `again` -- remove the marker (and any list of pages an install carried), so
  the next graphical boot shows every page of setup.
- `now` -- show setup now, on this desktop.
- `-h`, `--help` -- every command in one page.

## Description

First-boot setup is the full-screen wizard a new machine shows once, before
the desktop is used: region and formats, keyboard, time zone, appearance,
the computer's name, the network and the System Update server -- seven
steps on a rail, Back and Next along the bottom, and **Skip setup** under
the rail. Each choice is saved as it is made; skipping keeps what was
already chosen. The wizard is `/bin/wm/system/firstboot`; `fbs` is the half
you type.

    /$ fbs
    First-boot setup: done (finished 2026-10-11 20:14)
    /$ fbs again
    fbs: the next boot shows First-boot setup

**Whether it runs** is one file and two boot words (`userland/lib/ufbs.h`):

| | |
| --- | --- |
| `/var/lib/fbs/done` exists | it does not run -- the file's line says how it ended |
| `fbs=on` on the command line | it runs, marker or not |
| `fbs=off` on the command line | it never runs, and writes no marker |
| a live session (the root is GRUB's RAM image) | it does not run: the installer is what that boot is for |

The `firstboot` service (`/etc/services.d/firstboot`) runs `firstboot
--boot` after `toywm` on every graphical boot, and `--boot` exits at once
unless the table above says to show it.

**Development and test images boot straight to the desktop**: `make iso`,
`tools/fresh_disk.py` and the USB image write the marker when they seed
(`tools/seed_disk.py --fbs-done`). The live ISO is not marked, and `install`
leaves it off the disk it writes -- a new machine is set up by whoever boots
it. So a development image marked by `make iso` is marked again by the next
one even after `fbs again`; use `fbs now`, or `fbs=on`, to see setup there.

**An install skips what the live session answered.** `install` (and the
Install toy-os window) copies the session's settings and writes
`/var/lib/fbs/answered` on the new disk, naming each page one of whose
settings the session set -- set by someone, rather than left at its built-in
default. Setup there leaves those steps off the rail (Ready still shows what
they hold), removes the list when it ends, and, when every page was answered,
marks itself finished without showing. `fbs` names the carried pages:

    /$ fbs
    First-boot setup: not done -- it shows at the next graphical boot
    Carried over by the install, so not asked: Region & formats; Computer name;

The list is written by the install, never worked out at boot: a machine
somebody has used has settings stored everywhere, and `fbs again` there must
still ask every page -- it removes the list along with the marker.

**System Update writes it too**, where it is missing (`docs/update-design.md`):
a machine that is updated was set up already, so the update that brings
First-boot setup does not show it.

## What it deliberately does not do

- **No user account.** toy-os has no users or login yet (`docs/roadmap.md`);
  when it does, that page belongs here.
- **No language.** The interface is English only; Region sets how dates,
  times and numbers are written.
- **No Wi-Fi.** There is no wireless driver, so the Network step shows the
  wired adapter.

## See also

`bootcfg` (the boot words), `config` (every setting the wizard writes),
`docs/boot-flags.md`.
