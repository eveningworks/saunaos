# CLAUDE.md

Guidance for Claude sessions working in this repo. **This file is the
always-loaded context, so it holds RULES -- what stops a session doing
the wrong thing before it knows to look anything up.** The reasoning
behind each rule lives one file away in `docs/`; keep the pointer here
to a line. `README.md` and `apps/README.md` cover the architecture.

## What this is

A hobby x86-64 OS (Multiboot2/GRUB-booted, freestanding C + NASM) with
ring0/ring3 separation, per-process paging, an ELF64 loader, syscalls, a
preemptive scheduler, a ring-3 compositor and window manager, and
disk-backed filesystems (TFS3, plus FAT32 for `/boot`). No
cross-compiler -- host and target are both x86-64, so plain system
`gcc`/`ld`/`nasm` with freestanding flags work.

## Before you edit

- **Pull first.** This checkout is worked on from more than one session.
- **Read in this order**, stopping when you have what you need: this
  file, then **`docs/conventions/INDEX.md`** -- every convention in this
  repo by headline -- then **`docs/conventions/<area>.md` for the area
  you are about to touch**, which carries the bodies, then
  `docs/decisions.md` for "why is it like this" (**its INDEX first**),
  then `docs/roadmap.md` for whether the thing is already known broken.
  Anything drawn adds `docs/gui-guidelines.md`, which is binding.
- **Rules that matter only while a file is open live in `.claude/rules/`**
  (docs editing, the Makefile, GDB, the skills' layout, USB audio, the
  design-doc list) and load when a matching file is read or edited, as
  `userland/`, `kernel/fs/` and `kernel/lib/` have their own CLAUDE.md.
- **THE CODE WINS OVER A DOC THAT DISAGREES WITH IT.** Believe the code,
  FIX THE DOC in the same change, and say plainly in your response that
  you did -- a silent fix leaves the next session re-deriving the same
  contradiction. Not a licence to skip the docs: they are right far more
  often than not.

## How the user wants to collaborate

- **Offer a few real choices** before something non-trivial -- a data
  layout tradeoff, how far to build this session vs. just planning it.
  Not for a one-line obvious fix.
- **Keep chat compact and terse.** Don't restate what a diff or a build
  log already shows.
- **MOCK UP ANYTHING VISIBLE BEFORE CODING IT** (standing instruction,
  2026-09-29): a Design canvas, one artboard per option, in SaunaOS's own
  palette and with real data, published BEFORE the choice is asked and
  before any code -- for a new app, a redesign, even a marker in a tree.
  **Not for a change that is only TEXT** (a banner, a message): concrete
  examples in the chat are enough (2026-10-06).
  How: `.claude/skills/saunaos-feature-workflow/references/questions-that-worked.md`.
- **Before adding a feature to a GUI app, consider whether it should be
  a reusable `userland/ui/` widget** -- and ask the user either way.
- **Act like an experienced OS/UI designer**: where a real OS has a
  better approach, data structure or API shape, say so.
- **SAY WHAT REAL SYSTEMS DO, before proposing a design** -- Linux and
  Windows; for the desktop, Wayland and a compositor that implements it
  (KWin, Mutter, wlroots), or X11 where the history explains the shape.
  Then say whether SaunaOS follows or deliberately differs, and why. By
  area: kernel and process model -> Linux, NT; filesystem -> ext2/3/4,
  NTFS; windowing -> Wayland (+KWin/Mutter/wlroots/XFCE), X11; settings
  -> sysctl, dconf/GSettings, macOS `defaults`, the registry; init ->
  systemd, launchd; IPC -> D-Bus, Binder, Mach ports. It has changed
  decisions here, not decorated them: guard pages (`CONFIG_VMAP_STACK`),
  `(namespace, name)` settings, a poisoned revoked mapping
  (`wl_buffer.release`). **Copy the SHAPE, not the size**, and **check
  the claim before leaning on it** -- a wrong premise argued confidently
  is worse than no comparison at all.

## Conventions worth knowing before editing

The ones below are in full because they fire UNANNOUNCED -- a session
trips them before it knows to look anything up. Everything else is
indexed by headline in the next section.

### Boundaries

- **`kapi.h` is the one header apps include** for kernel capabilities
  (console, keyboard, mouse, timer/RTC, filesystem, graphics). Never
  `#include` a driver header from `apps/`, never call `inb`/`outb` from
  app code. A missing capability belongs behind a new function in
  `kernel/core` or `kernel/drivers`, exposed through `kapi.h`.
- **`userland/wm/wm.h` is a second, peer-level boundary** -- the
  GUI-specific equivalent, included by GUI apps for `window_*` helpers.
  `kapi.h` never includes `wm/wm.h` or `gui_apps.h`.
