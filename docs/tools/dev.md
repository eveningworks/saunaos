# `tools/dev/` -- Development tools

One entry per tool in `tools/dev/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`acpi_dump.py`** — pulls an ACPI table out of a running guest (via
  `acpi --dump`) in RANGES and reassembles it, then checks the length the
  table declares and the ACPI checksum, which every table's bytes sum to
  zero for. The ranges are not an optimisation: asking for a whole
  8605-byte DSDT in one command returned 8557 bytes with the missing
  lines SCATTERED rather than truncated, because that much hex outruns
  the debug console — the failure mode that looks like success. The
  checksum is what turns "I got some hex" into "I got the table"; a
  single flipped byte breaks it. `--print` emits a C array ready to paste
  into a KTEST, which is the point: `docs/aml-design.md` needs a real
  DSDT, and a hand-written one would only ever agree with the parser
  written beside it.
- **`aml_walk.py`** -- a DSDT dump on the HOST, classified: how its
  `_PRT` is written (a static package, a method returning one, a method
  choosing between two by a `PIC` flag, a method returning a reference),
  every entry decoded, and whether each link device's `_CRS` is a
  constant or a method that reads hardware. It is the measurement the
  kernel's `_PRT` reader was designed from, and the oracle for
  `kernel/acpi/aml_data.c` -- the two share no code. No AML disassembler
  is installed on this host; this is the one that exists. Refuses a dump
  whose ACPI checksum fails (a console dump loses bytes in transit; for
  a laptop write it to a file and `remote.py get` it). `--tree` prints
  the namespace. `--prw` lists every `_PRW` with its GPE and the deepest
  sleep state it wakes from, and how many can wake the machine from S5:
  the three shapes real firmware writes it in (a Name, `Return
  (Package)`, AMI's `Return (GPRW (gpe, state))`) are decoded, anything
  else is reported unread. On the ASUS all 17 were Methods and none
  woke from S5.
- **`backup_repo.sh`** -- a complete, verifiable backup of the GitHub
  repo: mirror clone, a bundle of LOCAL refs (catching branches never
  pushed), **every release asset**, and the repo/PR/issue metadata.
  Run it before anything that changes the repo's identity or history --
  a transfer, a visibility change, an account rename, a history
  rewrite. The reason it exists: `git clone --mirror` is NOT a backup
  here, because release assets (~130 MB of ISOs and disk images) live
  only on GitHub and cannot be recovered from a clone. It verifies the
  bundle and checksums any release shipping a `SHA256SUMS`, but does
  NOT run the restore test (clone the mirror, `make all`) -- do that by
  hand before relying on it, since matching hashes prove the bytes
  survived and only a build proves it restores to a working project.
  See `docs/decisions.md`.
- **`mutate.py`** -- a positive control done safely: `--edit FILE OLD
  NEW` (repeatable; OLD must occur exactly once), `--` and a test
  command. It backs the originals up to `build/.mutate/` before
  touching anything (`--recover` after a SIGKILL), builds, runs,
  restores in a `finally` and checks the bytes, then REBUILDS so the
  artifacts match the source again -- the step a hand-done control
  forgets. It shows the run's last lines AND every line with FAIL in it,
  so which check went red is on screen -- a control that fired on the
  wrong check proves nothing. Exit 0 = the control fired (the test sees the break), 1 = it
  did not, 2 = the mutated tree did not build. `--build` (default `make
  iso`) for a host-only check.
- **`samba_server.py`** -- a throwaway Samba in Docker for the SMB tools
  below: shares `media` (a user, `reader`), `public` (guest) and `sealed`
  (encryption required) with deterministic files -- a 3 MiB binary, a
  1500-entry folder, a name past Latin-1 and one inside it. Published on
  127.0.0.1 only; a guest reaches it as `10.0.2.2:<port>`. `--serve
  [--max SMB2_10] [--signing auto]` runs one by hand. The image
  (`saunaos-samba:1`) is BUILT ONCE with `--build`, which fetches samba
  from deb.debian.org; nothing after that leaves the machine. A fixture,
  not a test, so no runner names it.
- **`remote.py`** -- drive a SaunaOS machine over the NETWORK: run
  commands, push and pull files, or open an interactive session.

      python3 tools/dev/remote.py --host <machine-ip> exec "lsusb" "dmesg"
      python3 tools/dev/remote.py --host <machine-ip> put build/userland/bin/ls /bin/ls
      python3 tools/dev/remote.py --host <machine-ip> get /tmp/crash.log ./crash.log
      python3 tools/dev/remote.py --host <machine-ip> screenshot shot.png
      python3 tools/dev/remote.py --host <machine-ip> shell      # Ctrl-] quits
      python3 tools/dev/remote.py --host <machine-ip> sync seed/sync/bin /bin
      python3 tools/dev/remote.py --host <machine-ip> sync --delete --dry-run seed/sync/bin /bin
      python3 tools/dev/remote.py --host <machine-ip> --timeout 60 flash build/kernel.bin
      python3 tools/dev/remote.py --host <machine-ip> reboot --list
      python3 tools/dev/remote.py --host <machine-ip> reboot --entry "SaunaOS (no kernel debugger)" --wait 120
      python3 tools/dev/remote.py --host <machine-ip> update --from http://<dev-host-ip>:8080/dev

  **`update` is the ship-to-the-laptop round in one bounded command**:
  System Update from the server, its output written to a FILE on the
  machine (a kernel before the pty write clamp parked any one terminal
  write over 4 KB for good, and `update` prints its release notes in
  one), the log's tail shown, then the reboot and the wait until telnet
  answers, ending on the new version's banner. A run that stalls is cut
  off at `--timeout` with the tail saying which step it stopped at.
  `--no-reboot` stages only. A 503 from the server is a stale build:
  `make iso`, not just `make all`.

  **`reboot --entry` is one boot**: GRUB clears the choice before it
  boots it (`docs/commands/reboot.md`), so a session can take a laptop
  into its rescue or no-debugger entry and a plain `reboot` brings it
  back. The dropped session is the success reply; `--wait` polls, bounded,
  until telnet answers again.

  **This is the tool for the BARE-METAL laptop**, which `vm.py` cannot
  reach: `vm.py` drives a QEMU guest through its serial debug console,
  and the laptop has no serial console attached. It is where half of
  `docs/bugs.md` lives -- a USB mouse that will not bind, a power button
  that needs two presses, a garbled product string -- and until this
  existed, investigating any of them meant sitting at the machine. The
  guest side is `/bin/telnetd` and `/bin/tftpd`, both shipped DISABLED
  (`service enable telnetd`).

  **`screenshot` IS THE ONLY WAY TO SEE THAT MACHINE'S SCREEN FROM
  HERE.** `vm.py` can ask QMP for a screendump of a QEMU guest; the
  laptop has no such channel and nobody is sitting in front of it. So
  the capture is taken BY THE MACHINE -- `/bin/screenshot`, an ordinary
  program with no window that asks the compositor for pixels -- and
  fetched over the same TFTP link. `--window`, `--region X,Y,W,H`,
  `--pointer` and `--delay` pass straight through.

  **PNG by default**, because the point is to open it on THIS machine
  and nothing here decodes QOI; `.qoi` in the filename (or `--format`)
  gets the other one. The guest writes the file, so there is no
  host-side conversion step to go wrong. It lands in `/var/tmp` on the
  machine, not `/tmp`, which is a ramfs mount -- a screenshot is worth
  keeping if the fetch fails.

  It is also the one path that exercises the HARDWARE cursor plane:
  `--pointer` there draws a sprite the back buffer does not contain,
  which no emulated setup here reproduces (`guictl state` reports
  `hwcursor: true` on the laptop and `false` under QEMU).

  **`--timeout` MUST OUTLAST THE COMMAND, and the default is 15s.** A
  command that simply runs longer than that raises with the output it
  has so far -- which reads exactly like the command's RESULT rather
  than a truncated capture. A `kbd --timeout 25` recording was read as
  "no keypresses arrived" that way, which is the wrong answer to a
  question about a dead keyboard. Anything with its own duration
  (`kbd`, a sleep, a long `sum`) needs `--timeout` set above it; the
  error message says so now.

  **`flash` REPLACES THE KERNEL ON THE MACHINE'S OWN BOOT PARTITION**,
  which is four commands with one irreversible step in the middle when
  done by hand. It remounts the read-only `/boot` writable, rotates the
  RUNNING kernel to `/boot/kernel.old` so the "previous kernel"
  entry in `grub.cfg` is known-good, writes the new one, and reads a
  sha256 back off the partition, and then **REBOOTS -- which is the
  DEFAULT**, and only ever after that verify succeeds. `--no-reboot`
  leaves the machine up.

  **The default is that way round because NOT rebooting is the
  dangerous state.** A verified kernel that has not been booted leaves
  the machine running the OLD one against the NEW `/lib` -- the exact
  mismatch the rescue session below exists to survive, and the one that
  makes `telnetd` accept a connection and close it. That state used to
  be what you got by FORGETTING a flag, and every session forgot it.
  `--reboot` is still accepted and now says nothing.

  **IT HOLDS ONE SESSION FOR THE WHOLE FLASH, opened before the first
  write.** The comparison each tree needs is a shell -- it lists and
  hashes the remote tree -- and a flash replaces the programs that shell
  is made of. The session used to be opened just before `/lib`, on the
  theory that `/lib` was the only dangerous tree; `/bin` breaks a shell
  SOONER, because its new binaries are linked against libraries that
  have not been sent yet. So the sync opened a fresh session per tree,
  got `connection closed` on the one after `/bin`, and gave up having
  written no kernel -- leaving `/bin` new, `/lib` old and the machine
  reachable only over TFTP. Both laptops ended up there on 2026-09-07.
  A session opened before any write survives all of them and reboots at
  the end.

  **It SYNCS /bin, /lib, /tests and /usr FIRST**, because a kernel is
  half a build: an ABI struct that changes size moves fields under every
  binary compiled against the old one, and the machine then boots
  perfectly and cannot be given an address. `--kernel-only` skips it.

  **AND IT HOLDS A SESSION OPEN ACROSS THE `/lib` SYNC, WHICH IS WHAT
  LETS IT REBOOT ITSELF.** Replacing a shared library under a machine
  still running the OLD kernel kills every LATER spawn the moment the
  two disagree about a struct: `telnetd` accepts a connection and its
  shell dies, so no fresh session can be made and nothing can reboot the
  machine. That stranded both test laptops on 2026-09-07 and needed a
  power button on each. A session opened BEFORE that write is already
  running and survives it, and **`/bin/reboot` is statically linked**
  (the Makefile's third exception, beside `init` and `toywm`) so the one
  thing that session still has to spawn does not go through `/lib`.
  Neither half works alone -- a static `reboot` with no live shell to
  run it from is just as unreachable. Failing to hold the session is
  not fatal: the flash still completes and ends at the old power-button
  instruction.

  **MEASURED, not reasoned.** The pair shipped (`de533a5d`) with the
  rescue path untested, because a build whose `.so` files are unchanged
  never reaches it. A later flash carrying a real `libc.so` change
  (`d5489356`) did: the fresh session died after the `/lib` sync, the
  kernel was verified over TFTP instead, and the held session rebooted
  the machine -- which came back in ~12s with no power button pressed.
  That run is what the two paragraphs above describe; before it they
  were an argument.

  **NEVER PIPE A FLASH.** The exit status of `remote.py flash | tail` is
  TAIL'S, so a flash that FAILED reports success. That is not a
  hypothetical: on 2026-09-07 a flash piped through `tail` ended after
  the `/bin` sync with `sent 97 file(s)` as its last line and exit 0,
  and was reported as done. It had not written the kernel. The same
  flash run un-piped a few hours later printed `FAILED syncing /tests`
  and returned 1 -- the same failure, finally visible.

  Redirect (`> log 2>&1`) rather than pipe: that preserves the status.
  **stdout is LINE-BUFFERED now** (2026-09-27): block-buffered, a flash
  that hung and was killed lost every progress line and kept only its
  stderr, so where it hung went unrecorded.

  **NO WAIT IN IT IS UNBOUNDED, and `tools/tests/gate/remote_hang_test.py` holds it
  to that.** Three were, all found after a flash blocked for 25 minutes
  and stranded the Lenovo: a TFTP `put` answered only by a STALE ACK
  resent its window forever (any reply reset the retry count, and a
  zero-block "advance" reset the stall), `get` did the same on a
  repeated block, and `read_until_line` checked its deadline only when a
  `recv` timed out -- so a peer that never stopped talking never let it.
  A transfer now gives up when nothing has MOVED it for `STALL_ROUNDS`
  timeouts, silence or chatter alike. **A `put` resends its window every
  `RESEND_S` (2 s) without progress, stale ACKs or not** -- paced by the
  clock, so it cannot amplify -- and that must stay under `tftpd`'s 10 s
  give-up: waiting out `--timeout` landed the resend on a server that had
  left, whose request port answered `bad path`. Replies not from the
  transfer's peer are ignored (RFC 1350's TID rule). **And the chmod pass uses the
  flash's HELD session**: it opened a fresh one, which is exactly what
  dies once `/bin` is new and `/lib` old -- `could not set execute bits
  (connection closed)` -- leaving every mode unset. Which of these hung
  that flash is NOT established; the lost stdout is why. The docstring's
  "do not put a wall-clock timeout around this" stands: a flash killed
  mid-sync is the half-replaced machine, so the bound is per step.

  **What that failure actually was** is the `/bin`-before-`/lib` hazard
  now handled by the single held session (above) -- the first diagnosis
  here blamed a `timeout` wrapper, which was wrong, and the wrong story
  survived one commit.

  **THE RECOVERY IS TFTP, and it needs no shell.** (Still worth knowing
  even with the session fix: a flash interrupted any other way leaves
  the same state.) TFTP is a separate
  service and keeps answering when `telnetd` cannot spawn:

      python3 tools/dev/remote.py --host H get /boot/kernel.bin /tmp/k
      sha256sum /tmp/k build/kernel.bin     # do they match?
      python3 tools/dev/remote.py --host H put build/kernel.bin /boot/kernel.bin
      python3 tools/dev/remote.py --host H put seed/sync/lib/libc.so /lib/libc.so   # ...and each of the rest

  Read every file back with `get` and compare hashes -- that is the only
  confirmation available with no shell to ask. The machine still needs a
  POWER CYCLE afterwards, because rebooting it is the one thing TFTP
  cannot do and the held session died with the interrupted run.

  **`/etc` IS SYNCED, but NEW FILES ONLY** (`USERLAND_TREES`), so a new
  service descriptor does arrive while the machine's own configuration
  -- which services are enabled, its address -- is never overwritten.
  **A FLASH ADDS AND REPLACES; IT DELETES ONLY WITH `--prune`.** The
  sync sends what the staging tree has and leaves everything else
  alone, so a file REMOVED from the tree stays on a machine that was
  flashed while it existed -- with no error and nothing saying so (a
  deleted `/bin/lswin` kept answering, wrongly). `flash --prune`
  deletes what the build no longer has from `/bin` and `/tests` ONLY:
  `/etc` and `/home` are the machine's own, `/usr/share/doom` holds
  WADs it downloaded, and `/lib` may have a `.so` mapped. Anywhere
  else, `sync --delete` (rsync's, opt-in for the same reason; preview
  it with `--dry-run`) or `rm` by hand -- a settings descriptor removed
  from `data/etc/settings.d` still needs the `rm`.

  **`/etc/settings.d` IS THE EXCEPTION and is overwritten**, because
  those files are not that machine's configuration: they are shipped
  metadata declaring what each setting IS, and a stale copy with no
  `Type=` line declares nothing -- every desktop setting would vanish
  from System Settings on a flashed machine while the newly added ones
  appeared. It is its own tree, ahead of `etc`, which is also how the
  disk image has always treated it (`seed/sync`, content-hash synced).
  What NEW FILES ONLY means for an INTERRUPTED flash is worth knowing,
  because it is how this was misdiagnosed once: the trees go `bin`,
  `tests`, `usr`, `etc/settings.d`, `etc`, `lib`, so a run cut short
  after `/bin` leaves NEITHER the new `/etc` file NOR the new `/lib`. The missing service then looks like a
  policy ("flash must not touch /etc") rather than like the half-copy it
  is. If you are repairing one by hand, push the descriptor too:
  `put data/etc/services.d/<name> /etc/services.d/<name>`.

  **It REFUSES while `grub.cfg` says `set timeout=0`.** That is the
  whole safety argument: the rescue entry exists on every installed
  machine, and with no timeout GRUB draws no menu, so it cannot be
  picked and a bad kernel needs a USB stick. Fixing that is a one-line
  edit to `/boot/grub/grub.cfg` and deliberately not something
  this does for you -- see `docs/conventions/build.md`.

  **`sync` sends only what differs, and ASKS the machine rather than
  remembering.** It uploads a manifest of `<crc32> <size> <path>` and
  runs `sum -c` over it, so the answer comes from the files that are
  actually there -- a local record of what was last pushed is a second
  source of truth, and the case that matters most (somebody rebuilt and
  did not deploy) is exactly when it would be wrong. Measured against
  the bare-metal laptop: 79 of 86 files, 22.9 MB, 60 s; a second run
  moves nothing.

  Two traps it encodes, both found by its own positive control. **The
  manifest goes over as a FILE because a shell line is 128 bytes** --
  `KLINE_MAX`; the first version batched `sum` by path COUNT, ran past
  the limit, and the line was TRUNCATED mid-path, so every file after
  the cut reported "no such file" and was re-sent: 51 of 86 on a second
  run against a machine that was already correct. And **it creates the
  destination ROOT, not just subdirectories** -- a first sync to a path
  that did not exist wrote every file into nowhere while TFTP reported
  each one as sent, which `/bin` and `/lib` hid by already existing.
  That second one was a real OS bug as well, and it is FIXED: `open()`
  with `O_CREAT` answers -ENOENT for a missing parent now, so tftpd
  sends a TFTP error instead of acknowledging a discarded write. `sync`
  still creates the root, because a correct refusal is not a directory.

  **It is a PUSH, and the pull is the better shape** -- see
  `docs/update-design.md`, which argues for a `/bin/update` that fetches
  a manifest over HTTP so the machine needs nothing listening at all.
  `sync` still earns its place for a machine with no network
  configuration yet, and for a debug loop over one rebuilt binary.

  **One tool rather than "use telnet, then use curl"**, because the two
  halves are always used together and each has a trap that reads as the
  guest being broken: the telnet negotiation has to be answered before
  a command can be sent, and TFTP's reply-from-a-new-port is dropped by
  a stateful firewall (see `docs/commands/tftpd.md`).

  **`exec` frames on a RENDERED LINE, not a substring**, and that is the
  part worth knowing before changing it. It sends a marker `echo` after
  each command and reads until a rendered line equals the marker. Two
  reasons: the shell echoes what it is sent, so matching raw bytes would
  stop at the marker's own echoed command line, before the command it is
  framing has run; and the shared line editor repaints the whole line
  from column 0 on every keystroke, so `\r` must be replayed as a SEEK
  rather than stripped -- stripping it concatenates forty partial
  repaints into one line of garbage. A timeout RAISES rather than
  returning what it has, because a partial answer that looks like a
  whole one is the failure this exists to avoid.

  Not a terminal emulator: a full-screen program (`edit`, `less`) is not
  usable through `exec`. Use `shell` for those.
- **`update_server.py`** -- the PULL half of `remote.py flash`: serves
  builds to machines running `/bin/update` or the System Update window
  (`docs/commands/update.md`), on THREE CHANNELS it DECLARES at `GET
  /channels` (tab-separated, a `default` line, `alias stable main`):
  `/dev` is this checkout's `seed/sync`, live, marked `internal`; `/main`
  (also `/stable`) is the published snapshot
  `~/.local/share/saunaos/updates/current` points at; `/release` is the
  one the `release` link points at. `--publish` COPIES the current build
  into a new snapshot -- HARD-LINKING each file the main build before it
  shipped with the same crc and size -- and flips `current` atomically, a
  new symlink renamed over the old, keeping the last `--keep` (5) and
  never one a link points at; `--list` (M main, R release) and
  `--promote NAME [--to release]` are the rollback and the release;
  `--default` names the channel `/channels` calls the default (main). The
  server re-reads the link per request, so neither needs a restart.
  `--install-service` fills in `tools/systemd/saunaos-update.service` and
  enables it under `systemd --user`; `journalctl --user -u
  saunaos-update` is then the access log. **The unit names the script's PATH**, so
  one installed before the tools moved into `tools/dev/` (f662b8e3) still
  runs the old `tools/update_server.py` -- a copy no edit reaches. Check
  `systemctl --user cat saunaos-update | grep ExecStart`; rerunning
  `--install-service` rewrites it and restarts. `GET /` lists the channels; a
  request for a path outside them answers 404 naming them, which the
  client prints. Per channel, `GET <ch>/manifest` is generated
  from `seed/sync` on every request -- never a list kept beside it --
  with `remote.py`'s `USERLAND_TREES` IMPORTED, so the push and the pull
  agree that `/etc` and `/home` are new-files-only; `GET /files/<path>`
  serves only what the manifest names (a `/stable` snapshot freezes both
  at publish time). **A third gate, first**: `--publish` refuses a build
  that is not exactly HEAD -- `SAUNAOS_BUILD_ID` with `-dirty`, an id
  other than HEAD's, or a working tree with changes -- because the
  snapshot's `# commit` and its release notes come from that id, and a
  build of uncommitted work ships notes that miss its own change. The
  order is commit, then `preflight.sh` (which rebuilds with the commit's
  id), then `--publish`. **Two gates**: `--publish` refuses a build
  `preflight_stamp.py` does not cover (`--force` overrides and says so),
  and `/dev` answers 503 while `build/.seeding` exists -- the Makefile's
  seed step creates it first and removes it last, so a half-written
  `seed/sync` is never served (a failed seed leaves it, on purpose). The kernel is listed twice, ELF
  and gzipped, and the gzipped one only when `build/kernel.media` is not
  older than the ELF (`make all` rebuilds one and not the other). **A
  stale staging tree is refused per request with a 503** naming
  iso_guard's reason, rather than served: a client would otherwise
  install the previous build and report success. Paths are URL-quoted in
  the manifest because some names have spaces. `--print` shows the
  manifest (`--channel stable` for the published one); `--throttle KIB`
  slows each file so a progress bar has a middle to watch. Binds
  `0.0.0.0:8080` by default (the port the dev host's firewall leaves
  open); a QEMU guest reaches it as `10.0.2.2`. The test tools mount one
  source at the root instead (`make_handler({"": ...})`). **`GET
  <ch>/notes`** is the release notes: `release_notes()` reads every
  `Release-note:` LINE of the message body (not git's trailer parse,
  which sees only the last paragraph) and `refs/notes/release` git notes (for a
  commit pushed without a trailer) of the build's last 200 commits (`git log`
  from `SAUNAOS_BUILD_ID` in `version.h`, not HEAD), one line per commit;
  `--publish` freezes them as `notes` in the snapshot, and an older
  snapshot answers 404. The manifest carries `# commit` for the same
  reason.
  Unauthenticated, as the command page says.
