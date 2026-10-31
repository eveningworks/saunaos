<h1 align="center">SaunaOS</h1>

<p align="center">
  A hobby x86-64 operating system, written from scratch in C and assembly —
  built with Claude.<br>
  Boots via GRUB into a 64-bit kernel with a ring-3 desktop, networking,
  USB, sound and a journaling disk-backed filesystem.
</p>

<p align="center">
  <a href="https://github.com/eveningworks/saunaos/actions/workflows/build.yml">
    <img alt="release build" src="https://github.com/eveningworks/saunaos/actions/workflows/build.yml/badge.svg">
  </a>
  <img alt="Language" src="https://img.shields.io/badge/language-C%20%2B%20NASM-blue">
  <img alt="Target" src="https://img.shields.io/badge/target-x86__64-lightgrey">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
  <a href="https://claude.com/claude-code">
    <img alt="Built with Claude Code" src="https://img.shields.io/badge/built%20with-Claude%20Code-D97757">
  </a>
</p>

> [!NOTE]
> **An experiment in building an operating system with Claude, and in how
> far it can get.** All of
> SaunaOS is written by [Claude Code](https://claude.com/claude-code),
> Anthropic's agentic coding tool, except the third-party ports under
> `userland/ports/` (DOOM, NetSurf and its libraries, Mbed TLS, dash,
> cJSON): a human makes the
> design calls and decides what ships; Claude does the implementation,
> the testing and the documentation. The conventions it works under are
> in [CLAUDE.md](CLAUDE.md), and the reasoning behind every design choice
> — including the ones that turned out wrong — is in
> [docs/decisions.md](docs/decisions.md).
>
> Nobody has audited it for security, and it is sure to have holes, so
> please don't run it in production, or on a machine with anything on it
> you care about.

<p align="center">
  <img src="screenshots/readme/desktop.png" alt="SaunaOS desktop: the Start menu, frosted glass, open on All Apps over the Image Viewer, the File Manager, DOOM and a Terminal" width="49%">
  <img src="screenshots/readme/apps.png" alt="SaunaOS apps on the floating, frosted taskbar: a shaded teapot in Shapes, the Audio Player, Minesweeper, the Calculator and the Device Manager" width="49%">
</p>

<p align="center">
  <a href="#highlights">Highlights</a> ·
  <a href="#try-it">Try it</a> ·
  <a href="#status">Status</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="#license">License</a>
</p>

---

## Highlights

A hobby operating system built one subsystem at a time, with the
reasoning behind each design decision written down — including the ones
that turned out wrong. It boots on real hardware and under QEMU.

- **The machine, by hand** — Multiboot2 and the long-mode switch, per-process
  page tables, NX/W^X, SMEP/SMAP, kernel ASLR, ACPI, and a **tickless**
  one-shot timer.
- **Real processes** — an ELF64 loader, a preemptive scheduler, `init` as
  pid 1 supervising services, signals and job control, threads with TLS,
  and **dynamic linking** against `/lib/libc.so`.
- **Its own filesystem** — TFS3, journaled the way ext4's jbd2 is (one
  running transaction every operation shares, and a kernel thread that
  commits it in the background), with `fsck` and
  grow/shrink, beside FAT32 and exFAT, over IDE, AHCI, NVMe, virtio or a
  USB stick; SMB shares mount too. It **installs itself** to another disk
  and **updates itself** from a build server.
- **A desktop outside the kernel** — the window manager and every app run in
  ring 3: a **web browser** (NetSurf, with JavaScript, one sandboxed process
  per tab), a file manager, a tabbed terminal, Paint, Notepad, an image
  viewer, audio and video players, a character map, Disks, a task manager,
  System Settings, Minesweeper and **DOOM** -- and **Windows 3.1 programs**
  (Write, Solitaire, Hearts, Calculator) on an emulated 16-bit CPU.
- **Networking** — IPv4, ICMP, UDP and TCP on seven NIC drivers, with DHCP,
  DNS, `wget`, an `httpd` serving the machine's own files, and a VNC
  server for remote desktop.
- **USB, display and sound** — xHCI with hubs, report-descriptor HID and
  mass storage (BOT and UAS), an
  Intel driver that programs the panel's mode itself, and AC'97, HD Audio
  and USB Audio mixed by a ring-3 daemon, with MP3 and MIDI decoded here.
- **Its own manual** — `doc ls` on the machine renders the same page this
  repository holds.
- **Tested from the inside** — in-kernel tests with fault injection, ring-3
  diagnostics, and a GUI suite that drives the desktop and asserts on pixels.

**[docs/features.md](docs/features.md) is the long version** — what each
of those actually is, and where the interesting decisions were.

## Try it