- **`userland/ports/` IS NEVER EDITED IN PLACE** -- the vendoring tool
  deletes and recopies it; a change to a port is a patch in
  `userland/backends/<port>/patches/` (`docs/conventions/build.md`).
- **`kernel/include/` is split by audience and the build enforces it** --
  `api/` (what `apps/` may use), `abi/` (the kernel<->userland contract
  `userland/` shares), `kernel/` (internal, NOT on `apps/`'s include
  path, so reaching for one is a compile error). See
  `kernel/include/README.md`, including where a new header starts life.

### The shared toolkit

**Check `kernel/lib/` before hand-rolling a digit loop, a formatter, a
path join or a rasteriser.** Reachable through `kapi.h`, each with
KTESTs: `string.h`, `knum.h`, `kfmt.h`, `kpath.h`, `fixed.h`, `geom.h`,
`krandom.h`, `hid_parse.h`. **In ring 3, check
`docs/toolkit.md` first** -- every `userland/lib/` and `userland/ui/`
header in one line, GENERATED from the headers (`gen_toolkit_index.py`;
a new header needs a top comment saying what it is). What bites:

- **`kfmt.h` is one header but TWO files** -- `kfmt.c` is freestanding
  and shared with ring 3, the kernel sinks live in `kfmt_print.c`; a
  kernel include in the former silently takes `snprintf` from userland.
- **`fixed.h`'s angles are in TURNS**, so `FX_ONE` is a full rotation.
  No floating point in the kernel (`-mno-sse`).
- **`geom.h` is NOT a 3D engine** -- no matrices, faces, depth buffer or
  clipping; the face list is the CALLER's. `geom_rotate3` is yaw, then
  pitch, then roll, a fixed order.
- **`krandom.h` is NOT a CSPRNG**; `krandom_quality()` says how much to
  trust it (the enum is ORDERED BY TRUST). Read
  `kernel/lib/stack_protector.c` before moving the canary randomisation.
- **`ttf.h` PARSES UNTRUSTED INPUT, IN RING 3 ONLY** --
  `userland/lib/ttf.c`, for `/bin/fontd`; the kernel parses no font
  (f34019fc). Every read is bounds-checked, and it allocates NOTHING (a
  caller's `struct ttf_scratch`).
- **Draw through `uui_canvas` in ring 3, and `gfx_fill_circle()` in the
  kernel** -- the one shape ring 0 draws; bind another in `gfx.c` when a
  kernel caller needs it. Not `geom_*` directly. `geom.c` compiles TWICE
  from one source, so it may name nothing kernel-only; `geom.h` draws
  through a callback, never into a framebuffer. **Code with no ring-0
  caller lives in `userland/lib/`**, not shared (`rubberband.c`,
  `icon_grid.c`).
- **No `k_strcpy`: `k_strlcpy` with the DESTINATION's size**, never
  guessed. The `write_dec`/`write_hex` chains are FROZEN per file by
  `tools/check_chains.py`, a ratchet; new number formatting is
  `vga_printf()`/`klog_printf()`.
- **A formatter that doesn't fit writes NOTHING; a parser REJECTS rather
  than guesses.**
- **SIZE IT TO THE DATA: ALLOCATE AND GROW, NEVER INVENT A CAP**
  (2026-10-19) -- a list or string from the user, disk or network is
  allocated to fit, not a `static x[SOME_MAX]`; mirror a bound only when
  a protocol, ABI or the hardware sets it; the kernel keeps fixed tables
  only where it may not allocate. A cap that must stay LOGS when hit.
  `docs/conventions/kernel.md`.
- **SHARED FROM THE START WHEN IT IS GENERIC BY NATURE** -- its job
  names no app's data (a desktop-entry lookup, a find bar, a path
  helper) -- even with one caller; APP-SHAPED logic stays in the app. A
  second caller is a signal, not a gate (it was a gate until
  2026-10-02; `docs/decisions/workflow.md`). **Fix as touched**: a
  change to an app moves that app's generic helpers into `userland/lib/`
  or `userland/ui/`.

### Widgets and layout

**In `userland/CLAUDE.md`**, which loads when working under `userland/`:
`ops->hit` is a boolean, ops tables are the contract, layouts overflow
rather than shrink. `tools/check_widget_ops.py` enforces the first.

### Anything drawn

**Follows `docs/gui-guidelines.md`, which is binding; the six rules that
bite most are in `userland/CLAUDE.md`.** The one that bites outside it:
`gfx_draw_string()` does not clip -- `gfx_draw_string_clipped()` in any
fixed box. **A new scrolling view is TESTED for scrolling** (wheel
direction, thumb vs view, trough, drag: `tools/scroll_test.py`, rule 10
there) -- a mirrored scrollbar hid in three widgets until a folder gave one
enough rows.