- **`remote.py flash` REFUSES A STALE STAGING TREE**, the way
  `iso_guard.py` refuses a stale ISO, and for the same reason. It sends
  from `seed/sync`, which `make all` does NOT populate -- the `seed`
  target that `make iso` runs does. A flash after a bare `make all`
  therefore ships the PREVIOUS build's userland, and the sync cannot
  save you: it compares the machine against that stale staging and
  truthfully reports no differences. It is a clean-looking flash that
  changed nothing. `SAUNAOS_ALLOW_STALE_ISO=1` bypasses it.
  **`--force` is NOT the fix for a stale userland** and was believed to
  be for most of a session: the comparison is sound -- measured, a
  single flipped byte at unchanged size in a 512 KB file is detected and
  re-sent, and a corrupted `/bin` binary is repaired by a plain sync.
  What made `--force` look like the cure is that it was the only path
  that got PAST the early return described below. Reach for it when you
  suspect the MACHINE, not when you suspect the build.
- **A MATCHING KERNEL SKIPS THE KERNEL WRITE AND NOTHING ELSE.** A flash
  whose kernel already matched used to return before syncing `/bin`,
  `/lib`, `/tests` and `/usr`, printing `that kernel is already
  installed` -- a flash that silently changed nothing and read like
  success (`docs/bugs.md`). It now leaves `/boot` alone and syncs the
  userland as usual, which is what makes an ordinary incremental flash
  send exactly the files that changed.
