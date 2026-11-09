# `tools/lib/` -- The harness library

One entry per tool in `tools/lib/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`toolpath.py`** -- where everything in `tools/` lives. A script in
  any folder puts `lib/` on `sys.path` and imports it first; it then puts
  EVERY tools/ folder on `sys.path`, so `import harness` or `import
  filemanager_test` works from anywhere, as it did when tools/ was flat.
  `toolpath.REPO`, `toolpath.TOOLS`, `toolpath.tool("vm.py")` (a tool's
  absolute path by basename -- never `os.path.join(HERE, "vm.py")`, which
  breaks the moment a tool changes folder) and `toolpath.rel()`. **It
  refuses to load when two tools share a basename**, because the
  flat-namespace import would then pick one silently.

- **`runner_meta.py`** -- what runs each test, read from the test: a
  test's FOLDER names its runner and its `RUNNER = {...}` literal says the
  rest (`covers`, and optionally `name`, `args`, `vm`, `parallel`,
  `needs`, `shared_vm`, `gate`, `manual` -- the module docstring has each
  key). Read with `ast.literal_eval`, never imported, so it must stay a
  plain literal; a list of dicts runs one tool more than once
  (`audio_test.py` and `audio_test.py --card hda`). `--check` is
  `checks/check_runners.py`; `--folder tests/gate` prints `path TAB name
  TAB covers` for a shell loop (preflight's).

- **`preconditions.py`** -- `precondition_met()`, the `needs` keys of a
  RUNNER header (`iwad`, `samba`, `host_pillow`, `kcmdline`...): can this
  test run on this machine at all? A missing precondition is a SKIP with
  its reason, never a failure. Shared by `ondemand_sweep.py` and
  `hostchecks.py`.

- **`qmp_test.py`** -- QEMU/QMP GUI testing helpers — see
  `docs/testing.md`

- **`vm.py`** -- start a headless VM and run shell commands against it,
  getting text back; `start` is ready once init logs `startup finished`,
  not when the console first answers (`docs/testing.md`), `--virtio-disk` likewise -- and `--extra-disk PATH
  --extra-disk-kind virtio|ahci|nvme` puts that second disk on the
  controller a test needs; `--usb xhci|xhci+mouse` attaches an xHCI
  controller and USB HID devices, off by default because attaching a
  `usb-kbd` takes the keyboard AWAY from PS/2 — see `docs/testing.md`
  (`xhci+crowd` is eleven devices, a hub of eight keyboards among them);
  `--pci-fill N` adds N inert `pci-testdev`s behind PCI-to-PCI bridges,
  for a guest with more devices than the default machine's handful;
  `--usb-stick IMAGE[@PORT][,4k]` attaches an emulated USB stick (QEMU
  `usb-storage`, bulk-only; `,4k` for 4096-byte blocks; QMP ids
  `ustick0`, `ustick1`, ... for `device_del`); `--usb-host VID:PID` (or
  `BUS-PORT`, the host's own address, for two devices of one model)
  passes a REAL device off the host's bus through to the guest, which is
  the only way to reach a driver path no emulated device has — QEMU's
  `usb-audio` is UAC1 at one format, so every UAC2 path in `sound_usb.c`
  is unreachable without it; it needs write access to the device's
  `/dev/bus/usb/BBB/DDD` node (`root:root 0664` normally, so `sudo chmod
  o+rw` on it, which a replug resets) and it TAKES THE DEVICE AWAY from
  the host until the guest exits; `--net e1000|virtio|both|none` chooses
  the NIC, where `e1000` is what QEMU already attached implicitly to
  every guest ever launched here and `virtio` is the only way to reach
  `virtio_net.c`; `--hostfwd tcp::8080-:80` maps a host port onto a
  guest one, which is the ONLY way anything can start a conversation
  WITH the guest -- SLIRP is a NAT, so outbound needs no configuration
  and inbound needs this

- **`vm.py savevm NAME` / `vm.py start --loadvm NAME`** -- hold a machine
  and bring it back: `savevm` saves the running guest (RAM, devices,
  disk) inside its boot disk, which must be a `.qcow2` (`qemu-img create
  -f qcow2 -b disk-copy.img -F raw x.qcow2`; the format follows the
  extension), and `--loadvm` resumes it instead of booting -- with the
  SAME flags, since QEMU restores device state into whatever the command
  line built. Useful for a state that took long to reach. It restores
  onto any `--instance`. Measured 2026-11-02: restore to a live desktop
  4.1 s against a cold boot's 6.6 s, which is why the GUI suite does not
  use it (`docs/decisions/build.md`, the tools/ folders entry).

- **`harness.py`** -- what every test tool had written for itself: a
  results table (`Results`: `.check()`/`.ok()` returning the verdict,
  `.passes`/`.fails`, `.rows`, `.failed()`, `.skip(name, why)` for a
  check a run could not judge (`.skips`, and `, K skipped` on the
  summary; a run that only skipped exits 77, which `gui_regress.py`
  reports as SKIP and counts on its own), `poll(fn, timeout)`, a
  bounded wait on an observable, and `.finish(tool)` printing the
  `<tool>: N passed, M failed` line `gui_regress.py` reads),
  `copy_disk(src, dst, cwd=None)` (the sparse, reflinked copy), and
  `BodyServer` (a threaded HTTP(S) server on this machine answering
  with one body or a route table, reached from a guest as 10.0.2.2).
  `Results` also RECORDS the run in the local test history
  (`test_history.py`), keeping everything printed from its construction
  on as the log -- except under `gui_regress.py`, which records the
  tool's row itself. Moved in on 2026-09-30 from 67 tools' own tables, 40 `cp` lines and
  `hwdata_test`/`https_test`'s servers. **Only the part that did not
  differ** (`dup_scan.py`'s rule): a table with `report()`,
  `clipboard_test.py`'s `check(ok, what)` order, and `damage_hunt.py`'s
  copy with a plain-copy fallback keep their own. A new tool uses this.
- **`components.py`** -- the optional components on the host
  (`docs/software-design.md`): `load()` the `data/components/` files,
  `check()` them (and a staging tree), `owner_map()` -- the LONGEST
  covering path owns a file -- and `sizes()`. One module for the seed step
  (`stage_components.py`), the static check and `update_server.py`'s
  `component=` tags, so the three cannot disagree about whose a file is;
  the machine's reader is `userland/libs/base/ucomponent.c`.
- **`hostcheck.py`** -- the build half of a `*_hostcheck.py`: `write()`
  the C driver, `stage()` a repo source into the temp directory with the
  positive control's (before, after) edits -- STOPPING when a `before`
  is gone, since a control that patched nothing proves nothing -- and
  `compile()` with the host gcc (`STRICT` flags by default). Used by
  hash, utween, umd, utext, hid_parse and teapot; divti3, midi, regex,
  usnd, ugfx_text, uimg and uimg_codec build differently enough
  (sanitizers, shim headers, two compilers) to keep their own.
- **`serial_boot.py`** -- the TCP-serial plumbing the controller boot
  tests (`virtio_boot_test`, `ahci_test`, `partition_test`,
  `poweroff_test`) each had: `connect()`, `read_until()`, and
  `run_session()` (wait for the debug console, send commands -- a
  `(command, needle)` pair waits on the ARTIFACT rather than a fixed
  settle -- return the transcript, kill QEMU). Each tool keeps its own
  `launch()`, which is the part that really differs.
  `serial_console.py` is the unix-socket equivalent.
- **`fresh_disk.py`** -- a PRISTINE image of the current build, and
  whether an existing one has drifted. `make_fresh(dest)` is `make
  iso`'s recipe pointed elsewhere -- a sparse image, `seed_disk.py`,
  `install_grub.py` -- in ~20 s, touching neither `disk.img` nor the
  Makefile's `.seeded` stamp; `gui_regress.py` uses it by default.
  `drift(disk)` is check_layout's orphan list: files on the image no
  build puts there. It is INFORMATION, not a verdict -- a boot writes
  `/etc/saunaos.conf` too, and so does a test that changes a setting, so a
  name cannot say which. `predates.py` names it beside a PRE-EXISTING
  verdict, where it could change the answer.
- **`preflight_stamp.py`** -- `preflight.sh` writes `build/.preflight-pass`
  on PASS: the git TREE hash of every tracked and untracked
  non-ignored file (built in a scratch index; `.claude/` and `.mcp.json`
  left out). `why_not()` says whether that still covers the working
  tree AND whether the build has been re-seeded since. It is what
  `update_server.py --publish` checks, so a release is a tested build.
- **`gui_debug.py`** -- `DebugConsole`, the connection to the guest's
  serial debug console, and the thing to reach for BEFORE pixels: it
  returns facts to assert on rather than an image to interpret.
  **It reads TWO sockets on a `vm.py` guest**: commands and replies on
  the debug console's (`.vm.N.serial`, COM2), and the kernel log on
  `.vm.N.log` (COM1) through a background thread, into the same buffer
  `logs()`/`events()`/`damage_bugs()` have always read -- so a tool
  waiting for an app's log line (`settings: layout ...`) does not
  change. `events()` returns the log lines that arrived since the
  command before its sweep began. On a one-port guest there is no log
  socket and it reads the one wire as before. See docs/decisions.md,
  "The kernel log and the debug console are two serial ports".
  **`close_window(title)`** asks a window to close as its close button
  does and waits for it to go (False if it refused) -- `gui close` takes
  a z-order index, which callers used to look up by hand.
  `send(cmd)` runs a debug command, `json(cmd)` parses one with
  `--json`, `sh <cmd>` runs a KERNEL-shell command, and `settle()` waits
  for injected input to drain, for the clients it reached to draw it
  (`gui sync`; `sync()` alone is the round trip), then its grace for
  work after the frame (`settle(0)`: the frame alone) -- never replace it
  with a fixed sleep: the console is asynchronous. `windows()`/`window(title)` ask the WM
  what it is drawing, and `enter_gui()` polls the desktop ready instead
  of sleeping.

  **Waits the WM decides, and input it times** (`docs/gui-test-sync-design.md`):
  `wait_window(title, pid=)`, `wait_widget(title, name)` and
  `wait_gone(title)` ask `gui wait` and follow its ticket with `gui waited`
  -- the compositor evaluates the condition, a `--pid` that exits ends the
  wait at once, and a WM without `gui wait` falls back to polling.
  `spawn()` is built on `wait_window` (60 s default, paid only by a hang);
  `sync(title=)` also asks a window that is neither focused nor under the
  pointer; `input("key 0x09 alt; keyup 0xf798")` and `tap(k, hold_ms=)` send
  one `gui input` sequence whose gaps the guest times. `send()` gives a
  `sh PROGRAM` reply 60 s and PRINTS one cut short; `json()` waits up to 30 s
  for a `gui` provider not yet registered (the desktop restarting).

  **A REPLY THAT TIMES OUT USED TO SHIFT EVERY LATER REPLY BY ONE.**
  `send()` reads to the next prompt; a command slower than `timeout`
  (a long `dmesg` while a TCG guest is still booting) left its output
  and prompt in flight, the next command stopped at THAT prompt, and
  from then on each check was handed the previous command's answer --
  measured by forcing one timeout: `echo C` answered `BBB`, for the rest
  of the run. It surfaced as checks quoting unrelated output
  (`resolv.conf`, `rectory`) and runs of empty replies, in more than one
  audio tool. A timeout now marks the console out of step, and the next
  `send()` first resyncs on a marker nothing else prints (`_resync()`:
  an unknown command whose REPLY names a unique tag); what it skips goes
  to `log_lines`. Counting owed prompts was tried first and is wrong: a
  guest still booting can drop a typed line, and a prompt that never
  comes then wedges every later read.

  **`widgets()` / `widget_center()` / `widget_at()` answer "where is
  that control"** -- the question this module could not answer at all
  until the client began exporting its widget map (`docs/decisions.md`,
  and `abi/win_proto.h`'s `WIN_REQ_WIDGET`; the compositor cannot see
  inside a window, so it is the CLIENT that reports). `widgets()`
  returns `{name: {x, y, w, h, screen: {x, y}}}` for the frontmost
  window or one named by title, keyed by the app's own `uui_item.name`;
  `widget_center(name)` gives the SCREEN point to click, which is the
  half that otherwise gets re-derived (and got wrong) per tool; and
  `widget_at(x, y)` names what is under a point. `widget_center` RAISES
  on an unknown name and lists what the window does report, because
  returning a plausible `(0, 0)` lands the click on the window corner
  and fails the test somewhere else entirely. An empty map is the
  ordinary answer for a client with no named widgets, not an error.
  **26 tools still hand-parse the `: layout ` lines** for the same
  geometry; converting them is separate work, and `uidemo_test.py`
  cross-checks the two sources against each other.

  **`cursor_shape()` reports what the compositor would DRAW under the
  pointer** -- `gui state --json`'s `cursor.shape`, a `WM_CURSOR_*`
  (`userland/apps/system/toywm/wm_internal.h`), with the frame's edge rules and the
  client's own `WIN_REQ_CURSOR` already resolved against each other.
  That is the check for a client-side resize cursor: the alternative is
  recognising a 15x21 sprite in a screenshot, which cannot tell "the
  client asked for the wrong shape" from "the theme failed to load".
  `key(k, mods="ctrl")` sets the `KEY_MOD_*` bits delivered ALONGSIDE
  the key without re-encoding it, which is what a real keyboard does.

  **`processes()` gives every process as a dict** (pid, ppid, pgid,
  state, cpu, name), parsed from `/bin/ps` run through the kernel's
  shell -- a different reader from whatever ring-3 shell is under test,
  so a broken one cannot make the two agree.
  `processes_named(prefix)` filters it and drops zombies.

  **`cpu` is how you tell a SUSPENDED process from an idle one.** A
  state column says what the kernel thinks; only a number that stops
  advancing says the scheduler agrees, and that distinction is what
  caught a "stopped" process still being scheduled. A zombie is included
  by `processes()` on purpose: filter it out for "is it still running"
  and ask about it separately for "was everything reaped", because
  mixing the two makes every count ambiguous between them. Two tools
  hand-rolled this parser in one session before it moved here, which is
  this file's usual bar.

  **`pin_wallpaper(dbg, name="aurora")` shows a known picture and waits
  for the desktop's own `desktop: wallpaper <name>` line** -- for a tool
  whose pixel probes were calibrated on one wallpaper's colours, since the
  default (`lakeside`, a night scene) is near-black where a dimming,
  shadow or glass probe samples. It sets the TYPE before the picture: a
  setting whose `Requires=` is unmet refuses a write.

  **`warp_cursor(qmp, x, y)` puts the REAL cursor somewhere and CONFIRMS
  it arrived**, and `hover_frames(qmp, tmp, rest_at, hover_at)` returns
  the two settled frames a hover check compares. Both exist because
  `gui move` cannot hold a hover: an injected pointer position overrides
  the mouse for the single `wm_run()` iteration that consumes it, and
  the next one reads the driver again and snaps back -- exactly right
  for a click (press and release are edges), useless for a state that
  has to survive a capture. A tool that hovers with `gui move`
  photographs the frame after the pointer left and reports a working
  hover as dead. `warp_cursor` closes the loop (`goto()` is open-loop
  and the WM ACCELERATES the delta, so asking for (300,250) from the
  corner arrives at (450,374)); `hover_frames` adds the two things
  around it -- settled captures, and a REST frame with the pointer
  parked somewhere real, since the cursor sprite is part of the screen.

  **`drag_real(qmp, x0, y0, x1, y1)` is the only drag a ring-3 CLIENT
  actually sees**, and neither of the two obvious ones works on one.
  `gui drag` queues injected positions the WM consumes one per
  iteration, reading the real mouse in between -- so the client gets a
  leave event and no held motion at all. `QMPSession.drag()` sends its
  moves faster than the guest draws frames under TCG, so the WM sees ONE
  position change: measured, a 152px drag reported an identical `mx` on
  every frame from press to release, and the client was never told
  anything moved. This warps the real cursor once per step, CONFIRMED,
  so each step costs the guest a frame and the client sees motion.

  A kernel-side app notices neither problem, because the WM calls its
  `on_press` every tick with the current position -- which is why
  `scrollbar_test.py` passes on `gui drag` and a client test cannot.
  Only a client needs this; both failures look identical from the
  outside, and identical to the app simply ignoring the drag.

  **`gui resize W H` is the only way to resize a window**, and it is in
  the WM rather than here for a reason worth stating: the frame's grip
  needs a real pointer the compositor tracks across frames, and neither
  injected input nor `drag_real` can be one for a frame drag. A check
  that dragged the grip measured a window **988x498 before and after**
  -- unchanged -- and both its halves passed against a terminal that had
  never resized. Every client-side resize behaviour was untestable until
  this existed, which is how a broken one shipped.

  **`warp_confirmed(qmp, x, y, check)` confirms a DIFFERENT claim**, and
  the difference matters: `warp_cursor` proves the pointer is where you
  aimed, not that where you aimed is what you meant. A list row is one
  line tall, so a y computed from a stale origin, a scrolled view or a
  column header lands on a NEIGHBOURING row -- perfectly plausible, so
  the failure reads as "the click did nothing" instead of "the click hit
  the wrong thing". `check` is a callable returning truthy when the app
  itself reports the intended target under the pointer (its hovered row,
  its hot button); it re-warps until that holds. Assert on the return
  value, or a miss passes silently. Poll it with `layout_now()` rather
  than a wait-for-change helper: a layout block is emitted only when it
  CHANGES, so a warp that lands where the pointer already was logs
  nothing at all.

  **`changed_rows(rest, hover, box)` is the band form**, for a control
  whose rows have no reported geometry -- a dropdown popup, a listbox.
  It compares mean brightness per pixel row, so text contributes to both
  frames equally and only a wash moves the number. **Assert the BAND,
  not the change**: one band, containing the pointer, no taller than a
  row. "Something got darker" also passes on a repaint, a scroll, or a
  whole list highlighting at once. Two rules the CALLER still owns: park
  the pointer at one end of the control and measure the other (or the
  sprite's own pixels answer the question), and make sure the hovered
  row is not the SELECTED one, since selection correctly outranks hover
  and hovering the current value measures nothing.
- **`gui_flow.py`** -- named, composable QMP click-flows on top of
  `qmp_test.py`'s `QMPSession` (`GuiFlow` class: `enter_gui()`,
  `open_app(name)`, `run_system_action(label)`, `screenshot_named()`),
  so a testing session doesn't hand-derive Start-menu row pixel math
  from scratch every time.

  **Rows are found BY LABEL, from the kernel's own geometry** -- there
  is no list of apps in the file to keep in sync, and there was one
  until 2026-08-20. `APP_ORDER` mirrored the WM's desktop entries and
  the menu's top edge was DERIVED from its length, so when `Crash Test`
  was added to `/usr/wm/applications/` and not to the list, two things broke
  at once: every later app's index, and the computed origin. The visible
  symptom was `open_app("System Settings")` opening Task Manager.

  **Pass your own `console=` if you already hold a `DebugConsole`.** Two
  of them are two connections to one serial socket and the guest's reply
  goes to whichever is reading -- which presented as "the Start menu did
  not open" on a menu that was demonstrably open. `open_start_menu()` is
  idempotent (the Start button TOGGLES) and recalibrates the pointer
  first, so it is safe as the first thing a script does.

  For most purposes prefer `DebugConsole.open_app(name)`, which sends
  `gui open <name>` and involves no menu, no pixels and nothing to
  drift. `GuiFlow.open_app()` is for a test that wants the real menu
  exercised rather than bypassed -- which is why it still exists.

  **THE NAME IS THE DESKTOP ENTRY'S, INCLUDING ITS CASE**: "Terminal",
  not "terminal". `gui open` answers a wrong name with `no app named
  "x". Known apps: ...` and returns normally, so a caller that ignored
  the reply got a desktop with no window and a failure several checks
  later, on something unrelated. `DebugConsole.open_app()` RAISES on
  that now, quoting the guest's own list -- one line instead of a
  debugging round.
- **`vm.py reboot`** -- reboots the guest and returns when the NEW boot
  answers, judged by its uptime going BACKWARDS, since the old boot
  answers the console for a moment after the command. Refuses a guest
  started without `--reboot`, whose QEMU would simply exit. It replaces
  the hand-rolled "exec reboot, then poll uptime" loops (four in one
  session). For the laptop, `remote.py reboot --wait` is the same thing.
- **`vm.py stop` ASKS THE GUEST TO SHUT DOWN, and only then signals
  QEMU** -- QMP `system_powerdown`, waited for, with SIGTERM as an
  unconditional fallback so stopping a VM can never itself hang.
  `--hard` skips straight to the signal, for a guest wedged badly enough
  not to act on ACPI.

  **It is not politeness, it is the gate's correctness.** Terminating
  QEMU is a power cut from the guest's side, and `tfs3` writes its
  allocation bitmap unjournaled and set-before-use, so a crash between
  marking a block allocated and committing the transaction that
  references it LEAKS that block -- deliberately, because the other
  ordering risks double-allocating it. `fsck` then reports the debris
  and the three `fs` KTESTs that assert `leaked == 0` go red. That is
  how `preflight.sh` came to fail, on the first run and on a freshly
  seeded disk, a suite it had dirtied itself: it boots the image in
  `boot_smoke_test.py` and then runs `ktest_run.py`.
  `boot_smoke_test.py` shuts down the same way, over a **unix** QMP
  socket -- no `port_guard` slot, so it cannot clash with another guest.
  See `docs/decisions.md`.
- **A `vm.py` guest has TWO serial ports**: COM1, the kernel log alone,
  on `.vm.N.log`; COM2, the debug console, on `.vm.N.serial` -- the name
  every tool already connects to. `--instance N` derives both. A tool
  that builds its own one-port QEMU line still works: the kernel keeps
  both on COM1 when it finds no second UART.
- **`vm.py --kdebug`** -- a THIRD port, COM3, as the unix socket
  `.vm[.N].kdb`, for the kernel's GDB stub. Inert unless the image's GRUB
  line carries `kdebug=ttyS2` (add it to a COPY with
  `install_grub.add_boot_word()`); then `gdb build/kernel.bin -ex
  "target remote .vm.N.kdb"`. It must be the third `-serial` to be COM3,
  which is why it is a flag here rather than something `QEMU_EXTRA`
  appends.
- **`vm.py --kdebug-net`** -- a SECOND e1000, on its own SLIRP netdev,
  for the debugger's network transport: host udp `127.0.0.1:51000+N`
  forwards to its port 50000. It comes after every other NIC on the
  command line because `kdebug=net` takes the LAST 82540EM and the OS's
  driver keeps the first. Inert without `kdebug=net,...` on the image.
- **`vm.py --serial-log PATH`** -- everything the kernel LOGS (COM1),
  from the first byte, whether or not anything reads the log socket. **A `start` that fails says WHICH failure now**: QEMU
  EXITED (with `-no-reboot`, the guest reset -- a crash) or still
  running (a hang). Both used to print "never reached the debug
  console", and the serial output of either went nowhere; this is what
  found the boot livelock in `clocksource_register()` that only a dozen
  guests booting at once could provoke.
- **`vm.py --machine <type>`** -- the QEMU CHIPSET, default i440fx
  because that is what every existing test was written against.
  `--machine q35` is the only way to reach an ACPI 2.0-era machine here:
  i440fx presents a revision-0 RSDP, an RSDT, a 116-byte FADT and NO
  reset register, while q35 presents a 244-byte revision-3 FADT with a
  real reset register at port 0xcf9 and an MCFG beside it -- so the two
  exercise different halves of `kernel/acpi/`. **q35 has no legacy IDE
  at all**, so it implies `--disk-kind ahci` rather than silently
  booting a machine with no disk, which would read as a kernel bug.
- **`vm.py --uefi`** -- OVMF instead of SeaBIOS, booting the disk's
  `EFI/BOOT/BOOTX64.EFI` (the Makefile's `UEFI=1`). Implies `--machine
  q35`, because PIIX IDE DMA never completes under OVMF (`docs/bugs.md`);
  the variable store is `.vm.N.ovmf_vars.fd`, one per slot. `--boot auto`
  asks `has_efi_loader()` instead of GRUB's boot sector.
- **`vm.py put <host-file> [guest-path]`** -- copies a file INTO the
  running guest, over TFTP, defaulting to `/tmp/<name>`. It closes the
  asymmetry that `remote.py` had put/get/sync for the bare-metal machine
  while the VM side had no file transfer at all: planting a file in a
  guest meant seeding a disk image from the host and rebooting, which is
  fine for a fixture decided before boot and useless for anything a test
  wants to put there mid-run.
  **The transfer is `remote.py`'s `do_put()`, not a second copy of it**
  -- the same TFTP client, already carrying the blksize/windowsize
  negotiation and the retries. What differs is only how the guest is
  reached: a QEMU hostfwd onto 127.0.0.1 instead of the laptop's
  address, on a port derived from `--instance` like every other
  per-slot resource, and added to EVERY launch so this works against
  any guest `vm.py` started rather than one somebody remembered to
  forward a port for.
  **It starts `tftpd` in the guest if it is not running**, because it is
  not a service in the default image -- `/etc/services.d` has none, the
  bare-metal laptop enables it and a QEMU guest does not. Spawned rather
  than enabled: a service would persist into the next boot and change
  what every other tool is testing.
  It needs networking, so `--net none` has no path for it.
- **`vm.py spawn <path> [args]`** -- spawns a guest program and prints
  the FILE it writes its report to, waiting until that file stops
  changing. It replaces a three-command dance that was hand-rolled four
  times in one session: `exec "spawn ..."`, sleep a guessed number of
  seconds, `exec "cat /tmp/....out"`.
  **Why a file rather than the console**: a SPAWNED program's output
  arrives while this harness is between commands, where it is dropped --
  so every spawned test here writes its verdict to `/tmp` and the
  harness reads that. Waiting on the artifact rather than on a sleep is
  this repo's own rule, and it is what makes this reliable rather than
  merely shorter.
  **Why spawn rather than `run`**: `run` waits for the program and
  returns its stdout, so a spawned program is the form for one that
  must outlive the command and be judged from a file it writes.
  It STRIPS the kernel's own log lines before deciding whether the file
  has appeared -- without that, `elf_run:`/`syscall:` noise around every
  `cat` looks like stable output and the first version returned three
  kernel lines for a benchmark that had not finished running.
- **`vm.started_ok(output)`** -- the one line every caller uses to decide
  whether a `vm.py ... start` worked, and it exists because the obvious
  test is wrong in a way that looks right. `vm.py` answers a slot that is
  already taken with `vm: already running`, and **"al-ready" contains
  "ready"** -- so `"ready" in output` passes on the one output that means
  the opposite, after which the tool talks to a serial socket that is not
  there and fails several steps later, pointing at whatever it was doing
  by then. Eight tools here carried that test; there is one place to be
  right now. Import it as `import vm as vm_mod` (the module runs nothing
  at import; its CLI is behind `__main__`).

  The sibling hazard, which no helper can fix: **a stopped guest keeps
  its QMP port for about a minute**, because a `QMPSession` leaves the
  connection in TIME_WAIT on the server side and `port_guard` -- rightly
  -- refuses a start against a held port. A tool that boots more than
  once should take a **fresh slot per boot**
  (`port_guard.find_free_instance()`) rather than pinning one and waiting
  it out.
- **`shell_flow.py`** -- the same idea as `gui_flow.py`, for the
  PHYSICAL (pre-`gui`) shell instead of the GUI (though
  `type_command()` alone drives anything keystrokes reach, the GUI
  Terminal included): `ShellFlow.
  run_command(cmd, subdir=...)` types a full command -- including
  spaces/hyphens/underscores/a few other punctuation chars
  `qmp_test.py`'s `send_text()` can't handle on its own, and
  **UPPERCASE, which `send_text()` silently DROPS** (typing `PATH`
  through it produces nothing at all; `ShellFlow` sends shift+key) --
  presses
  Enter, waits, and screenshots, instead of hand-interleaving
  `send_text()`/`send_key('spc')`/`combo(['shift','minus'])` calls
  character by character every session (a real mistake -- a dropped
  space, a hyphen typed where an underscore was needed -- happened
  twice in the session this was built in). Returns a screenshot path,
  not parsed text: this kernel's console auto-selects a framebuffer
  backend (glyphs drawn as pixels) whenever GRUB provides one, which
  is the normal case for this project's QEMU launch flags, so there's
  no legacy-VGA-text-buffer memory-read shortcut to plain text the way
  there might be on a kernel that only ever used 0xB8000 -- see the
  module's own docstring.
- **`serial_console.py`** -- boot a guest with COM1 as a SOCKET and drive
  it as text in / text out. Not a test: the shared channel under
  `ktest_run.py` and `faulttest_run.py`, which had written half of it
  each. A serial console is the standard answer to "the graphical
  session owns the keyboard", the same reason Linux developers drive a
  guest with `console=ttyS0` -- it does not care who holds the screen.
  Two things it adds over a raw socket. It launches with **`wait=on`**,
  so QEMU blocks until the harness connects and the transcript starts at
  the kernel's first byte (with `nowait` the banner a caller waits for
  has already gone, which hung every run until the timeout) -- which is
  also why it cannot use `qmp_test.py`'s `launch_qemu_cmd()`, whose
  `-daemonize` never returns for a QEMU blocked on `accept()`. And it
  turns a dead wire into a REPORT rather than a traceback:
  `diagnostics()` gives elapsed time, bytes received, whether QEMU is
  alive and with what code, the tail of QEMU's own log, and the last
  thing the guest said. "0 bytes, QEMU exited 1, could not bind" and
  "40 KB, QEMU healthy, guest never reached the banner" are different
  bugs that used to print the same sentence. **Sample `diagnostics()`
  while the guest is still up** -- after teardown every failure reports
  "QEMU exited with code 0", which is the harness's own kill.
  **`send()` waits for the previous command's prompt** (bounded): the
  console is a terminal, so a line typed while a command runs is that
  command's INPUT -- `ktest` sent under a running `sh fsck repair`
  arrived as `nt`. A string printed during a command is not the
  boundary; the prompt is. `prompt_wait=0` types at a running command
  on purpose.
- **`port_guard.py`** -- refuses to start a guest on a QMP or VNC port
  another guest already holds, picks a free slot for callers that ask,
  and is where a tool that DRIVES a guest gets its `--instance N` flag
  (`add_instance_args()` + `resolve_instance()`): one number derives
  both the QMP port and the serial socket, so the two cannot name
  different guests -- which `--qmp-port 4447` with the default socket
  silently did across 47 tools. The legacy pair still works; a lone one
  derives its partner from its slot and prints which; a mix is refused. It exists because **a QMP port clash does not fail as a port
  clash**: everything here defaults to 4445, a second launch silently
  fights the first, and the error lands minutes later as a
  `BrokenPipeError`/`ConnectionResetError` against whichever tool was
  mid-command -- accusing whichever one was unlucky, never the one that
  caused it. Three "failures" in one session (`scrollbar`, then `uapp`
  and `forcequit`) were all this. Wired into the same two places
  `iso_guard.py` is, which are the only two places anything here starts
  a guest: `launch_qemu_cmd()` and `vm.py`'s start path. Two things to
  know. **The check is a BIND, not a connect** -- QEMU's monitor accepts
  ONE client, so a second connect can hang rather than refuse, while a
  bind asks exactly the question QEMU is about to ask. And
  **`find_free_instance()` is NOT a lock**: two callers picking "the
  lowest free slot" in the same instant get the same answer, so it
  narrows the window and `assert_ports_free()` at the launch catches the
  residue. `SAUNAOS_ALLOW_PORT_CLASH=1` bypasses it deliberately.
- **`pixel_probe.py`** -- reads exact pixel values out of screenshots,
  and tabulates the same points across several (`--compare a.png b.png
  --at 85,100 --at 215,100`), flagging which moved and which didn't.
  This is how `docs/gui-guidelines.md` says to verify a GUI change, and
  the rule exists because a hover state that shifted the background by
  TWO units out of 255 looked entirely plausible in a PNG. Always
  include a point that should NOT change -- half the assertion is the
  neighbour staying put. `--box N` averages a square, for anti-aliased
  edges where a single pixel is a coin toss.
- **`panic_resolve.py`** -- paste a panic (from the log, or typed off a
  photograph) and it names every address in it, RIP and stack scan
  alike, annotating the original lines. It finds the relocation delta
  from the text itself -- from the `kernel relocated +0x...` line if the
  text has one, and **otherwise by DERIVING it from the faulting symbol**:
  the kernel resolves that symbol itself, so `in foo+0xef` beside
  `RIP=0x...` is an nm lookup and a subtraction away from the delta. That
  second path is the one a bare-metal panic usually needs, because a
  panic photographed off a screen has long since scrolled the relocation
  line away -- and doing it by hand is an nm lookup at exactly the moment
  nobody wants one. It PRINTS the arithmetic, so a reader can check it
  rather than trust it. **It checks the BUILD ID first and refuses to
  be quietly wrong**: resolving against a different build gives
  confident, plausible, wrong names -- verified, the address that was
  `try_merge_next` in one report is `rtc_read_local` a few commits
  later. `--elf` points it at a userland ELF for a ring-3 crash, and
  `--delta 0` goes with it -- userland is not relocated, so a live
  ring-3 RIP (from `kstack slots`, say) has no delta in the text for it
  to find. **Reach for this instead of `nm`**: a hand-rolled
  nearest-symbol-below over `nm` output named `sys_thread_detach` for an
  address that DWARF resolves to `sys_yield` at `rt/sys.c:217`, and an
  hour went into reconciling a function the program never calls.
  **`--crash <file>` reads a ring-3 crash report** from `/var/crash`
  (`kernel/proc/crash_report.c`): prints its text header, finds the ELF
  from the report's `program:` line (a Makefile seed rename such as
  `uterm` is looked up, never guessed), and names RIP and every word on
  the saved stack that lands in an executable segment. Get the file off
  a machine with `remote.py get`, or out of a disk image with
  `tfs3_writer.py read`.
- **`qmp_test.py`'s `QMPSession.hmp(cmd)`** -- run a QEMU MONITOR
  command and get its text. **The one oracle the guest cannot fake**:
  every other probe here asks the guest about itself, in code that is
  usually the code under suspicion. `info registers` gives CPL, RIP and
  `HLT` (halted with interrupts enabled means an interrupt that is not
  coming, and CPL says which ring is really executing); `info pic` gives
  the 8259s' `imr`/`isr`/`irr`, where an unacked IRQ is visible and
  nowhere else; `info pci` says what is on the bus before a driver has
  an opinion. Together the first two eliminated the entire interrupt
  layer of the compositor-stall investigation in one command.
  **Sample a distribution, never one reading** -- it stops the vCPU, so
  a mostly-idle guest reads as halted every single time.
- **`remote_gui.py`** -- THE GUI TOOLS, POINTED AT THE BARE-METAL
  MACHINE. Not a runner: two objects serving the interfaces a tool
  already drives -- `RemoteScreen` for `QMPSession` (screenshots through
  the machine's own `screenshot` program, fetched over TFTP; input
  through `guictl`) and `RemoteConsole` for `DebugConsole` (the same
  `gui` vocabulary over telnet, subclassed so every helper from
  `menu_row()` to `hover_frames()` comes across and cannot drift).
  `SAUNAOS_REMOTE_HOST=<ip>` is what redirects a tool, and it is read in
  the two constructors rather than in a flag each tool has to grow --
  fifty-odd tools, fifty chances to forget. Three things it does NOT
  pretend: a call needing QEMU (`hmp()`, a held button, relative motion)
  raises `RemoteUnsupported` naming the reason; a settle compares the
  caller's box or everything above the taskbar, because a whole frame is
  never identical twice with a clock in it; and the kernel log comes
  from `dmesg` as a delta, since telnet is not the wire the klog is on.
  ONE session per process, given back at exit -- `inetd` serves four.
  Driven by `gui_regress.py --host`; `docs/testing.md` has the worked
  commands.
- **`iso_guard.py`** -- refuses to boot a stale `saunaos.iso`, called
  from `vm.py` and `qmp_test.py`'s `launch_qemu_cmd()`. `make all`
  without `make iso`, or a `make iso` that FAILED, otherwise leaves the
  whole suite testing the previous build and reporting a clean PASS --
  which is the worst possible direction and has cost time in many
  sessions. Each source tree is checked against the artifact it feeds
  (`userland/` -> `build/userland`, not `kernel.bin`), and the seeding
  step is witnessed by `build/.seeded` rather than `disk.img`'s mtime,
  because seeding is content-hash based and a byte-identical rebuild
  correctly rewrites nothing. `SAUNAOS_ALLOW_STALE_ISO=1` bypasses it, for
  deliberately booting an older image -- e.g. building an earlier commit
  to prove a failure predates your work. A w16 SAMPLE (`userland/utils/w16/
  samples/*.asm`) lands in `seed/sync/`, not `build/userland`, so the
  `seed` step touches `build/userland/.w16-samples` after building them: an
  edited sample otherwise read as a build that never ran.

  **It guards the STAGING tree and single staged FILES too, which is the
  same trap one layer down.** `make all` writes `build/` and stops;
  `seed/sync/` is populated by the `seed` step `make iso` runs. So
  `check_staging_fresh()` refuses a `remote.py flash` from stale
  staging, and `check_staged_file()` refuses a `remote.py put` that
  names one `seed/sync/` file older than its `build/` counterpart --
  naming the `build/` path to send instead. The single-file case was
  added 2026-09-22 after a rebuilt sound plugin was pushed twice from
  staging while the fix sat in `build/`: the second push reproduced the
  first push's symptom exactly, which reads as the fix not working
  rather than as the fix never having been sent. Same
  `SAUNAOS_ALLOW_STALE_ISO=1` bypass, because it is the same mistake.

  It also WARNS (never refuses) when a COPY of `disk.img` passed with
  `--disk` is older than the last seed. Testing against a copy is the
  documented way to dodge QEMU's write lock and to stop `make iso`
  re-seeding an image underneath a running VM -- but `make iso` re-seeds
  the real `disk.img` with the newly built `/bin` binaries AND installs
  the new kernel on it, so a copy taken before a rebuild is an entire
  earlier build: it boots the previous kernel too, with nothing
  mismatched to give it away. (Before the disk carried a kernel this was
  the narrower "new kernel from the ISO against the old userland"; the
  warning names whichever case applies.) Either way it reads exactly
  like a bug in the app. Measured 2026-08-19, in the
  worst possible place: a POSITIVE CONTROL for `/bin/ls`'s truncation
  message, where the guest ran the previous `ls` and the message did not
  appear -- a control that fails reads as "the feature is broken", not
  "the fixture is stale". A warning rather than a refusal because a copy
  is often deliberately old (a fixture staged by `tfs3_writer.py`, an
  image kept for a reproduction); it names the re-copy command.

  **It asks the compiler which artifact a source feeds; it does not
  guess from the directory.** Pairing by tree is a guess, and it was
  wrong for `kernel/include/api/build_date.h` -- which sits under
  `kernel/` and is included only by `userland/apps/system/toywm/desktop.c`, so it never
  rebuilds the kernel. `gen_version.sh` rewrites that file whenever the
  DAY changes, so this refused a perfectly current image on the first
  build after every midnight and then self-healed as soon as anything
  touched the kernel, which is why it went unnoticed. The `.d` files
  `-MMD` already writes carry the real answer (and `check_deps.py`
  proves that tracking is live), so the mapping is read from them. A
  source no `.d` mentions keeps the directory pairing, which is the
  conservative fallback -- so this can only ever make the guard quieter
  about a file the compiler positively placed elsewhere, never about a
  new one.
- **`tree_lock.py`** -- **THE TEST LOCK: nothing rebuilds or edits the
  tree while a test reads it.** A shared `flock` on `.test-lock` at the
  repo root (`make clean` deletes `build/`), held until the holder
  exits, so a killed test leaves nothing stale; `.test-lock.d/<pid>`
  names each holder for the message. `hold()` is called by
  `iso_guard.py`'s boot and staging checks, `harness.Results`,
  `gui_regress.py` and `ondemand_sweep.py`, and `preflight.sh` takes it
  in shell for its whole run. A holder exports `SAUNAOS_TEST_LOCK_HELD`,
  which lets its OWN `make` through. Two things refuse while it is held:
  the Makefile (top-level, not `make -n` or `make help`), and `tree_lock.py
  hook`, the PreToolUse hook in `.claude/settings.json`, which denies an
  Edit/Write outside `docs/`, `tools/`, `.claude/` and `*.md`, a `make`
  as a command, a tree-moving `git` (stash, checkout, reset, pull,
  merge, rebase and the like), and a shell command that WRITES a source
  file -- `sed -i`/`perl -i`, a `>`/`>>` or `tee` target, a heredoc
  script's `open(PATH, 'w')` -- judged like an Edit (scratch files,
  `build/`, docs and tools pass). Commands are read with heredoc bodies
  and quoted strings removed, so a script that only WRITES the words
  `git stash` is not refused. `tools/hostchecks/tree_lock_hostcheck.py`
  holds the lock and asserts 19 verdicts both ways. **Each checkout has its own lock**: the hook
  checks the one the call touches (the edited file's checkout; for a
  command, the cwd's or a leading `cd DIR &&`'s), so a git worktree's
  `make` is not refused because of a test in the main tree, and its own
  tests guard it. `python3 tools/lib/tree_lock.py` says who holds it
  (exit 1 if anyone does); `SAUNAOS_IGNORE_TEST_LOCK=1` bypasses both. It
  exists because the written rule ("do not rebuild while a test is
  running", in the skill's testing lessons) was broken twice in one
  session.
- **`mkpart_test.py`** -- writes a synthetic legacy MBR or GPT partition
  table onto a disk image, for testing `kernel/drivers/partition.c`'s
  parser (`parttable` shell command). Its mount-preserving guarantee
  was designed for (and verified against) TFS2 images; a TFS3 image
  deliberately leaves its first 32 KiB untouched for exactly this, so
  coexistence is by-design there, but the tool hasn't been re-verified
  against one -- check before trusting it on TFS3. TFS2-mount-preserving: patches
  only the partition-table byte ranges TFS2 itself never touches
  (reads the existing LBA 0 sector first rather than blindly
  overwriting it), so the real filesystem underneath still mounts
  normally afterward instead of `tfs_init()` seeing foreign magic and
  auto-reformatting. `--mbr`/`--gpt`; see its own docstring for the
  CRC32/GUID-encoding details and `docs/decisions.md` for why GPT
  verification needed a host-compiled unit test instead of a live
  boot (TFS2's own journal header collides with the GPT header's LBA).

  **`--layout SIZE[:KIND][,...]` writes a REAL, usable table instead** --
  partitions laid end to end from LBA 2048 (1 MiB alignment), sized by
  a `K`/`M`/`G` suffix or bare sectors, with exactly one allowed to be
  `rest`. The optional `:KIND` is the partition TYPE -- `data` (the
  default), `bios` (a BIOS boot partition, where GRUB's `core.img` is
  embedded) or `esp` (an EFI System Partition, the FAT32 `/boot`). The
  kernel reads those types too, and refuses to mount or format either
  of the last two. **Its GUIDs (and an MBR's disk signature) are
  RANDOM**, as the kernel's `mkpart` writes them: a partition's GUID is
  the PARTUUID grub.cfg passes as `bootpart=`, so two images must not
  share one. `regenerate_guids(path)` is `sgdisk -G` for a COPY, which
  otherwise carries the original's. `--gpt --layout` also writes the BACKUP header and entry
  array in the last 33 sectors, which the synthetic mode does not: a
  partition editor on another system reads the backup to cross-check
  the primary and "repairs" a disk that has none. This is what
  `seed_disk.py --partition` calls, and its per-partition GUIDs are
  derived from the index rather than random, so `make iso` stays
  reproducible (the KERNEL randomises them -- a disk written on a
  running machine has no such requirement).

  **`--print-volume` prints `<base_lba> <sectors>`** for the image's
  filesystem volume -- the whole image when there is no table. It is the
  shell-callable form of `volume_of()`, which is the one place the host
  answers "where does the volume start"; every tool that reaches into a
  disk image asks it rather than hardcoding 2048. **It is not "partition
  1" any more**: a bootable image's partition 1 holds GRUB, so
  `volume_of()` finds the partition carrying a TFS3 superblock and falls
  back to the first one that is not the firmware's.

  Note the GPT-verification caveat above is now HISTORY rather than a
  live limitation: TFS3 is the default and reserves volume blocks 0-7,
  so `kernel/drivers/partition_test.c` round-trips a real GPT through
  the writer and the parser inside a running kernel.
- **`private_tmp.py`** -- a private `TMPDIR` per test tool, deleted
  when the tool returns: systemd's `PrivateTmp=`, for a runner.
  `gui_regress.py` and `ondemand_sweep.py` run every tool inside one.
  It exists because tools copy disk images and keep screenshot
  directories under `tempfile` and mostly never remove them. `/tmp` is
  a RAM-backed tmpfs here, and the leftovers of many runs filled
  it and ended a sweep at tool 83 of 91 with ENOSPC. **A tool's OUTPUT
  is what a runner keeps**; anything meant to outlive the run goes to a
  path the caller names (`--logs`, `--shot`). A tool run BY HAND still
  leaves its temp files behind; `QMPSession.screenshot()` now deletes
  the `.ppm` it derives itself, and the hardcoded `/tmp` images in
  `osk_test`, `msi_test` and `damage_hunt` follow `TMPDIR`.
- **`test_history.py`** -- a LOCAL record of every test run, kept in
  `~/.local/share/saunaos/test-history/`. It is never in the repo and
  never pushed (`SAUNAOS_TEST_HISTORY=DIR` moves it, `=off` stops it).
  `preflight.sh`, `ktest_run.py`, `gui_regress.py` (a row per tool and
  one for the run) and every tool on `harness.Results` append a line as
  they finish. Each line holds the commit and whether the tree was
  dirty, the verdict, the checks that failed, the duration, the
  checkout and its filesystem, the host's kernel and QEMU and its
  load, and a parent id tying a child run to its preflight or
  gui_regress. Each run's whole log is gzipped under `logs/`, pruned
  oldest first past `SAUNAOS_TEST_HISTORY_MAX_MB` (1024); `runs.jsonl`
  is kept for good.

      python3 tools/lib/test_history.py                     # the last 20 runs
      python3 tools/lib/test_history.py "sched_test.c:114"  # first/last seen, by commit, hosts
      python3 tools/lib/test_history.py --tool ktest_run --last 50
      python3 tools/lib/test_history.py --show <id>         # one run's record and log

  When a run fails, it prints a `history:` line per failed check: how
  many of the tool's last 20 runs failed it, and when the tool last
  passed. It exists because a failure on 2026-10-24 took hours to place:
  it was red at HEAD and green at the last published build, the host
  kernel had changed that morning, and nothing recorded which runs had
  passed where. **A check is matched by its NAME** (the text before
  ` (file:line)`), because line numbers move between commits.
  Recording never fails a run. gui_regress.py records each tool's row
  itself and sets `SAUNAOS_TEST_HISTORY_BY_PARENT` so harness.Results
  does not record it a second time. `--selftest` runs in preflight.