### The line editor

**ONE LINE EDITOR, COMPILED TWICE** (`kernel/lib/klineedit.c`, also in
`libuapp.a`): `/bin/tosh`, the GUI Terminal and the physical shell share
it, so an editing key goes in the core's keymap, never one front end.
The rest -- memory, `kline_free()`, ANSI decoding, history -- is in
`kernel/lib/CLAUDE.md`.

### The filesystem

- **Each mount has ONE SLEEPING LOCK, and its holder may sleep in a disk
  wait. Never take it, or any kmutex, with the preemption guard raised or
  with interrupts off and no scheduler slot** -- that spins forever
  behind a sleeping holder. Lock order: parent mount before child. A
  quiet disk across several calls is `fs_exclusive_begin()`, never the
  preemption guard.
- **There is no `fs_read()`**: `fs_read_into(path, buf, cap)`, which
  refuses an oversized file, or `fs_read_range()`.
- The full rules (lock drops, tfs3 inode locks, config reads) are in
  `kernel/fs/CLAUDE.md`.

### The sandbox

- **A sandboxed process's missing file is usually its unveil list**:
  `sandbox: pid N refused path X` in `dmesg` (the first 8 per sandbox).
  Ring 3 reads locale and settings data as FILES too, so a sandbox that
  reads settings unveils `/etc/settings.d` and the setting's `File=`.
  A syscall from a group it gave up kills it (SIGSYS, a crash report
  naming the call). `ps --sandbox` shows what each process kept.
- **A program parsing hostile input pledges before the first byte, and
  refuses to run if it cannot** (webcontent, fontd, `thumb`);
  `docs/conventions/kernel.md` has the rule, `abi/sandbox_abi.h` the API.
- **`shm_unlink` is the CREATOR's alone** (-EPERM otherwise), and a
  uchan server adopts a ring only from the pid that made it.

### The static checks that fail the build

Each waives with a named comment, and the reason is the mechanism, not
the waiver. `docs/tools.md` has what each one looks for.

| Check | Fails on | Waiver |
|---|---|---|
| `check_widget_ops.py` | a widget ops slot it needs left NULL (`draw` needs `natural_size`+`set_geometry`; `set_geometry` needs `bounds`; `press` needs `release`; `key` needs `accepts_focus`; a scrollbar must not route `hit` through its row hit) | `widget-ops-ok:` |
| `check_key_routing.py` | the inverse -- a filled slot nobody can reach: an app naming a `.key` widget without `.focus` or `.on_key`, or declaring `.widgets` while hand-routing its menu bar | `key-routing-ok:` |
| `check_drivers.py` | a `.c` under `kernel/drivers/` with no `DRIVER_DECLARE`. It is FILE-SCOPE DATA, not a call inside `init()` | `driver-none:` |
| `check_dispatch.py` | a dispatch chain over ~20 branches that should be a table (this project already has `display_driver`, `block_device`, `clocksource`, `syscall_table.c`) | `dispatch-ok:` |
| `check_font_state.py` | a function that calls `ugfx_set_font()` and never puts the old font back (it zoomed a whole window once) | `font-ok:` |
| `check_text_loops.py` | a BYTE of a string measured or drawn as a character (`ugfx_char_advance(s[i])`) -- strings are UTF-8; step with `ugfx_text_decode()` | `text-loop-ok:` |
| `check_control_states.py` | a menu bar or toolbar with no `item_flags`: no command is ever greyed | `flags-ok:` |
| `check_layout.py` | a staged file with no tracked source behind it | -- |
| `check_test_sleeps.py` | a `tools/*_test.py` that gained a `time.sleep(` past its frozen count -- wait on what you assert (`settle()` syncs, `harness.poll()`) | `sleep-ok:` |

### Files, comments and history

- **Split a file once it has grown genuinely harder to work with** --
  more than one concern, or slow to find things in -- not preemptively,
  and never against a line count quoted in a doc (`wc -l`; generated
  files like `kernel/drivers/font_ttf.c` excluded). Follow
  `userland/wm/`: split by concern, share state through an
  `_internal.h`, record why in a top-of-file comment.
- **PREFER FACTS THAT CANNOT GO STALE. Do not cite a number that some
  other file has to keep true.** Safe to point at, all by TITLE: a named
  file or symbol, a `docs/decisions.md` section, a `docs/roadmap.md`
  milestone, a rule here or in the feature-workflow skill. **For the
  PRESENT point at a title; for the PAST point at a COMMIT** -- a short
  SHA with what it did ("the poison-page fix (978ebf7)").