- **`serial_capture.py`** -- read a running VM's serial console RAW,
  optionally sending one command first. The case `gui_debug.py` cannot
  cover: a command that KILLS the guest. `DebugConsole` is
  request/response, so a panicking kernel never returns a prompt and the
  panic block is discarded as a timeout -- and `capture_panic()` only
  helps while the console still answers. Only ONE reader may hold the
  socket, so drive input through `--gui` (QMP) or this tool's `--send`,
  never a second console. Exits 2 if the capture contains a `PANIC:`,
  because a run that caught one is not a successful test. Pipe it into
  `panic_resolve.py`.
- **`gdb/saunaos.py`** -- gdb helpers for a stopped kernel, loaded with
  `gdb -x tools/gdb/saunaos.py build/kernel.bin`: `toy-dmesg [N]` (the klog
  ring, read from memory), `toy-ps` (the process table), `toy-symbols`
  (every loaded kernel module, and the SELECTED thread's program, ld-toy
  and `/lib` libraries -- rerun it after `thread N`), and an unwinder
  that takes `bt` past `isr_common` by reading the trap frame `isr.asm`
  pushed -- claimed only from the `mov rdi, rsp` after the last push
  (found by disassembly, not a byte count) and at `isr_resume_frame`,
  never mid-push, where the frame is half built. A ring-3 frame and
  `kernel_main` end a backtrace rather than letting GDB walk into
  garbage -- which it does, fluently, with no CFI to stop it. Works over
  QEMU's stub and the kernel's. `kdebug_test.py` checks all three, asks
  the unwinder (`TRAP_UNWINDER.claims()`) about four addresses, and
  turning the recognition off reddens exactly its checks.