**Without building anything:** download `saunaos-live.iso` from
[Releases](https://github.com/eveningworks/saunaos/releases) and boot it
in QEMU — `run_release.sh`, beside it, does that for you — or write
`saunaos-usb.img.gz` to a USB stick for real hardware. It boots BIOS/CSM
or UEFI, with Secure Boot off.

> [!WARNING]
> **Running it is at your own risk, in a VM or on real hardware.** SaunaOS is a
> hobby experiment and comes with no warranty: if running or installing it
> breaks something, that is on you, not on this project. Read
> [On real hardware](docs/building.md#running-a-release) first.

**From source** (on Debian/Ubuntu; [other distributions](docs/building.md#dependencies)):

```bash
sudo apt install build-essential nasm grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools qemu-system-x86 python3
git clone https://github.com/eveningworks/saunaos.git && cd saunaos
make run
```

The desktop comes up on its own. *Exit to shell* in the Start menu drops
to a prompt; `help` lists the commands and `doc <command>` reads a page.
`make run KVM=1` is much faster where `/dev/kvm` exists.

**[docs/building.md](docs/building.md)** has the rest — every `make run`
flag, `build.conf`, real hardware, testing a change and troubleshooting.

## Status

A hobby project under active development, not production software. The
current version is in [VERSION](VERSION); tagged builds are on
[Releases](https://github.com/eveningworks/saunaos/releases).

**Works today:** real hardware and QEMU boot; the shell; four
filesystems (TFS3, FAT32, exFAT, ramfs), SMB shares, partitions with
resize, and an installer; the ring-3 desktop and its
apps; processes with pipes, signals, threads, job control, `mmap` and
dynamic linking; per-process sandboxes for the programs that parse
untrusted input; loadable kernel modules; pseudo-terminals; USB with
hubs, hot-plug, HID, mass storage, Ethernet and audio; sound on three device classes,
including DOOM with music; an Intel display driver with backlight
control; ACPI shutdown; and networking good enough for the web browser
and `wget` to fetch real pages over HTTPS and for `httpd` to serve the
machine's own files.

**Known gaps:**

- **No Secure Boot** — UEFI boots through an unsigned GRUB, so it must be off.
- **One CPU** — other cores are discovered, and every one reports offline.
- **No privilege model** — no user accounts or permission checks.
- No `mprotect`, and swap that pages nothing out yet.

[docs/roadmap.md](docs/roadmap.md) tracks all of it, and
[docs/bugs.md](docs/bugs.md) what is broken right now.

## Documentation

Everything under [docs/](docs/) is written as the work happens,
including the decisions that turned out wrong.

| Document | Contents |
|---|---|
| [docs/building.md](docs/building.md) | Dependencies, every way to boot it, build options, releases on real hardware, testing a change, troubleshooting. |
| [docs/features.md](docs/features.md) | What is built, layer by layer, and where the interesting decisions were. |
| [docs/architecture.md](docs/architecture.md) | Which directory holds what, and why the boundaries are where they are. |
| [docs/decisions.md](docs/decisions.md) | "Why is this built this way?", indexed by topic. **Start here when something looks odd.** |
| [docs/roadmap.md](docs/roadmap.md) · [docs/bugs.md](docs/bugs.md) | What is not built yet, and what is broken — kept apart, because they are different questions. |
| [docs/commands.md](docs/commands.md) | The command index; each page also ships on the machine. |
| [docs/testing.md](docs/testing.md) · [docs/tools.md](docs/tools.md) | Driving the OS headlessly, and every script in `tools/`. |
| [CLAUDE.md](CLAUDE.md) | The conventions every change is written under. |

Reference material for one subsystem lives beside it —
[driver-guide](docs/driver-guide.md) and [devices](docs/devices.md) for
drivers, [gui-guidelines](docs/gui-guidelines.md) for the desktop,
[tfs3-spec](docs/tfs3-spec.md) for the filesystem's byte layout,
[boot-flags](docs/boot-flags.md) for the GRUB command line, and a design
document per staged subsystem.

## License

MIT — see [LICENSE](LICENSE), which carries the full inventory of
third-party material. In short:

- **`userland/ports/doom/` is GPL-2-or-later** (doomgeneric). It builds
  into one binary that nothing else links, so it is an aggregation and
  the rest of the repository stays MIT. The Doom IWAD is **not** in this
  repository — `tools/fetch_wad.py` obtains one, under id Software's own
  terms.
- **`userland/ports/netsurf/` is GPL-2.0-only** (NetSurf), under the
  same arrangement: it links only into `/bin/webcontent`, the page engine
  every web view runs as a separate process, and `/tests/nsmonkey`. The
  browser and the apps that show pages talk to that process over shared
  memory and stay MIT. The libraries NetSurf is built from (libcss,
  libdom, libhubbub, expat and the rest beside it) are MIT, under their
  own copyrights.
- `userland/ports/mbedtls/` (Mbed TLS) is taken under the Apache-2.0
  side of its dual licence, and builds into `/lib/libssl.so` -- except
  in the two NetSurf programs, which take its GPL-2.0-or-later option.
- `userland/ports/dash/` is BSD-3-Clause and builds into `/bin/dash`;
  its one GPL file, `mksignames.c`, is not compiled --
  `tools/gen_signames.py` generates that table instead.
- `userland/ports/cjson/` is MIT, under its own copyright.
- The runtime-loadable fonts in `data/fonts/` are under the Bitstream
  Vera and SIL Open Font licenses, and the baked JetBrains Mono glyphs
  in `kernel/drivers/font_ttf.c` under the SIL Open Font License 1.1.
- The bundled `pci.ids` and `usb.ids` databases in `data/` are taken
  under the BSD-3-Clause side of their dual licence, and three Terminal
  colour schemes carry other projects' palettes (Solarized, Tango,
  Konsole).

`tools/check_licenses.py` fails the build if a vendored port or a shipped
font is missing from that inventory.