- **A comment's length tracks how SURPRISING and DANGEROUS the code is**,
  not its history; two things earn length, **the invariant** and **the
  trap**. Would the sentence be true had nobody got it wrong first? Does
  `docs/decisions.md` already say it? Is a real system NAMED (a clause)
  or used to JUSTIFY (a paragraph, which belongs in decisions)? Over ~6
  lines on a field or small static function is a smell unless it is a
  genuine trap. **Cap the anecdote at one clause.** Existing long
  comments are deliberately not being retro-trimmed.
- **THERE IS NO CHANGELOG.** What changed, file by file -> the COMMIT
  MESSAGE (`git log` is the record). How it works and its trap -> a
  comment by the code. Why this way -> `docs/decisions.md`, IN FULL.
  What is BROKEN -> `docs/bugs.md`, one line each, repro in
  `docs/roadmap-details.md`; a fixed bug is DELETED, an intermittent one
  gives its RATE, an unestablished cause is said plainly, and
  PRE-EXISTING means MEASURED. What is NOT BUILT -> `docs/roadmap.md`
  (its "Known limitations" is for things working as designed).
- **A COMMIT MESSAGE IS PROBLEM, THEN CHANGE, THEN FILES** -- imperative
  subject under ~72 chars with an area prefix (`settings:`, `wm:`,
  `kernel:`); a paragraph or two on what was wrong and why; a bullet per
  change; every changed file with a one-line note. No capitalised lede
  sentences, no war stories, no forensics. **A change a person can SEE
  or DO ends with a `Release-note: <new|improved|fixed>: <one sentence>`
  trailer, and an unseen change to the OS with `Release-note: internal:
  <Area>: <plain words>`** -- System Update's What's new
  (`docs/conventions/build.md`). Bodies before 2026-08-24 are
  an older essay voice (one-liners before 2026-08-15), not rewritten.

### Traps that only bite while testing

Here rather than in `docs/testing.md` because each makes a broken thing
look fine.

- **A PROBE THAT OUTRUNS THE LOG DESTROYS THE EVIDENCE IT GATHERS, AND A
  RATE-LIMITED PROBE LOOKS EXACTLY LIKE A LOOP THAT STOPPED.** The klog
  ring holds a few thousand lines (128 KiB): stay under a line a second, give every
  probe in a comparison the SAME limiter, and use a file-backed serial
  log for anything verbose.
- **Four ways a GUI test passes without testing anything:** "it
  responds" is not "it is drawn" (the ring-3 Calculator shipped with no
  visible buttons); moving identical content is pixel-identical, so
  number the rows; a test must not assume what it is testing. Ask what a
  broken version would still pass -- and park the pointer outside a
  compared box: its sprite alone makes two frames differ.
- **A positive control can turn nothing red because the test's DATA
  never reached the code under test** -- suspect the fixture before the
  harness, and ask what input actually reaches the branch.
- **A screendump compared against another screendump must be a SETTLED
  frame** (`QMPSession.screenshot()` is, by default; `stable=False` opts
  out) -- and so must one compared against ITSELF; a client that drew
  into its buffer is not necessarily composited. **A poll whose exit
  condition is weaker than what the code after it needs is a flake.**
  **Settled is not caught up**: a frame is evidence about the
  FRAMEBUFFER, never the machine -- ask the serial console before
  writing "hung".
- **Verify GUI changes by reading PIXEL VALUES** (`tools/pixel_probe.py`),
  and always sample a control that should NOT have changed.
- **A hover state needs `DebugConsole.hover_frames()`, never `gui
  move`** -- an injected position lasts ONE `wm_run()` iteration. Assert
  the BAND, not the change; SELECTION OUTRANKS HOVER. **The same goes for
  the WHEEL and a TOOLTIP**: they go where the REAL pointer is, so park it
  with `warp_cursor(qmp, x, y)` (`scroll_test.check_view(warp=True)`).
- **USB AUDIO TESTING HAS TRAPS OF ITS OWN** -- which path a G6 result
  exercised (UAC1/UAC2, the bind line), two playbacks back to back being
  ONE stream: `.claude/rules/usb-audio.md`, which loads with the usb and
  sound drivers, soundd and the audio tools.
- **NOTHING REBUILDS OR EDITS THE TREE UNDER A RUNNING TEST, AND THAT IS
  ENFORCED** (`tools/tree_lock.py`): a test holds a shared lock until it
  exits, `make` refuses while it is held, and a PreToolUse hook
  (`.claude/settings.json`, tracked) denies a source edit, a `make` or a
  tree-moving `git`. Docs, `tools/` and `.claude/` stay editable. A NEW
  tool that boots QEMU without `iso_guard` or `harness.Results` calls
  `tree_lock.hold()` itself.