- **`kdebug_bridge.py`** -- carries GDB's TCP to the debugger's keyed
  UDP (`kdebug=net`), the role WinDbg's KDNET client plays on Windows:
  `gdb -> 127.0.0.1:1235 -> [bridge] -> target:50000`. Defaults match
  `make run KDEBUG=net` (target `127.0.0.1:50000`, key from
  `build/kdebug.key`); for a real machine, `--target IP:50000 --key HEX`.
  Only the stdlib (`hmac`, `hashlib`). **Each gdb connection is a new
  session** -- a keyed hello and a nonce from each side, which every MAC
  then covers -- so the bridge may outlive the target: after a reboot,
  reconnect gdb. "no answer ... is it armed, and is this its key?"
  means the hello got nothing back. `UdpLink` is socket-shaped, which is
  how `kdebug_test.py --net` puts its protocol client on it.
- **`wait_for.sh`** -- wait for a PID to exit or a file to appear, with a
  MANDATORY timeout. It exists because the hand-rolled version has a
  failure mode that outlives the session: `while ps aux | grep -q
  "[f]oo"; do sleep 15; done` can NEVER exit, because the waiting
  shell's own command line carries the word `foo` somewhere -- the
  reporting `grep` after the loop is enough -- so `ps` sees it and the
  condition stays true. The `[f]oo` trick hides the grep from itself and
  does nothing about the shell around it. **Two of those ran for two
  hours in one session while `preflight.sh` reported none**, because its
  detector looked for `while [` and these were `while ps ... | grep`
  (both are fixed). This matches no process NAMES at all: a pid is
  `kill -0` and a file is a test, neither of which can see the waiter.
  Expiry is exit 2, distinct from the awaited thing failing, because a
  waiter that gave up silently reads as success. A pid that is already
  gone is exit 0, not an error -- that is the condition already true,
  which is the common case when a waiter is armed a moment too late.
  **Prefer not needing it**: a backgrounded command's own completion
  notification is the signal, and a waiter beside it is redundant even
  when it works (CLAUDE.md). Named by no runner -- it is a helper, not a
  test.
