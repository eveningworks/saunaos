---
paths:
  - "kernel/**"
  - "userland/**"
  - "apps/**"
---

# Design documents

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

**Design documents -- read the one for the area first**; each carries
the finding that decides the scope, several the case AGAINST:
`query-design.md` (BUILT), `errno-design.md`, `libc-design.md` (tolibc,
BUILT -- before libc-shaped work), `cc-design.md` (an on-OS `cc`,
PLANNED -- before compiler/SDK work, and before believing the roadmap's
"out of scope" line covers it), `umdf-design.md` (a driver as a
process, PLANNED -- IOMMU-less DMA buys crash isolation, NOT
containment), `dynlink-design.md`, `signals-design.md` (signals, TTY and
job control are ONE problem), `blocking-design.md` (stages 1-3 BUILT --
before `switch_to()`, `block_common()` or `FS_OP()`),
`fslock-design.md` (stage 0 BUILT -- before tfs3's scratch state or a
caller sleeping under the fs lock), `smp-design.md` (stage 1 BUILT --
before a module-level buffer anything a syscall reaches uses),
`modules-design.md`, `update-design.md`, `rootfs-design.md`,
`winserver-ring3-design.md` (stages 0-1 BUILT -- before
`kernel/proc/win_*.c`), `kdebug-design.md` (the GDB stub: serial BUILT,
the network BUILT, on QEMU's e1000 and on real r8169s -- the Lenovo,
the desktop),
`netstack-design.md` (the network stack in ring 3, PLANNED -- before
socket or `kernel/net/` work), `trace-design.md` (`strace` decoding in
ring 3 and a debugger's stop, stages 1-3 BUILT -- before `strace.c` or
`ptrace` work), `input-policy-design.md` (keymaps in the compositor, the pointer
kept, stage 1 BUILT -- before `keyboard_layout.c` or `WIN_EV_RAW_KEY`
work), `w16-design.md` (16-bit Windows programs on an emulated CPU, PLANNED --
before NE or `userland/w16/` work), `browser-design.md` (a NetSurf port, stages 0-6 (JavaScript) and the
sandbox BUILT -- before NetSurf, `uui_webview` or HTTP/1.1 work in
`libhttp`), `usb-storage-design.md` (USB sticks: BOT, UAS, exFAT,
removal, root on a stick; stages 1, 2, 4 and 5 BUILT -- before
`usb_storage.c`, a removable `block_device`, `blk_device_gone()` or
automountd).