- **A NEW TOOL THAT SEEDS A BOOTABLE IMAGE PASSES `seed_disk.py
  --fbs-done`**, or First-boot setup's full-screen wizard covers the
  desktop and every GUI check after it fails (`docs/commands/fbs.md`).
- **`seed/sync/` KEEPS WHAT YOU DELETE FROM `data/`**: `make clean`
  (staging) THEN `make clean-disk` (the image). **NEVER hand-write into
  `seed/sync/`** -- gitignored build staging that `make clean` deletes;
  the source is `data/` (Start-menu entries in `data/wm/applications/`,
  icons via `tools/gen_icons.py` -> `data/icons/`). Confirm `git status`
  shows a new data file.

## Conventions indexed in `docs/conventions/INDEX.md`

**Everything above is what fires UNANNOUNCED. The other conventions -- several hundred --
are indexed by headline in `docs/conventions/INDEX.md`, one hop away** --
`kernel.md`, `gui.md`, `storage.md`, `shell.md` and `build.md` carry the
bodies, and the index names every rule in all five.

**Read the index before editing an area**, ahead of the area file: skim
for a headline that tells you something you did not know, then open the
area file for its body. It lived here until it was 41 KB of always-loaded
context, which is carried rather than read; moving it deleted no rule.

**A new convention goes in `docs/conventions/<area>.md` AND gets its
headline added to that index.** Half of that pairing is not optional.

## This checkout, and the repo it pushes to

**The repo is `eveningworks/saunaos`**, an ORGANIZATION since 2026-08-16
(a personal repo has no read-only collaborator role; `docs/decisions.md`).
The maintainer's account was renamed and the old handle scrubbed by a
history rewrite: the remote is `git@github.com:eveningworks/saunaos.git`,
nothing in the tree should name a personal account (`grep -rn` before
believing a doc), and **run `tools/backup_repo.sh` before any change to
the repo's identity or history.**

- Git identity (`SaunaOS` / `noreply@saunaos.local`) is PER-REPOSITORY; a
  clone does not carry it and `preflight.sh` refuses without one. **It
  is the privacy convention, not a default to override** -- never let a
  commit carry the maintainer's real name or email. **A Claude Code
  CLOUD session commits as `Claude <noreply@anthropic.com>` instead**,
  which its signing key makes Verified on GitHub (2026-10-23,
  `docs/decisions/build.md`). Commands: `docs/development-setup.md`.
- **A CLAUDE CODE CLOUD SESSION IS NOT THE CACHYOS BOX**: Ubuntu 24.04,
  nothing installed, its own global identity, no KVM, pushes to its
  `claude/...` branch, no `gh`. Setup: `docs/development-setup.md`.
- Push ordinary verified work to `main` without asking; **confirm tags,
  Releases, force-pushes and history rewrites first** -- one-way.

## Building

**`docs/testing.md` is the full testing reference; `docs/tools.md`
covers every script.**

```
make all    # kernel.bin + userland ELFs -- reaches NO boot medium
make iso    # + saunaos.iso; seeds disk.img and installs GRUB + kernel on
            # it -- run before ANY headless test
make test   # boot headless, run the in-kernel suite
make verify # the pre-delivery gate (= tools/preflight.sh)
make run    # QEMU with an SDL window (the user's machine)
make live-iso / usb-image / debug
```

**ONE RUN TARGET; every way to boot is a variable on it** -- never add a
target for a combination. Check with `make -n run <FLAGS>`: `KVM=1
VIRTIO=1 AUDIO=1 NOGRAPHIC=1 MENU=1 MEM=512 BALLOON=0`, `DISK=`/`VGA=`/`INPUT=virtio`,
`WINDOW=full|fit` (sdl cannot scale; `fit` alone helps a bigger guest
mode, and alone blurs the font), `LIVE=1`, `BOOT=cd|disk`, `STRIP=0`,
`COMPRESS=0`, `KDEBUG=1|net` (the kernel's GDB stub on localhost:1235;
`net` needs `tools/kdebug_bridge.py` running).

- **`VIRTIO=1` MEANS EVERY DEVICE CLASS** -- disk, GPU and input; a
  per-class value overrides it.
- **`BOOT` IS DERIVED**: the disk, when `disk.img` carries GRUB and the
  kernel.
- **Editing the Makefile** -- definitions are DEFERRED (`=`, `$(if ...)`),
  `LIVE` changes the prerequisite, `MENU=1` derives `GRUB_TIMEOUT`:
  `.claude/rules/makefile.md`, which loads with `Makefile`.
