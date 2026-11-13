# A TTY layer, pseudo-terminals, and one implementation of `Ctrl-C`

A staged plan, in the shape `docs/signals-design.md` and
`docs/query-design.md` used. It answers the question that document left
open: **`Ctrl-C` works on the physical console and does nothing in the
GUI Terminal, and fixing that twice is the wrong answer.**

**Status: STAGES 1, 2 AND 3 ARE BUILT (2026-08-22). `Ctrl-C` works in a
Terminal window, through the same code as the physical console's.** A
terminal is an object, the console is `tty0`, `SYS_OPENPTY` hands out a
pair, and the GUI Terminal is a real terminal emulator running
`/bin/tosh` on a pty. Stage 4 (virtual terminals) is still a plan --
though the JOB CONTROL this layer was a prerequisite for is built, and
landed the same day (`docs/signals-design.md`'s stage 4). Each
stage's section says what actually landed and where it differs from what
was planned here, because two things did. **OUTPUT PROCESSING (`OPOST`,
`ONLCR`, `TAB3`) AND THE SCROLLING REGION (DECSTBM with IL/DL/SU/SD/RI)
ARE BUILT TOO (2026-11-11)** -- "Output processing and the scrolling
region", after stage 4.

## Why this is not "add ptys"

The GUI Terminal cannot interrupt a job because it has no terminal. It
reads keys as WINDOW EVENTS, so it never touches fd 0, never claims the
console, and has no foreground group -- and `userland/libs/base/tosh.c`'s
`stdin_ok` exists precisely to stop it lending its children a keyboard
it does not own. Every child it spawns gets an empty stdin (a pipe with
the write end closed), which is what a process with no controlling
terminal gets on a real system, and is honest.

The tempting fix is to let the Terminal notice `Ctrl-C` in its own key
handler and signal the child it spawned. It knows the pid; it would
work. It would also make the Terminal a THIRD place that decides what
`Ctrl-C` means, beside the keyboard driver and `kernel/tty.h` -- and
this repo has spent several changes deleting exactly that shape (see
`docs/decisions/shell.md`'s "A command with a read half and a write half
moves as one piece"). The `ls` wrapper builtin died for it.

So the real question is not "how does the Terminal send SIGINT". It is
**what is a terminal in this OS**, and the answer has to serve the
physical console and a window equally, or it is not an abstraction.

## What exists today, measured

Checked against the tree before this was written, the same way
`init-design.md` was.

- **There is one console and its state is two globals.**
  `kernel/proc/tty.c` holds `g_owner_pid` and `g_fg_pgid`, reached only
  through functions -- `kernel/tty.h` says outright that when multiple
  terminals land "this state becomes per terminal, which is
  bookkeeping". That prediction is what makes stage 1 cheap.
- **INTR recognition is one line in the keyboard IRQ.**
  `keyboard.c:278`: `if (code == 0x03 && tty_intr()) return;`. Its own
  comment says it belongs to a line discipline "and there is not one
  yet".
- **There is exactly one input ring, and it carries modifiers.**
  `ring_push()` stored `(mods << 16) | code` in a 256-entry ring (since
  replaced: the console terminal's queue, and a compositor's own key
  stream -- `keyboard_try_get_key()`). Three
  consumers pop it: the ring-0 blocking readers, `sys_do_read_console()`
  for ring 3, and `win_input.c` for the compositor. They do not compete,
  because only one is live at a time -- `keyboard_blocking_suspended()`
  is the interlock.
- **Two independent reasons stand the ring-0 reader down**, and
  `api/keyboard.h` keeps them as separate flags because both can hold at
  once: a compositor owns the keyboard, or a ring-3 process claimed
  fd 0.
- **A pipe is already the object a pty wants to be.** `pipe.c` is a
  refcounted kernel object with a bounded buffer, a blocking read, a
  wait channel, and an EOF that means "empty AND no writers". Its
  `-1 = would block` / `0 = EOF` split is the contract a tty needs too.
- **The fd table is one table with a `kind` tag.** `SYS_CLOSE`,
  `fd_inherit()` and `fd_release_all()` look only at `used`/`refs` and
  `owner_pml4`, so a new kind costs them nothing. Adding one is an enum
  value and a case in three switches.
- **There is no device namespace.** `vfs.c` has no mount table (that is
  why `docs/query-design.md` refused a `/proc`), so there is nowhere to
  put a `/dev/ptmx` or a `/dev/pts/3`, and inventing one for this would
  be building the wrong thing first.
- **`SYS_TCSETPGRP`/`SYS_TCGETPGRP` already exist**, and take no fd:
  they mean "the console", because there is only one.

## What real systems do

**Linux.** A terminal is a `struct tty_struct` with a LINE DISCIPLINE
attached (`N_TTY` by default), a driver underneath (VT, serial, or pty)
and a `termios` describing its behaviour. A pty is a driver like any
other: `/dev/ptmx` hands out a master, `/dev/pts/N` is the slave, and
the discipline sits between them. `Ctrl-C` in a terminal window is:
compositor delivers the key to the emulator -> the emulator writes
`0x03` into the master -> the discipline sees `VINTR` with `ISIG` set ->
`SIGINT` to that tty's foreground process group. The emulator never
touches the keyboard device, and there is exactly one implementation of
what `Ctrl-C` means for every terminal on the machine.

The VT the compositor itself runs on is a separate matter: it is put in
`KD_GRAPHICS` and its keyboard MUTED (`KDSKBMODE`, `K_OFF`), so the
kernel stops turning scancodes into tty input at all while the
compositor reads evdev directly.

**Windows** arrived at the same place two decades later. Before Win10,
`conhost` owned a console object that processes ATTACHED to, and
`GenerateConsoleCtrlEvent` was the Ctrl-C path; ConPTY is a
pseudoconsole pair and is deliberately pty-shaped.

**What SaunaOS should copy, and where it must differ.**

- **Copy the SHAPE: a tty object, a discipline, a driver underneath, a
  foreground group per tty.** That is the abstraction that makes the
  console and a window the same kind of thing.
- **Differ on the NAMESPACE.** No `/dev/ptmx`, because there are no
  device nodes and a mount table is a different project. A pty comes
  from a syscall that returns BOTH fds at once -- the shape of BSD's
  `openpty(3)` rather than of `posix_openpt`. This also deletes the
  `setsid()` + `TIOCSCTTY` dance: a process is handed its terminal
  rather than acquiring one by opening a path.
- **Differ on the SIZE.** `termios` is four flag words and 32 control
  characters in POSIX, nearly all of which describe hardware that has
  not existed for forty years. This took two lflags and one control
  character, and grows only when something asks -- it is four lflags
  (`ICANON`, `ECHO`, `ISIG`, `ECHOCTL`), three oflags and five control
  characters now.
- **A compositor holding the keyboard mutes the console tty**, which is
  the direct analogue of `K_OFF` on the VT and is what stops the
  discipline swallowing keystrokes the desktop is about to receive.

## The shape

```
                        struct tty
  ┌──────────────────────────────────────────────────────────┐
  │  termios  { lflag: ICANON|ECHO|ISIG,  cc[VINTR] = 0x03 }  │
  │  fg_pgid, owner_pid                                       │
  │                                                           │
  │   input:  raw queue ──► LINE DISCIPLINE ──► canon queue   │
  │                          echo, erase, kill,               │
  │                          INTR ─► SIGINT to fg_pgid        │
  │   output: queue                                           │
  └───────▲──────────────────────────────────┬────────────────┘
          │ tty_input_byte()                 │ tty_output()
          │                                  ▼
   ┌──────┴───────┐                   ┌──────────────┐
   │  the DRIVER  │                   │  the DRIVER  │
   ├──────────────┤                   ├──────────────┤
tty0│ keyboard IRQ │                   │  vga_write() │   the physical console
   ├──────────────┤                   ├──────────────┤
ttyN│ master write │                   │ master read  │   a pty
   └──────────────┘                   └──────────────┘
```

One object, two drivers. The console's driver is the keyboard and the
framebuffer; a pty's driver is *the other fd*. Everything above the
dashed line -- canonical mode, echo, `Ctrl-C`, the foreground group --
is written once and is identical for both. That is the whole point, and
it is the test of whether the abstraction is real.

**Reading a slave** is `read(fd)`: raw queue in raw mode, canon queue in
canonical mode, parking on the tty's own wait channel when empty.
**Reading a master** drains the output queue. **Writing a master**
pushes bytes through the discipline as if typed. **Writing a slave**
appends to the output queue, which for tty0 means `vga_write()`.

## What this is NOT

- **Not a `/dev` namespace.** No paths, no device nodes, no mount table.
  A terminal is an fd you were given -- the one exception being
  `/dev/tty1`..`/dev/tty4`, names in `sys_open()`'s table that only init
  may open, to start a shell on a virtual terminal.
- **Not a replacement for `klineedit.c`.** This is the sharp edge of the
  chosen design and is stated plainly: canonical mode DUPLICATES what
  `kernel/lib/klineedit.c` already does in ring 3, and every shell here
  will turn ICANON off immediately -- exactly as `readline` does on
  Linux. Canonical mode exists for programs that have no editor of their
  own and want a line, which today is `catin` and anything a future
  session writes. **The default is POSIX's** (`ICANON|ECHO|ISIG`, plus `ECHOCTL`
  as on Linux), and
  the three shells explicitly set raw, because a program that knows
  nothing about terminals should get the behaviour it expects, not this
  OS's.
- **Not job control.** `fg`/`bg`/`SIGTSTP` are stage 4 of
  `docs/signals-design.md` and need this, not the other way round. That
  prediction held exactly: job control landed on top of this layer
  hours later, and needed two things FROM it -- the SUSP character,
  which went in beside INTR in the same discipline function, and the
  foreground group, which is what decides whether a reader is a
  background one. It added nothing here.
- **Not virtual terminals yet.** `Ctrl+Alt+F1..F4` is bookkeeping once
  the object exists, and is a separate stage on purpose.
- **Not a `termios` a Unix program could be ported against.** Four
  lflags, three oflags, five control characters. `docs/libc-design.md`'s completeness rule does not apply here:
  this is a kernel interface, and the project bar (a second real caller)
  does.

## Staging

Each stage is a commit that lands green on its own.

### Stage 1 -- the object, and the console becomes `tty0` -- **BUILT**

`kernel/tty/` gains `struct tty`, the discipline, and a registry. The
keyboard IRQ stops recognising INTR and calls `tty_input_byte()`
instead; `keyboard_try_getchar()` becomes a read of tty0's raw queue, so
there is still exactly ONE ring and no keystroke can be delivered twice.
`tty_console_owner()`/`tty_foreground_pgid()` become per-tty with tty0
as the console, keeping their signatures so nothing above them changes.
`QUERY_TTY` becomes a LIST -- one record per terminal -- and `/bin/tty`
grows a row per terminal.

**Nothing user-visible changes.** The payoff is that stages 2-4 become
small. The risk is the console read path, which is load-bearing for
every headless test: `tools/tests/sweep/console_shell_test.py` and
`tools/tests/sweep/ctrlc_test.py` are the gates that say whether it still works.

A compositor owning the keyboard BYPASSES the discipline, feeding the
raw queue directly, which is `K_OFF` on the VT and is what keeps
`win_input.c` working unchanged.

**What actually happened**, beyond the plan: `SCHED_CHAN_KEY` had to go,
and that turned out to be the most interesting part. A global "a key
happened" channel was correct with one console and is the thundering
herd with one terminal per window. And the discipline's KTESTs found a
real defect on their first run -- the input queue was the same size as
the canonical buffer, so a maximum-length line plus its newline did not
fit and the tail was dropped, breaking the no-truncation promise one
layer below where it is made.

### Stage 2 -- `SYS_OPENPTY`, and `termios` -- **BUILT**

`SYS_OPENPTY` returns a master fd and a slave fd. `FD_KIND_TTY_MASTER`
and `FD_KIND_TTY_SLAVE` join the fd table; the read/write switches route
to the tty; `fd_desc_unref()` closes an end. `SYS_TCGETATTR`/
`SYS_TCSETATTR` take an fd, and `SYS_TCSETPGRP`/`SYS_TCGETPGRP` are
generalised from "the console" to an fd -- with fd-less callers meaning
tty0, so nothing that exists breaks.

Teardown is where ptys go wrong, so it is stated up front: the last
master close makes the slave read EOF (the pipe rule); the last slave
close makes the master read EOF; and a tty is freed only when both ends
are gone. `/tests/pty_test` proves the pair end to end, including that a
`0x03` written to the master reaches a spawned child as `SIGINT`.

**AND ONE PROMISE IN THIS DOCUMENT WAS REVERSED, which is the honest
part.** Stage 1 said `tty0` starts raw and that the special case "goes
away with stage 2". It did -- but not the way this said. The plan was to
give `tty0` POSIX defaults and have the shells ask for raw, and that is
exactly what happened; what was not foreseen is that `kernel/shell/shell.c`
CANNOT call `tty_set_termios()`, because `kernel/shell/` is deliberately not on
the `kernel/include/kernel/` include path. The capability got a function
on the app-facing side instead (`keyboard_console_set_raw()`, in
`api/keyboard.h`), which is what `kernel/include/README.md` says to do
and is a better answer than the one planned: ring 0 and ring 3 now ask
for the same thing through their own boundaries, over one
implementation.

Two smaller ones. `SYS_TCSETPGRP` and `SYS_TCGETPGRP` had to GAIN an fd
-- they meant "the console" because there was one, and a shell in a
window moving the console's foreground group would aim the physical
keyboard's `Ctrl-C` at its own child. And a sixth wait reason
(`SCHED_WAIT_TTY`) was needed for the OUTPUT side, distinct from
`SCHED_WAIT_KEY`: they are different queues with different channels, so
`ps` says `block(tty)` for a master waiting on its shell and
`block(key)` for a shell waiting on its keyboard.

### Stage 3 -- the GUI Terminal becomes a terminal -- **BUILT**

`userland/apps/apps/terminal/terminal.c` stops linking `tosh` as a library. It
opens a pty, spawns `/bin/tosh` on the slave, writes keystrokes into the
master and paints what comes out. It deletes `stdin_ok`, `empty_stdin()`
and the output sink, and the shell inside a Terminal window becomes a
REAL PROCESS visible in `ps`.

**The known risk, named before starting**: `klineedit.c`'s console front
end repaints a line with `\r` and two passes, because `vga_cursor_move()`
is a non-destructive seek ring 3 cannot reach. The Terminal's scrollback
(`utext`) has never had to honour a carriage return.

**It turned out to be twelve lines**, because the two-pass repaint needs
exactly one thing that `utext` lacked -- overwrite in place
(`utext_set()`). The emulator is a line start and a column: `\n` ends a
line, `\r` sets the column to 0, `\b` moves it back, and anything else
overwrites at the column or appends past the end. What made it small is
that `/bin/tosh` emits nothing else; ANSI is a roadmap item, and the
kernel console already has a parser (`kernel/lib/ansi.c`) that this
window should end up sharing rather than duplicating.

**TWO THINGS THE PLAN DID NOT FORESEE, and both were found by tests
rather than by reading.**

**A pty must be claimed by its first READER, not by whoever opened it.**
`pty_create()` originally made the opener the owner, which is the
emulator -- a process that never reads the slave. So the shell's
`tcsetpgrp()` was refused with `-EPERM`, the foreground group stayed
wrong, and `Ctrl-C` in a window signalled a group with nothing in it. The
fix is the console's own rule, and the code is now the same shape in both
places: claimed on the first read.

**There is no non-blocking read, and a terminal emulator needs one.** The
window has to service the compositor AND drain its child and cannot sit
blocked in either. `SYS_SET_NONBLOCK` is `fcntl(F_SETFL, O_NONBLOCK)`
cut to the one flag; the real answer is `poll()`, which is a bigger
project and is on the roadmap. Until then the Terminal drains on a 30ms
tick, and says so.

**And `Ctrl-D` needed `VEOF`**, which the discipline did not have -- plus
the thing every real shell does and this one did not: **restore the
terminal's mode around a child**. A shell that leaves the terminal raw
gives its children a terminal with no `VEOF` at all, so `cat` with no
arguments could never end. `/bin/tosh` saves the mode at startup and
brackets every command with it, which is exactly what `readline` does.

### Stage 4 -- virtual terminals, and what follows

`Ctrl+Alt+F1..F4` switching between several tty0-like consoles; per-tty
scrollback; `isatty()` over `SYS_FSTAT`'s existing `SYS_STAT_TTY` flag;
window size, which is the `ioctl` every full-screen program wants. Each
is small once the object exists, and none is in this session's scope.

**`isatty()` and window size are BUILT** (2026-08-22), as are the
signal-generating characters; what is left of this stage is virtual
terminals and per-tty scrollback -- the second DEFERRED until the first
exists (picked 2026-11-11; Linux removed fbcon's and vgacon's
scrollback in 5.9). And the prediction that "each is small
once the object exists" has now been tested twice by things that were
not on this list -- SUSP was three lines in the discipline, and the
background-read rule was one function and two call sites.

#### The virtual terminals: what was picked, and the plan (2026-11-11)

**Picked:** the KERNEL catches `Ctrl+Alt+F1..F4` in the keyboard driver,
ahead of the compositor, so a hung desktop can be escaped (Windows' SAS
shape; Wayland on Linux cannot). The desktop is on F1; F2-F4 get
`/bin/tosh` on the FIRST switch to each (systemd's `autovt@`); on a text
boot F1 is today's console. Each VT keeps its own scrollback, allocated
when it is first used. Scrollback was "deferred until VTs exist"; it now
comes with them.

**What "bookkeeping" turned out to hide, measured before building:**

- **The console keeps NO cell grid.** What is on screen exists only as
  pixels in gfx's back buffer, plus a byte-stream scrollback that cannot
  rebuild a cursor-addressed screen. A VT that is not on screen must be
  redrawable on a switch, so each needs a screen buffer of its own --
  Linux's `vc_screenbuf`. That, not the switching, is the work.
- **Every piece of per-screen state is a file-scope static in `vga.c`**:
  the cursor, colours, ANSI parser, scrolling region, saved cursor, UTF-8
  decoder, cursor blink, and the 256 KB scrollback ring.
- **The compositor draws straight into the scanout** (`win_surface.c`'s
  grant), and the kernel already knows how to point a holder's mapping at
  a scratch frame and to re-grant it after a mode change -- the pieces of
  Linux's DRM-master drop and `VT_PROCESS` release/acquire.

**The plan, each stage green on its own:**

- **B -- the VT core.** A `struct vc` holding the per-screen state and a
  cell grid (character + attribute per cell); `vga.c`'s statics become
  the ACTIVE vc's, and output to an inactive one updates only its grid.
  Text VTs 2-4 are ttys on a vc driver. `Ctrl+Alt+Fn` in `keyboard.c`
  switches, redrawing the screen from the target's grid. Testable on a
  text boot with no desktop in the picture.

  **BUILT 2026-11-13.** `vga.c` keeps four `struct vc`, each with its
  grid (the 0xB8000 layout, font byte | attribute), cursor, colours,
  ANSI parser, region, UTF-8 decoder and scrollback; every write
  updates the grid and only the SHOWN vc's reaches the pixels. VT1 is
  the kernel's console and tty0's, so the kernel's own text lands there
  whichever VT is shown (picked 2026-11-13; Linux prints on the
  foreground VT). VT2-4 are ttys named `vt2`..`vt4` (`kernel/tty/vt.c`),
  created on the first switch. Three things the plan did not say:
  - **The switch is a KERNEL THREAD's** (`vt`): the keyboard IRQ only
    records the request, because a switch redraws a whole screen and
    the first one to a VT allocates. Linux defers its own to a
    workqueue. A kernel thread runs with interrupts off, so a key's
    echo cannot land in the middle of a redraw.
  - **Shift+PageUp/PageDown scroll the shown VT's history from the
    keyboard driver**, as on a Linux VT: the kernel shell's own
    PageUp only works where IT reads the keys, and a ring-3 shell's
    reader never sees it.
  - **Back to live is the GRID**, not the scrollback's last lines: the
    ring records the byte stream, which cannot rebuild a
    cursor-addressed screen (`vt_test.py` went red with the old
    repaint).
- **C -- the desktop on F1.** Switching away from it points the
  compositor's grant at the scratch frame (its stores land nowhere) and
  hands the scanout to the console; switching back re-grants it and asks
  for a full repaint, the path a mode change already takes. The
  keyboard's bypass follows the active VT.

  **BUILT 2026-11-13.** Leaving VT1 HIDES the desktop whether or not a
  compositor runs, so one that starts behind a text VT stays there:
  every scanout mapping (holder, lessee, stale lessee) goes to the
  scratch frame, presents become no-ops, the display flips to buffer 0
  (the console's), the cursor plane goes off, the pointer and key
  PRESSES stop reaching it -- releases of keys it saw pressed still do,
  or Ctrl and Alt would stay held from the Ctrl+Alt+F2 itself -- and it
  gets `WIN_EV_VT` 0. Coming back re-maps the scanouts, flips to the
  compositor's last frame and sends `WIN_EV_VT` 1; toywm draws a full
  frame. Not the mode-change path: `WIN_EV_SCREEN` re-lays every window
  out and closes the menus, and nothing about the screen changed. While
  hidden toywm draws nothing and its idle clock stands still, or the
  screen would go dark under a text VT. A hung desktop cannot refuse:
  the kernel does all of it.
- **D -- a shell on first use, and scrollback per VT.** The kernel tells
  init that VT n became active with nobody on it, and init starts
  `/bin/tosh` there with the VT's tty as 0/1/2 (a service descriptor of
  the `autovt@` kind). Each vc gets its own scrollback ring when first
  used; F1 keeps today's static one, which must work before the heap.

  **BUILT 2026-11-13** (the scrollback with stage B). The `vt` thread
  sends init SIGWINCH when it shows a VT no session holds -- the
  signal Linux's keyboard request sends init. `/etc/services.d/autovt`
  says `TTY=vt`, which makes it a TEMPLATE: init opens `/dev/ttyN`
  (init's alone; `ENXIO` until VT n has been shown, so the open is the
  question) -- an open of a VT no session holds HANGS IT UP and clears
  it, getty's `vhangup()`, so a job the last shell left behind reads end
  of file and gets `EIO` on a write instead of drawing on the next
  shell's screen (the serial terminal's old sessions still fall back to
  the console: `hangup_to_console`) -- prints `SaunaOS -- tty2 ·
  Ctrl+Alt+F1 returns to the desktop` (`console` on a text boot) and
  spawns `/bin/tosh` on it in a session of its own. An exit brings a
  new one on a cleared screen (picked 2026-11-13: getty's
  `Restart=always`), under the services' crash-loop rule; `service`
  lists each instance as `autovt@ttyN`. Two differences from the
  picked mockup: the banner's dash is `--`, since the console's font is
  Latin-1 and has no em dash, and tosh's own one-line banner (its
  build) follows it.