- **`watch_vm.sh`** -- attach a VIEW-ONLY VNC viewer to a headless VM,
  so a run can be watched live without interfering with it.
  `tools/dev/watch_vm.sh [slot...]`; the display derives from the VM slot
  exactly as `vm.py --instance N` does (slot N is `:5+N`, TCP
  `5905+N`). View-only is the point, not a preference -- a connected
  viewer's real mouse motion goes into the same emulated PS/2 device
  the synthetic input uses, and the two fighting looks exactly like a
  flaky test. Remmina's quick-connect URI (`remmina -c
  vnc://localhost:5905`) has NO view-only option, so this writes a
  saved profile with `viewonly=1` and launches that instead, which is
  the whole reason it's a script rather than a line in this file.
- **`predates.py`** -- answers "did this failure exist before my
  changes?" by measuring rather than guessing: stashes the tree,
  rebuilds at HEAD, runs the command you name, restores, and prints
  both results.

      python3 tools/dev/predates.py "python3 tools/tests/sweep/init_test.py"

  CLAUDE.md's rule is that *"it predates me" is a measurement*, and the
  procedure for taking it was written down in prose with a footgun in
  it. This is that procedure as a script. It stashes with `-u` (without
  it your new files stay in the tree and the "HEAD" build is not HEAD
  -- it silently measures HEAD-plus-your-new-files), recovers **by
  SHA** rather than by index or a bare `pop`, restores in a `finally`
  so a Ctrl-C still puts the tree back, drops the stash only after a
  **clean** apply, and refuses to run mid-rebase or mid-merge.

  It answers "was this already red", and says so: pre-existing is not
  the same as unrelated, since a change can make a latent bug reachable
  without causing it.

  **TWO TRAPS IT NOW HANDLES ITSELF, both of which produced a wrong
  answer here before it did.** The `-u` that makes the HEAD build honest
  also takes UNTRACKED files away -- so a tool written this session does
  not exist during the HEAD run, and the failure reads as the tool being
  broken rather than absent (an argparse "unrecognized arguments" is
  what it actually looks like). It names such files up front now, and
  says the comparison is not a measurement.

  And **restoring the SOURCE does not restore the ARTIFACTS**: `build/`
  and any boot image still hold what HEAD produced, so whatever is run
  next silently tests the other kernel while every file on disk says
  otherwise. That reported a working fix as broken here, twice. With
  `--build` it rebuilds the working tree after restoring, so the tree is
  left as it was found -- including what was built from it.

  **AND ONE TRAP IT CAN ONLY NAME: BOTH SIDES BOOT THE SAME `disk.img`.**
  `make iso` syncs rather than reformats, so state a test left on the
  image -- `/etc/windows.conf`'s remembered window positions,
  `/etc/desktop.conf` -- is there for HEAD's run as much as yours, and a
  dirty FIXTURE measures as PRE-EXISTING. On 2026-09-30 four GUI tools
  measured pre-existing that way; after `make clean-disk && make iso`
  three passed and the fourth dropped to the one check already in
  `docs/bugs.md`. Take the fresh image FIRST (CLAUDE.md's rule), then
  measure what is still red. A PRE-EXISTING verdict now lists what has
  drifted onto the image (`fresh_disk.drift()`), and `gui_regress.py`
  builds a fresh image by default, which removes the trap for it.
- **`web_server.py`** -- the test web server the network tests share: an
  HTTP/1.1 keep-alive server on this machine that COUNTS CONNECTIONS (the
  only honest oracle for reuse), serving a corpus directory plus
  `?gzip`, `?chunked`, `?close`, `?drop` (closes without saying so),
  `/redirect/N/...`, `/redirect303/...`, `/echo`, `/cookie/set` and
  `/cookie/show`, `/download/NAME?size=N` (an attachment of known bytes,
  `download_bytes()`; `&drop=K` breaks it after K, `&slow=1` trickles it
  for a cancel), `/stats` and `/reset`. `image_delay=S` (`--image-delay`) holds
  every image S seconds, so images land after the page has laid out, as
  on a slow network -- the order that crashed NetSurf's `box_coords()`. `make_ca()` makes a test CA and
  leaves it signed -- good (for 10.0.2.2 and 127.0.0.1), expired and
  wrong-name -- plus a self-signed one, with `openssl`. The handshake runs
  in the CONNECTION's thread, so a client that refuses a certificate and
  walks away wedges nothing. Runnable by hand: `--root DIR --port N
  [--tls DIR --leaf good|expired|wrongname]`.
- **`loc.py`** -- how big this project is, honestly: source lines with
  generated files, comments and blank lines all excluded, and the
  with-comments figure beside it. Not `wc -l`, because the answer
  depends entirely on knowing which files are GENERATED and that list is
  not guessable -- `kernel/drivers/font_ttf.c` alone is ~16,700 lines of
  baked glyph tables from `genttf.py`, a fifth of the tree, and says
  nothing about how much code anyone wrote. The list is `GENERATED` at
  the top of the script; **anything a `tools/gen_*` writes into the
  source tree has to be added to it**, since a missed entry inflates the
  count silently, which is this script's one failure mode. Its comment
  stripper tracks string and character literals, so a `"//"` inside a
  format string is not read as a comment -- a naive stripper
  under-counts exactly the files that do the most string work.
  `--by-dir` groups by directory instead of language, `--files N` lists
  the largest N. Reports the OS itself separately from `tools/`, which
  is a fifth of the tree and ships in nothing. NOT in `preflight.sh`: a
  line count is a fact to look up, not a gate, and this file's own rule
  is that a number nobody has to keep true is the only safe kind.

  **`--by-origin` is the view the other two hide.** The default buckets
  by language and `--by-dir` by top-level directory, so `userland/ports/`
  -- which holds MORE lines than the OS written here, mbedtls alone
  being ~116k -- counted as ours. That is the same silent inflation the
  `GENERATED` list exists to prevent, arriving from a different
  direction, and it made "how big is this project" unanswerable without
  doing arithmetic by hand.