- **`EXTRAS=1` IS THE ONLY THING THAT MAKES A BUILD REACH THE NETWORK,
  AND AN EXTRAS IMAGE IS NOT YOURS TO PUBLISH.** It fetches
  differently-licensed material after showing its licence
  (`LICENSE=agree` for CI; no terminal = refused).
- **Boot flags bake into the media**: `make iso KCMDLINE="video=1920x1080
  nokaslr"`; every word is in `docs/boot-flags.md`. **`video=` only does
  something on a MODESETTING driver.**
- **Source discovery is recursive** (a new directory under `kernel/` or
  `apps/` needs no Makefile edit) and **a CFLAGS change is not
  dependency-tracked** (`make clean` after one) -- the detail is in
  `.claude/rules/makefile.md`.
- **`make all` without `make iso` leaves the whole suite testing the
  PREVIOUS build**, as a clean PASS; `tools/iso_guard.py` refuses that.
- **Every automated test runs TCG, so a green suite says nothing about
  SPEED-dependent bugs or guest MEMORY TYPES** (TCG ignores PAT); both
  have shipped. Try `tools/vm.py --kvm` before calling a hardware-only
  report untestable; `tools/kvm_soak.py` is the standing check.
- **Test against a COPY of `disk.img` if the user might have QEMU open**
  (`cp --reflink=auto --sparse=always`), and **a copy goes stale the
  moment you rebuild** -- it then boots an earlier build. **A copy is not
  enough for `make iso`/`make verify`/`preflight.sh`**, which re-seed
  the real image: check `ps aux | grep qemu-system` and ASK the user to
  close theirs first (standing request).
- **BEFORE BELIEVING ANY GUI TEST FAILURE, RE-RUN IT ON A FRESH IMAGE**
  -- `gui_regress.py` builds one by default now (`fresh_disk.py`); a tool
  run by hand, or `predates.py`, still boots `disk.img` (`make clean-disk
  && make iso`): `make iso` syncs rather than reformats, and apps write. Still red: rebuild `HEAD` to
  prove it isn't yours. **A test that applies a setting changes the
  machine for every later tool** (`settings_test`'s pointer speed made
  unrelated hover tools fail).
- **`strace <binary>`** beats temporary `klog_write()` calls (`-e`
  filters, `-c` counts); it is a `/bin` program, so `spawn` it at a `#`
  prompt, where its trace lands in `dmesg`.
- **READ THE ABI COMMENT OF ANY CALL YOU SWAP IN** -- the ring-3 GUI
  port swapped a non-blocking `scheduler_poll()` for `sys_waitpid()`,
  whose first line said **BLOCKS**.
- **A MECHANISM THAT EXPLAINS THE SYMPTOMS IS NOT THE MECHANISM THAT
  CAUSED THEM** -- do the cheap disproving check, and prefer a
  discriminating experiment over a plausible story.

## Debugging with GDB

`make debug` (QEMU's stub) or **the kernel's own stub** (`make run
KDEBUG=1`, the one that works on bare metal): `docs/testing.md`,
"Debugging with GDB"; `.claude/rules/gdb.md` loads with `tools/gdb/` and
the kdebug code.

## Testing in QEMU headlessly, via QMP

**Is it a GUI change at all?** If not, `tools/vm.py` is faster and gives
text. Cheapest first: `boot_smoke_test.py` -> `make test`/`vm.py exec` ->
QMP. `docs/testing.md` has the `QMPSession`/`GuiFlow` API and its gotchas.

- **Use `tools/qmp_test.py`, don't rederive it**; extend the module.
- **Never `pkill -f qemu-system-x86_64`** -- it cannot tell your guest
  from the user's window. Kill only the PID your launch wrote to
  `-pidfile`.
- **TWO GUESTS ON ONE QMP PORT DO NOT FAIL AS A PORT CLASH** -- they
  surface MINUTES later as a `BrokenPipeError` in some other tool
  (`tools/port_guard.py` refuses at launch). **Every tool takes
  `--instance N`** (the QMP port and both serial sockets -- the debug console's `.vm.N.serial` on COM2, the kernel log's `.vm.N.log` on COM1 -- from one number); `auto`
  belongs only to what LAUNCHES a guest; name a slot whenever anything
  else may run (`gui_regress.py` holds `0..DEFAULT_JOBS-1`). Concurrency
  is fine for an answer, not for judging the suite.
- **DON'T ADD A WAIT LOOP FOR WORK ALREADY IN THE BACKGROUND** -- its
  completion notification is the signal. **If a wait is genuinely
  needed, wait on a PID or an artifact, bounded** (`tools/wait_for.sh
  <pid>`, `--file PATH`). **Never match a process by NAME**: the
  waiter's own command line contains it, so it can never exit (`[f]oo`
  does not save it).