## Output processing and the scrolling region -- **BUILT 2026-11-11**

Picked 2026-11-11: Linux's shape for both, in BOTH terminals. Until
then every renderer here returned the carriage on a line feed -- the
console in `vga.c`, the GUI Terminal in `uvterm.c` ("no OPOST: LF is
CRLF here"), the serial terminal by inserting a CR in its driver, and
`telnetd` by translating in ring 3. Four places deciding what `\n`
means, which is the shape this layer exists to delete.

**What real systems do.** Linux's n_tty runs `process_output()` on
every byte written to a terminal, and on what it echoes: with `OPOST`
and `ONLCR` a `\n` goes out as `\r\n`; with `TAB3` (`XTABS`) a tab
goes out as spaces to the next multiple of 8, measured against a COLUMN
the discipline keeps from what it sent (a CR zeroes it, a backspace
takes one, a UTF-8 continuation byte counts nothing under `IUTF8`). The
terminal under it -- a VT, xterm, a pty's emulator -- treats LF as a
bare line feed: down a row, same column. The kernel's own console
output (`printk`) reaches the VT through `vt_console_print()`, which
does its own CR for a `\n`, beside the tty's `con_write()`. DECSTBM
sets a region of rows that scroll; a line feed on its bottom row
scrolls only those rows, RI on its top row scrolls them down, IL/DL
work from the cursor's row to the region's bottom (the cursor to column
1), SU/SD move the whole region; xterm keeps in its scrollback only
what leaves the top of the WHOLE screen.

**What was built:**

- `struct tty_termios` has an `oflag` word: `TTY_OPOST`, `TTY_ONLCR`
  (both on by default, `TTY_OFLAG_DEFAULT`) and `TTY_TAB3` (off).
  `kernel/tty/ldisc.c`'s `tty_ldisc_output()` applies them to every
  write and every echo, for every terminal; `process_byte()` is the one
  place the rules live.
- **Both renderers moved to xterm's line feed.** `uvterm.c` keeps the
  column on LF (and on VT/FF, as xterm does); the console's
  `vga_putc_console()` -- tty0's driver -- does too, while `vga_putc()`
  keeps the carriage-returning newline for the kernel's own writers,
  exactly the `vt_console_print()` split.
- `serial_tty.c`'s private CR and `telnetd`'s translation went; libc's
  `c_oflag` round-trips (`TABDLY`/`TAB0`/`XTABS` beside `TAB3`), and
  `cfmakeraw()` clears `OPOST` as glibc's does.
- **The region's RULES are shared** -- `ansi_region_set()` and
  `ansi_region_op()` in `kernel/lib/ansi.c`, compiled into both rings --
  so the console and the window cannot disagree about where a line
  lands. Each terminal stores its region and moves rows when told:
  `uvterm.c` in its cell grid, `vga.c` with `gfx_scroll_band()` (or the
  text buffer on the legacy backend).

**Where it differs, and why:**

- **`oflag` IS 16 BITS, IN WHAT WAS PADDING, BEHIND A MARKER.**
  `struct tty_termios` keeps its 12 bytes, so nothing needed a
  `TOY_ABI_VERSION` bump -- but a program built before it may hand over
  UNINITIALISED padding (the old libc's `tcsetattr()` did), so the
  kernel takes `oflag` only when `have_oflag` is `TTY_HAVE_OFLAG` and
  otherwise keeps the terminal's own. A program that read the termios
  first carries both back.
- **Three output flags, not POSIX's dozen.** No `ONLRET`, `OCRNL`,
  `ONOCR`, delays or fill characters: nothing here asks for them.
- **A pty write is sized AFTER processing, in the pass that sends it.**
  `pty_slave_write()` sends through `tty_ldisc_output_some()` only what
  fits the ring once `\n` has doubled, and returns the caller's count:
  what fits goes, nothing fitting parks.
- **ECHOCTL, on by default as on Linux.** An echoed control character
  goes out as `^X`, never raw: without it Alt+M typed at `cat` echoed
  `ESC m`, and with a capital `ESC M` is reverse index -- the cursor
  jumped. Erase rubs out both cells.
- **THE KERNEL'S OWN TEXT IGNORES THE REGION**, where Linux's printk
  honours it: a program that set one and died would otherwise trap
  every later klog line, and a panic banner, in its rows.
- **The console's region resets when its grid changes** (a font change,
  `vga_clear()`), as xterm's does on a resize; the GUI Terminal resets
  it on a resize and on Reset. The console's scrollback stays a record
  of the byte STREAM, so rows moved inside a region are not in it --
  the same caveat cursor movement already carried.

**Tests.** KTESTs in `kernel/tty/tty_test.c` (ONLCR on and off, OPOST
as the master switch, TAB3 against the column, an echoed Enter, a pty
write sized by ONLCR) and `kernel/lib/ansi_test.c` (each sequence, each
rule); `uvterm_test` runs `userland/tests/ansiregion_seq.h`'s numbered
rows against the grid; `tools/tests/sweep/ansi_cursor_test.py` reads
the same scenario as pixels in a Terminal window and on the console,
plus `#\n#` under `OPOST` on and off. Each went red on a deliberately
broken build (ONLCR emitting a bare LF, both renderers returning the
carriage, a line feed ignoring the region).

**Found on the way, fixed with stage B of the virtual terminals:** the
console did not present the tail of a ring-3 write while nobody read the
console -- `ESC[0A`'s check failed every run. `vga_idle()`, from
`scheduler_idle()`, is the deferred present that answers it.

## The controlling terminal -- **BUILT 2026-11-11**

Picked 2026-11-11 with the virtual terminals: Linux's hangup rules,
`tcgetsid()`, and an open of one's own terminal.

**What real systems do.** On Linux a session leader acquires a
controlling terminal (opening one without `O_NOCTTY`, or `TIOCSCTTY`);
every process in the session then has it, and `open("/dev/tty")` opens
it. When the leader exits, `disassociate_ctty()` sends SIGHUP (and
SIGCONT) to the terminal's foreground group and the terminal belongs to
no session; when the terminal hangs up (a pty master closed), the
session leader and the foreground group are signalled. A daemon leaves
its session with `setsid()` first, or dies with it.

**What was built:**

- **The terminal names the session; a process does not name a
  terminal.** `tty_of_session(sid)` finds a process's controlling
  terminal from the session `tty_set_owner()` (or `SPAWN_SETSID`)
  recorded, so there is one fact and nothing per process to keep in
  step.
- **A session leader's exit lets the terminal go** --
  `tty_session_leader_gone()`, from `scheduler_on_exit()`: owner,
  session and foreground cleared, SIGHUP then SIGCONT to the foreground
  group. A pty slave is then claimed afresh by its next reader.
- **A closed master signals the session leader too**, not only the
  job in front: a shell waiting on a job is outside that job's group and
  outlived its window before.
- `SYS_TCGETSID` (libc `tcgetsid()`), and `SYS_OPEN_CTTY`, which is
  libc's `open("/dev/tty")` -- and `fopen()`'s, and `ctermid()` names it.
  The descriptor is the kind the terminal's own holders have: one more
  pty slave end, the console, or the serial terminal by index. `ENXIO`
  without one, Linux's answer.

**Where it differs:** no `O_NOCTTY`/`TIOCSCTTY`/`TIOCNOTTY` -- a terminal
is HANDED to a session (`SYS_OPENPTY`, `SPAWN_SETSID`) or claimed by its
first reader, never acquired by opening a path, so there is nothing for
them to switch off. Two consequences, each a rule in the code:

- **Every kernel-started process leads a session of its own**
  (`sched_fork.c`), so each serial-debug-console command is a session
  leader. That session's shell is the kernel, which hangs the terminal
  up itself without a signal, so a command's exit sends NO hangup --
  otherwise `open` and `spawn` there killed what they had just started.
  `/bin/spawn` starts its child with `SPAWN_SETSID` anyway (`setsid(1)`),
  for when it runs under a real shell.
- **A master's closer is never hung up by its own close.** The first
  reader of a slave claims it for the reader's session, so a program
  driving a pty whose child read first owns that session -- Linux's
  `O_NOCTTY` is what spares a program there.

**Tests:** `/tests/ctty_test` (spawned) starts sessions on ptys: its
terminal by `tcgetsid()` and by `/dev/tty`, a leader's exit hanging up
its job, a closed master reaching a leader that waits on a job, and
`ENXIO` with no terminal. The two hangup checks went red with the exit
hook and the master's leader signal taken out.

## Open questions

- **Does the raw queue keep carrying modifiers?** It does today
  (`(mods << 16) | code`), because the ring-0 GUI readers want them and
  a byte cannot express them. A tty is a BYTE stream. Stage 1 keeps the
  modifier word on the raw queue and lets the discipline see bytes,
  which works because every code this driver produces already fits in a
  byte -- but it means "the raw queue" is not quite a byte queue, and
  that should be either fixed or documented rather than left ambiguous.
- **What owns the console tty when nothing has read it?** Ownership is
  claimed on first read. Sessions exist now (`setsid()`, `SPAWN_SETSID`)
  and a pty is handed to a new session at spawn, but tty0 never is
  (`proc_syscalls.c` says why: a Terminal window's shell took the
  machine's keyboard) -- so the console stays first-reader.
- **Should `/bin/tosh` on a pty still be the same binary?** Yes, and it
  is the test of the abstraction: if the shell needs to know whether its
  terminal is a window or the physical console, the layer has failed.
