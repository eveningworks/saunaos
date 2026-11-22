---
paths:
  - "kernel/**"
  - "userland/**"
---

# Design documents

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

**Design documents -- read the one for the area first**; each carries
the finding that decides the scope, several the case AGAINST:
`query-design.md` (BUILT), `errno-design.md`, `libc-design.md` (tolibc,
BUILT -- before libc-shaped work), `tree-design.md` (the source tree by role, manifests, named libraries,
stages 1-5 BUILT -- before moving a directory, adding a component or
placing a module in a library),
`cc-design.md` (an on-OS `cc`,
stages 0 (the SDK), 1 (the host cross toolchain), 2 (C++ cross-built: g++, libstdc++), 3 (GCC on the machine, `make native-toolchain`) and 4 (GNU make on the machine) BUILT, DYNAMIC BY DEFAULT since 2026-11-17, GCC chosen for C++ -- before compiler/SDK/C++ work, and before believing the roadmap's
"out of scope" line covers it), `umdf-design.md` (a driver as a
process, PLANNED -- IOMMU-less DMA buys crash isolation, NOT
containment), `dynlink-design.md`, `signals-design.md` (signals, TTY and
job control are ONE problem), `blocking-design.md` (stages 1-3 BUILT --
before `switch_to()`, `block_common()` or `FS_OP()`),
`fslock-design.md` (stages 0-4a, 5a, 5b, 5c BUILT, 5d's evidence taken bar the Lenovo -- before tfs3's journal, its scratch state or a
caller sleeping under the fs lock), `smp-design.md` (stages 1-6 BUILT -- the APs started, the ticket lock, call IPI and kernel lock, the TLB shootdown, processes on every core BY DEFAULT 2026-11-20; stage 7, splitting the lock: `lockstat`, a per-core guard and heap/frame spinlocks BUILT 2026-11-21, then MPSAFE syscall rows and the clock page the same day, then the run-queue lock and an AP's lock-free scheduler events, then (2026-11-22) the descriptor tables' locks and a lock-free `lseek`, then a lock-free park and a pipe's `read`/`write`, then a ramfs file's `read`/`write` (trylocked, the mount table in a sequence count) --
before a module-level buffer anything a syscall reaches uses),
`lockdep-design.md` (lock-order checking, `option lockdep`: classes, a
learned order graph, irq-safety, BUILT 2026-11-21 -- before a new lock,
a lock inside a struct, or nesting two of one kind),
`modules-design.md`, `aml-design.md` (the ACPI namespace walk, stages
0-2 BUILT; stage 4 -- method execution by a vendored uACPI -- SCHEDULED
2026-11-10, before any AML, embedded-controller or `kernel/third_party/`
work), `update-design.md`, `software-design.md` (optional components + the Software app, stages 1-5 BUILT, and the installer's Software step -- before upd.c component work, `base/ucomponent.h`, the app, or what `base/uinstall.h` copies), `rootfs-design.md`,
`winserver-ring3-design.md` (stages 0-1 BUILT -- before
`kernel/proc/win_*.c`), `kdebug-design.md` (the GDB stub: serial BUILT,
the network BUILT, on QEMU's e1000 and on real r8169s -- the Lenovo,
the desktop),
`sdl-design.md` (SDL3 + sdl2-compat as the platform layer for ported
programs, SaunaOS an SDL private platform; stages 0-4 BUILT 2026-11-17, stage 4 ScummVM (`make scummvm`) --
before `userland/third_party/sdl3/`, SDL's drivers, `uui_datacard`, or a port that
wants SDL), `netstack-design.md` (the network stack in ring 3, PLANNED -- before
socket or `kernel/net/` work), `trace-design.md` (`strace` decoding in
ring 3 and a debugger's stop, stages 1-3 BUILT; the kernel EVENT ring
(`ktrace.h`, `/bin/trace`) BUILT 2026-11-21 -- before `strace.c`,
`ptrace`, `ktrace.c` or a new trace point), `input-policy-design.md` (keymaps in the compositor, the pointer
kept, stages 1 and 3 BUILT -- before `keyboard_layout.c` or `WIN_EV_RAW_KEY`
work), `w16-design.md` (16-bit Windows programs on an emulated CPU, stages 0-3
BUILT, stage 4 -- the user's own programs -- ongoing, with the side-by-side
comparison against real 3.11 -- before NE or `userland/utils/w16/` work), `w16-jit-design.md` (translating w16's 32-bit code, and W^X executable memory with `mprotect()` first; stage 0 (`SYS_MPROTECT`, W^X, `prot_exec`) and stage 1 (blocks, lazy flags, call-outs) BUILT, chaining next -- before `jit16*.c`, a JIT, `SYS_MPROTECT` or a `PROT_EXEC` rule), `w16-tasks-design.md` (several Windows programs in one w16, a shared VDM as WOW and Wine run them: per-task vs per-VDM state, the Win16 lock, shared DLL data -- DESIGN ONLY 2026-11-15, the maintainer to choose -- before WinExec, a second program, DDE or a DLL two programs share), `browser-design.md` (a NetSurf port, stages 0-6 (JavaScript) and the
sandbox BUILT -- before NetSurf, `uui_webview` or HTTP/1.1 work in
`libhttp`), `usb-storage-design.md` (USB sticks: BOT, UAS, exFAT,
removal, root on a stick; stages 1, 2, 4 and 5 BUILT -- before
`usb_storage.c`, a removable `block_device`, `blk_device_gone()` or
automountd), `gui-unittest-design.md` (the toolkit and apps tested in-process, KTEST-style: stage 1, the host widget harness, BUILT 2026-11-17 (`tools/hostchecks/uitest_hostcheck.py`); stage 2, whole apps headless on the host (`uitest_app.c`), BUILT 2026-11-17 -- before a widget test, an app test, the harness, or a headless app), `uixx-design.md` (the C++ layer over libui, header-only in the SDK, gtkmm's shape; stages 1-3 (value widgets; menus, tabs, timers, message box) BUILT 2026-11-18 -- before `userland/libs/uixx/` or a C++ GUI program), `gui-test-sync-design.md` (GUI tests that wait for state, not time: guest-side
waits, guest-timed input, app idle -- the 2026-11-05 flake measurement and
four stages; 1 and 2 BUILT, stage 3's diagnosis done, stage 4 and the app-busy protocol not -- before `gui_debug.py`'s waits, `wm_debug.c`'s
commands or a new GUI test), `ui-files-design.md` (a window's widgets, menus and Options pages in a `.ui` file read at start, stages 1, 4a
and 5 BUILT and 4b's menus, `menubar`, `segmented` and toolbar controls -- before `uui_builder`, `udecl`, a program's `.ui` file, or a new
widget type for it), `utf8-design.md` (UTF-8 in every string, decoded where text
is drawn or edited; steps 1 (the GUI), 2 (terminal, shell, layouts, console) and 3 (names and text on disk, FAT32 and SMB names) BUILT, four steps -- before `ugfx`'s text
functions, `uui_edit`, `utext`, `klineedit`, `uvterm` cells, keyboard
layouts past Latin-1, or a file name's encoding).