- **Prefer `tools/gui_debug.py` to pixels** for anything not about
  rendering; call `DebugConsole.settle()`, never a fixed sleep.
- **Screenshots are a TESTING TOOL, not a deliverable** -- a scratch
  directory, never `screenshots/`; show one only when seeing it is the
  answer.

## tools/

**`docs/tools.md` is the full reference** -- what each script does, why
it exists, the traps it encodes. Read it before reaching for a tool you
haven't used recently, and add to it (not a one-off script) when
something would save a future session real time -- the bar is "does this
fix a rederive-from-scratch cost". `--list` on `gui_regress.py` and
`ondemand_sweep.py` names what each one runs; `preflight.sh` takes no
`--list` -- its header comment lists its steps.

**The question-to-tool index is at the top of `docs/tools.md`** -- is it
MINE (`predates.py`), when did this check last fail or pass, at which
commit and on which host (`test_history.py`, the LOCAL record of every
run -- ask it FIRST when a check goes red), can my test SEE the break (`mutate.py`), is it safe
to commit (`preflight.sh`, your `vm.py` guest stopped), wait safely
(`wait_for.sh`), before any change to the repo's history
(`backup_repo.sh`) -- with the host tools (`bear`, `ruff`, `shellcheck`,
`ccache`).

**WHAT THE USER SAYS, AND WHAT IT MEANS** -- ask if a request is
ambiguous between the matrix and GitHub:

| Phrasing | Means |
|---|---|
| "run the matrix" / "check the old QEMUs" | `tools/qemu_matrix.py` -- LOCAL, Docker, ~15s per version. Answers "does this depend on the host's QEMU?" |
| "run CI" / "kick off Actions" | `gh workflow run build.yml`. REMOTE, ~90s. Answers "does it build from a clean clone on someone else's machine?" |
| "run preflight" / "verify" / "run the tests" | `bash tools/preflight.sh` -- the ordinary per-change gate |

- **`qemu_matrix.py` RUNS AT A RELEASE AND WHEN THE USER ASKS** (standing
  instruction). **OFFER it** when a change plausibly depends on the
  host's QEMU or CPU -- a driver, a poll loop, a timeout, a clocksource,
  DMA. A finding is a REPORT, not a blocker. `--suite usertest --runs N`
  turns the ring-3 suite into a rate.
- **GITHUB CI RUNS ON A RELEASE TAG AND ON DEMAND ONLY** (2026-08-19) --
  it is for a CLEAN-CHECKOUT build on someone else's machine.
- **`gui_regress.py` is the standard check** after touching `userland/`
  or anything the WM draws -- always `--logs DIR`. Its wall clock is the
  larger of the slowest tool and the sum over jobs; the per-tool timeout
  is a hang guard. `DEFAULT_JOBS` is one guest per PHYSICAL CORE, capped
  at 16 (2026-10-24: one per thread went red on the i7-11700 host).
- **`damage_sweep.py`/`damage_hunt.py` are in NO runner** (reason in
  `ondemand_sweep.py`'s exclusions) -- run them by hand after touching
  anything that draws or damages. `damage_sweep.py` needs a guest with
  SOUND HARDWARE for the volume panel (`vm.py --audio-wav <path> --audio
  both start`). **A clean result proves nothing until
  `--positive-control` has shown it can fail** -- true of every positive
  control here.
- **`iso_guard.py` refuses a stale `saunaos.iso`**;
  `SAUNAOS_ALLOW_STALE_ISO=1` bypasses it deliberately.

## docs/

**This file holds RULES; reference material lives in `docs/` behind a
one-line pointer.** Add detail there.

**What each file in `docs/` is, and the rules for editing it** -- a
convention's headline in `INDEX.md`, a decision entry then
`gen_decisions_index.py`, milestones NAMED, `**NEXT**` then
`gen_next_up.py`, a roadmap item ONE line -- are in
`.claude/rules/docs-editing.md`, which loads with any `docs/` file.
**Design documents (`docs/*-design.md`): read the one for the area
first** -- the list, with what each has BUILT and when to read it, is
`.claude/rules/design-docs.md`, loaded with `kernel/`, `userland/` and
`apps/` files.

- **A NEW COMMAND NEEDS ITS `docs/commands/` PAGE IN THE SAME CHANGE** --
  the build refuses otherwise, and a declared `cmd_usage()` string must
  appear on it verbatim (exemptions: `check_docs.py`'s
  `COMMAND_PAGE_EXEMPT`). **It takes its arguments through
  `lib/uargs.h`'s table**, which gives it `-h`/`--help`; the page must
  name every option and command in the table.

## Two skills live IN this repo

`.claude/skills/saunaos-feature-workflow/` -- the end-to-end playbook
(research first, offer real choices, build and test with proof, write
the docs, ship). `.claude/skills/network-egress-disclosure/` -- what to
disclose when testing reached a host outside this machine.

Both are tracked here rather than in `~/.claude/skills/` so they have
history, diff review and a backup. **Update them in the repo**, or the
two copies drift and the untracked one silently wins.

**The agents in `.claude/agents/` are the delegation targets** -- each
fixes its model and tool list, which is what makes it cheap to start;
when to use them is the feature workflow's "Delegating to agents".

**`SKILL.md` is the PLAYBOOK; the lessons live in `references/`** -- the
layout, the ~10 KB limit and "point at ONE section" are
`.claude/rules/skills.md`, loaded with `.claude/skills/**`.

## Delivering changes

The files are already on the real checkout: commit with plain `git`,
and push verified work.

- **END EVERY DELIVERY WITH A SHORT "TRY IT YOURSELF" GUIDE** (standing
  instruction) whenever a change alters something a person can SEE or DO:
  the `make run` flags, the app or command, what to TYPE and what to
  EXPECT -- "`spin_test 900000`, then Ctrl-C -> job stops, `^C`, prompt
  back", not "Ctrl-C now works". **Say when it is NOT reachable from the
  default boot** (`Ctrl-C` needs a `text` target; `hires` work needs
  `KCMDLINE="video=1920x1080"`). Nothing to see: say so in a line.
- **LIST EVERY FILE ADDED OR EDITED** in the final response, compactly
  (standing instruction).
- **WHEN THE WORK IS DONE, SWEEP THE DOCS BEFORE THE HAND-OVER**
  (standing instruction, 2026-10-20): grep `docs/` and the CLAUDE.md
  files for what the change made stale -- a roadmap item now done or
  newly found (`roadmap-details.md` too), `docs/features.md`, a
  convention for a new trap (+ its INDEX headline), a design or spec
  doc, `docs/tools.md`, any doc the code now CONTRADICTS -- and commit
  the fixes as a `docs:` commit. `check_docs.py` checks shapes, not
  truth; a multi-commit change leaves docs behind unless swept.
- **IF TESTING LEFT THIS MACHINE, REPORT EVERY EXTERNAL HOST IT REACHED
  -- IN THE CHAT, NOT IN THE COMMIT** (standing instruction, 2026-09-07):
  end with `External hosts contacted during testing:` -- each host, its
  resolved ADDRESSES, the protocol, what did it -- or `none (SLIRP and
  localhost only)`. **SLIRP's 10.0.2.3 is a FORWARDER, not a resolver**:
  a lookup through it leaves the machine. Prefer a server on loopback.
  Full rule: `.claude/skills/network-egress-disclosure/`.
- **WHEN A CHANGE MOVES A LAYER BOUNDARY, DRAW THE STACK** (standing
  instruction) -- ASCII, the changed part marked, plus the directory
  tree when files moved -- **ONCE, as it ends up**, in the hand-over.
  Boundary moves: a new subsystem directory; a new registry or
  implementation of one; a header changing audience; a call site moving
  between rings or between kernel and driver; anything changing what may
  include what -- not a bug fix, a new widget or a doc edit. **Show BOTH
  axes** when they differ: what it sits ON and what it PLUGS INTO
  (virtio: one transport stack; virtio-blk into `block_device`,
  virtio-gpu into `display_driver`).
- **Never write personal information into any file** -- ask first, or
  anonymise and say so plainly. **The repo is public: this network's
  addresses, MACs and hostnames live in `local_info.txt`** (untracked);
  a tracked file says `<asus-ip>`, `<lenovo-ip>`, `<machine-ip>`.
- **Reusable tooling goes in `tools/`**, with `docs/tools.md` updated --
  and **A NEW TEST TOOL MUST BE NAMED BY A RUNNER** (`preflight.sh`,
  `gui_regress.py`, `ondemand_sweep.py`, or the sweep's exclusions with
  a reason). Nothing enforces it: audit `tools/*_test.py` against each
  runner's `--list` (`preflight.sh`'s header comment).
- **AT EVERY COMMIT, SWEEP THE BACKGROUND SHELLS** (standing
  instruction, 2026-08-30): list what is still running and close what
  the work no longer needs (`ps aux | grep "[z]sh -c source"`;
  `preflight.sh` prints long polls -- minutes is normal, hours is the
  leak). Kill by PID.
