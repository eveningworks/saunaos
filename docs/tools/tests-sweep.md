# `tools/tests/sweep/` -- On-demand tests (the sweep)

One entry per tool in `tools/tests/sweep/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by `tools/runners/ondemand_sweep.py` (`tools/t sweep`), on demand.
Each tool's `RUNNER` header says how: `parallel`, `needs` (a
precondition from `tools/lib/preconditions.py`; missing is a SKIP),
`shared_vm`, `args`.

- **`cross_gcc_test.py`** -- `docs/cc-design.md` stage 1's exit
  criterion: `x86_64-saunaos-gcc -O2 hello.c -o hello`, NO other flags,
  gives an `ET_EXEC` whose entry is `_start` at `0x8000000000` with no
  `PT_INTERP`, which `vm.py put` copies onto a guest booted from a COPY of
  `disk.img` (slot 6 by default) and `tosh -c` runs with stdout to a file;
  its lines must equal the same source built by the host's plain gcc and
  run here (printf, malloc/realloc, a struct by value, varargs,
  setjmp/longjmp, floats, errno). It also asserts the defaults arrived: a
  protected function reads `__stack_chk_guard` (not `%fs:40`) RIP-relative,
  and `__saunaos__` is predefined where `__linux__` is not.
  `--positive-control` links through a specs file with no startfile (the
  spec losing `crt0.o`) and must fail. A SKIP (77) without a toolchain
  (`needs: cross_gcc`); `TOOLCHAIN=`/`--toolchain` names another prefix,
  which is then pointed at this checkout's `build/sysroot`.
- **`native_cc_test.py`** -- `docs/cc-design.md` stage 3's exit
  criterion, ON the guest: an image seeded after `make native-toolchain`
  boots (slot 6 by default, a COPY of `disk.img`), the sources go in with
  `vm.py put`, and `/usr/bin/cc` and `/usr/bin/c++` compile them THERE --
  a C program (structs by value, varargs, `setjmp`, `qsort`, floats), a
  two-file C program compiled with `-c` and linked by a third `cc`, and a
  C++ one with iostream, `unique_ptr`, a virtual call, a `map` and a
  throw caught by base class through a destructor. Each runs on the guest
  and its lines must equal the host gcc/g++'s build of the same source.
  The compiles run from a dash script (for `2>&1`) that exports init's
  `PATH`: a program the debug console spawns has no environment, and the
  driver finds its own prefix by looking `argv[0]` up on `PATH`. It
  PRINTS, never asserts, each compile's time by the guest's own clock (a
  stopwatch it compiles there first) and the largest compiler process
  `ps` showed; `--kvm` for a time worth quoting, `--mem` for a smaller
  guest. `--positive-control` adds `-nostartfiles` to every guest compile
  and must fail. A SKIP (77) unless the seed tree carries `/usr/bin/cc`
  (`needs: native_cc`).
- **`native_corpus_test.py`** -- `docs/cc-design.md` stage 5's exit
  criterion: a CORPUS compiled ON the machine and by the host's
  `x86_64-saunaos-gcc`/`g++` -- one GCC built twice -- must agree the way
  GCC's own bootstrap compares its stages. Three suites: its own programs
  (`cc_corpus/`: stdio, stdlib, math, the C language, files and time,
  threads, C++ language and library, a multi-unit program), GCC's
  `gcc.c-torture/execute` and libstdc++'s `dg-do run` tests, the last two
  read from the fetched GCC tarball by `tools/lib/gcc_testsuite.py`.
  Four checks: every case the cross compiler builds, the machine builds;
  every OBJECT is byte-identical (the cross side passes
  `-ffile-prefix-map` from its toolchain prefixes to `/usr`, so a header's
  `__FILE__` names the same path); every linked program is the same file
  or loads the same bytes (`loaded_sha()`: entry, program headers,
  PT_LOAD/PT_TLS, the ELF header's section-table fields zeroed -- the
  cross toolchain's libgcc keeps debug info the image's is stripped of),
  and one that differs is run both ways on the machine; and each test's
  own verdict matches `cc_corpus/known.txt`, the NAMED list, so a new
  failure and a listed test that passes are both red -- a listed case
  may also fail to BUILD on the machine, for the reason the list gives.
  A test runs up to 120 s on the machine (`RUN_LIMIT_S`) times its own
  `dg-timeout-factor`. The host's own
  gcc+glibc build of each case runs as an informational third column,
  which is how tolibc's printf gaps were found. A guest-side runner,
  `cc_corpus/corpus_run.c`, runs the job list with a time limit each; the
  machine's files are named by short ids, which keeps every path there
  short, and its hashes are hints the host confirms from the bytes.
  Without `--full` it runs a sample (`--every torture=10,libstdc++=40`);
  `--full` is a night: 10.3 hours on the ASUS, and a KVM guest had done
  only 82% of it in that time, nearly all of it libstdc++ (the 1,681
  torture tests take minutes). `--host IP` runs it on a
  real machine through `tools/dev/remote.py` instead of a guest.
  `--collect` judges what a run left in its `--work` directory without
  running anything there -- after a reader fix, or a run stopped with
  Ctrl-C (the guest still powers off, and a case with no result is
  reported as not run) -- and `--skip-done TSV` then runs only the cases
  that report has no guest build for. A program fetched back is judged
  only once it matches the machine's own sum or a second fetch.
  `--positive-control objects` (the machine compiles with `-fno-ident`)
  and `--positive-control known` (the list ignored) must fail. A guest
  powers itself off (`reboot --poweroff`) before the host reads its
  image: `vm.py stop`'s ACPI request is not heard (`docs/bugs.md`). A
  SKIP (77) without the native toolchain on the image (`needs:
  native_cc`); the GCC suites are left out, said so, without the tarball.
- **`native_make_test.py`** -- `docs/cc-design.md` stage 4's exit
  criterion: GNU make ON the guest drives `cc` and `c++` through one
  Makefile (a two-file C program with header dependencies, a C++ program
  by make's built-in rules, a `$(MAKE) -C sub`), in steps -- `make calc`,
  `make`, `make` again, `touch util.h; make`, msg.h changed then `make`,
  `make clean` twice, `make -j2`, `make -O -j2` (output sync, which takes
  an fcntl lock and must not warn) -- and every step's printed COMMANDS
  must equal the HOST's GNU make running the same step on the same files
  (sorted for `-j2`), so a missing rebuild and an extra one both fail;
  the programs' output must equal the host build's, including the new
  message after msg.h changed. `make -j2 naps` (two `sleep 1`s) must take
  well under the serial run measured just before it, by the guest's own
  clock. PRINTS each build step's time; `--kvm` for a time worth quoting.
  `--positive-control` drops msg.h from main.o's rule on the guest and
  must fail. The "fast" step changes msg.h within the second its object
  was built and must still rebuild (file times finer than a second); the
  older steps' `sleep 2`s predate that. A SKIP (77) unless the seed tree carries
  `/usr/bin/make` and `cc` (`needs: native_make`).
- **`cross_gxx_test.py`** -- `docs/cc-design.md` stage 2's exit
  criterion, `cross_gcc_test.py`'s shape for C++: `x86_64-saunaos-g++
  -std=c++17 -O2 main.cpp thrower.cpp`, no other flags, run on a guest
  (slot 6 by default) and compared line for line with the host g++'s build
  of the same two files. The FIRST line is a global's constructor and the
  LAST its destructor, printed after `main()` returns, so a runtime that
  runs no constructors or no exit handlers cannot pass; between them a
  throw unwinds a destructor across two functions, another is caught
  across translation units by base class and rethrown, and the program
  uses `std::string`, `vector`, `map`, `unique_ptr`, a virtual call,
  `typeid`, iostream formatting, four `std::thread`s on a mutex, and
  `<cmath>`'s C99 half (`std::round`, `std::cbrt`, `std::hypot`,
  `std::lround`, a `float` and a `long double` overload, told apart by
  `sizeof`) -- which exists only when tolibc's `<math.h>` is all of C99,
  and was "'round' is not a member of 'std'" against the toolchain built
  before it was. One throw leaves a `qsort()` comparator, so libc must
  keep its unwind tables (`--positive-control libc_eh_frame` discards
  libc.a's alone). A second program does `import std` (`-std=c++23
  -fmodules --compile-std-module`), built, run on the guest and compared
  with the host's build of it the same way. It
  also checks `g++ -v` says `Thread model: posix`, that every C library
  header in the sysroot compiles as C++ on its own (the toolkit's are
  left out: not C++-clean yet), and prints the binary's size. `--positive-control eh_frame` (or `init_array`) links
  with a copy of the sysroot's `saunaos.ld` that discards that section,
  and must fail. A SKIP (77) without a toolchain, or with a stage-1 one
  that has no `g++`.
- **`boot_entry_test.py`** -- a one-shot GRUB entry is taken ONCE, and
  GRUB is what ends it. Boots a copy of `disk.img` with reboots allowed,
  adds a `target=text` entry so the boot it takes is visible from inside
  (no `toywm`), and **deletes `loadenv.mod` from /boot's module
  directory first** -- the pass has to come from the CORE image, which is
  all an installed laptop has, or it would pass for the wrong reason.
  Then `reboot --entry` (text boot, `next_entry` already empty) and a
  plain `reboot` (the default, graphical), plus the two refusals: an
  unknown entry, and a core recorded without `loadenv`. On demand: it
  reboots twice. **The debug console's shell has no redirection** -- a
  file the test needs in the guest goes in with `vm.py put`.
- **`boot_migrate_test.py`** -- a machine with the old `/boot/boot`
  layout moves up to `/boot` by itself and boots from the result. Writes
  the OLD layout onto a copy of `disk.img` (`install_grub.py
  --legacy-layout`, plus a `kernel.old` and a marker word on the default
  entry), boots it, waits for the `bootmigrate` service, checks the flat
  tree, then REBOOTS: the marker on the second boot's command line can
  only have come through the rewritten core image, since the old tree is
  gone. Its control -- the core write skipped -- fails at "came back".
  On demand: it reboots.
- **`rename_migrate_test.py`** -- a machine installed as toy-os keeps its
  settings and log when it boots as SaunaOS. On a copy of `disk.img` it
  saves a setting, stops `logd` (running, it recreates the log behind
  the move), gives `/etc/saunaos.conf` and `/var/log/saunaos.log` their
  old names, reboots through `vm.py reboot` (a console poll right after
  `reboot` is still answered by the OLD boot), and checks the kernel
  moved all three and the setting reads back; then that a machine with
  BOTH names keeps both. Its control -- the move's call removed -- fails
  the three move checks. On demand: it reboots.
- **`bootcfg_test.py`** -- `bootcfg` against a real `/boot`: an edit
  lands with the old file as `grub.cfg.bak` (byte for byte), exactly one
  line changes, no `.new` is left and `/boot` is read-only again; a
  misspelt word is refused with a suggestion and the file is unchanged;
  `--force` writes it and `undo` removes it. Then `bootcfg try 0
  +target=text` and a reboot must come up in the TEXT target with
  `bootcfg.trial` on the running command line, and the reboot after it
  in the default again -- GRUB cleared the choice. On demand: it reboots
  twice. `/tests/bootcfg_test` (in `usertest_run.py`) covers the model
  itself on fixtures.
- **`screentrial_test.py`** -- a screen size tried at a restart, on a
  QXL guest (no driver here, so GRUB's VESA framebuffer, as an RTX on a
  BIOS boot): `bootcfg resolution 1024x768` comes up at 1024x768 with
  the "Keep this resolution?" window, Enter keeps it and the next boot
  asks nothing; 1366x768, which QXL's BIOS does not offer, comes up at
  1280x720, is remembered in `/etc/display.conf` and restarts onto the
  kept size; an unanswered 800x600 restarts by itself. A boot is told
  from the one before it by a file in `/tmp` (ramfs). On demand: it
  reboots six times, about four minutes.
- **`uefi_boot_test.py`** -- a UEFI boot off the disk's own loader, under
  OVMF on q35 with QXL (vesafb on GOP's framebuffer, as an RTX with CSM
  off). On a copy of `disk.img`: with `EFI/BOOT/BOOTX64.EFI` deleted UEFI
  does NOT boot it (the control); a BIOS boot's `install --bootloader
  confirm` puts it back; UEFI then reaches the desktop at 1280x720; and
  `bootcfg resolution 2560x1440` makes the next boot 2560x1440, a size
  QXL's VESA BIOS does not offer. SKIPS (77) without OVMF. On demand,
  about four minutes.
- **`smb_test.py`** -- network shares in a GUEST: `/bin/smb`, `mount -t
  smb` and `smb login`, a saved share across a cold boot, a killed
  worker replaced, an unreachable server failing fast, and a forgotten
  share unmounted. The 3 MiB file is hashed through both paths; the
  file is also hashed straight off the mount (`sum`'s 8 KiB reads, what
  the kernel's read-ahead window answers), `open smb://...` mounts a
  session share and saves nothing, and the
  1500-entry folder is walked (`cp -r`) to its last entry, since `ls`
  stops at one listing on every filesystem (`docs/bugs.md`). The
  positive control saves a wrong hash. `ondemand_sweep.py`, serial.
- **`fm_network_test.py`** -- the File Manager's Network rows: a share
  row found by its icon's teal pixels, Sign in drawn (the window dims,
  the padlock shows) and answered, Add network share writing
  `/etc/mounts.d` and opening the share, Properties on the share's row
  (the volume page: its address, Connected, the user it signed in as --
  the `properties: row` lines), Forget this share from the row's own
  menu (two Ups from nothing: Properties is the last row), a share
  dragged above the other (`dev:` lines in `/etc/places.conf`), an `smb://` address in the address bar opening a
  session share whose Save to Network bar saves it, and the "not
  answering" dialog.
  Each step waits on the app's `files: netshare` log line. The positive
  control types a wrong password. `ondemand_sweep.py`, serial.
- **`vnc_test.py`** -- `/bin/remoted`'s VNC server, from an RFB client
  written in the tool (DES through `openssl`, ZRLE through Python's
  `zlib`, so neither shares code with the server). The checks: a wrong
  password refused and the right one let in; a Raw frame against QEMU's
  screendump; ZRLE at 32 and 16 bits against the Raw frame; Tight
  decoded here (Python's zlib, PIL's JPEG) -- without a quality level
  the Raw frame exactly, through fill and palette tiles and no JPEG,
  and at quality level 5 within 24 levels of it with JPEG used; a
  PointerEvent landing exactly where the compositor reports the pointer;
  Super opening the Start menu; an incremental update smaller than the
  screen that patches back to the screendump; and `echo VNC_typed-42!`
  typed into a Terminal arriving in `/etc/tosh_history`; with the Cursor
  pseudo-encoding, the shape arriving and a pointer move sending no
  pixels near the pointer, and with PointerPos the machine's own mouse
  (moved through QMP) reported where the compositor has it while the
  viewer's own move is not echoed (not "none at all": the taskbar clock may
  tick in the same second, and did once); the clipboard -- a viewer's
  Latin-1 cut text reaching a second viewer byte for byte and not echoed
  to the first, and pasted into the Terminal arriving in the history as
  UTF-8 (read over TFTP: `vm.py exec` output mangles non-ASCII); the
  Extended Clipboard -- Caps on SetEncodings, a viewer's Notify answered
  with Request, its Provide (zlib ended by a sync flush, as TigerVNC
  writes it) of `ż ó ł €` reaching a second Extended viewer byte for
  byte through Notify/Request/Provide and a plain viewer as Latin-1 with
  `?`, no Notify echoed to the copier, and the paste arriving as UTF-8;
  then
  asking at the screen --
  the card naming the viewer, Allow letting it in, Deny refusing it in
  RFB's words, "Ask every time" offering no Always allow, Always allow
  with View only letting it in read-only and adding the address to
  `/etc/remote.conf`, the next connection then unasked -- the session
  listed live with the tray icon lit, the flyout's View only stopping the
  viewer's mouse and its Disconnect ending the session, and the Settings
  page's VNC switch writing the file; then VeNCrypt, through Python's
  own `ssl` (sharing nothing with mbedTLS): X509Vnc in, plain listed
  first, the certificate's SHA-256 the fingerprint in
  `/etc/remote.conf`, a Raw frame over TLS against the screendump, a
  VERIFYING connection (the certificate as CA, `127.0.0.1` checked
  against its names), X509Plain refusing a wrong password and taking
  the right one; under `encryption = require` VeNCrypt alone offered and
  an RFB 3.3 viewer told why it is refused; and two silent connections
  holding both sessions until the handshake deadline drops them; last,
  `config set resolution 1024x768` reaching a DesktopSize viewer as the
  new size, the whole screen after it against the screendump, a viewer
  without DesktopSize closed, and the old size followed back. It
  boots with `--cpu max`, since without RDRAND no key is made. In the
  typing check the capitals and Shift punctuation are the point, since
  they need the kernel to press Shift around the key. On demand: it boots its own guest on a
  COPY of `disk.img`, guest port 5900 on host 15900+N.

  **The 99.5% bar is the pointer**: remoted draws it into the picture and
  QEMU's dump does not, so it is parked in the corner first. A broken
  encoder, stride or pixel format moves most of the frame. **A frame that
  does not decode is a FAIL of the check that asked for it**, not a
  traceback, so the input checks (which run on Raw) still report. Seen
  red under `mutate.py`: palette-RLE runs without their flag bit (every
  ZRLE check), and no Shift around a shifted character (the typing check
  read `echo vnc-typed-421`).
- **`remote_test.py`** -- `telnetd`, `tftpd` and `remote.py` end to end
  against a QEMU guest, including `sync --delete` (with a plain sync
  as its control). On demand: it boots its own guest
  and ENABLES services that ship disabled, so it leaves `disk.img` with
  a network shell turned on -- `make clean-disk && make iso` afterwards
  if that matters. A session must END from the client's side: `exit 3`
  and Ctrl-D each close the connection, with `exit now` (refused, the
  session up) as the control. **Its lossy puts drop one block on purpose**
  (`do_put(drop_once=...)`) -- the last of a window, the first of one,
  and the final block -- and must recover inside `tftpd`'s give-up. **Its
  lossy gets lose a window's ACK and send a stale one**
  (`do_get(drop_ack_once=..., stale_ack_once=...)`), and the bytes must
  come back whole: `tftpd` once refilled the window on a resend and sent
  the next blocks under the old numbers (155d355b).

  Three of its checks exist because of a specific way this can pass
  while broken. It asserts a command's OUTPUT and that the command LINE
  is absent, because a broken framer returns the echo and looks fine.
  Its round-trip content is address-derived rather than uniform, since
  a run of identical bytes cannot tell a working block walk from one
  that repeats a block. And one file is an exact multiple of 512 bytes,
  the case needing a final zero-length DATA packet that a hand-written
  TFTP nearly always gets wrong.

  It also encodes init's rule that **an admin `stop` outranks
  `Restart=`**: the test disables and stops both services to establish
  its own precondition, and `enable` alone does not bring them back.
  Getting that wrong reported both services `running` while nothing
  answered the network.
- **`ansi_cursor_test.py`** -- ANSI cursor movement and erasing, checked
  as PIXELS. `kernel/lib/ansi.c` is a pure state machine whose KTESTs
  assert what a sequence RESOLVES to with no display at all; this is the
  other half, whether `vga.c` then puts ink in the right cell. **The
  desktop owns the screen, so it removes the toywm service descriptor
  and kills it first** -- the same unsupervise-then-kill pattern
  `compositor_death_test.py` uses, and for the same reason.
  **It SELF-CALIBRATES its cell size from the pattern it drew**, because
  this project's layout is font-derived and a hardcoded 8x16 rots the
  day the default font size moves. It did: the first version assumed 16
  against a real 14, which presented as every row after the third being
  "one row high" while the kernel was correct throughout. Every check
  has a BLANK NEIGHBOUR -- "something was drawn" is satisfied by a
  console that ignores cursor movement entirely. Run on demand.
- **`sanitize_run.py`** -- builds a COPY of the tree (tracked files
  plus new ones, uncommitted edits included) with `make UBSAN=1 KASAN=1
  iso` in a scratch directory (`--ubsan` / `--kasan` for one), then runs
  a desktop boot (reading `dmesg` AND `log`, since a service's stderr
  goes to the application log), the kernel suite and the ring-3 suite
  on it, and lists every site that reported, once each, with where it
  fired. A KASAN report's link-time pc is resolved to `file:line` with
  `addr2line` against the scratch build. Exit 0 clean, 1 sites found, 2
  build or control failure. On demand, in `ondemand_sweep.py`.
  **Why a copy:** an instrumented build re-seeds `disk.img` and rewrites
  `saunaos.iso`, which a runner must not do to a checkout unasked.
  **The positive controls are sites it EXPECTS:** `kernel/lib/ubsan_test.c`
  and `userland/tests/ubsan_test.c` each commit one signed overflow, and
  `kernel/lib/kasan_test.c` one of each KASAN class (heap overflow,
  use-after-free, double free, freed page, stack, global). Any missing
  means that part of the build is not instrumented, and a clean result
  would mean nothing -- exit 2.
  **A suite failing here is printed but is not a finding**: an
  instrumented kernel is several times slower, and KASAN's quarantine
  delays every kfree's merge, so a test with a wall-clock budget or an
  exact heap-layout expectation can fail here and pass normally. Only
  reported sites decide the exit code. Seen on every instrumented run so
  far: `cwd` and `fd` KTESTs (a wall-clock wait for a spawned process),
  and `libc_test`, the FIRST ring-3 test, with no exit code -- its `run`
  never reaches the shell (no `elf_run` line), while every later test
  runs; the first command after an instrumented boot is lost, cause not
  established. `--keep DIR` keeps the tree for
  `addr2line` against its ELFs; a ring-3 report's `pc` is link-time for
  an executable and a runtime address inside a shared library.
- **`faulttest_run.py`** -- the ring-3 diagnostics that FAULT ON
  PURPOSE, which `usertest_run.py` correctly excludes and which
  therefore nothing ran at all. A faulting binary has no exit code and
  no output of its own, so the assertion is the KERNEL's report, read
  out of the serial log: each entry names required AND forbidden
  substrings, which is where the value is -- a stack overflow and a
  null dereference are both page faults, and every entry doubles as the
  positive control for its neighbours. Each test gets its own QEMU,
  because a ring-3 crash takes the debug console's command loop down
  with it. **The command goes over COM1, not the keyboard**
  (`serial_console.py`), and that is what makes it work at all: it used
  to type at the physical shell over QMP, which stopped working the day
  the desktop began starting at boot -- a compositor holding the role
  parks every ring-0 blocking reader, so the keystrokes went to the
  desktop and all three entries failed identically having never run.
  **Assertions match only what arrives AFTER the command**, anchored on
  the guest going quiet rather than on the console's banner: the banner
  lands early and the kernel keeps printing for seconds afterwards, so a
  banner-anchored window still contains boot output and an entry
  requiring a boot string passed vacuously (measured, on a control).
  Not in `gui_regress.py`; run it after touching the fault path, the ELF
  loader, or the user address-space layout. `stack_smash_test` is
  deliberately absent -- its message goes to the process's stdout, i.e.
  the screen, so there is nothing in the log to assert on.
- **`mem_stress.py`** -- several memory hogs at once: does the machine
  survive running out, and is the memory each one got actually its own?
  Drives `/tests/memtest`, which takes a DEFAULT 64 MiB (the bound is
  ~2046 MiB of address space now, and taking all of it would mean trying
  to allocate the machine), writes a pattern derived from the ADDRESS
  plus a random per-process salt, and reads it back. The heap LIMIT is
  still asserted, by asking for a terabyte and requiring the refusal --
  a cap silently stops covering a bound unless something else does. **The address-derived pattern is the whole design**: a constant
  fill cannot detect two virtual pages sharing one physical frame,
  because both read back the constant and look perfect -- with this,
  the loser reads a value that is a valid pattern for a different
  address or a different salt, and says so. Proven by deliberately
  aliasing every 64th heap page, which it catches and names.
  **`--mem` shrinks the guest so exhaustion is REACHABLE** -- one
  process cannot fill a 2 GiB machine, `-n 8 --mem 128` does it -- and
  the summary says whether memory actually ran out, because a run where
  everything fitted is a different result, not a better one. Ends by
  running `meminfo audit`. `memtest` alone is in `usertest_run.py`, so
  the single-process integrity check runs on every preflight.
- **`oom_test.py`** -- running OUT of memory, on purpose, on a 384 MiB
  desktop. One `/tests/memtest` (the VICTIM) takes all but 16 MiB of
  what a user page can get and holds it (`memtest <MiB> <seconds>`); a
  second (the ASKER) then wants 48 MiB. The OOM killer (`kernel/oom.h`)
  must end the VICTIM, the biggest, and let the asker finish and verify
  its pattern -- with init and the compositor the same pids afterwards,
  `meminfo --audit` clean and the frames back. **THE CONTROL IS THE
  SECOND ROUND, SAME BOOT**: `kernel.oom_kill_allocating_task on`
  (Linux's sysctl) must send the same pair the OTHER way, the asker
  ended and the victim passing. A first round that passed because the
  ask happened to fit would fail there, which is what makes it evidence.
  **Each round matches only OOM lines NEW since it started** -- the
  ring still holds the last round's, and matching the first one in
  `dmesg` read round one's victim as round two's answer. ~6 minutes
  under TCG. Run it after touching `pmm.c`'s user reserve, the fault
  paths, `scheduler_kill()` or `oom.c`.
- **`fstorture_test.py`** -- the filesystems against a MODEL, then
  power cuts. Drives `/tests/fstorture`, whose every op is a function of
  a seed, so a failure names a seed that replays it (and prints the last
  sixteen ops). **Phase 1** runs fsx's shape -- in-place writes,
  appends, truncates both ways, holes, renames across directories,
  atomic replaces -- on TFS3 (`/var`), FAT32 (a second virtio disk the
  HOST formats with mkfs.fat, mounted at `/mnt/fst`) and ramfs (`/tmp`),
  checking every read and the whole tree every 50 ops; then `fsck` must
  be clean, and after a sync and shutdown the host reads the final
  files back with readers of its own -- `tfs3_writer.py read` (which
  reads holes as zeros for this) and mtools, plus `fsck.fat -n` -- and
  each must match the hash the guest printed. **Phase 2**, `--cuts N`
  (4): `fstorture cut` replaces and deletes files in GENERATIONS, each
  followed by sync(), saying `durable G` at most once a second; QEMU is
  SIGTERMed a random 2-10 s in; the next boot must hold every file at
  generation G's version or a later one, WHOLE, never torn or rolled
  back (`fstorture check`, which regenerates the stream from the seed).
  It assumes NO ordering between syncs -- POSIX does not promise one.
  **After a cut fsck may find LEAKED blocks and wrong free counts, and
  nothing else** -- tfs3 marks a block allocated before the transaction
  that uses it, so a crash leaks by design -- and `fsck -r` must then
  leave TFS3 clean. Cuts alternate TFS3 and FAT32, and every earlier
  cut's directory is re-checked on each later boot.
  `--positive-control` shows the check failing on a flipped byte
  (`fstorture poke`) and on a durable generation that never ran. **It
  found a real bug the first time**: a FAT32 file growing past EOF read
  stale bytes in the tail of its old last cluster (zero_past_eof(),
  docs/conventions/storage.md). **WHAT A CUT DOES NOT MODEL**: a drive
  that loses writes it had not flushed -- QEMU's writes reach the host's
  page cache, which outlives QEMU. ~10 minutes under TCG; needs sfdisk,
  mkfs.fat, fsck.fat and mtools. Run it after touching tfs3, fat32,
  ramfs, the journal or the block cache.
- **`frame_balance.py`** -- does a process's teardown balance? Spawns a
  process, lets it exit, and compares the physical allocator's
  free-frame count against the baseline. Two directions, needing
  opposite fixes: DOWN and staying down is a leak; UP is an OVER-FREE,
  which is worse and quieter -- teardown handed back frames the process
  never owned. It found exactly that (an exiting GUI client returning
  pages of kernel `.rodata`, see `docs/decisions.md`). **It boots its
  own VM because an over-free fires only ONCE**: the second exit finds
  those frames already free, so any run that reuses a booted VM
  measures nothing. A `fork` cycle (`/tests/fork_test --forks 20`)
  sits between the control and the GUI cycles: twenty copy-on-write
  children and their parent must return every frame, which is the
  refcount's own balance check. The non-GUI control must stay flat -- if it drifts,
  the fault is in ordinary teardown or in the harness, not in the
  borrowed-mapping path. Run it after touching `vmm.c`'s mapping or
  teardown paths, or after adding any mapping of memory a process does
  not own.
- **`stdin_test.py`** -- blocking stdin (fd 0) and the standalone ring-3
  shell `/bin/tosh`. Three properties, each with a failure the others
  miss: a line typed at tosh RUNS (asserted through the filesystem --
  `file_test` is on tosh's PATH and writes `/filetest.txt`, so the check
  is a round trip from keystroke to key ring to a parked process's
  trapframe to a spawn, and nothing short of the whole path satisfies
  it); an idle tosh is BLOCKED, not spinning (`kstack slots` reports
  state 4, and a spin-poll implementation passes the first check
  perfectly); and the console has exactly ONE reader, checked in both
  directions -- `touch` is a kernel-shell builtin and NOT on tosh's
  PATH, so typing `touch /claimprobe.txt` at tosh must create nothing,
  and after Ctrl-D the same line must work. Without both directions
  "the claim works" and "the claim is stuck on" look identical.
  **Two preconditions it establishes itself**, and both are the point:
  it puts the guest on the US keyboard layout (`sh keyboard us`, or
  `kbd=us` on the GRUB line), because a QMP qcode names a PHYSICAL key
  by its US label and under this OS's `se` default every `/` arrived as
  `-` -- `spawn /bin/tosh` became `spawn -bin-tosh` and the substring
  assertions passed against a file genuinely called `-claimprobe.txt`;
  and it takes the desktop out of init's hands (`rm
  /etc/services.d/toywm`, then kill) because a desktop owns the
  keyboard and a supervised one comes straight back. It therefore edits
  `/etc` -- run it against a throwaway copy of `disk.img`.
  Positive controls, measured: removing `keyboard.c`'s
  `scheduler_wake(SCHED_WAIT_KEY)` reddens three checks, removing
  `keyboard_claim_console()` reddens two, and neither reddens the
  other's.
- **`iso9660_test.py`** -- disc images mounted from a FILE
  (`kernel/fs/iso9660.c`), judged by xorriso: a plain ISO 9660 and a
  Joliet image made by `xorriso -as mkisofs --norock` from one tree (deep
  folders, long and non-ASCII names, an empty file the plain disc writes
  `EMPTY.`), staged into a throwaway copy of `disk.img`, mounted with
  `mount <file> <dir>`, every folder listed and every file summed
  (`sum -a sha256`) against xorriso's own extraction; then `rm` of the
  image must fail while it is mounted and work after `umount`.
  `--image DISC.iso` adds a disc of your own (root listed, 24 files
  sampled). ONE FILE PER `sum` CALL: the debug console's line is short
  and eight paths arrived cut off, which read as eight wrong digests.
  Seen red: keeping the bare trailing dot fails the listing and the sum.
  `ondemand_sweep.py` runs it; skipped without xorriso.
- **`disc_desktop_test.py`** -- the desktop's side of disc images: an
  xorriso-made `TESTDISC` staged with `tfs3_writer.py` into a throwaway
  copy of `disk.img` beside a text file and a text file NAMED `.iso`.
  The File Manager's menu row "Mount as a CD drive" right after Open for
  the image and absent for the text file; See more's row DRAWN greyed
  for the text file and live for the image (the label's darkest ink,
  Properties as the control) and a click on the greyed row asking
  nothing; the details pane's disc facts and Mount; a DOUBLE-CLICK
  mounting it (the mount table) and the pane going to `/media/TESTDISC`;
  its row under This computer (DRAWN: the rect changed) with the
  drive's details offering Eject; opening it again making no second
  mount; the row's Eject unmounting it; the fake image refused in the
  status line; and the tray's drives flyout listing it as an image row
  that did NOT open itself, its Eject unmounting it. Seen red with
  `mutate.py`: the double-click no longer mounting fails the mount
  checks, and Mount never greyed fails the pixel check. The File
  Manager's `files:` lines are read from `filemanager_test`'s transcript
  (`_ALL_BUF`): its layout waits sweep them off the console. In
  `ondemand_sweep.py`; skipped without xorriso.
- **`ls_test.py`** -- `/bin/ls`: ordering, the format flags, colour and
  a directory past one listing page. Fourteen checks against a THROWAWAY
  copy of `disk.img`, into which it stages a 300-entry directory from the
  host with `tfs3_writer.py` -- a page is 256 entries, so nothing a person
  would type at a shell crosses it, and a test that made a handful of
  files would be green with the paging removed. It must list all 300,
  once each and in order (it stopped at 256 until 2026-10-14). Two of
  the checks exist because their failures are SILENT: `ls` listed a
  40-file directory as 32 entries and simply stopped, and the console's
  ANSI parser being unwired shows up as literal `[1;36m` beside every
  directory name rather than as missing colour (the captured console
  output cannot show colour at all). Positive controls, measured:
  disabling `ansi_feed()`'s dispatch reddens the escape check -- with
  `\x1b[1;36mbin\x1b[0m/` printed in the failure detail -- and making
  `ulistdir()` stop after one page reddens the whole-listing check
  (`listed 256, first gap at f0256`), each naming its own failure.
- **`hires_test.py`** -- a desktop ABOVE the mode this OS boots into by
  default, and whether a ring-3 client window can actually fill it. Two
  constants have to move together and did not: the mode the display
  layer selects (`DISPLAY_MAX_W/H` plus the ladder in
  `kernel/drivers/display/display.c`, and whether a modesetting driver
  picked it up) and `WIN_CLIENT_MAX_W/H` (`abi/win_proto.h`), the
  largest buffer the window server will hand a client. When the second
  is smaller, MAXIMIZE FAILS SILENTLY -- the WM proposes the new content
  size, `resize_window()` refuses, and the window wears full-screen
  chrome around a stale buffer with nothing logged.

  The load-bearing check is a PIXEL, not the WM's own numbers:
  `gui windows --json` reporting w=1920 is second-hand (the WM adopts a
  client's size only when the client acks), so the test samples a point
  far outside any 1280x720 buffer and requires it to change from the
  desktop's background colour to the client's. A taskbar pixel is
  sampled alongside it and must NOT change, so "everything repainted"
  cannot pass it. Positive control, measured: with
  `WIN_CLIENT_MAX_W/H` put back to 1280x720 it reddens exactly three
  checks -- the frame size, the content size and the far-corner pixel --
  and leaves the taskbar neighbour green.

  **It needs an ISO built differently from the one every other tool
  wants**, which is why it is not in `gui_regress.py` or
  `preflight.sh`: the mode is chosen at boot, so it comes from the
  kernel command line.

  ```
  make iso KCMDLINE="video=1920x1080"     # re-seeds disk.img
  python3 tools/lib/vm.py start
  python3 tools/tests/sweep/hires_test.py
  python3 tools/lib/vm.py stop
  make iso                                # put the default ISO back
  ```

  `--require-min` (default 1920x1080) FAILS rather than skips when the
  guest is not actually running big enough -- at 1280x720 every
  assertion in the file passes vacuously, which is this repo's "the data
  never reached the code under test" trap exactly.
- **`taskbar_test.py`** -- opens Notepad until the taskbar overflows,
  and asserts the strip never reaches the tray. Checks against
  `gui taskbar --json`, which comes from the SAME `taskbar_layout()`
  that draws the buttons and hit-tests them (`userland/apps/system/toywm/wm_taskbar.c`)
  -- before that fix the debug console was a fourth, independent walk of
  the window list and was already eight pixels wrong, so every test
  click aimed at a reported button centre missed it. Drives every
  threshold the layout has: natural width, shrink-to-fit, the floor, and
  collapsing windows of one application into a counted button with a
  jump-list popup. Slow (each window is a real process, so budget a
  couple of minutes), deliberately NOT in `gui_regress.py`.
  Also `desktop.taskbar.combine` (always groups two windows, `full`
  does not, `never` overflows into the overflow button and its list
  raises a hidden window) and "Close all N windows" (offered only past
  one window; closes three; with one refusing, the notice after the 5 s
  wait and its Force Quit; a second Close all joining a waiting one asks
  only its own windows, one that asks nobody does not restart the wait,
  and after the wait a Close all asks again; long titles elide but the card's count
  survives), and a drag-reorder with windows hidden keeps them hidden.
  `--only height|combine|closeall|join|names|draghidden|overflow`
  runs sections; every setting it touches is unset at the end.
  Its positive control is worth reading in the file: the first attempt
  reddened three checks and left the OVERFLOW check green, because the
  layout's placement guard still refused to put a button past the strip
  -- the control never reached the code the check was about. Removing
  the width policy AND the guard reproduces the reported bug at 14
  windows, and the first run is why there is now an unconditional
  assertion that no window is left off the strip.
- **`init_test.py`** -- init as pid 1 AND as a supervisor, end to end:
  the kernel spawns it, it holds pid 1, an idle init is BLOCKED rather
  than spinning, `kill 1` is refused, abandoned children are adopted AND
  reaped, the process table returns to its baseline, `SYS_SLEEP` passes
  its own checks, the target it read matches the settings REGISTRY, the
  desktop is init's child, a service that cannot start is given up on
  without taking the desktop with it, killing the desktop brings it back
  with no shell involved, and `After=`/`Before=` decide the order the
  services are spawned in, and `Ready=notify` makes `After=` wait for a
  service to be USABLE rather than merely spawned. **The readiness
  checks assert on dmesg TIMESTAMPS, not on log order** -- the desktop
  announces itself about 300 ms after it is spawned, and a service
  ordered after it must start at the announcement rather than the
  spawn, which is a difference a reading of the transcript cannot see.
  Its other half is the timeout: `/tests/notready` is a fixture that
  stays alive and never announces anything, so the barrier can be
  watched expiring and its dependent starting anyway -- the property
  that keeps a hung service from being able to leave the machine with
  nothing started. **The ordering checks are written against
  the order they create the files in** -- both groups of three demand
  the REVERSE of it, from opposite ends of the relation, so an init
  ignoring the keys would have to be handed a perfectly reversed
  directory listing twice to pass; a group whose expected order happened
  to match creation order would be no test at all. **The slot count is the assertion** for
  the reaping half -- a zombie nobody reaps is invisible until the table
  fills up, and every individual process behaves perfectly either way.
  Two fixtures are load-bearing: `/tests/orphan_test` must be started
  with `spawn` (it has to have a pid to orphan its children from), and the crash-loop fixture is
  a descriptor naming a nonexistent binary, written onto the disk COPY
  from the host so the machine boots with it already in place -- as are
  the three readiness descriptors, for the same reason: both barriers
  resolve in the first few seconds, and a descriptor that arrives after
  the boot has nothing left to observe. Two
  positive controls, in its docstring: `heir = 0` in
  `reparent_children()` reddens the two adoption checks, and dropping
  init's give-up reddens the crash-loop one while the desktop check stays
  green. **What it does NOT cover, and says so: the `target=text`
  command-line override**, which needs its own ISO -- `make iso
  KCMDLINE="target=text"`. Not in `gui_regress.py`; run it after
  touching the scheduler's parentage, reaping, `SYS_SLEEP`, the target
  setting, the service descriptors or readiness. **It currently fails 12
  of its checks, deterministically and pre-existing** -- see
  `docs/bugs.md`; a run that reports 19/31 is the known-good state, not
  a regression.
**`enter_gui()` TURNS THE LAYOUT LOG ON**, once, for every tool that
calls it. Toykit apps report their widget geometry so a test can drive
them by asking rather than by guessing pixels, and that report is OFF by
default (`desktop.layout_log`) because it is written every frame and
made `dmesg` unreadable on any machine with a window open. An app reads
the setting when it STARTS, so it has to be on before anything is
launched -- which is what that function is for. A tool that opens a
window without going through it will find its layout polls timing out.
It goes back off at exit only if this process turned it on, and only
through `atexit` -- a killed run on the bare-metal machines leaves it on,
where it costs real frames (`docs/testing.md`, beside "An app's layout
report is ON only after `enter_gui()`").
- **`terminal_probe.py`** -- **the GUI Terminal, asked what it actually
  does**: the editing keys, paging, scrolling and clearing. Written
  because answering three ordinary questions about it took FIVE invalid
  runs before one valid one, and every failure was in the harness rather
  than in the OS. It encodes all five: **a fixture smaller than one
  screen cannot test a pager** (the first attempt used a one-screen
  config file, so `less` exited at once and the space landed at a shell
  prompt -- `/tests/sample.txt` exists for this, with every line naming
  its own number); **the taskbar clock ticks once a second**, so any two
  full-screen captures differ regardless -- crop to the CONTENT rect the
  WM reports rather than computing one from a guessed title height;
  **`open_app` wants the entry's exact name** ("Terminal", not
  "terminal"), and a wrong one opens nothing and returns normally;
  **`send_text()` silently drops uppercase and most punctuation**, so
  `echo AB > /f` arrives as `echo  f` -- type through `gui key` instead;
  and **"the frame changed" is not a measurement**, because the caret
  BLINKS -- it reports the PERCENTAGE of the content area that moved,
  where typing ten characters is ~0.2%, a page turn is >20%, and a caret
  alone is ~0.02%. Without that number, `less` not paging at all reads
  as "changed".

  **Two kinds of probe, and the first is the one to reach for.** A
  keystroke that reached the shell and did the right thing leaves
  different BYTES ON DISK, so the keymap probes drive the Terminal with
  keys and assert through the FILESYSTEM -- which no redraw timing can
  fake, and which is how nine editing keys were confirmed working in a
  single run. Pixels are kept for the questions only pixels answer:
  whether the screen cleared (the discriminator is that what is LEFT is
  >90% background -- a clear and a one-line scroll both move a lot of
  pixels), whether it scrolled, whether a pager paged. Both halves lead
  with a CONTROL, and the control doubles as the scale for everything
  after it. `--keys` and `--pixels` run one half. Boots its own VM; not
  in `gui_regress.py`.
- **`kbd_test.py`** -- **`kbd`'s four columns, on both input drivers.**
  Boots twice (PS/2, then `--virtio-input`), types four keys chosen so
  each exercises a different part of the path, and asserts every column
  of the table against what each stage must say: `a` (the plain case),
  `Shift+a` (the same keycode producing a different character, plus a
  modifier key that produces none), `F5` (a `KEY_*` code rather than a
  character), and Up (an extended `0xE0` scancode, whose keycode is NOT
  its wire byte -- the one shape a naive tap gets wrong).

  **THE LOAD-BEARING HALF IS THE SECOND BOOT.** Reading back `1e 30 'a'`
  on PS/2 proves only that the tool can read its own kernel's ring; a tap
  that echoed the wire byte would pass it. The same keys on virtio-input
  must give the SAME keycode and the SAME character with the scancode
  column BLANK -- the input core's whole reason for existing, and not
  something an implementation that is not really reading each stage can
  fake. Stated as its own check at the end rather than left implicit in
  two passing lists.

  **It also checks the OFF state first, before anything arms the tap.**
  `kernel.kbdtap` is off out of the box, so that is the state the system
  ships in and the one that matters most: keys are typed with the tap
  off and the log must be empty, and `kbd` must SAY the tap is off
  rather than print an empty table -- an empty table is what a broken
  tap looks like too. Live mode is then checked from an off tap, so
  arming and disarming are asserted rather than assumed.

  Three positive controls were run and each reddened exactly its own
  claim. Dropping the wire byte (`key_event(..., 0, extended)`) failed
  the five scancode checks and the parity check, leaving every
  keycode/character check green. Making `kbdtap_produced()` a no-op
  failed the character checks in BOTH boots -- and the live-mode Esc
  check with them, which is the sign that check is genuinely coupled to
  the data it tests.

  **The third control fired NOTHING, and fixing that changed the
  program.** Deleting the privacy gate from `kbdtap_key()` -- so the
  kernel records while the switch says off -- left all 40 checks green,
  because `kbd --last` consulted the switch and returned before reading
  the ring. The in-kernel KTEST caught it on the right assertion; this
  tool structurally could not. `--last` reads the ring FIRST now and
  reports a non-empty ring under an "off" switch as a loud anomaly, and
  the same control then reddens four checks across both drivers with
  "10 rows" naming the leak. **A tool that consults a flag before
  looking at the thing the flag describes cannot check the flag.**

  Three harness traps are written into it, all paid for here. **`"ready"
  not in out` matches "al-ready running"**, so the readiness test passes
  on the one output that means the opposite -- matched as `"vm: ready"`.
  **A fresh slot per boot**, because this tool's own `QMPSession` leaves
  the QMP port in TIME_WAIT on the server side for about a minute after
  the guest dies, and `port_guard` correctly refuses the next boot on it;
  pinning both boots to one slot made a socket-lifetime problem look like
  a virtio failure. And **`ps` is read for STATE, not for the name** --
  the kernel shell's `spawn` does not reap, so earlier `kbd` zombies make
  "is kbd running" answer yes forever.

  Runs `kbd --last` through `run` rather than `spawn`,
  because a spawned process's stdout goes to the console framebuffer and
  never reaches the serial socket. Live mode is checked through the
  process table instead, for exactly that reason. Not in
  `gui_regress.py`: it boots twice and prints a table, so there are no
  pixels in it.
- **`keyboard_paths_test.py`** -- **the same keys do the same thing
  whichever driver reported them.** Boots the `text` target twice per
  input path (PS/2, then `--virtio-input`), on the `se` layout, and
  types two lines.

  It exists because that property was NOT true and nothing noticed. The
  layout tables were keyed on AT set-1 scancodes, so the input core had
  to translate evdev DOWN into that encoding for every non-PS/2 device
  -- a hand-kept table pointing the wrong way, with a hole at
  `KEY_102ND`, the ISO key that carries `|` on every Nordic layout. A
  pipeline could be typed on PS/2 and not on virtio-input.

  **THE ORACLE IS THE FILESYSTEM, NOT THE SCREEN**, and that is the
  reusable part. `touch /kb_probe.txt` proves `_` arrived -- which a
  screenshot cannot, because `_` draws NOTHING on the ring-0 console
  (its ink is below `line_h`), so a lost keystroke and an invisible
  glyph look identical. `echo x | touch /kb_pipe.txt` proves `|` arrived
  AND piped: without the pipe the same keystrokes are `echo x touch
  /kb_pipe.txt`, which creates nothing, so the file's existence is the
  whole discrimination. Deliberately no `>` in it -- mixing a redirect
  into a keyboard check makes a failure ambiguous between the two.

  Two harness lessons are written into it. A QMP qcode names a PHYSICAL
  key by its US label, so on `se` every punctuation key must be spelled
  out (`_` is Shift over the key US calls `/`). And **`altgr` is not a
  qcode** -- `alt_r` is; an invalid one is refused by QMP and sends
  nothing, which reads exactly like the guest dropping the key.
- **`debug_tty_test.py`** -- **the serial debug console is a terminal.**
  Attaches to a running guest and types raw bytes at the console's own
  port: `sh cat` reads a typed line, Ctrl-D ends it, Ctrl-C stops a
  `spin_test` that is confirmed still running, Ctrl-Z does NOT strand a
  command, a program that went raw does not leave the line raw, a job a
  command leaves behind (`tosh -c "counter_a &"`) prints NOTHING on the
  port once the prompt is back (it is confirmed alive across the
  window), `sh spawn`'s job is detached from the start, and a spawned
  reader does not take the next command's line. Its docstring lists a
  positive control per check, each verified. Not DebugConsole, because
  it has to speak while a command runs.
- **`ctrlc_test.py`** -- **`Ctrl-C` interrupts the foreground JOB and
  nothing else**, end to end through the real keyboard (stages 0-2 of
  `docs/signals-design.md`). Boots the `text` target twice, the same way
  `console_shell_test.py` does and for the same reasons, because a real
  ring-3 shell on the physical keyboard is the only place this feature
  exists -- the GUI Terminal reads keys as window events, owns no
  console and has no foreground group.

  Twelve checks, and four of them are the ones that matter. A **spinning
  job** dies: `spin_test` writes nothing and makes no syscalls while it
  spins, so the only path that can reach it is the timer-tick delivery
  in `isr_dispatch`. A **two-stage pipeline** dies as a UNIT, which is
  the check process groups exist for -- signalling the foreground *pid*
  instead of the group leaves one stage running and the shell waiting on
  it forever. The **shell survives**, which every other check here would
  pass without. And at an **empty prompt** the key is still a keystroke:
  the kernel must not signal, the byte reaches the line editor, and the
  line is abandoned -- asserted by typing a command, pressing Ctrl-C,
  pressing Enter, and requiring that the command did NOT run, with a
  control that runs the same line uncancelled so "the file is absent"
  cannot pass against a wedged shell.

  **Both halves were confirmed with positive controls.** Making
  `signal_char()` return 0 reddens the three job checks and leaves the
  empty-prompt ones green; replacing `signal_send_group()` with a
  single-pid send reddens exactly the pipeline check.
- **`jobs_test.py`** -- **job control end to end through the real
  keyboard**: `Ctrl-Z` suspends, `jobs` lists, `fg`/`bg` resume, `&`
  backgrounds, and a background reader is stopped rather than served.
  `ctrlc_test.py`'s sibling, deliberately the same shape: the two keys
  go down the same path in `kernel/tty/ldisc.c` and differ only in what
  they do at the end of it, so the tools differ only in what they
  assert. Boots the `text` target twice, for the same reason.

  Five checks carry it. **The job survives AND stops** -- surviving is
  the whole difference from Ctrl-C, but a "suspended" process that keeps
  running is what a missing check in the scheduler's picker looks like,
  and no process list can see it; the CPU column can, because nothing
  that is not scheduled can accrue any. **A pipeline suspends as a unit
  and is ONE job**, which is the check process groups exist for on this
  side too. **`fg` hands the terminal over**, asserted by Ctrl-C'ing the
  resumed job: a resume that forgot the terminal leaves the job running
  perfectly while the key goes to the line editor, which looks like a
  hung job rather than a shell bug. And **a background reader is
  stopped**, asserted twice over -- `cat &` ends up `stopped`, and the
  shell still runs the command typed after it, which is the half a
  process list cannot show. And **a finished background job is reaped
  with NO KEY PRESSED** -- the shell reports and reaps at a prompt, and
  until `SIGCHLD` reached a shell the only thing that produced a prompt
  was a keystroke, so a `&` job that finished while nobody was typing
  sat as a zombie until the next Enter. Asserted by PID rather than
  against an empty table, because the sections before it deliberately
  leave jobs behind -- including a STOPPED `cat`, which cannot be reaped
  at all until something continues it. Its own control is built in: the
  job has to be seen RUNNING first, or "no zombie" is also what a job
  that never started looks like.

  It also counts **zombies**, which nothing else here does: a `fg` that
  waits only for the stage whose status it reports leaves a resumed
  pipeline's other stages unreaped forever, and every other check passes
  while it happens.

  It reads `jobs` output by REDIRECTING it to a file and `cat`ing that
  back over the serial socket -- a builtin prints through the shell's
  own sink, which goes to the physical screen, and reading it back
  through the kernel's `cat` is an independent path to the same bytes.

  **Four positive controls, each reddening a different set.** Deleting
  the SUSP branch in `tty_ldisc_input()` reddens the stopped and
  pipeline checks while "the job is alive" stays GREEN -- correct, and
  why that check is not the interesting one. Removing `job_foreground()`
  from `fg` reddens exactly one: the Ctrl-C after the resume. And making
  `tty_check_background_read()` serve everybody reddens the SIGTTIN
  pair, with `cat` sitting in `block(key)` and the next typed command
  never reaching the shell at all -- which is the keystroke theft, shown
  rather than argued. And taking the `SIGCHLD` handler out of
  `/bin/tosh` reddens the no-keystroke reap and, downstream of the same
  cause, the zombie count -- while all twenty-eight other checks stay
  green, which is what says the new one is measuring something none of
  them could.
- **`console_shell_test.py`** -- a `text` boot reaches a RING-3 shell
  prompt and the kernel shell is not involved (`docs/init-design.md`'s
  stage 4). Eleven checks: init is what started `/bin/tosh`, an idle
  tosh is BLOCKED rather than spinning, a typed line spawns a program
  (asserted through the FILESYSTEM -- `file_test` writes
  `/filetest.txt`), a kernel-shell builtin typed at that prompt creates
  nothing, and Ctrl-D is followed by a fresh prompt rather than a dead
  console. Five more assert REDIRECTION -- `>` to a file, `>>`
  appending rather than truncating (running the same command twice
  must leave TWO copies, since a `>>` that silently truncated looks
  identical to a working `>`), `<` feeding `/tests/catin`, a
  BUILTIN redirecting (tosh's `ls` prints through the shell's own
  sink rather than fd 1, so this is the check that the sink swap
  works), and a failed redirect NOT running the command. Three assert
  PIPELINES -- two stages, three stages (so the loop is exercised
  rather than a special case for two), and a BUILTIN feeding one.
  Five more assert CONSOLE OWNERSHIP, which only exists on this boot --
  under a desktop nobody owns the console and every one of them would
  pass vacuously: `tty` names the shell as the owner, it has a
  foreground group (the invariant that makes Ctrl-C mean anything), the
  keyboard stood ring 0 down because a ring-3 process claimed fd 0 and
  NOT because a compositor took it, `ps` reports the shell as
  `block(child)` while it waits for the very job writing the file, and
  no blocked row reports a bare `block` -- which is what a state column
  that had lost the reason would still satisfy. Read back through a
  FILE, since a /bin program's output goes to the screen tosh owns
  rather than to the serial socket.

  **Two checks cover `dmesg` reaching the kernel log from ring 3**,
  which is the half the `#` prompt cannot prove: SYS_QUERY answers kernel
  callers too, so a check run there would pass against a build
  where a real scheduled process could not read the log at all. They are
  STRUCTURAL -- every klog line is `[<seconds>] text`, a stamp klog.c
  adds per logical line, and nothing else the shell could emit here has
  it. The first draft looked for a boot line instead and failed against
  a working `dmesg`, because `-n 5` tails the NEWEST lines and those are
  whatever the machine did a moment ago.
  Three more assert the SHARED LINE EDITOR at that prompt --
  Home+Delete editing mid-line, Ctrl-U killing a line before it runs,
  and Up recalling the previous command -- each through a filesystem
  round trip, so a redraw that merely looks plausible cannot satisfy
  them and an append-only editor fails all three. It BOOTS TWICE against
  a disk copy: the first boot sets `system.default_target text` and the
  US keyboard layout, the second is the one under test. Two traps it encodes. **The restart check is a
  second `init: started tosh` in the log, never a changed pid** -- pids
  used to be slot numbers, so a replacement could report the dead one's
  number; they cycle now, but the log line is still the evidence.
  And **`dmesg` is `sh dmesg`**: the debug console has no such command
  of its own, and the first version of this tool "read the log"
  successfully because the serial stream carries live klog lines, so
  recent lines were there and older ones were not. Its positive control
  is recorded honestly in its docstring -- disabling the stand-down
  reddens only the log check, because `keyboard_claim_console()` covers
  the steady state and what the gate removes is the ten-millisecond
  window before that claim exists. Not in `gui_regress.py` (it reboots
  and rewrites `/etc`); run it after touching init's services, the
  console claim, or `kernel/shell/apps.c`.
- **`ping_rtt.py`** -- the compositor<->client ROUND TRIP in
  microseconds, read from the ping every client is already sent
  (`gui compositor --json`'s `ping_us_last/max/avg`). Attaches to a
  running guest, opens three apps and waits for a dozen answers. It
  exists because stage 8 of `docs/winserver-ring3-design.md` moved a
  client's events off a kernel queue onto its own ring, and the one
  thing a kernel wake is plausibly better at is latency -- so the move
  carries a number on each side. Quote DIFFERENCES on one host; the
  absolute figure is a TCG guest's. On demand (`ondemand_sweep.py`).
- **`latency_under_io.py`** -- WHAT HEAVY DISK I/O DOES TO DESKTOP
  LATENCY, measured from both ends: the compositor's own `work`/`wake`/
  `ping` distributions (`gui latency --json`, the EFFECT) against the
  kernel's per-syscall stall table (`sh stalls`, the CAUSE). Attaches to
  a running guest, opens three apps, samples a quiet baseline, then
  spawns `/bin/diskbench` and samples again while it runs -- waiting on
  the benchmark's own `done` line rather than on a sleep, since a guest
  under load takes as long as it takes. It is the yardstick
  `docs/roadmap.md`'s interruptible-syscall work is measured against, so
  the point is a BEFORE and an AFTER on one host, never an absolute.
  Exit 1 only when it collected nothing; no threshold on the latency
  itself, because there is no baseline to pick one from yet.
  **`--load cpu`** swaps diskbench for `--spinners` busy `spin_test`s
  over `--load-seconds`: the yardstick for the scheduling classes, read
  as an A/B on ONE build with toywm's descriptor with and without its
  `CPUSchedulingPolicy=` lines (`vm.py put` the edited file, then a
  stop and start -- `vm.py reboot` refuses on a guest started without
  `--reboot`). The spinners are spawned by the desktop, so they are
  ordinary whatever toywm is, and are ended by pid.
  **`wake` is the number to read**: when another process holds the CPU
  the WM does no work at all, so every frame it eventually runs looks
  fast and only the gap between the wait it asked for and the one it got
  moves -- which is cyclictest's measurement, and it was confirmed here
  (`work` got FASTER under load while `wake` went up tenfold).
  **Give it a TSC clocksource or half the report is floor noise**:
  `python3 tools/lib/vm.py --instance N --kvm --cpu host,+invtsc start` is
  the only way to reach one in this environment, and without it the
  compositor's figures quantise to the PIT's 10 ms while the stall table
  -- which is TSC-timed in the kernel -- resolves either way. A reported
  granularity of **0 means the clock never advanced**, the worst reading
  rather than the best, and the tool says so in words. On demand
  (`ondemand_sweep.py`).
- **`fs_isolation.py`** -- DOES I/O ON ONE PATH SLOW THE FILESYSTEM DOWN
  ON ANOTHER? The yardstick for `docs/fslock-design.md`, whose stages
  are each a claim about who stops waiting for whom -- which
  `latency_under_io.py` cannot see, since the compositor's reads all
  land on `/`. Times a PROBE (`/tests/fslat_bench`: one `stat()` per
  tick, each call timed) on one path alone, then again while a
  `diskbench` LOAD runs on another, and prints count/avg/p99/max both
  ways. Defaults: probe `/tmp` (ramfs), load `/var/tmp` (the disk) --
  stage 2's claim; `--probe-path /etc` asks the same-volume question of
  stages 3-4. **THE PROBE MUST SLEEP BETWEEN CALLS** (`--gap-ms`, default
  1, which the timer rounds to a tick): a tight loop spends nearly all
  its time holding the lock, so under ONE lock it starves the load
  instead of waiting behind it, and the first version of this tool
  reported stage 1 as the isolated one. **It exits 1 if the load
  finished before the probe**, since that arm measured a quiet machine.
  Attaches to a running guest; use `--kvm --cpu host,+invtsc`. Compare
  two builds, never an absolute. On demand (`ondemand_sweep.py`).
  **`--during PROFILE` picks the load's phase**: diskbench starts with
  SEQ-write, which creates its file and so allocates, and a probe
  started at its first progress line measures mostly that -- fslock 3b
  (overwrites only) read as nothing there and ~200x faster
  `--during RND4K-write`. **It follows the LATEST progress line and
  requires the probe to END inside the phase** (exit 1 otherwise): a
  phase shorter than the probe -- SEQ-write of 256 MiB is ~1.5 s under
  KVM -- measures the next one. Size the load so the phase outlasts
  `--secs` (`--load-size 2048 --secs 6` for RND4K-write under KVM).
  **THE GUEST TIMES THE PHASE, not this tool** (2026-11-11): the probe
  runs as `fslat_bench --during P --log <the load's report>`, which waits
  for the phase in the report and prints `fslat: during P held yes|no`.
  Polling the report from the host every half second missed a whole
  RND4K-write under KVM, since `sh cat` answers late while the disk is
  busy. The report paths are short (`/tmp/fsi.*`) because that spawn
  line must fit the debug console's 128 bytes.
- **`part_test.py`** -- `/bin/part delete` against a GPT the HOST's
  `sfdisk` wrote, with what SaunaOS's whole-table writer cannot carry: a
  Linux-filesystem type, a non-ASCII name and the RequiredPartition
  attribute. The guest deletes partition 1; the host then reads the
  image: every other entry byte-identical in BOTH arrays, the disk GUID
  sfdisk's, `sfdisk --verify` clean, and in the guest the survivors keep
  their names (`p2`, `p3`). The refusals -- no `confirm`, a whole disk,
  a mounted partition, the system disk's -- change nothing. In
  `ondemand_sweep.py`; needs `sfdisk`. Positive control: naming a window
  by its position instead of its slot fails "the others keep their
  names".
- **`tfs3_grow_test.py`** -- `/bin/part resize` grows a FILLED tfs3
  volume, twice (48 -> 300 -> 700 MiB: one group to three, then three to
  six, where group 1's backup lies inside the old volume), then SHRINKS
  it to 150 MiB (six groups to two) after one folder leaves group 0 and
  another fills past it -- the kernel log must say over 1000 blocks
  moved, or the shrink had nothing to do -- on a second
  disk the HOST partitioned with sfdisk. Each claim is believed from a
  path that is not `part`'s output: the host reads the GPT and the tfs3
  superblock inside the partition back; `fsck` is clean; every file
  verifies (`mkfiles --verify`) and two hash as before (`sum`); 200 more
  files go into the new space; a remount. The refusals -- mounted, a
  shrink below what the files need, exFAT, the system disk, no
  `confirm` -- change neither the table nor the superblock. In `ondemand_sweep.py`; needs `sfdisk`.
  Positive controls (mutate.py): `part` with its filesystem step removed
  fails the host's superblock check; a grow that keeps the old last
  group's tail marked in use fails fsck. A grow or shrink cut short at
  each write, and inodes renumbered out of a dropped group, are the
  KTESTs' (`kernel/fs/tfs3_resize_test.c`).
- **`fat32_grow_test.py`** -- `/bin/part resize` grows FAT32 volumes
  ANOTHER SYSTEM made, then shrinks one: the host partitions a second disk with sfdisk and
  fills it with mkfs.fat and mtools (files in holes, `Käse.txt`,
  `日本語.txt`, a deep directory). p1 grows 64 -> 200 -> 400 MiB, each a
  SHIFT (its FATs grow); p2 is sized by the host so its FATs have spare
  entries, filled to its last clusters, and grown by exactly those -- IN
  PLACE (the host sees the same FAT size and data start). After each
  grow the host extracts the partition: `fsck.fat -n` clean and naming
  no boot-sector/backup difference (it reports one with exit 0, so the
  text is checked), the backup boot sector compared byte for byte,
  every file copied back out with mtools and hashed, the long names
  listed. The guest lists the names, writes more than the old volume
  had free and verifies it, and fsck.fat is clean over SaunaOS's writes
  too; a remount and a second grow. Then p1 SHRINKS 400 -> 280 MiB with
  the guest's files at its top (a 220 MB pad written under them, then
  deleted): fsck.fat clean, the host's files byte-identical, the moved
  files verified in the guest, the FATs and data start unchanged; below
  the 65525-cluster floor it is refused. A shrink with directory
  patching disabled fails the KTESTs (0 of 45 files read back, seen
  2026-10-30). Refusals: mounted, a shrink below what its files need, a
  volume marked dirty (p3, set before boot -- a write under a running
  guest sits behind its block cache), and the no-journal warning before
  `confirm`. In `ondemand_sweep.py`; needs sfdisk, dosfstools, mtools.
  Positive controls (mutate.py on `kernel/fs/fat32_resize.c`), each
  seen red 2026-10-29: the grown FATs' new sectors left unwritten
  (`if (0 && nfat > F)` in `write_fats()`) fails fsck.fat; the lowest
  cluster of each run left unmoved (`(lo - 1) * spc` in `move_data()`)
  fails fsck.fat, mtools and the guest's listing; FSInfo given the old
  free count fails fsck.fat on both paths; the backup boot sector left
  unwritten fails both backup checks on both paths. The KTESTs in
  `kernel/fs/fat32_resize_test.c` cover the same on RAM volumes, a
  4K-sector one included.
- **`disks_resize_test.py`** -- Disks' Resize dialog (R1) on a guest of
  its own, beside a host-partitioned disk: the dialog's parts all lie
  inside its box and do not overlap (the overflow rule, from Disks'
  `rz*` layout lines), the fields start at the size and the room after
  it, the used part is hatched and the handle the accent (pixels, with a
  scrim pixel as the control), a real drag moves the handle and the
  fields follow, a typed size and Enter QUEUE the resize with the disk
  untouched (lsblk) and Apply runs it (lsblk, fsck, `mkfiles --verify`),
  FAT32 gets the bar with its no-journal note, Apply's confirm repeats
  it, and a typed 64 MiB grows it (lsblk, fsck), and exFAT gets a
  reason, not a bar. In
  `ondemand_sweep.py`. Positive controls: no hatching fails "hatched"; a
  label row wider than the box fails the inside-the-box check (a body
  merely NARROWER does not: the dialog's own text keeps the box wide).
- **`disks_test.py`** -- Disks on a guest of its own: the system disk
  pinned as the root and a BLANK 256 MiB virtio disk beside it, in
  `ondemand_sweep.py` (it boots its own hardware). On the system disk
  Format and Mount do nothing and Delete says why, and Check shows its
  stages while a pass runs and Escape (the dialog's Stop) ends Disks'
  own pass -- REPAIRS of `/` (`fsck -r`, unstoppable) are queued ahead
  of it so a key can land in time, since `/` alone takes half a second.
  Positive control: without Disks' warm-up of its progress drawing, the
  Stop never lands. On the blank one a
  new GPT and partition (believed from `lsblk`), mount (from `df`, and
  a file written through it), Check, unmount, a second partition, a
  format to FAT32 (believed because it then MOUNTS as fat32), a delete;
  then the HOST reads the GPT: the first partition is still 'data' at
  LBA 2048, kept across two table rewrites. Positive control: a
  rewrite that drops the entries it should keep fails "the first kept".
  Every change QUEUES (`base/udiskops.h`) and is believed both ways:
  nothing on the disk while it waits (lsblk), there after Apply. Remove
  on a queued create takes its format too, Undo takes a Delete back with
  nothing deleted, and a partition mounted behind Disks' back makes its
  queued Format fail and the Delete after it NOT run (df, lsblk). A
  mounted partition shows the Note's Unmount (which unmounts it), and the
  REAL pointer resting on the greyed Format brings its tooltip with the
  reason line (`toolbar.tip`/`toolbar.tipnote`, uui_toolbar's describe). Eight
  steps queued run `scroll_test.check_view()` on the pending list. The
  queue's own rules on fixture disks are `udiskops_test` in
  `usertest_run.py`.
- **`cursor_ibeam_test.py`** -- named pointer shapes (~20 checks): a
  client naming its own (`WIN_REQ_CURSOR`), the compositor's clamp, and
  the one shape the compositor raises by itself. It covers all four ways
  a shape is named -- a widget's ops table
  (`uui_textbox` in UI Demo, with no app code at all), an app's own
  `uapp_set_cursor()` (Notepad's document), a window-wide constant set
  at open (the Terminal's grid), and the WM's own modal chrome
  (Notepad's Save-as field) -- with a control point beside each: the
  dropdown next to the textbox, the menu bar and scrollbar and status
  bar around the document, the dialog's panel below the field.
  Two things worth knowing before editing it. **The two shapes are told
  apart by where they sit RELATIVE TO THE HOTSPOT**, not by size or by
  ink: the arrow is drawn from the pointer down and right (bbox starts
  at 0,0), the I-beam is centred on it (bbox starts near -3,-8). That is
  what makes a wrong shape unable to pass as the right one, and a
  MISSING one fail both tests rather than one. And **the sprite is
  isolated by a THREE-frame diff** -- away, here, away again, keeping
  only pixels that differ from both away-frames -- because the
  two-frame version reported Notepad's menu bar as an I-beam when the
  menu title under the pointer redrew its hover. Its load-bearing check
  is the clamp: the same window's TITLE BAR, and the taskbar with the
  window deliberately dragged UNDER it, must both read as arrow, which
  a compositor that simply believed the client would fail while every
  other check still passed.
  Its last two phases are the BUSY pointer, and they are built to keep
  its two sources apart. `/tests/hangclient`'s `b` key names
  `WIN_CURSOR_WAIT` and keeps pumping, so the tool asserts the busy
  shape appears WHILE the WM still considers the window healthy --
  without that pairing, "busy appeared" would equally mean the app hung.
  Then `h` wedges it and NOBODY ASKS IT TO CLOSE, which makes that phase
  the ping cadence's test as much as the cursor's: before
  `WM_PING_INTERVAL_DEFAULT` existed, `not_responding` could not become
  true without a close attempt and the check fails outright.
  It shortens both `gui pingtimeout` and `gui pinginterval` (detection
  is interval + timeout, not either alone) and sets them back.
  On demand, not in `gui_regress.py`.
- **`console_bleed_test.py`** -- the kernel console vs the desktop (5
  checks). A ring-3 process writing to fd 1 reaches the framebuffer
  console, which draws into its own back buffer and then BLITS THAT OVER
  THE WHOLE SCREEN -- so `dmesg` covered 100% of the desktop and DOOM's
  startup banner was how it got noticed. TWO properties, and a fix that
  only stopped the console DRAWING would pass one and fail the other:
  nothing bleeds through while the desktop is up, and the accumulated
  text is still there once it goes away. The second is checked against
  the console as it looked BEFORE the desktop started -- "is there any
  text" would pass vacuously, since the boot log is already on that
  screen. The bleed check carries its own control (a real window opening
  MUST move the same pixels), for the reason `idle_desktop_test.py`
  exists. It kills the desktop rather than driving the Start menu, since
  both reach `compositor_gone()` and one of them cannot fail for reasons
  this tool is not about -- and it DELETES `/etc/services.d/toywm` first
  so init cannot restart it mid-check, without restoring it (there is
  nothing in the guest to copy it back from; `make iso` re-seeds it).
  On demand, not in `gui_regress.py`.
- **`font_test.py`** -- runtime fonts end to end (~33 checks): `/bin/fontd`
  rasterizes and publishes a `.ttf` from `/usr/share/fonts`, switching
  faces reaches the screen with NO restart, a size change re-rasterizes,
  and the baked font still draws when the face is `builtin`. **EVERY WAIT
  IS ON fontd's GENERATION** (`diag font`), and then on the compositor's
  `font_gen` in `gui state --json` reaching it -- the `wm: font changed`
  line fires on `WIN_EV_FONT`, before fontd republishes, and a capture
  timed by it measured the previous face. It restores the settings it
  changed and closes Font Demo, so consecutive runs agree. Its second half opens **Font Demo** (now a font
  previewer) and asserts on the numbers that app measures for itself:
  that bold is distinct from regular and that kerning TIGHTENS a sample
  rather than loosening it (both on the SESSION face) -- and the
  interactive half a static demo cannot have: the previewer's size ladder
  loads on open, and DRIVING ITS DROPDOWN to another family renders a
  genuinely different face, proven by the pangram's width changing. The
  bold/kerning checks run on `liberation-sans` deliberately -- on the
  default monospace face bold has the regular advances and there is no
  `kern` table at all, so every one of them would pass vacuously.

  It also PROBES THE DEMO'S PIXELS, and the reason is the lesson: the
  measurement checks above all passed while the demo rendered every
  letter as a hollow outline, because they read numbers out of the
  mapped atlas and nothing looked at the screen. The probe asserts the
  window's DOMINANT colour is the panel background -- not an ink count,
  which was tried first and passed the broken build by a wide margin
  because a black background counts as ink. Read its docstring before editing, for the same reason
  `cursor_theme_test.py`'s says so: the baked font is a complete working
  fallback, so "text is on screen" proves nothing at all -- every check
  is a DIFFERENCE between two states. The load-bearing one is that
  `liberation-sans` (proportional) draws the same right-aligned text
  starting ~50px further right than `dejavu-sans-mono` does; a build
  that ignored per-glyph advances passes every other check in the file
  and fails exactly that one, which was verified by making it do so.
  AND EVERY FONT CHANGE WAITS ON THE COMPOSITOR'S OWN LINE, never on a
  sleep: `set_face()`/`set_size()` go through one helper that waits for
  a `wm: font changed` that is NEW since the spawn. The three hops
  between a setting and a repaint (a short-lived ring-3 process, the
  kernel, the client) take a load-dependent total, and a fixed sleep let
  a change land AFTER the drain that followed it -- so the next wait
  returned the PREVIOUS face's cell and two different faces compared
  equal. The sharper half is that a change still in flight is observed
  by the NEXT call's wait: `set_size(14)` then `set_face("builtin")`
  returned on the size's line and measured the old face, which reads
  exactly like "switching to builtin did nothing".

  Two measurement traps it encodes: measure the RIGHT-ALIGNED version
  text rather than the desktop icon captions (those are clipped to the
  icon cell, so a narrower font mostly just un-truncates them and moves
  three pixels), and crop the frame yourself -- `stable_pixels()` writes
  the whole screen and only COMPARES the box, so scanning its output
  counts the taskbar as ink.

  **Its last section drives `/bin/font`**, the glyph probe -- both
  views, the hash agreement between them, the descender flag on `g`, and
  the discriminating pair that gives the whole thing teeth: `g` has ink
  and a SPACE does not. A probe that always answered "ink yes" passes
  every other check in that section; one that always answered "blank"
  passes the space check alone. It reaches the space by CODEPOINT
  (`0x20`), because a space cannot be passed as an argument through any
  shell here. Two mechanics worth not rediscovering: the output is
  captured through a FILE (`tosh -c ... > /path`), because a spawned
  process's stdout goes to the console it inherited and `gui spawn`
  hands the debug console back only its own "spawned as pid N" line --
  a probe asserting on that would pass on any output at all; and the
  path is unique per call, since a shared one read back after a failed
  spawn returns the PREVIOUS call's output, which is the quietest way a
  check like this can lie.

  **It is in `ondemand_sweep.py`, NOT in `gui_regress.py`, and that is
  the point of the move.** Several of its checks compare what the KERNEL
  reports for a glyph against what a CLIENT draws and require a match --
  which stopped being true when the kernel's font path was deleted, so
  they are red BY DESIGN and no build can turn them green.
  `docs/bugs.md` carries the owed rewrite (ask fontd what the session
  font is; wait on the beacon rather than on `WIN_EV_FONT`). Until then
  it is off the standard gate rather than dropped, because the ~20
  checks around them still test real things -- a gate that always cries
  wolf gets ignored, which is why per-push CI was retired here too.
- **`serial_backpressure_test.py`** -- a COM1 consumer that stops
  reading must not stop the MACHINE. It attaches a client to the serial
  socket, goes deliberately deaf, asks the guest for several KB
  (`sh dmesg`), and requires the taskbar clock to keep ticking. Written
  after the entry `docs/bugs.md` carried as "a filesystem write during
  the desktop's STARTUP stalls the clock", which was neither the
  filesystem's fault nor the compositor's: `serial_putc()` waited
  unbounded on a THRE bit that QEMU's socket chardev stops setting when
  its peer stops draining, so the kernel spun there **with interrupts
  still on** -- ticks advancing, PIC clean, scheduler still picking the
  compositor, nothing running. The write only supplied the log volume.
  **Two things to know before editing it.** Its positive control is in
  the docstring and fires on exactly one check (1 distinct image in 8
  against 4-5), which is what makes it a test rather than a
  demonstration. And **do not make it drain the socket to "fix" a
  failure** -- being deaf IS the fixture, and a drainer makes it pass
  against a kernel that still hangs. Run on demand; it needs a guest
  started by `vm.py` (a socket serial, not `-serial file:`, which never
  applies backpressure and so cannot see this at all).
- **`many_files_test.py`** -- every list that used to be a fixed table,
  filled past it: a dash script (`vm.py put` + `exec`) seeds folders, and
  each app's own count line must report all of it -- 12 Terminal tabs,
  each with its shell on its own pty (the kernel held 8 ptys), Image
  Viewer's 80 images, Video's 210, the Audio Player's 140 tracks, Crash
  Reports' 70+, Help's 300 extra pages in 40 categories and 20 folders,
  Notepad's and the Terminal's find over 3120 matches, the File
  Manager's folder tree past 96 rows and its search's 431 hits. The
  seed files only PROBE as their type (an 8 KiB video head, a crash
  report's header and `program:` line). Too slow for `gui_regress.py`
  (the seeding is ~850 spawns under TCG), so it is in
  `ondemand_sweep.py`; its positive control is the build before, where
  every count stops at its old cap.
- **`filemanager_test.py`** -- the File Manager: two panes, marking, and
  real file operations, plus the Explorer chrome -- the breadcrumb's
  segments and Ctrl+L field, Places and Devices, the search filter, the
  details pane and the status bar. **Buttons are addressed by NAME**
  (`BTN`, one table mapping a name to its strip -- the nav buttons, the
  command bar or the status bar's view switch -- and its index there),
  and drop-down rows by the row rects the menu itself logs
  (`view_pick()`), never by pitch. **A context-menu row is picked BY
  LABEL** -- `ctx_mark()` before the right-click, `ctx_rows(mark)` reads
  the app's `files: ctxmenu rows a|b|-|c` line from the transcript after
  it (`_collect()` drains every `files:` line, so a plain log read can
  miss it), `ctx_pick()` clicks that label's logged rect -- because
  Cut/Copy/Rename/Delete lead every file's menu as a strip and move the
  rows below; `pins_gui_test.py` and `launch_test.py` use the same pair. F7, F2 and Ctrl+Shift+N are checked
  as IN-PLACE edits (the app's `renaming` line); the Options window is
  driven to "Rename in a dialog" to check that F2 then asks, and back to
  Defaults for what follows. **Run by `ondemand_sweep.py --only files`, not
  `gui_regress.py`** (moved 2026-09-29): it is the slowest GUI tool by far
  and carried two standing reds, so run it after touching the File
  Manager or `userland/apps/apps/files/`. **A check runs only after its prerequisite**:
  the view toggles are driven through the View drop-down (`view_pick()`),
  which requires the button's and the row's reported rects, and a step whose prerequisite failed is
  printed as `SKIP` with the reason rather than attempted -- the
  two-pane, tree-hidden view is restored (and verified) before anything
  later runs, and the run stops with a failing summary if it cannot be
  -- including when the context menu was never confirmed closed, since
  an open menu takes the press wherever it lands and a restoring
  toolbar click would dismiss it rather than toggle anything.
  **It puts both panes in Details first** (from the status bar's view
  switch, which is itself a check): the app opens in icons view, and
  every row this tool aims at is row-height arithmetic. **The result of every operation is checked
  through `ls`, not through the app** -- the manager is the thing under
  test, so it cannot also be the witness; a version that updated its own
  list and spawned nothing would pass its own view. Four checks are
  shaped so a broken version cannot survive them. **Delete is a PAIR**:
  F8 then Esc must leave the file on disk, and only then does F8-then-
  Enter remove it -- the first half is what fails on a missing
  confirmation. **The marked copy requires the UNMARKED file to be
  absent**, or "it copied everything" would pass too. **Backspace must
  also select the directory it just left**, which only a correct `up()`
  produces. And **the active-pane pixel check samples the OTHER pane's
  header as its control**, so a paint that marked both would fail. It
  drives the app with `gui key <code>` rather than QMP keystrokes, so
  nothing here depends on the guest's keyboard layout. Two harness traps
  it paid for: a batch of log lines can hold SEVERAL frames, so the
  parser takes only the last (reading each field's last occurrence
  across the batch mixed one frame's `active` with another's
  `selected`), and characters typed with `settle=False` outrun the
  client so a name field commits empty -- which reads exactly like a
  broken `mkdir`. **It closes other windows between phases but never a
  POPUP**: a tooltip is the app's own, and closing it from outside once
  left the compositor's "not responding" box over the window, eating the
  next click (uapp now destroys a popup it is asked to close).

  **Its type-ahead phase is 4b, deliberately not renumbered in**:
  inserting a numbered phase would have renumbered eight, which is this
  repo's docs-edit hazard applied to a test file. It also forced phase 5
  to establish its own selection with Home rather than inheriting the
  phase above's -- the insertion broke it on the first run, which is the
  argument for the rule rather than a reason to move the phase.
- **`diskmark_test.py`** -- drives the **Disk Mark** GUI benchmark and
  asserts on it, then on its run chart (samples and four phase marks),
  its history (the run is a line in `/var/lib/diskmark/history` and F9
  shows the table) and Stop (Esc kills, reaps and cleans up a second
  run). Then the TEST SIZE without running one: Custom... opens a
  dialog bounded by the volume's room, `3`, `g`, Enter sets 3 GiB and
  `/etc/diskmark.conf` keeps it across a restart, a size over the room
  is refused, and the first preset puts it back to 16 MiB so no later
  tool inherits a GiB run. Then the LOCATION: Choose folder... opens the
  chooser at Documents, a folder with a SPACE in its name is chosen,
  remembered and RUN IN (the path reaches diskbench whole), deleted it
  is refused at Run, and at the next start it and a folder in RAM are
  dropped from the file; `diskbench` itself refuses `100T`
  (`does-not-fit`) and `1Gx`. **The load-bearing check is the NUMBERS, not the run
  finishing**: a build whose throughput arithmetic truncated to zero
  logged "all four passes complete" and drew four tiles reading
  `0.0 MB/s`, with correct IOPS beside them, so everything an "it ran"
  check looks at was green.

  **It also covers the spawn, the report file and the reap**, since the
  app does no I/O itself -- it runs `/bin/diskbench` and polls what that
  writes.

  Two more that earned their place the same way. **The results must be
  DRAWN, not merely logged** -- each tile compared as pixels against its
  own before-image, which is the Calculator-with-invisible-buttons
  failure. And **the title bar is the CONTROL**: it must NOT change. That
  one immediately found a real bug -- a 4 MiB slice took over a second
  on an emulated disk, so the client missed the compositor's pings and
  ran the whole benchmark reading `Disk Mark (Not Responding)`. The fix
  was to bound a slice in TIME rather than in bytes, which self-tunes to
  the device.

  Geometry comes from the client's own `diskmark: layout ...` lines,
  never re-derived here -- hand-computed coordinates cost two
  build-and-test cycles while the app was being written. On demand
  rather than in `gui_regress.py`: it does real disk I/O for tens of
  seconds, and a benchmark run beside eleven other VMs measures
  contention. `blank_window_test.py` covers "it draws" in the gate,
  since that one opens every app in the registry.
- **`doom_sound_test.py`** -- **DOOM's effects and music, judged on the
  HOST**, and the only thing that exercises `userland/third_party/doom/glue/dg_sound.c`,
  `dg_music.c` and `opl_saunaos.c` at all. On demand twice over: it needs
  an IWAD (not in the repository) and boots its own AC97 guest, and it
  SKIPS cleanly without a WAD.

  **Its load-bearing check is the PAIR OF BOOTS, not either one.** Doom
  has two independent audio paths and one recording cannot separate
  them -- music-only and effects-only builds both make noise. So it
  plays the attract demo normally and then again with `-nomusic`, and
  requires COVERAGE to collapse: OPL music is continuous, so run 1
  fills nearly every 50 ms window, while run 2 is the same gunshots
  with silence between them. If music were dead, run 1 would already be
  bursty; if effects were dead, run 2 would be silent. Neither failure
  can satisfy both. `-nomusic` reaching the game is why
  `userland/apps/apps/doom/doom.c` forwards its argv.

  What it cannot see: whether the music is the RIGHT music. Nothing
  checks pitch or melody against the WAD's MIDI, so a wrong instrument
  bank or a transposed OPL passes.
- **`audio_test.py`** -- AC'97 and the PCM stream, judged on the HOST:
  the guest plays two seconds of A440 (`/tests/tone`) and QEMU's wav
  audiodev (`vm.py --audio-wav`) records what the DEVICE emitted to a
  host file, where the frequency is measured by zero-crossing count --
  an oracle sharing no code with the driver, the core or the tone
  generator. Three failure modes fail three different checks: a dead
  DMA engine records silence, a wrong rate the wrong pitch, and a
  broken consumed-chunk zeroing more than 2s of tone (the app's own
  STOP bounds that leak, so the `sound` KTEST is the first-line guard
  -- it reddened under the positive control before the recording did).
  **Measure within BURSTS**: under TCG the guest falls behind wall
  clock and QEMU pads the recording with host-side silence, so the
  file's timeline says nothing about the guest. The ac97 KTESTs run
  un-skipped only here, which makes "0 skipped" the load-bearing
  assertion (the ahci lesson). On demand, not in the gate: it boots
  its own guest with extra hardware.

  **It boots TWICE, and the second boot is the sharper half.** One
  recording cannot hold two tones and be judged by frequency, so the
  WAV phase gets its own: the guest runs `/bin/aplay /tests/sine1k.wav`,
  which is the whole of `userland/libs/media/usnd.h` -- the RIFF chunk walk,
  the 44.1 -> 48 kHz resampler, the mixer and the sink. The fixture is
  **44.1 kHz on purpose**: a build that did not resample at all would
  play its 1 kHz tone at 1088 Hz, and the 30 Hz tolerance is set to
  reject exactly that while accepting the interpolator's own error.
  Nothing else in this repo can see a broken resampler -- `/tests/
  usnd_test` checks frame counts, which a wrong-rate build gets right.
  **`--card hda` runs the same three boots against QEMU's
  `ich9-intel-hda` + `hda-output`** and expects the `hda` driver and
  KTESTs; `ondemand_sweep.py` names both rows. The oracle found the
  HDA driver's first real bug in one run -- an amplifier-capability
  field read in the wrong order set "0 dB" to -53 dB, and a tone at
  4% amplitude measured as silence.

  **Its last boot is FOUR RESTARTS, and it asserts on a hole's SHAPE.**
  Each chime is played only once the log says soundd released the card,
  so every one is a fresh open and START. The mixer used to fill the
  whole ring on that first pass whatever the client had produced, and
  the recording showed a hole inside the chime of exactly 2, 8, 10 or 12
  chunks. TCG's padding is the reason not to assert "no gaps": the check
  fails only on a gap that is a WHOLE NUMBER OF CHUNKS (10.67 ms), which
  the mixer makes and TCG does not. Measured 2026-09-23: red 2 runs of
  2 with only soundd's fill rule reverted (holes of 21.3-298.6 ms, every
  one a whole number of chunks), green 3 of 3 on the fixed soundd.
  **`--only restart` runs that boot alone**, about a minute instead of
  the whole tool, which is what makes a positive control affordable.
  Each chime prints how long the release wait and aplay took, so a stall
  names its step instead of reading as the tool hanging.

  **IT SETS THE MASTER VOLUME ITSELF** (`establish()`), because its
  image is a copy of whatever `disk.img` holds. A `volume=25` left there
  by an earlier run turned ten checks red with nothing wrong in the
  build (the AC97 driver played 25% as silence then). A
  failure here that the old build does not share should be retried on a
  `make clean-disk` image before it is believed.
- **`soundd_test.py`** -- the sound daemon, judged on the HOST by the
  one question no in-guest check can answer: are TWO programs audible at
  the same time? A mixer that silently served one client and dropped the
  other passes every "did it crash", "did aplay return 0" and "is the
  service running" check there is. So the guest plays `sine1k.wav` and
  `sine440.wav` from two separate processes AT ONCE and the host
  requires BOTH frequencies in the one recording.
  **Zero crossings cannot do this** -- two mixed tones cross zero at
  neither of their frequencies -- so it measures energy at each
  frequency with a Goertzel filter, and samples a THIRD frequency
  nobody played as the control: if 700 Hz reads as loud as 440, the
  measurement is noise and the two real readings mean nothing. It
  measures the loudest half-second rather than the whole file, for
  `audio_test.py`'s reason (TCG pads the recording with host silence).
  **`--positive-control` is built in**: it stops the daemon first, so
  the second `aplay` gets -EBUSY and only one tone can reach the card.
  That run MUST show one frequency and not two -- it is what proves the
  measurement can distinguish the states at all, and it reproduces the
  exact bug the daemon exists to fix. On demand, not in the gate.

  One trap it encodes: **no trailing `&`**. `spawn` at the ring-0 shell
  already returns as soon as the child exists and that shell has no job
  control, so an ampersand arrives as a second ARGUMENT -- it failed
  once as "cannot open file", with the daemon working perfectly.
- **`audio_rate_test.py`** -- a card's rate follows what plays, on
  QEMU's HDA (`--card ac97` for the AC97's variable rate), judged on the
  HOST: `/tests/sine1k.flac` (1 kHz, 1.5 s, 44.1 kHz) must record at
  exactly 1000 Hz and 1500 ms whether the card switched to it (the
  default) or was held at 48 kHz (`sndfmt -r 48000`); a 48 kHz file
  joining a 44.1 kHz one must not move the card; with soundd stopped the
  program's own sink decides the same way. A wrong rate is 1088 or 919
  Hz. **Pitch by INTERPOLATED zero crossings over one continuous run** --
  `audio_test.py`'s `measure()` counts within 50 ms windows and reads ~1%
  low, too coarse here. The no-daemon run's length is only reported: its
  start loses ~50 ms on the commit before this too (docs/bugs.md). On
  demand (`ondemand_sweep.py`: `audio_rate`, `audio_rate97`).
- **`usb_audio_test.py`** -- USB Audio Class 1.0 playback, on the same
  host-side oracle `audio_test.py` uses, pointed at a different bus. The
  guest plays A440 through an isochronous OUT endpoint and QEMU's wav
  audiodev records what the DEVICE emitted; a configured endpoint that
  never gets a packet records silence, a wrong packet size records the
  wrong pitch. **Three boots, and the middle one is why `vm.py` grew a
  second recording.** With `--audio both` each card writes its OWN wav
  file, so "the tone is in the USB file and not the AC97 one" is an
  assertion -- on one audiodev the file holds their mix and device
  selection is untestable. Its positive control (making `sound_select()`
  ignore the name) put **4.05 s at 436 Hz in the USB file and 0.00 s in
  the AC97 one**, which is exactly the signature a working selection
  cannot produce. It also unplugs the device mid-run (`device_del`),
  which is a state no PCI card can reach, and checks that the choice
  came back from `/etc` on a reboot -- from the FIRST LINE of `config
  get`, since boot chatter mentions `ac97` too and a substring match
  passed on a guest whose setting had not been restored at all. On
  demand, not in the gate: it boots its own guest with extra hardware.

  **It reported "the emulated device plays nothing" (filed 2026-09-21),
  and the device played.** `/tests/tone` opens the stream directly, soundd holds
  it from boot until 2 s after its last client, and the tool spawned the
  tone without waiting -- so the app was refused, wrote no verdict, and
  the device recorded silence. It waits for the release now
  (`wait_card_free()`), as `audio_test.py` always had to. Two more
  staled with the OS: a USB DAC registers as `usb-<vid><pid>` since
  f57e1134 (`USB_DEV`, QEMU's being 46f4:0002), and the tool sets volume
  100 on its image copy, `audio_test.py`'s `establish()`.
- **`audio_loopback_test.py`** -- **the only tool here that judges the
  SOUND rather than the driver.** A cable from the Creative G6's
  headphone out into the motherboard's line in, so the analogue output
  of a passed-through DAC can be recorded and counted. Everything else
  in this section measures what the driver DID; this measures what came
  out. It exists because `docs/bugs.md` records `usbaudio.so` as
  "crackles", which is not a measurement, in an entry that is itself a
  warning about trusting the wrong number -- packets per second went UP
  while the audio got worse.

  **How it counts a crackle.** A sine obeys `x[n] = 2cos(w)x[n-1] -
  x[n-2]` exactly, so every sample is predictable from the two before
  it and any break in the stream -- a gap, a repeated packet, a torn
  buffer -- shows up as a residual spike. It reports DROPOUTS (the
  envelope collapsing: the ring ran dry) separately from CLICKS (the
  waveform tearing while the level holds: the samples were there and
  wrong), because those have different causes.

  **`--selftest` is the positive control and runs by default**: the
  HOST plays a tone and records it, proving the loop carries signal
  before any guest measurement is believed. It doubles as the
  analyser's NEGATIVE control -- a known-clean tone must report zero
  dropouts and zero clicks, so a glitch count on a guest capture is
  real rather than a detector artefact. That control earned its place
  immediately: the first run reported one dropout and one click on a
  perfect signal, because the capture's leading and trailing silence
  and the tone's own hard edges were being counted. Run the selftest
  BEFORE passing the G6 through -- once QEMU holds the device the host
  cannot open it for playback. Then `--record SECONDS` captures while
  the guest plays, and `--analyse WAV` re-reads a capture later.

  **`--depth` is the 24-bit proof**, and the one measurement of the
  s32 path no emulated card can make. A guest owning the G6 plays a
  1187 Hz tone at -100.8 dBFS -- 0.3 of one 16-bit step, which a 16-bit
  path rounds to EXACT SILENCE -- beside a calibration tone 20.8 dB
  louder and the quiet tone's 16-bit twin. A Goertzel filter over 14 s
  of 32-bit capture digs the tone out of the line-in's hiss; it must
  stand 10 dB clear of the twin, and a -90.8 dBFS leg must land within
  3 dB of the calibration's prediction (the chain is linear there).
  How far the -100.8 tone falls from linear is REPORTED, not asserted:
  it wandered from -4.9 to +5.4 dB over four runs while -90.8 stayed
  within 1.2 dB -- that close to the noise one capture is not a level
  measurement. The positive control (the USB driver narrowing to 16 bits
  inside the 32-bit slot, its memcpy fast path off) sends the quiet tone
  to -127 dBFS and reddens both assertions. **Not 1 kHz**: with the G6 attached the line-in carries lines
  at the USB frame rate's harmonics, -94 dBFS at 1 kHz in digital
  silence. The guest's usb-audio bind line is printed first, so the result
  says which alternate setting it went through.

  **It SKIPS when the loop is open, and that is deliberate.** The cable
  is not normally connected; an absent cable and a silent guest are
  both flat ADC hiss, so failing would convict the OS of a fault in the
  test rig. Two traps it encodes, both measured: the ALC892 comes up
  with **+30 dB `Capture` AND +30 dB `Line Boost`** -- 60 dB on a line
  input fed by a headphone amp, which clips into mush and reads as a
  broken cable -- and every capture opens with a **-26 dBFS ADC
  start-up transient decaying over ~0.7 s**, louder than the tone's
  noise floor, so anything hunting for a level finds it first. The
  measured baseline of the rig at unity gain (G6 `Speaker` 107) is
  0.02% THD, 70 dB channel separation and a ~60 dB SNR ceiling set by
  the ADC, not by the G6. `local_info.txt` has the full calibration.

  **`--crackle` is the A/B the bug needed**, and it is one mode rather
  than a second tool because the finding IS a comparison -- a number
  from one driver alone is what misled `docs/bugs.md` for weeks. It
  encodes a steady 1 kHz tone as an MP3 (the DECODER has to be in the
  producer; a WAV of the same tone is pristine through both drivers and
  measures nothing), boots a guest with the G6 passed through, and plays
  it once through the in-kernel `usb-audio` and once through the ring-3
  `usbaudio.so` -- one cable, one stimulus, one analyser, so the
  difference is attributable. It proves the cable BEFORE the guest takes
  the device, because once QEMU holds the G6 the host cannot play
  through it to check. Needs `lame` and SKIPS without it, the same rule
  that keeps ffmpeg out of the gate. **It is a REPORT, not a gate**: the
  crackle is a known open bug, and a check that is permanently red is
  one people learn to ignore, so it returns non-zero only when a leg
  produced no audio at all -- that is the rig going wrong, not the bug
  still existing.

  **`--rate --host <ip>` is the 44.1 kHz proof, on BARE METAL** with the
  G6 plugged into that machine (its headphone-out still on this host's
  line-in): a 1187 Hz tone written at 44.1 kHz, played once with the
  card following it to 44.1 kHz and once held at 48 kHz, must come back
  at 1187 Hz with no phase jumps in 5 ms windows (`phase_jumps()`), and
  the clock must read back 44100. **Not through a guest**: QEMU's USB
  passthrough breaks packets whose length varies -- 364 jumps in 398
  windows under TCG, 398 under KVM, 0 on the ASUS and 0 from Linux
  (docs/testing.md). It SKIPS when
  the G6 bound UAC1 at full speed (48 kHz only): replug it. **`--ring3`**
  runs it through `/lib/snd/usbaudio.so` instead, stopping the machine's
  own `snddrv` service for the leg and starting it again after -- the
  kernel takes ONE ring-3 sound driver, and an ASUS's HDA already holds
  it. Both modes also check the stream FOLLOWED the card's feedback
  endpoint (`clock: the device's` in the driver's stop line). It never
  plays unless the G6 is the machine's active output, and puts the
  output back afterwards.
- **`devclaim_test.py`** -- a ring-3 process takes the sound card OFF
  THE KERNEL, reads its registers, and gives it back (stage 2 of
  `docs/umdf-design.md`). It boots its own guest with an
  `ich9-intel-hda` because `hda` is the only driver whose device the
  default headless boot both HAS and can let go of -- so this is the
  only run in which the kernel actually releases a device it was
  driving. **The
  assertions are about the DRIVER's state, read from outside the
  claim**, because the in-guest test's unbound-device leg is green on
  any machine and would pass against a claim that recorded a pid and
  unbound nothing: `lspci -k` says `kernel driver: hda` before, the
  process reads `HD Audio 1.0` out of BAR0 while it holds it, and
  `lspci -k` says `kernel driver: hda` again afterwards. The round trip
  is `sound: hda0 registered` appearing TWICE in one boot -- once at
  probe, once when `DEV_RELEASE_REBIND` re-probes -- which nothing but
  a real remove-and-reprobe produces. `--no-card` is the positive
  control: the same run with no controller, where the in-guest test
  must SAY it skipped the unbind leg, so a tool whose HDA assertions
  silently matched nothing cannot look like a pass. On demand, not in
  the gate: it boots its own guest with extra hardware.
- **`device_count_test.py`** -- more devices than the old fixed tables
  held. PCI: `vm.py --pci-fill 40` (forty inert `pci-testdev`s behind
  PCI-to-PCI bridges, 31 a bridge) -- the kernel's count must pass 32,
  `lspci` must print a row per device counted, and both bridges' buses
  must appear. USB: `vm.py --usb xhci+crowd` (a hub of eight keyboards,
  a mouse and a tablet: eleven devices) -- all eleven must enumerate and
  `lsusb` list them, and the HID table, which stays fixed because the
  interrupt path walks it, must log that it is full. QEMU's default
  machine has a handful of each, so nothing else reaches either limit.
  Its positive control is an older build: before the tables were sized
  to the bus it reads `pci: 32 device(s) found` and eight USB devices.
- **`hdacodec_test.py`** -- a ring-3 driver reads the HD Audio codec
  graph and has to reach the SAME ANSWER the kernel does (stage 3 of
  `docs/umdf-design.md`). It boots its own guest with an
  `ich9-intel-hda`, for the same reason `devclaim_test.py` does, and
  runs `/bin/lscodec`: claim, map BAR0, take a DMA buffer for the
  command ring, bring the controller up, walk the graph, release with
  `DEV_RELEASE_REBIND`. **A plausible graph is not evidence** -- a stub
  that invented one would satisfy "it named a vendor and a route" -- so
  the load-bearing assertion is the AGREEMENT: the kernel's `hda`
  driver logs the pin and DAC it picked over its own CORB/RIRB, ring 3
  prints the pin and DAC it picked over a granted DMA buffer, and the
  two must match. Around it: `lspci -k` reports the card `(claimable)`
  before the run, `dmesg` shows the DMA grant raising BUS MASTERING,
  and `sound: hda0 registered` appears once more than the pre-claim
  baseline. The inverse of the claimable check SKIPS rather than
  passing quietly on a machine where every bound driver is removable,
  which a plain QEMU guest is -- `dev_claim.c`'s KTEST covers that
  half. `--no-card` is the positive control: `lscodec` must SAY there
  is no controller and exit non-zero. On demand, not in the gate: it
  boots its own guest with extra hardware.
- **`mixer_test.py`** -- the PER-APPLICATION volume, from the tray
  flyout down to what `soundd` actually applies. It boots its own guest
  with an `ich9-intel-hda` because `volume_test.py` cannot cover this:
  that guest has no sound card, so `soundd` exits with "no sound
  device", there is no beacon, no roster, and the per-app section is
  correctly absent from the panel it drives. **The assertions are a
  ROUND TRIP through four components that share no code** -- a row
  appears only while a client plays and carries the name `soundd` mixes
  it under (not the `snd.<pid>` its ring is called), a click a quarter
  along the track moves that row, `/etc/sound.conf` gains the key so
  the value outlives the widget, and the daemon reads it back and says
  so. That last one is counted as a DIFFERENCE from a baseline rather
  than as "is the line present": `soundd` reloads the moment the flyout
  writes, so the line already exists from the client still playing
  then, and the first version of the check passed without the second
  stream ever being mixed. It runs on a COPY of `disk.img` -- the
  flyout genuinely writes `/etc/sound.conf`, and a crash between the
  write and the cleanup would leave `aplay` quietened for every later
  tool on the real image. On demand, not in the gate.
- **`install_test.py`** gained an `mbr` medium (`--media mbr`) covering
  `install --mbr` end to end, and **three fixes to the tool itself, all
  of which made a healthy system report as broken**. `--instance auto`
  was passed through to EVERY `vm.py` call, and vm.py resolves `auto`
  per invocation -- so `start` took one slot and each later `exec`
  resolved to a DIFFERENT free one with no guest in it; every command
  came back empty, ten checks failed naming nothing, and the guest was
  left running to poison the next run. It resolves the slot ONCE now.
  The readiness check tested `"ready" not in out` against vm.py's
  refusal message, **"al*ready* running"** -- so a guest that never
  started looked like one that came up. And the first `exec` now waits
  on the console ANSWERING rather than on `start` returning, since on
  live media vm.py reports ready before the debug console takes
  commands. Its `--positive-control` zeroes the installed boot sector
  and must redden the six boot checks.
- **`msi_test.py`** -- the Local APIC, and the xHCI delivering through
  MSI-X instead of its shared pin. **Its load-bearing check is that
  interrupts ARRIVE, and enumeration cannot show that**: every control
  transfer in the xHCI driver polls the event ring, so a controller
  whose interrupts go nowhere still finds its devices, registers them
  and logs "running" -- the whole boot looks perfect. Only an
  asynchronous HID report needs a real interrupt, so the tool moves the
  mouse and requires the controller's interrupt count AND its decoded
  reports to rise; with INTx disabled by the MSI-X programming, an
  interrupt that arrives can only have come from the vector. **Its
  control is a boot flag** -- `make iso KCMDLINE="nomsi"` puts the
  machine back on the 8259 -- and it is documented in the tool rather
  than run by it, since baking a flag rewrites the shared media
  (`ahci_test.py` makes the same call about `noahci`).

  **A SECOND PHASE covers virtio**, in its own VM slot: virtio-input and
  virtio-net take a vector each, and the same "enumerates fine, delivers
  nothing" hazard applies, so the checks are again counts that must rise
  -- decoded input events, and an ICMP round trip to QEMU's own SLIRP
  gateway that can only arrive through the receive queue. **The two
  phases must not share a slot**: `QMPSession` leaves the QMP port in
  `TIME_WAIT` for about a minute, so reusing it either waits that out or
  is refused by `port_guard` as a clash with the guest just killed
  (`kbd_test.py` hit the same thing and solved it the same way).

  **What a control here MEASURED, and it is worth not re-deriving:**
  gating `virtio_irq_is_ours()` on the ISR byte under MSI-X changed no
  count at all, because QEMU's `virtio_irq()` writes the ISR before
  dispatching the vector. The control that DOES go red is returning 0
  from it outright -- 0 events decoded against 33 vectors delivered --
  and that is what shows the tool can see a dead path. On demand.
- **`usb_storage_test.py`** -- USB sticks on QEMU's `usb-storage`, the
  bulk-only transport every real stick speaks: a FAT32 stick built on
  the host with a known file, and a blank one with 4096-byte blocks. It
  reads the host's file back (several 64 KiB transfers), has the guest
  copy a file and PULLS the stick at once (QMP `device_del`), then reads
  the copy out of the image on the HOST -- quick removal judged by the
  data, not by a flush count -- checks the pulled mount says `removed`,
  that unmounting releases the disk, that a stick plugged in while
  running takes the lowest free name, and formats, writes and reads the
  4K stick. Controls that turned it red: a driver dropping every write,
  and disk names counted rather than chosen (a re-plug got a second
  `usb1`). automountd mounts the boot-time stick at `/media/TESTSTICK`
  and the replug at `/media/REPLUG`; it is stopped in between so the
  pulled mount is the kernel's alone to handle. Needs dosfstools and
  mtools. In `ondemand_sweep.py`.
- **`usb_desktop_test.py`** -- the desktop's side of USB sticks, on a
  stick hot-plugged into a running desktop: the tray item hidden with
  none, the flyout OPENING ITSELF and closing itself again (and DRAWN:
  its rect differs once it has closed, a far corner does not), Eject
  pressed where `gui drives --json` says it is, the notice "<label> can
  be pulled out", the File Manager's Removable heading and its row's
  own Eject button (aimed from the tree's layout log), and a stick
  pulled without Eject told as "was pulled out", and a stick with TWO
  volumes counted as one, drawn as its stick row (Eject) with each
  volume indented under it (Open; a card of its own, read from the
  pixels) and ejected whole. Last, a SECOND boot with a stick already in
  the port, in a free slot of its own: listed, and once mounted the
  flyout has not opened itself (`self_opens` 0). automountd is held back
  on that disk copy until the desktop is up, because QEMU otherwise
  mounts the stick first and the check could not see a flyout keyed on
  the mount -- verified red with that rule put back. `--positive-control`
  pulls instead of ejecting: the eject checks go red. Needs dosfstools
  and mtools. In `ondemand_sweep.py`.
- **`nvme_test.py`** -- one QEMU NVMe controller with TWO namespaces
  and no other disk: namespace 1 a copy of `disk.img` (SeaBIOS boots it,
  the root mounts from `nvme0p3`), namespace 2 blank with 4096-byte
  blocks (`mkpart`, `mkfs`, mount, copy). Two boots, on demand.

  Its load-bearing checks: `ktest nvme` with **0 skipped and every
  KTEST in `nvme_test.c` run** (counted from the source, so a stale
  filter fails); the `sum`s after a reboot; and TRIM measured from the
  HOST, asserting the 4K image GREW by the 40 MiB written before it
  asserts the delete shrank it -- without the growth, the shrink passes
  vacuously (`docs/roadmap-details.md` has the tool where that
  happened). The KTEST `a completion raises the interrupt` is what
  proves MSI-X delivers: a dead vector fails no read, because a
  sleeping waiter wakes at its deadline and reaps anyway. The control
  that turned it red was a handler that never counted.
- **`bigdisk_test.py`** -- a **sparse 2200 GiB disk past 2 TiB**,
  attached as a second disk on virtio-blk, AHCI and NVMe in turn beside
  the IDE root (pinned with `root=ata0p3` via `add_boot_word()`; the
  kernel would pick it anyway now -- `bootpart=` names it -- but the pin
  keeps the test independent of that rule). The guest makes a
  64 MiB partition 2 wholly past sector 2^32, formats it TFS3, writes and
  reads a marker, fscks it. Three boots, on demand.

  The oracle is the HOST reading the image: the kernel's reported sector
  count, partition 2's start in the GPT, the backup header at the TRUE
  last sector, the marker inside partition 2 -- and NOTHING written
  where a 32-bit wrap would land (`SEEK_DATA` over the sparse file).
  The guest reading the marker back proves nothing on its own: under a
  wrapped write path the readback was served from cache while the data
  reached the disk nowhere, which is what turned the wrap check from
  "the marker is not there" into "nothing is there".
- **`sector4k_test.py`** -- a **virtio disk with 4096-byte logical
  sectors** (`logical_block_size=4096`) beside the IDE root: `mkpart`,
  `mkfs -t fat32`, `mkfs`, mount both, copy a multi-block file onto each,
  reboot, compare `sum`s. Two boots, on demand.

  **Its oracles are the ones the guest cannot fake**: the GPT header at
  byte 4096 with first-usable LBA 6, the FAT32 BPB's 4096, and mtools
  extracting the file byte-identical. And it greps `dmesg` for the
  block layer's "not whole ... blocks" refusal and virtio's IOERR line,
  which is what went red when the control removed the alignment check
  and reverted TFS3 to one-sector reads. **A mount point that failed to
  mount is still a directory on the root**, so a `cp` into it reads back
  fine; the read-back checks require the `mount` listing to show each
  path on its own partition, which is how the first version passed a
  failed TFS3 mount.
- **`ahci_test.py`** -- boots with the filesystem on a **SATA drive
  behind an ICH9 host bus adapter**, which is the only thing here that
  reaches `kernel/drivers/ahci.c` at all. Several boots, on demand.

  **Its load-bearing check is `0 skipped`.** Every `ahci` KTEST guards
  itself on a controller being on the PCI bus, so on the default IDE
  boot all five skip and the suite is green because nothing ran -- the
  exact failure mode this repo keeps finding. Asserting the skip count
  is what makes the KTESTs mean anything.

  The other three that a broken driver would not pass: **`df` reporting
  a persistent TFS3 root** (a kernel that ignored the controller still
  BOOTS, just into ramfs, so "it booted" proves nothing); **a REBOOT**
  between the write and the read, since a write that only reached a
  buffer passes a same-boot read-back; and **mtools reading the same
  `/boot/grub/grub.cfg` off the host**, an oracle sharing no code with
  either the AHCI driver or `fat32.c` -- `mtype` on the image versus the
  guest's own `cat`, the same call `fat32_test.py` and
  `regex_hostcheck.py` make. That phase SKIPS without mtools.

  **TRIM gets its own oracle, and it is the host filesystem.** A drive
  that acknowledges DSM and discards nothing is indistinguishable from a
  working one inside SaunaOS -- that exact failure shipped once in
  `ata.c`, where the range list went out over PIO and never arrived. So
  the tool writes 40 MiB, deletes it, and requires the sparse image's
  ALLOCATED size (`st_blocks`) to come back to baseline. Both halves are
  asserted: a check that only looked for the blocks coming BACK passes
  trivially on an image that never grew.

  A later boot rewrites the image's `grub.cfg` with mtools to add
  `noahci`, and asserts the driver STILL runs while the block layer does
  not take it -- the precedence rung, and the only way to test a boot
  word without `make iso KCMDLINE=`, which would rebuild the media every
  other tool shares.

  Its positive control is to stop `build_prdt()` advancing the buffer
  address between entries. That reddens exactly the multi-entry PRDT
  KTEST and NOTHING ELSE -- the machine boots, mounts, writes and
  reboots perfectly, because TFS3's 4 KiB blocks are one PRD entry. It
  is a good demonstration of why the check is written as a comparison
  against single-sector reads rather than as "the data came back".
- **`shutdown_test.py`** -- `reboot` makes init stop the services in
  the REVERSE of their start order, then everything else (6 checks).
  Boots its own guest on a copy of `disk.img` with `--serial-log`,
  because a reboot ends the guest (`-no-reboot`) and init's lines are
  read from the file afterwards. Adds `/tests/stubborn` as a service at
  runtime (`After=toywm`, `StopTimeout=1500`; it ignores SIGTERM) and
  spawns a `/bin/sleep` that is no service, then checks: the shutdown
  line and QEMU exiting; the `init: stopping` names equal the boot's
  `init: started` names reversed (at least four); the fixture's
  `took 1.5 s, killed` line with services still stopped after it; the
  `other processes, SIGTERM` count; `init: rebooting` last. Positive
  control: the stop loop walked forwards turns the order and fixture
  checks red. In `ondemand_sweep.py`.
- **`poweroff_test.py`** -- proves the machine stops through **its own
  ACPI tables** rather than a hardcoded port, on two chipsets.
  It boots its own guests and deliberately ends three of them.

  **"Did it shut down" is exactly the assertion a broken version
  passes.** The old implementation wrote `outw(0x604, 0x2000)`, and
  QEMU's own FADT happens to name port 0x604 and sleep type 0 -- so the
  hardcode and a correct table walk stop the guest identically. What
  separates them is the kernel log: `acpi: S5 via PM1a ...` is printed
  by the path that read the FADT and the DSDT's `_S5_`, and
  `power: falling back to the QEMU/Bochs PM1a_CNT port trick` only when
  that path declined. The check is the first line's presence AND the
  second's absence, plus the QEMU process really exiting -- which is a
  fact about the host that the guest cannot fake.

  **Two chipsets, because one is not a sample.** i440fx (the default)
  presents a revision-0 RSDP, an RSDT, a 116-byte FADT and NO reset
  register; `-machine q35` presents a 244-byte revision-3 FADT with a
  real reset register at port 0xcf9 and an MCFG beside it. The reset
  half can only be tested on the second, and its assertion is that the
  machine COMES BACK (two kernel banners on one QEMU process), not
  merely that the process ended. **Not two "debug console ready"
  lines**: every boot prints that twice, the command goes out between
  them, and a count taken then was satisfied by the FIRST boot -- the
  read stopped before the reset, which passed only while a reset was
  instant.

  Its positive control is to make `acpi_poweroff()` return 0 at its
  first line: the two log lines swap over, the guest still powers off,
  and exactly the two discriminating checks go red. What it cannot
  cover is VirtualBox and real hardware, which is what motivated the
  feature -- what is testable here is that the numbers come from the
  tables rather than from a constant, which is the property those
  platforms need.
- **`virtio_boot_test.py`** -- boots with **no IDE controller at all**
  and the filesystem on virtio-blk, then writes a file, REBOOTS, and
  reads it back (6 checks). Builds its own QEMU; on demand, not in the
  gate. It exists because the `virtio` KTESTs cannot cover this: on
  those boots ATA still owns the filesystem, so nothing exercises
  `block_virtio.c`'s adapter or `vfs.c`'s precedence rule. And the
  reboot is the point -- a write that only reached a cache passes a
  same-boot read-back, so the round trip is what proves the bytes
  landed on the disk. Its positive control is to make
  `virtio_blk_write_sectors()` return success without issuing anything:
  that reddens exactly the round-trip check.
- **`virtio_gpu_test.py`** -- the ONLY thing here that boots
  `-vga virtio`, which is the whole reason it exists: every other GUI
  tool and `make test` launch the default adapter, so the virtio-gpu
  KTESTs would skip on every run and the suite would stay green either
  way. It launches a guest through `vm.py --vga virtio`, runs
  `ktest virtio-gpu` INSIDE it (the driver's own state -- active
  display, surface geometry, a flush being exactly two commands) and
  reads the PIXELS from outside (the desktop is drawn, and it keeps
  changing, which is the control that separates a live display from one
  frozen after its first frame). Its pixel-format oracle is a second
  boot on `-vga std`: the same OS drawing the same desktop on a
  known-good layout. That comparison is the only check that survived
  the positive controls -- "is anything on screen" passes on a black
  screen, and a red/blue channel test passes on a format that rotates
  channels rather than swapping two. The cursor plane is checked by
  what CAN be observed (the commands complete, and showing the pointer
  repaints no framebuffer pixels), because a device-composited cursor
  is handed to the display client out of band and never appears in a
  `screendump` at all. On demand, not in the gate.
- **`panic_store_test.py`** -- the panic store (`kernel/panic_store.h`):
  a kernel panic's log survives the warm reset that follows it, and
  logd appends it to the DEAD boot's file. **IT PANICS ITS GUEST ON
  PURPOSE**, through `config set kernel.crash gp-fault` on a disk COPY
  whose GRUB line gains `faultinject` (`install_grub.add_boot_word()`),
  so it needs `vm.py --reboot` and lives in the sweep. It checks that a
  cold boot recovers nothing, that the countdown restarts after ~10 s
  rather than at once, that the dead boot's log ends with the crash
  announcement and the `#GP` report, and that a cleared record is not
  filed again. The trigger runs in the BACKGROUND: a panicked guest never
  answers, and waiting on the exec started the countdown's clock a whole
  timeout late. `--positive-control` restarts COLD after the panic, where
  every recovery check must go red.
- **`kdebug_test.py`** -- the kernel's GDB stub (`kernel/debug/`),
  end to end over a real serial port: its own boot of a disk COPY with
  `kdebug=ttyS2` and `vm.py --kdebug`. A small protocol client in the
  tool makes each assertion one packet, so it needs no GDB: attaching
  stops a RUNNING kernel; `qOffsets` equals the KASLR delta the boot log
  printed, and memory at a symbol plus that delta is `kernel.bin`'s own
  bytes (read from its `PT_LOAD` segments -- `kernel.debug` has no
  code); a breakpoint in `heap_total_bytes()` stops `meminfo` with the PC
  ON it; a step moves one instruction and executes the real one, not the
  int3; a write watchpoint on `clockevent.c`'s `g_ticks` fires and names
  its address; an NMI from the monitor and a `^C` each break in; detach
  resumes the stopped command. Then, if `gdb` is on PATH, a real GDB
  must disassemble a function BY NAME, which needs qOffsets and memory
  reads together. **Pick a target symbol that is unique and not
  inlined**: `dbg_cmd_meminfo` is static and called once, so `-O2`
  inlined it away, and `g_events` is a static name in two files.
  `--positive-control` boots without `kdebug=`, where THE ATTACH CHECK
  ITSELF must fail -- not merely something, since "the stub is armed"
  fails on every unarmed boot -- and `kdfiled` must have exited (armed,
  it must be waiting blocked; it once spun on every unarmed boot). **`--net`** runs all of it over the network transport
  (`vm.py --kdebug-net`, a fresh random key each run, the real GDB
  through `kdebug_bridge.py`) and adds checks that a hello under the
  WRONG key and an exact REPLAY of an accepted datagram both get
  silence (removing the sequence rule turns the replay check red); that
  after a NEW session a datagram from the old one gets silence and the
  bridge refuses a reply from it; and that one bridge outlives a real
  REBOOT of the guest, gdb simply reconnecting. Both modes step onto the
  idle loop's `hlt`, which must come back one byte on rather than halt
  with the interrupts the step masks. The real GDB
  also sends a 300 KB file with `remote put` to `/tmp` and to the
  read-only `/boot`, and the guest's own `sum -a sha256` must match the
  host's, with `/boot` read-only again after; a garbled `pwrite` must
  make the close answer `EIO` and leave no file. **The real-GDB
  check requires a live `info registers rip`**: GDB disassembles
  `kernel.bin` from DISK when the connection fails, which once passed a
  run where the attach had timed out. Both modes are in
  `ondemand_sweep.py`.
- **`netheal_test.py`** -- `/bin/netheal`, which reboots a machine (twice at
  most) when it comes up with no network. **THIS TOOL REBOOTS ITS GUEST TWICE
  ON PURPOSE**, which is the property under test, so it needs
  `vm.py --reboot` (the default `-no-reboot` ends QEMU on a guest
  reboot instead) and lives in the sweep rather than the gate. The
  assertion that matters is not that it reboots but that it **STOPS**:
  the counter in `/var/lib/netheal` is the whole safety design, and
  without it a machine with genuinely no NIC reboots forever. So the
  tool checks the counter reaches the ceiling, then samples uptime
  twice and requires it to GROW -- a machine still rebooting keeps
  resetting it. It also checks the give-up line is logged once, and
  that a boot WITH an address clears the counter back to zero. Its
  positive control leaves the feature off, where a clean report would
  mean the tool is watching nothing.
- **`logrotate_test.py`** -- `logd`'s per-boot retention:
  `/var/log/boot/<n>.log`, `storage.log.keep`, and `log -p N`.
  **REBOOTS ITS GUEST FIVE TIMES ON PURPOSE**, because retention across
  boots cannot be checked inside one, so it needs `vm.py --reboot` and
  lives in the sweep rather than the gate. Four assertions, each aimed
  at a way the retention can look fine and not be: the numbering has no
  gap and no reuse (a reused number silently OVERWRITES the evidence
  somebody rebooted to keep); `log -p 1` and `log -p 3` return
  DIFFERENT text, which a resolver ignoring `N` could not; `log -p 99`
  is refused rather than answering with the wrong boot; `log -p -n 2`
  still honours `-n`, since the optional count could swallow it; and
  lowering `storage.log.keep` actually deletes rather than stranding
  the files above the new limit. It finishes by squeezing
  `storage.log.max` to 1 MiB and checking a boot that fills its share
  stops and says so. Its positive control sets `log_keep 1` -- the old
  two-file behaviour -- where a clean report would mean it is not
  watching the depth. It RESTORES the two settings in a `finally`,
  because a machine left at 1 MiB truncates every later tool's log and
  that reads as a hang.
- **`shutdown_sync_test.py`** -- a write made just before `reboot` must
  survive it. **REBOOTS ITS GUEST FOUR TIMES**, since the property is
  precisely what survives a reboot, so it lives in the sweep. It exists
  for a measured bug: `system_reboot()` flushed the ATA SECTOR CACHE and
  nothing else, which is a layer BELOW the filesystem -- `storage.sync =
  batched` keeps a TFS3 journal transaction open across writes, so
  blocks that transaction still owns never reached the cache and a flush
  could not save them. A `config set` followed at once by `reboot` left
  `/etc/storage.conf` at ZERO BYTES: the new value lost, and the old one
  too, because `fs_write()` truncates first and the truncation landed
  while the data did not. The three assertions separate the cases that
  fail differently -- updating an existing config file, creating one
  that did not exist, and an ordinary file, so the property is shown to
  be the filesystem's rather than one setting's. **The gap between the
  write and the `reboot` is the whole experiment**: a `sync` or a second
  of idle commits the transaction on its own and nothing is proved,
  which is why the helper issues them as one command list. Its positive
  control points the probe at `/tmp`, a tmpfs mount that CANNOT survive
  a reboot by design -- a clean report there means the tool is not
  reading back across the reboot at all.
- **`usb_multi_test.py`** -- TWO xHCI controllers, each driven as its own
  bus (`vm.py --usb xhci+two`, `make run USB=xhci+two`): a mouse on the
  first, a hub with the keyboard behind it on the second. The keystroke
  is the oracle and controls itself (QEMU routes keys to the usb-kbd
  alone), so an undriven second controller types nothing. It checks both
  buses come up, one interrupt-driven `usb-xhci` source each, `lsusb`'s
  `Bus 1` lines and `devctl`'s `usb:1-` names beside the first
  controller's unchanged ones, a full ring lap of keystrokes on the
  second controller, and an unplug and replug of its keyboard. Red on
  the driver before 2026-10-24, which drove the first controller only.
  In `ondemand_sweep.py` (`usbmulti`), ~1 minute.
- **`usb_test.py`** -- an xHCI controller and a HID boot keyboard and
  mouse (`vm.py --usb xhci` / `--usb xhci+mouse` / `--usb xhci+hub`, or
  `make run USB=xhci+mouse`). Eight phases: keyboard, ring wrap, mouse,
  HOT-PLUG (QMP `device_add`/`device_del` on the running guest -- the
  only headless stand-in for a human plugging a mouse in, and the
  detach check must scope its lsdev assertion to the Input sources
  section, because the capture also carries the kernel's own
  `usb-mouse unregistered` log line), and HUB (both HID devices behind
  a `usb-hub`, which is what exercises route strings; QEMU's hub is
  full-speed, so the TT path stays hardware-only), and WIDE (`--usb
  xhci+wide`: a 30-port controller with the keyboard on port 20, since
  `XHCI_MAX_PORTS` was 16 and a Kaby Lake PCH's SuperSpeed ports are
  17..22 -- QEMU numbers USB3 first, so its USB2 ports land at 16..30),
  and NETREPLUG (QEMU's `usb-net` plugged and unplugged six times
  behind a QMP `netdev_add`: it must bind as CDC-ECM every time, the
  control interface bound and the data interface claimed with it --
  it offers RNDIS first, so this proves the configuration picker passes
  over it -- and the frame allocator's free count after each unplug
  must not fall, where a driver keeping its DMA buffers falls four
  frames a replug. A leak check that passes because nothing bound is
  why the bind count is asserted too), and NETD (usb-net cards on their
  own `10.0.5.0/24` user network: one named by an `/etc/net.conf` rule
  is replugged faster than netd's pass and must come back under that
  name with a lease, then five more distinct MACs in turn must each be
  leased -- netd's table of four used to fill with departed cards. The
  rule is put BEFORE the test opens the debug console, since `vm.py put`
  drives that console itself and a second connection is refused).

  **It is self-controlling, and that was measured before a line of the
  driver was written.** QEMU activates a keyboard handler the moment
  `usb-kbd` is attached and routes host keystrokes to THAT device, so a
  guest whose USB driver does not work receives nothing at all -- not
  over USB and not over PS/2 either. Proven on a build with no USB
  support: with the devices attached a typed `touch /marker` left no
  file, and without them the identical sequence left one. So every
  keystroke assertion here already has its control, with no scaffolding
  and no `i8042=off` needed. It is also why the axis is OFF by default:
  adding it to a tool written against PS/2 silently deprives that tool
  of input, which reads exactly like a guest bug.

  **The load-bearing check is the RING WRAP, not the first keystroke.**
  A driver that ignores the event ring's cycle bit, or never re-posts a
  consumed TRB, works perfectly for exactly one lap of the ring -- 256
  TRBs, about 128 keystrokes -- and then goes permanently deaf. A test
  typing a short marker cannot tell that from a working driver. Phase 2
  types 25 files, well past a full ring, and asserts the LAST one as
  well as the first. Its positive control is worth repeating: with the
  cycle flip removed, files 8 onward vanish while the FIRST file still
  passes.

  The oracle throughout is the FILESYSTEM read back over the serial
  console, never the screen -- a keystroke that worked leaves bytes on
  disk, and the serial console does not care who owns the keyboard.

  Two harness traps it encodes, both of which cost a wrong diagnosis.
  **`send_key` silently drops a character it has no qcode for**, so
  `touch /usb_one.txt` quietly created `usbone.txt` and read as a dead
  driver; anything not a bare lowercase letter or digit is spelled out,
  and a shifted character goes through `combo()`. And **the serial
  socket must be drained as the test types**: with the text target every
  spawn logs to COM1, nothing else reads it, the buffer fills, the
  guest's serial write blocks and the whole kernel stalls -- which
  presents as a driver dying after N keystrokes and recovers the instant
  anything reads the socket, so a dump taken afterwards looks perfectly
  healthy. Measured: 5 of 25 files without the drain, 25 of 25 with it.

  `--kvm` is not optional before believing a change here. The INTx storm
  the acknowledge path guards against is invisible under TCG -- the
  virtio-input version of it hung 3 boots in 3 under KVM and 0 in 3
  without. `--phase keyboard|wrap|mouse` runs one phase. On demand, not
  in the gate.
- **`virtio_input_test.py`** -- the virtio keyboard, mouse and tablet
  (`vm.py --virtio-input`, which is the only thing that attaches them).
  The guest keeps its PS/2 pair as well, deliberately: the input core is
  supposed to take several sources at once. Five checks earn their
  place. **The guest must finish enumerating all three devices with the
  POINTER MOVING through the whole boot** -- motion is injected over QMP
  from the instant QEMU starts, so the tablet has events waiting the
  moment its INTx is enabled. That is the check the virtio-input INTx
  ordering bug was found by, and it only runs with `/dev/kvm`: the race
  hung 3 boots out of 3 under KVM and 0 out of 3 under TCG, so on a
  machine without KVM it prints a SKIP with the reason rather than a
  green check that cannot fail. It also asserts the ENUMERATION rather
  than the boot's exit code, because a first version tested `vm.py
  start`'s return and passed on a guest that had wedged partway through
  -- readiness comes from the serial console, which is up before
  virtio-input runs. The tablet must report `abs` and the mouse must NOT, or the
  capability is telling us nothing. Two devices must SHARE an interrupt
  line -- QEMU routes the mouse and tablet onto IRQ 10 together, which
  is the case `kernel/arch/x86_64/irq.c`'s handler chain was rewritten
  for. An absolute position must land EXACTLY where the arithmetic says
  (the tablet's 0..32767 range scaled to the screen). And Super must
  open the Start menu, which is one assertion covering the whole path
  from the virtqueue through the input core, the key ring and the
  kernel's raw-input forwarder to the ring-3 compositor.

  It reads the IRQ assignments from `lsdev`, NOT from `dmesg`: the boot
  line saying which line each device took has rolled out of the kernel's
  ring buffer by the time a desktop has been up a few seconds. An oracle
  that expires fails for reasons unrelated to the code. On demand, not
  in the gate.
- **`live_boot_test.py`** -- boots `saunaos-live.iso` with NO disk and
  asserts a shipped binary RUNS, plus that `df` reports the image's real
  size and says it does not persist. Not in `gui_regress.py` (it builds
  its own QEMU); run it after touching the block layer, TFS3's geometry
  or the live path. **"It booted" proves nothing here** -- the kernel
  falls back to ramfs and still reaches a shell and a desktop, which is
  exactly what an empty live boot looks like.

  **Repaired 2026-08-25, after being unrunnable for an unknown period.**
  It parsed `df`'s `used:`/`total:` LINES, which became a TABLE when
  `df` moved to `/bin`, so every number came back 0 and three checks
  failed against a live boot that was working perfectly. Nobody saw it
  because the live image could not be BUILT: `LIVE_IMG_MB` was a
  hardcoded 24 and the seed tree had reached ~33 MiB. Both are fixed --
  the size is derived from the tree now -- and the lesson is the one
  `check_tool_commands.py` cannot catch: **it sees a command that was
  renamed away, never one whose OUTPUT changed.**

  **A SECOND PHASE, 2026-08-27: the mount POLICY** (`--phase policy`,
  and part of a bare run). "Live only when asked for, or when there is
  no disk" is what stops a live session displacing an installed system,
  and it was enforced by asking `ata_present()` -- the LEGACY IDE probe
  -- so on a machine whose only disk is AHCI or virtio the kernel
  concluded "no disk" and took over anyway. **Every live test here
  booted with NO DISK, which is the one configuration where the right
  and the wrong predicate agree.** The phase boots one scratch ISO (it
  builds its own, with the `live` word left out) twice and requires
  OPPOSITE outcomes -- declined with an AHCI disk attached, used without
  one -- so each run is the other's control. Reported by a user booting
  real hardware, which is where this class of bug lives.

  It reads the kernel's own `fs:` lines off a serial log rather than
  driving the console, and it builds its ISO from `build/kernel.bin`, so
  **build before running it** -- a tool that restores a working tree
  without rebuilding (`predates.py`) leaves it testing the other
  kernel.
- **`doom_test.py`** -- DOOM runs, draws, animates and takes input,
  including an arrow that still turns with Ctrl (fire) or Shift (run)
  held and Alt+Space (strafe + use) NOT opening the menu, because a
  level in play holds the WM's shortcut inhibitor and DOOM's own menu
  gives it back (both sides' log lines) -- Doom reads
  keys by position (`on_phys_key`), so it is driven through QMP, never
  `gui key`, which injects translated keys only. **ON DEMAND,
  not in `gui_regress.py`**: it needs an IWAD, and the IWAD is
  deliberately not in this repository, so a checkout without one gets a
  clean SKIP rather than a failure. The IWAD reaches the image only with
  `EXTRAS=1`; without it, `vm.py put data/doom/doom1.wad
  /usr/share/doom/doom1.wad` into a scratch guest does.

  Two of its checks are shaped by mistakes made writing it. **"It
  animates" samples seven frames, not two** -- Doom's attract mode cycles
  demo, title, credits, and the title screen is legitimately STATIC, so a
  two-sample comparison lands on it often enough to flake. **The frame
  rate comes from the app's own report, not from the screen**, which the
  positive control proves is the right choice: breaking the blit turns
  three pixel checks red while the rate check stays green and the number
  goes UP, because it is no longer paying for a blit.
- **`tfs3_v1_test.py`** -- **repaired 2026-08-25, it had been 0 of 8**:
  it drove `cat`, `mv` and `dmesg` against an image with no `/bin` on
  it, months after those became `/bin` programs, and its image was flat
  once the kernel stopped mounting those. It uses `rescue cat` and
  friends now and builds a partitioned image. Boots a freshly built TFS3 **v1** image and
  proves the kernel still mounts and uses the older on-disk layout (8
  checks). Run it after touching TFS3's geometry, journal, or any
  operation's credit count. It exists because v2 made v1 support
  simultaneously untested AND untestable -- once `format` wrote v2,
  nothing in the repo could produce a v1 image at all, so
  `tfs3_writer.py format --fs-version 1` was added alongside it. Same
  rule as `ata nodma` keeping the PIO path reachable: a fallback
  nothing can reach is a guess. Its load-bearing check is that a
  cross-parent DIRECTORY move is refused there (v1's four journal slots
  cannot hold the five-block transaction) and that the refusal changed
  NOTHING -- the only end-to-end view of the credit reservation.
- **`fs_switch_test.py`** -- boots a COPY of disk.img and proves the
  multi-backend story end-to-end: probe mounts the image's own
  format, `fsformat` live-switches both ways (wipefs rule included),
  writes work on each side, files survive reboots, fsck ends clean.
  Run it after touching anything in `kernel/fs/`; it exercises the
  probe/format/remount/reboot cycle no KTEST can (the suite runs
  inside one booted kernel).
- **`boot_splash_test.py`** -- the boot splash (`kernel/drivers/splash.c`,
  `userland/apps/system/toywm/wm_setup_hold.c`): one guest on a copy of `disk.img`, five
  boots, a screendump burst through each. An ordinary boot shows the
  splash and NO frame of the text console, and the kernel hands it to the
  desktop; Esc during it brings the log; First-boot setup's hold carries
  the kernel's logo on the SAME PIXELS and its spinner turns;
  `system.boot_messages=detailed` holds a frame on an ordinary boot; and
  the text target shows the log -- which is also the control that the
  console-frame test can see a console at all. In `ondemand_sweep.py`;
  about 4 minutes under TCG. Reboots get `--timeout 180`: the burst slows
  the guest past `vm.py reboot`'s 30-second default.
- **`installer_test.py`** -- the Install SaunaOS window
  (`userland/apps/system/installer/installer.c`) on `saunaos-live.iso` with one BLANK
  512 MiB IDE disk, then that disk booted with nothing else attached. In
  `ondemand_sweep.py`; about 7 minutes under TCG. Phase 0 boots a
  one-entry ISO it makes with `installer=on` (`live_boot_test.py`'s
  `build_unasked_iso()`, given the entry): the window is up full screen
  with the desktop launcher behind it and no notice. Phase 1 also checks
  the end of the run is reported ONCE (a tick that kept treating it as new
  re-read Took's clock every quarter second), and phase 2 that the
  compositor held the desktop back until setup was up. Phase 1 checks what a
  live session offers unasked -- the desktop launcher, the compositor's
  live-session notice, and NO First-boot setup -- sets the computer's name
  and the region as a person trying the session would, walks Welcome,
  Disk and Ready by the window's own layout log, CANCELS the erase
  confirm and has the HOST check the disk is still blank, then confirms
  and waits for Finished with every stage logged in order. Phase 2 boots
  the disk alone: rooted on `ata0p3`, the name carried, First-boot setup
  skipping EXACTLY the two pages the session set, and no launcher or
  notice on an installed machine. **The live ISO is judged by the tool
  itself** (iso_guard only knows `saunaos.iso`): missing, or older than
  the kernel, the toolkit libraries, the installer or `install`, it SKIPS (exit
  77) rather than test the previous build. Positive control: the first
  run caught the live session's launcher riding along onto the new
  machine ("an installed machine gets no launcher"), which is why the
  install now leaves `UINSTALL_LAUNCHER` behind.

  **The Software step** (2026-11-09): phase 1 removes DOOM in the
  session (`component remove doom`), and the step must start from that
  set -- DOOM unticked and greyed, the rest ticked -- read from the
  program's `installer: software in= out= absent= order=` line AND from
  the checkboxes' pixels (`software.box N`, a ticked row as the control).
  Unticking the engine must untick the Browser, ticking the Browser must
  tick the engine, Demos and tests is left out, and Ready names the
  choice (`installer: ready: software ...`). Phase 2 reads the installed
  disk: no `/bin/wm/demos`, `/tests` holding nsmonkey alone, no DOOM,
  `removed demos` and `removed doom` in `/var/lib/update/components`,
  and the Browser, the engine and Windows 3.x support present. Before
  that it writes 24 made-up definitions and runs `scroll_test`'s
  `check_view()` on the checklist -- here, since the Disk step needs a
  disk a GUI guest does not have. Seen red on the build before the step
  existed (step 2 was Ready).
- **`install_test.py`** -- does SaunaOS install itself onto another disk,
  and does that disk then BOOT? Two phases, and the second is the one
  that matters: it boots the installed image with **nothing else
  attached** and asserts the machine that comes up is that one -- by its
  root partition's sector count, which is the source's size otherwise.
  It then boots the same image alone under UEFI (OVMF on q35) to the
  desktop -- on the MBR run too, whose 0xEF boot partition UEFI reads as
  an ESP; without OVMF that one check is a SKIP line.
  Without that, a guest with the ISO still in the drive boots the ISO's
  kernel and mounts the target's root, which reads exactly like a
  successful install.

  **The disk arrangement is the point, and it is inverted from the way it
  reads: the SYSTEM goes on virtio and the blank TARGET on IDE.** Disk
  precedence is virtio-blk, then AHCI, then ATA, so a blank virtio disk
  beside an IDE root outranks it, the root scan finds no filesystem, and
  the guest comes up on ramfs with no `/bin`. Inverting it is what lets
  this tool run against a stock `make iso` with no `KCMDLINE` (compare
  `hires_test.py`, which does need one).

  **It runs every medium** (`--media disk|live|grown|mbr|bootloader|all`,
  default all), because they are different code paths and the live one is how a real machine
  gets SaunaOS: `saunaos-live.iso`'s root is a RAM image with no `/boot` at
  all, which is why the install payload lives in `/install`. The live
  half needs `make live-iso` and is skipped with a message when the ISO
  is absent.

  **The disk run installs with a USB stick mounted** at
  `/media/INSTSTICK`, and reads the target back to check the stick's
  marker file stayed behind and its mount point is there, empty: the copy
  stays on one filesystem (`cp -x`). `installer_test.py` mounts the same
  stick in its live session. Red on the installer before 2026-10-24,
  which copied a 114 GB stick onto a bare-metal machine's NVMe.

  **The disk run leaves Demos and tests out** (`--without demos`) and
  reads the target back: no `/bin/wm/demos`, `/tests` holding the
  engine's nsmonkey alone, `removed demos` in its update state, and the
  Browser, DOOM and the Retro Launcher there. Off the plan alone it also
  checks an unknown id is refused naming the real ones, and that
  `--without webengine` takes the Browser along and shrinks the copy the
  plan quotes. Red before `--without` existed ("unknown option").

  **Every medium reads the target's `grub.cfg` back**: the payload's
  `set timeout=0` must have become `set timeout=3`, with no `.bak` left
  for `bootcfg undo` -- `update` installs no kernel without a menu, so an
  install that copied the file as it was could never be updated. The
  payload's own timeout is read too, or a `MENU=1` build would pass it
  blind. Red before 2026-10-24.

  `--positive-control` zeroes each installed boot sector before its boot
  phase, so those checks must go RED; a clean run proves nothing until
  that has been seen. On demand only (`ondemand_sweep.py` names it) -- it
  boots four guests and takes a few minutes.
- **`highmem_test.py`** -- the WHOLE KTEST suite on an 8 GiB guest
  (`ktest_run.py --mem 8192`), failing if an above-4-GiB check skipped
  rather than ran, which is what those do on every other runner's
  256 MiB boot. The whole suite rather than `mm` because a machine that
  size moves the 64-bit PCI window to 768 GiB, which is what broke every
  virtio test the first time it ran. Proves the high frames are managed,
  mapped and zoned, and that the kernel heap, user pages, page tables
  and window buffers come from them. Then a second phase asks the same
  question from OUTSIDE the kernel: one `/tests/memtest` is spawned on
  its own guest and the high zone's free count must drop by more than
  128 MiB. That half is what would survive a KTEST fixture drifting away
  from the path a process really takes. Named by `ondemand_sweep.py`.
- **`highmem_consume.py`** -- the same question at SCALE, and the proof
  stage of "More than 4 GiB of RAM". Six `/tests/memtest` copies at 1 GiB
  each on an 8 GiB guest, with the high zone's free count POLLED for its
  minimum -- a copy frees everything as it exits, so a reading taken
  afterwards reports an empty machine. Measured: 5.00 GiB of high frames
  held at once, which the ~2.9 GiB low zone could not have supplied.
  Every copy verifies its own address-derived pattern, so two of them
  sharing a frame is caught rather than assumed, and `meminfo --audit`
  must be clean afterwards.

  **Its positive control is to put one consumer back on `DMA32`.**
  Reverting `proc_syscalls.c`'s heap fault-in took the peak from 5.00 GiB
  to 0.01 GiB and killed every copy, which is the shape to re-run before
  trusting a clean pass here.

  **It counts memtest results as a DELTA**, because the klog ring keeps
  whatever an earlier run left in it and a stale `PASSED` satisfies the
  exit condition while copies are still holding memory -- which then
  reads as a leak. That is `mem_stress.py`'s "replace, never append"
  rule arriving from a different direction. Not a gate: it boots its own
  8 GiB guest and takes about a minute. Named by `ondemand_sweep.py`.
- **`fileop_test.py`** -- `lib/ufileop` through `/bin/cp`, `/bin/mv` and
  `/bin/rm`. The copy loop and the tree walk are ONE implementation
  shared by those three and by the File Manager, and this is what keeps
  "testable as text at a prompt" true after the GUI stopped spawning
  children. Asserts on the listing read back with `ls` -- a different
  reader from the program that did the work. Its positive control is the
  deepest-first unlink ordering: reverse it and `rm -r` leaves every
  directory behind, which is the one check that reddens.
- **`trash_test.py`** -- the Recycle Bin (`lib/utrash`) through
  `/bin/trash`, the same library the File Manager's and the desktop's
  Delete use: put, the numbered name a second same-named item gets,
  the freedesktop `.trashinfo` (a `+` must be `%2B` in `Path=`), restore
  with the content checked by size, a restore onto a taken name refused,
  a folder round trip, no bin on a RAM volume, and `empty`. Its positive
  control is the percent-encoding: let `+` through and the info-file
  check reddens.
- **`remote_test.py`** gained TFTP OPTION coverage (2026-09-01): both
  directions at the negotiated block (`remote.py`'s `WANT_BLKSIZE`, 8192
  -- so each block is fragmented and this is also reassembly's
  end-to-end check), and the fallback when a client asks for nothing.

  **The fallback is the half that breaks silently.** A transfer that
  quietly drops to 512-byte lockstep still succeeds and only looks slow,
  so a round-trip check cannot see it -- which is why one check reads
  the server's own log for `blksize <WANT_BLKSIZE>, window 3` rather than
  inferring agreement from the bytes arriving.
- **`guictl_test.py`** -- `/bin/guictl`, the ring-3 front end for the
  window manager's `gui` diagnostics. ATTACHES to a running `vm.py`
  guest and needs a desktop up.

  **Its load-bearing check asks the same subcommand twice** -- once
  through `guictl` (ring 3, `SYS_DIAG`, polled) and once through
  the serial debug console's own `gui` (ring 0, waits in place) -- and
  requires the two answers to match. A guictl-only check would pass
  against a program that printed a plausible answer of its own; the
  console shares no code with it below the window server.

  The second is that an unknown subcommand is REPORTED, which was dead
  for months: the kernel cleared the incoming flags before reading them,
  so `WIN_DEBUG_F_UNKNOWN` never arrived and `gui nosuchthing` printed
  nothing on the serial console either. A check asserting only that
  known commands answer would not have seen it.
- **`https_test.py`** -- drives `/bin/wget`'s HTTPS path against a TLS
  server on this machine: a threaded Python `http.server` with a
  self-signed certificate, reached through SLIRP's 10.0.2.2. **Nothing
  leaves the machine**, which is what lets it run offline; verifying
  against a PUBLIC certificate is a different question and is not this
  tool's.

  Nine checks, and the ORDER is the design -- each is only meaningful
  because the one before it can fail. A verified fetch returns the body
  over TLS 1.3 with no warning; the same fetch is REFUSED when the store
  holds the wrong anchor; refused DIFFERENTLY when the store is empty
  (a machine that was never told whom to trust is not the same as a
  certificate nobody vouches for); `-k` fetches and says so; the entropy
  gate refuses TSC jitter by name; and plain `http://` still works,
  which is the regression check for routing wget through the library.

  **It reboots between the three trust-store states** rather than
  editing `/etc/ssl/certs` under a running kernel, and it clears the
  store by LISTING it rather than by deleting the names it wrote -- an
  image built with `EXTRAS=1` already holds 121 Mozilla roots, and
  leaving those behind turns the "empty store" check into a verification
  failure, which is a fixture the test did not establish wearing the
  costume of a regression. It drives a COPY of `disk.img`, sparsely
  copied.

  `--positive-control` trusts the WRONG anchor while still expecting the
  verified fetch to succeed; three checks must go red, and a run that
  stays green is a run where the certificate is not being verified at
  all. Measured: 6 of 9 pass under it.

  ON DEMAND: it needs `openssl` to make the certificates and SKIPS
  cleanly without it.
- **`component_test.py`** -- `/bin/component` end to end
  (`docs/software-design.md`, stage 2), against `update_server.py`'s
  handler in-process on a loopback port, as `update_test.py` is. Remove
  `demos`: its files go (read back with `ls`), `/tests/nsmonkey` stays
  (the Web page engine's, a longer path), the state file says `removed
  demos`; an update -- and one from a server that tags NOTHING -- leaves
  them out; a server without this machine's build makes `add` say
  "update first, then add it" and change nothing; `add` then fetches
  them from the per-build route and `sum` agrees with the manifest;
  removing the engine takes the Browser, adding the Browser brings the
  engine. Beside those: a build that does not STAGE `demos` at all
  leaves it on the removed list; the failed `add` closes the update log
  it opened (`strace -e open,close`); a per-build file whose connection
  the server drops once is fetched again (`retrying once`); an untagged
  per-build manifest is tagged on the machine; and with no `--from`,
  `add` goes to the channel the record's `# from` names, not a
  configured server without the build. `--positive-control` hides that the files are optional (the
  tags renamed, the definitions moved away) and check 3 must fail; red
  too with the skip in `upd.c` mutated out (`mutate.py`).
- **`update_test.py`** -- `/bin/update` end to end, in its own guests,
  against `update_server.py`'s handler run IN-PROCESS on an ephemeral
  loopback port (so two runs cannot clash, and nothing leaves the
  machine). Seventeen checks, each read back through an independent
  reader (`sum`, `cat`, `dmesg`): a fresh `disk.img` is up to date
  against its own build; a damaged binary and a deleted document are
  found (the second as NEW) and installed live; a file the server
  CORRUPTS in transit is refused with the old file and no `.upd` left;
  a damaged LIBRARY is staged, not replaced, and the SECOND guest --
  the same disk rebooted -- applies it before init; `--server` persists.
  **Channels**: a second in-process server lists three, Release a
  version OLDER with one file different; `.../stable` is split into the
  server and `main`, `--channels` lists them with Dev `[internal]`, a
  behind channel installs nothing until `--go-back`, an unknown name is
  refused, and main brings the newer file back.
  **Stale files**: the server's manifest can HIDE paths, so a file a
  release stopped shipping is removed (`/bin/hello`), one edited on the
  guest is kept and said so, and a stale library is staged as a `-` line
  and removed by the next boot -- then served again to put the guest
  back. Removing the "edited here" crc guard by `mutate.py` takes the
  run to 31 of 32. **`/etc`, dpkg's conffile rule**: the server can
  REPLACE a file's body and its manifest line (`Server.replace`), so an
  unedited `/etc/shells` takes a "new release", an edited one is kept
  and its new version lands in `/var/lib/update/new/etc/shells`; making
  the unedited branch keep instead took the run to 40 of 41. A service
  `service disable`d on the guest is not reinstalled by the next update,
  and `service enable` brings it back; reinstalling every missing file
  instead took the run to 50 of 51.
  **Release notes**: `--check` prints the notes newer
  than the guest's build and none older, cut first at the commit the
  guest was compiled from and then -- after an install from a manifest
  claiming another `# commit` -- at the RECORD's.
  `--positive-control` serves the "corrupted" file intact while still
  expecting the refusal: two checks must go red (measured: 15 of 17).
  **The kernel path** (checks 18-25, skipped without mtools): a copy of
  this build's kernel with one boot message changed to a same-length
  twin is served; on the stock image (`set timeout=0`) the update is
  refused whole and `/boot` untouched; with the timeout raised on the
  HOST through mtools it installs, keeps `kernel.old`, and the next boot
  prints the twin message. Its control, by `mutate.py`, was disabling
  the refusal: 23 of 25. In `ondemand_sweep.py`.
- **`hwdata_test.py`** -- drives `hwdata update` against a plain
  `http.server` on this machine, reached through SLIRP's 10.0.2.2, the
  same arrangement `https_test.py` uses. **Nothing leaves the machine**;
  whether pci-ids.ucw.cz still serves what it used to is not this tool's
  question and could not be answered offline anyway.

  Twelve checks, and the load-bearing one is the SECOND: a truncated
  body is refused and the old database survives byte for byte. That is
  the whole reason the command exists rather than a note saying to run
  `wget -O <path>`, which opens with `O_TRUNC` and leaves a half-file
  that still parses. A large ERROR PAGE is refused separately, because
  the size floor alone would pass it and the vendor-line count is what
  does not.

  The survival checks compare a `sum` of the file taken through an
  INDEPENDENT program, not the byte count the updater printed -- and the
  final one `cat`s the new file, because "hwdata says it wrote 1.6 MB"
  proves it wrote a file, not that the file is the one `lspci` reads.
  The served body carries a marker vendor the real database does not
  have, so that check cannot be satisfied by the copy already there.

  `--positive-control` serves the TRUNCATED body to the good-fetch check
  as well; four checks must go red. Measured: 8 of 12 pass under it.

  ON DEMAND: it launches its own guest against a sparse copy of
  `disk.img` and needs no host tools.
- **`http_test.py`** -- libhttp's HTTP/1.1 half through `/bin/wget`,
  against `web_server.py` on this machine (on demand, `ondemand_sweep.py`'s
  `http`). Every check is judged by the SERVER or the BYTES, never by what
  the client says about itself: three URLs on one command line must be
  ONE connection in the server's own count; a 40 KB body that arrived
  chunked, gzipped, or gzipped inside chunked must hash (`sum -a sha256`)
  to the file; a connection the server dropped without saying so is
  retried on a fresh one; redirects are followed by default and handed
  back with `--max-redirect 0`; a POST carries its body and a 303 turns
  it into a GET; `-S` prints the headers; https over a test CA reuses
  ONE TLS connection, and an expired, wrong-name and self-signed
  certificate are each refused with THEIR reason. Shown red by never
  pooling (the two reuse checks), by shortening every chunk over 50
  bytes (the chunked hashes) and by not retrying a stale connection.
- **`libc_diff.py`** -- tolibc's formatter and number parsers against
  glibc, case by case. No guest: it compiles `kernel/lib/kfmt.c` and
  `userland/libs/libc/stdlib.c` with the host gcc, links them beside glibc,
  and runs ~20,000 generated cases through both -- the integer and the
  float conversions each as a flags x width x precision matrix. Needs gcc
  and nothing else.

  **It exists because tolibc's bar is COMPLETENESS**, which a
  happy-path test cannot hold: such a test checks the cases whoever
  wrote it thought of, which are the cases they got right. It found
  `%hhu` of 256 printing 256, a sign emitted outside its padded field,
  `%.3o` ignoring the precision, `%5c` ignoring the width, `%0+d`
  emitted literally because flags were order-dependent, and
  `strtol("0", &end, 0)` reporting no conversion at all; its float
  matrix (2026-11-08) found `0`, `+`, ` ` and `#` all ignored on
  `%f`/`%e`/`%g`. It also found
  that `libc3_test.c` ASSERTED one of those bugs -- a self-referential
  suite defends what it got wrong, and a differential one cannot.

  **IT ONCE COMPARED tolibc AGAINST ITSELF AND REPORTED PERFECT
  AGREEMENT.** `userland/libs/libc/include/stdio.h` makes `vsnprintf` a static
  inline around `k_vsnprintf` and `#define`s `snprintf` to
  `k_snprintf`, so putting that directory on the probe's include path
  made both sides the same code. The probe is compiled in its own
  translation unit with no tolibc headers, declaring the entry points
  by hand -- and it now asserts at startup that `snprintf` and
  `k_snprintf` are different functions, always, not behind a flag. That
  check is the one that would have caught it.

  **Symbol clash**: tolibc defines `strtol`, `abort`, `qsort` and
  twenty more names glibc also defines. They are renamed at the C level
  (`#define strtol toy_strtol` ahead of an `#include` of the source)
  rather than with objcopy, because that also redirects tolibc's
  internal calls and leaves genuine externals alone.

  `strtod` is compared **bit for bit**: it is correctly rounded since
  2026-11-10 (`userland/libs/libc/strtod.c`), and
  `tools/hostchecks/libc_fmt_hostcheck.py` is the deep check of it and
  of the float conversions. `--loose-float` allows the one ULP it once
  needed.

  **And `<math.h>`, in ULPs** (2026-11-08; `--only math`, or with the
  rest by default): every function in its double, float and long double
  form -- 229 checks, about 370,000 cases, the rounding-sensitive ones
  (`rint`, `nearbyint`, `lrint`, `llrint`, `fma`, `sqrt`) in all four
  rounding modes -- over a value set built per function from integers
  and `Fraction`s, so every input is exact in its type: the edges (signed
  zeros, the subnormal boundaries, the largest finite, the infinities,
  NaN), log-uniform values across the whole exponent range, and what
  each function gets wrong -- arguments a hair from a multiple of pi/2
  (and the double nearest one), integers and halves, poles, the
  overflow thresholds, fma's exact ties. The error is the distance
  between the two results' bit patterns, the worst per function is
  printed, and `MATH_BOUNDS` is the documented accuracy it fails above
  (`docs/libc-design.md`, stage 13). A sign of zero, a NaN against a
  number, or any integer output that differs (an exponent, `remquo`'s
  quotient bits, `lrint`) fails whatever the bound. **The oracle is
  glibc, not the truth**: its `cbrt` is 2 ulp off where ours is right,
  checked with an integer cube root, and `MATH_BOUNDS` says so. All of
  tolibc's maths sources are compiled HOSTED with every public name
  renamed (`MATH_SYMS`), so the freestanding `sqrt` macro stays out of
  the way. **`--break sin` / `--break fma` are the positive controls**:
  each builds that function deliberately wrong (a sin-kernel coefficient
  off by 1e-14, 4 ulp near pi/4; fma rounding a tie away from even) and
  must see it reported. A first sin break, off in the sixth digit, stayed
  under an ulp everywhere and the harness rightly said nothing -- a
  control has to be a break that matters. `--math-only sin,fma` narrows
  the run; `--verbose-math` lists the exact functions at 0 too.
- **`dash_test.py`** -- does the vendored dash BEHAVE like a shell? 18
  constructs run as real scripts and compared against real output:
  pipelines, redirection, here-documents, functions, parameter
  expansion, arithmetic, `case`, loops, command substitution, `test`,
  `trap EXIT`, positional parameters and exit status.

  **`dash_gap.py` proves dash compiles and links, which is a statement
  about the C LIBRARY rather than about the shell.** Before this, the
  whole of the evidence that dash worked was `dash -c pwd` printing
  `/` -- one builtin, one exit path, none of the parts a shell is for.

  **The scripts are FILES on the disk, not `dash -c` strings.** A shell
  test is made of quotes, and sending them through this script, then
  `vm.py`, then `tosh`, then dash gives four lexers a turn at them --
  which mangled every early attempt, a pipe arriving at `echo` as a
  literal argument. `tools/build/tfs3_writer.py` writes each script into a
  SPARSE copy of `disk.img` and dash is handed a path, which removes all
  four.

  **One case is deliberately not run**, and it is named in the file as
  `HANGS_THE_GUEST`: a pipeline whose reader fails to exec spins the
  writer at 100% CPU forever and the guest stops answering at all
  (`docs/bugs.md`). It becomes an ordinary case the day the pipe path
  reports the failure.

  A **warm-up** command runs before the table and its output is thrown
  away: the first exec after boot comes back empty while the guest is
  still emitting boot lines, which otherwise fails whichever case
  happens to be first and reads as a bug in that case.
  `--positive-control` expects a line no shell would print and requires
  the run to go red.
- **`dash_gap.py`** -- what the vendored dash port still needs from
  tolibc, measured by compiling it. No guest and no network: it runs
  dash's six build-time generators on the host, then compiles all 32
  sources against `userland/libs/libc/include/` and reports what the compiler
  refuses. Needs gcc and nothing else.

  **It exists because the requirement list kept being DERIVED, and was
  wrong in both directions twice.** `docs/roadmap.md` named `getrlimit`,
  `getpwnam`, `sysconf`, `times`, `fnmatch` and `glob` -- every one an
  `AC_CHECK_FUNCS` probe dash has a fallback for and never needs -- and
  named none of the ten missing headers, `SIGPIPE`, `uid_t`/`gid_t`,
  `DT_LNK`, the errno constants or `htonl`. A list somebody has to keep
  true is the shape this repo keeps deleting; this re-measures instead.

  **`-nostdinc` IS THE WHOLE POINT, and leaving it out invents a pass.**
  `USERLAND_CFLAGS` carries `-ffreestanding`, which does NOT stop
  `#include <sys/ioctl.h>` finding `/usr/include`. Measured without it
  this reported 17 of 32 sources compiling, plus two tolibc "bugs" that
  were really glibc's declarations colliding with tolibc's. With it,
  31 of 32 failed on missing headers. Any port built here needs the
  flag for the same reason.

  **Header discovery ITERATES**, because a missing header is a fatal
  error: gcc stops there and never sees the includes below it, so one
  pass found six of the ten. It shims what it found and asks again
  until nothing new appears. The shims are measurement scaffolding --
  enough to see the symbols behind a header, not a proposal for what
  tolibc should ship -- and a header with no shim is reported as such
  rather than counted as understood.

  `--positive-control` withdraws a shim the run just proved was needed
  and requires it to come back as a gap. It does this inside the temp
  copy, never in `userland/libs/libc/include/`, so a crash mid-control cannot
  leave the tree broken.
- **`w16_sound_test.py`** -- a Win16 program's sounds, judged on the
  HOST as `audio_test.py` judges a tone: `sound.exe` (a NASM sample)
  plays a 1102.5 Hz WAV from its own memory (sndPlaySound SND_MEMORY),
  MessageBeep's Asterisk event, two SOUND.DRV quarter notes of value 46
  (A440), then five SetVoiceSound 880 Hz tones (~2.4 s, 3.11's 2.44 ms
  tick) that WaitSoundState waits out, into an AC97 that QEMU's wav
  audiodev records; its exit code also says a 600-byte queue held exactly
  100 sounds. Each
  is told apart by PITCH, counted in 50 ms windows that must hold the
  band three in a row (a sound's edge window reads any pitch). Three
  boots on copies of disk.img: the defaults (the beep is SaunaOS's chime,
  not 2000 Hz), a WIN.INI on drive C: naming a 2000 Hz TEST.WAV for
  SystemAsterisk (the beep is it), and `sounds=off` (exit 0, silence).
  `--positive-control` rebuilds the sample with its WAV's rate halved:
  the WAV check goes red. On demand (`ondemand_sweep.py`): it needs a
  guest with a sound card.
- **`doc_test.py`** -- `/bin/doc` in a guest, over the serial debug
  console. Boots its own VM.

  **The load-bearing check is the pair of searches.** `-k` and `-K`
  would both pass a test that only asked "did anything come back", so
  the tool finds -- ON THE HOST, from the pages themselves -- a word
  that appears in some page's BODY and in no page's name, title,
  category or first sentence, then requires `-K` to find it and `-k`
  not to. A `doc` with both flags wired to the same code passes
  everything else here. Choosing the word on the host rather than
  naming one is what stops the check quietly ceasing to discriminate
  as the pages are edited.

  It also proves the pages REACHED THE IMAGE, which is a different
  thing from being staged (CLAUDE.md's rule about `seed/sync/`), and
  that a name that does not exist SUGGESTS rather than only refusing.

  **What it deliberately cannot see**, said in its own docstring rather
  than faked: the "not a terminal" half -- that `doc ls > out.txt`
  dumps and drops its colour. The kernel debug console has no
  redirection and quotes do not survive it, so `tosh -c` is out of
  reach, and ANSI escapes never arrive as text anyway because
  `kernel/lib/ansi.c` parses them first -- the same reason
  `ls_test.py` cannot assert on colour.
- **`module_test.py`** -- loadable kernel modules the way a person uses
  them: `modload`/`modunload`/`lsmod` on `hello.ko`, the two refusals
  (an unexported symbol named in the log, a truncated file), and the
  check no KTEST can make -- unloading the e1000 MODULE takes the
  network away and loading it again brings it back, with `/bin/netd`
  re-leasing on its own. Asserts the DOWN half before the UP half, since
  a reload that silently did nothing would pass on the boot-time lease.
  ATTACHES to a running `vm.py` guest; ~30 s.
- **`sum_test.py`** -- `/bin/sum` itself, in a guest, against digests
  computed on the HOST. It ATTACHES to a running `vm.py` guest.

  **The load-bearing check is the cross-ring one**: `/bin/hello` in the
  guest is `build/userland/utils/hello.elf` on the host byte for byte, so
  the guest's line must equal what `zlib`/`hashlib` say here. A
  guest-only comparison would pass just as happily with a consistently
  wrong implementation on both ends of it.

  `-c` is asserted BOTH ways -- a manifest `sum` wrote must verify, and
  one carrying a right CRC with a wrong SIZE must fail -- because a
  verifier that says OK to everything is indistinguishable from a
  working one on the OK alone. That second case is also what proves the
  size field in `cksum`'s line is being compared at all.

  **Its fixtures are polled for the LAST TOKEN of the line, not for any
  content.** The debug console's `spawn` returns as soon as the child
  starts, and a read landing mid-write returned `3830823995 ` with the
  rest still to come -- the verify behind it then reported a malformed
  manifest, which reads exactly like a broken parser. Two checks failed
  that way on a `sum` that was correct.
- **`grep_test.py`** -- `/bin/grep` driven by typing into a GUI Terminal
  and asserting through the filesystem, reusing `terminal_probe.py`'s
  `Terminal` helper.

  **Its load-bearing check is the PIPE**, because every other check
  names a file on the command line and would pass with grep unable to
  read stdin at all. The pipe cannot be reached from the kernel debug
  console: that shell splits on spaces and hands `|` to the program as
  an argument, so `dmesg | grep partition` there runs `dmesg` with three
  arguments and fails in a way that reads as a broken grep.

  It asserts against `/tests/sample.txt` (401 numbered lines) rather
  than `dmesg`, which was the first version and was wrong: dmesg's ring
  is finite, so the boot lines a pattern names AGE OUT, and the check
  passed on a fresh boot and failed minutes later with grep working
  perfectly.
- **`multidisk_test.py`** -- two disks on two different drivers, which is
  the one configuration nothing else here boots. Every other tool
  attaches exactly one disk, and that is precisely the shape where
  "which driver ran" and "which disk is root" cannot disagree -- so the
  short-circuiting `if (!blk_virtio_init() && !blk_ahci_init())
  blk_ata_init();` was invisible to all of them while it made a
  machine's second drive not exist.

  Ten checks: both disks enumerated and named, `root=` overriding the
  driver precedence, the OTHER disk's partitions named too, exactly one
  device marked as the root, a named device resolving where an unknown
  one is refused by name, and an unknown `root=` reporting what it does
  have and booting anyway. Its positive control is the old short circuit
  restored, which reddens six of them and whose detail shows the
  bug outright -- `have ahci0` and no `ata0` at all.

  **IT IS ALSO THE ONLY GUEST WITH SOMEWHERE SAFE TO WRECK A VOLUME.**
  TFS3's post-commit journal KTEST ends with the volume read-only
  until something replays it, which on the root would fail every test
  after it -- so that test skips unless a second TFS3 mount exists,
  and this tool mounts one at `/mnt2` and runs `ktest fs` there. Three
  of the ten checks are that: the second volume really mounts (it
  could not, when every backend declared `max_mounts = 1`), the suite
  runs with nothing skipped, and what went read-only was `/mnt2` and
  not `/`. It `fsck repair`s first, as `ktest_run.py` does, or the
  blocks a killed boot leaked fail four unrelated fsck assertions.

  It builds its own boot image per phase (`make iso KCMDLINE=...`, then
  a copy), so **it rewrites `disk.img`'s GRUB line** and puts it back at
  the end. **So it cannot run BESIDE another tool**: its rebuilds land
  mid-run under anything else booting `saunaos.iso`, which `iso_guard`
  then refuses as stale (seen with `install_test.py` alongside). On
  demand, not in any gate.
- **`rootdisk_test.py`** -- **every SATA drive, and which disk is the
  root.** Eight boots, on demand. **A**: three SATA drives on TWO
  `ich9-ahci` HBAs, the system disk on HBA 0 PORT 1 (booted by
  `bootindex`), a host-seeded data disk on port 0 and a blank disk on
  the second HBA -- three drives named, the root from port 1 rather than
  the first drive, the port-0 disk reading the host's marker AND keeping
  a file across a remount, `ahci` marking all three, `ktest ahci` with
  none skipped. **B**: a blank virtio disk beside an IDE system disk,
  with `bootpart=` and then with it removed from grub.cfg
  (`install_grub.drop_boot_word()`): both must root on IDE. **C**: two
  system disks, the virtio one with its partition GUIDs changed
  (`mkpart_test.regenerate_guids()`, `sgdisk -G`: both GPT headers
  re-CRCed) -- `bootpart=` must pick IDE and removing it must flip the
  root to virtio, which is what proves `bootpart=` decided rather than
  agreed. **D**: an old install's grub.cfg (no probe lines, no word):
  `install --bootloader` must preview and then add `bootpart=` to both
  entries, add nothing a second time, and the next boot must carry it.
  **E**: the ISO booted as a CD beside a system disk -- GRUB's probe
  answers `none` there (it does not fail), the boot must reach the
  kernel, and the kernel must say the loader named no partition.

  Its controls (`mutate.py`): AHCI stopping at one drive per HBA
  reddens every check of A; the content rule reduced to precedence
  boots B into ramfs; ignoring `bootpart=` reddens C's first check and
  only that. A's serial socket lives under `--tmp`, and a UNIX socket
  path over 107 bytes makes QEMU refuse to start -- keep `--tmp` short.
- **`net_test.py`** -- the network stack end to end, on both NICs, with
  the verdict taken on the HOST. The KTESTs in `kernel/net/net_test.c`
  drive the protocols through `eth_input()` and need no network at all,
  which is what makes them fast and what makes them silent about
  whether a frame ever reached a wire.

  **Two independent oracles, and neither shares a line with the guest.**
  QEMU's user-mode network (SLIRP) answers the pings, so a wrong ARP, a
  wrong checksum or a wrong destination is simply never replied to. And
  every frame is dumped to a pcap (`-object filter-dump`) and decoded
  here, with the IPv4 and ICMP checksums RECOMPUTED from the bytes on
  the wire -- the same call `regex_hostcheck.py` and `fat32_test.py`
  make, because SLIRP can be lenient about something a decoder cannot.
  The decoder is thirty hand-written lines rather than scapy: a
  dependency the machine might not have is a check that silently stops
  running.

  The TCP phase also runs `speedtest --url` against its host server (a
  rate over two parallel connections, and more than one request served)
  and checks in the pcap that the guest's SYN offers window scaling --
  the one TCP option whose absence changes nothing a KTEST can see.

  The e1000 (the card QEMU's default machine has always
  had, so it needs no flag); **virtio-net, which is the only thing here
  that reaches `kernel/drivers/virtio/virtio_net.c`**; two cards at
  once, moved onto DIFFERENT subnets so that "the traffic left through
  the card that owns the subnet, and not the other one" is a per-device
  counter rather than "a ping worked"; a machine with `-nic none`, where
  the assertion is that `ping` reports instead of hanging; and the ARP
  retransmit rate, which is a regression test with a measurement behind
  it -- two pings at an unanswered address put **104 frames** on the
  wire before `kernel/net/arp.c` rate-limited requests, and 5 after.

  Then the four that came with UDP. **A real Python socket on the host**
  receives the guest's datagram, echoes it, and the guest reads the echo
  -- a round trip where neither end shares a line with the other; the
  capture is then checked for a valid UDP checksum, which covers a
  pseudo-header that is on no wire and is the one thing a stack can get
  wrong while agreeing with itself perfectly. **ICMP port unreachable**,
  judged from the capture after the HOST sends to a port nothing is
  bound to (`hostfwd`, the only way to make a datagram arrive at the
  guest). **DHCP on 192.168.76.0/24**, which is the load-bearing DHCP
  check: on QEMU's default network a real lease and a hardcoded
  10.0.2.15 are indistinguishable. Nobody types anything -- init runs
  the client -- so this is also the check that a machine configures its
  own network at boot.

  **Phase 9 is LINK-LOCAL**, on the one segment SLIRP cannot be: a
  socket netdev whose only peer is the test itself, so nothing answers
  DHCP. Two boots, because the question worth asking is not "did it pick
  an address" but "does it give one up when somebody already has it" --
  the first boot claims one unopposed, the second is ANSWERED for that
  exact address by `LinkPeer` and must end up somewhere else. The peer
  is the oracle as well as the neighbour: the ARP probes (sender
  0.0.0.0) and the announcements (sender = the claimed address) are read
  off the wire on the host. Predicting the address here instead would be
  checking this OS's arithmetic against a copy of itself.

  And **DNS**, which SKIPS when the
  host itself cannot resolve -- an offline machine is not a bug in this
  OS -- while still checking the "no nameserver configured" path, which
  needs nothing but the guest.

  **Phase 11 is TCP**, against python's own `http.server` on the host --
  local rather than a site on the internet, because the suite must not
  depend on this machine having connectivity (the DNS phase, which
  genuinely does, SKIPS instead). An independent server will not
  complete a handshake the guest gets wrong, and the capture is checked
  for a real three-way handshake plus TCP checksums recomputed here over
  the pseudo-header -- the same trap UDP has, made worse by TCP having
  no length field of its own.

  **The last phase is `inetd`**, and it carries the two checks the
  serial server cannot pass. `/bin/cat` is run as an echo server --
  it copies fd 0 to fd 1 and knows nothing about sockets, so bytes
  coming back are the only available proof that the accepted connection
  really landed on the child's standard streams. Then a client connects
  and **says nothing**, which parks a one-at-a-time server in `read()`
  forever, and a second client is required to get a complete response.
  "Both were answered eventually" is what a serial server passes, so the
  first connection is left hanging on purpose rather than closed. The
  control was run: against plain `httpd` the second request times out.
  A third check asks for **ten connections in a row, each compared
  against the staged bytes** -- a server that answers three and wedges
  passes both of the others.

  **It also found a harness bug that had been reading as an OS limit.**
  Nothing here drained the serial socket while a server held the
  console, so the guest filled COM1's buffer and stalled in its own
  write -- which looks exactly like a server exhausting sockets after N
  connections, and was diagnosed that way first. The server phase had been
  passing with four connections of margin, so one extra log line per
  request was enough to turn it red. `Shell.drain_start()` is the fix,
  the same thing `usb_test.py` does while it types.

  It found four real defects while being written: the ARP storm above; a
  sequence number burned by every retried send, so a capture showed a
  ping starting at 4; sockets never released on close, so the third run
  of a program opening four of them could open none; and one in the
  harness worth knowing, since it looked like a guest bug -- SLIRP is a
  NAT and rewrites the source port, so the guest's own ephemeral port is
  visible only in the capture. Boots ten guests against a COPY of
  `disk.img`; on demand, not in any gate.
- **`ntp_test.py`** -- network time end to end, against a server on this
  machine's own loopback. `kernel/core/ktime_test.c` proves the wall
  clock can be set and read; it says nothing about whether a packet on
  the wire produces the right number, which is this half.

  **Nothing leaves the machine, and it works with the cable out.** The
  server is a plain Python UDP socket on `127.0.0.1`, reached because
  SLIRP maps whatever the guest sends to `10.0.2.2` onto the host's
  loopback -- `net_test.py`'s UDP phase does the same. It binds an
  unprivileged port rather than 123, which is the whole reason `/bin/ntpd`
  has a `-p` flag: a test server cannot bind 123, and the alternative was
  a suite that reached a real time server on the maintainer's connection.

  **The oracle is the host, and it judges both directions.** The reply
  carries an instant the test CHOSE -- a date in 2013, BACKWARDS from any
  plausible boot, so a guest that ignored it and let its clock run
  forward cannot pass by accident. And the REQUEST is validated here:
  version 4, mode 3, 48 bytes, a non-zero nonce. A client that sent
  version 3 would be answered by a real server and would still be wrong.

  **It asserts on state, never on what the guest printed.** An earlier
  version parsed `ntpd`'s own messages and reported a working client as
  dead, because a spawned program's stdout does not dependably reach the
  serial console this drives (`settings_test.py` records the same trap).
  What the guest DID is visible in two places that cannot lie: the
  datagram the host received, and the clock afterwards.

  `--positive-control` answers with a deliberately wrong epoch conversion
  -- 1900 where 1970 was meant, the single likeliest client bug. It lands
  the guest in 2079 and the clock checks must go red; the tool INVERTS
  its verdict under the flag, so a control that changes nothing is
  itself reported as a failure.

  **Its reboot check SKIPS under QEMU, and says so rather than passing.**
  QEMU re-seeds its emulated MC146818 from the HOST clock on machine
  reset, so a guest's CMOS write cannot outlive a reboot however correct
  it is -- measured, by watching the guest's own "rtc: hardware clock
  reads" boot line come back matching the host to the second. The write
  itself is proved in the same boot by the KTEST that reads the hardware
  back through `rtc_read()`.

  **On the bare-metal laptop it does survive, measured 2026-09-03**: a
  sync, a reboot, and `clock.step.count` back at 0 with the time still
  correct -- so the clock had come from the CMOS rather than from
  anything that ran after boot. Boots one guest against a COPY of
  `disk.img`; on demand.
- **`partition_test.py`** -- boots SaunaOS with its filesystem **inside**
  an MBR or GPT partition. The only thing that exercises `vfs.c`'s
  boot-time partition scan and `block_part.c`'s window end to end; the
  `partition` KTEST suite proves the encoder and parser agree on a RAM
  disk and cannot prove either of those.

  **Its load-bearing check is `df`, not "it booted".** A kernel that
  ignored partitions entirely still boots -- it finds nothing at LBA 0,
  refuses to format a partitioned disk and runs RAM-only, which looks
  like a bad image rather than a missing feature. And one that found
  the partition but got the window wrong would mount something and
  report the DISK's size. So the assertion is that the mounted volume
  is 256 MiB while the image is 2 GiB, which only a correct window
  produces. It also reboots (a write that reached only a cache would
  pass a same-boot read-back), reads `parttable` from inside a
  partition (which the old `ata_read_sector()` path could not do), and
  finishes by driving `/bin/mkpart` in the guest -- including the
  refusal without `confirm`.

  ON DEMAND, not in the gate: two images, four boots. Same category as
  `virtio_boot_test.py` and `live_boot_test.py`.
- **`fat32_test.py`** -- FAT32 and the mount table, checked against an
  **independent implementation**. `kernel/fs/fat32_test.c` formats a
  512 KiB RAM volume and drives the backend directly, which proves the
  driver agrees with ITSELF -- and a shared misreading of the format
  passes both halves, because the same person wrote the writer and the
  reader. So the oracle here is the HOST: `mtools` reads back what the
  guest wrote and `fsck.fat` audits the volume afterwards, neither
  sharing a line with `kernel/fs/fat32.c`. Same call as
  `regex_hostcheck.py` (against GLIBC) and `uimg_hostcheck.py` (against
  libjpeg).

  It also covers the half no KTEST can reach: that the ESP on the REAL
  disk -- a volume written by mtools, holding GRUB's own files --
  mounts at `/boot` on an ordinary boot, and comes up READ-ONLY.

  **Three checks that discriminate**, each replacing one that would
  pass on a broken driver. "`ls /boot` lists something" passes on a
  driver that mangles every long name, so it reads GRUB's own
  `grub.cfg` and looks for text only a correct chain walk produces. "A
  file written reads back" passes on a driver whose format is privately
  wrong, so the seeded `/bin/ls` is extracted with mtools and compared
  byte for byte against `seed/sync/bin/ls` (not `build/`'s, which the
  seed strips: comparing with that failed every run). And "the volume
  still works" passes on one leaking clusters or cross-linking chains,
  so `fsck.fat` has the last word. **Names past ASCII both ways**
  (UTF-8 step 3): mtools, in a UTF-8 locale, writes `Käse.txt` and a CJK
  name for the guest to list; the guest `cp -r`s them (which creates the
  names without typing them on the serial console) and mtools lists the
  copies, with Windows' `K_SE~1.TXT` alias. Red on the build before.

  ON DEMAND: two boots against a COPY of `disk.img` (never the real
  one), and it SKIPS cleanly without `mtools`. Needs `dosfstools` for
  the audit half.
- **`utf8migrate_boot_test.py`** -- the `utf8migrate` SERVICE across two
  boots, judged on the HOST. A copy of `disk.img` gets Latin-1 names and
  contents written by `tfs3_writer` (raw bytes the serial console could
  not type), boots, shuts down cleanly and is read back on the host: the
  name and the bytes converted, an already-UTF-8 file untouched, the
  marker and the log written; a second boot must log "nothing to do" and
  leave the disk as it was. `/tests/utf8migrate_test` (usertest) covers
  the program's rules over a fixture with `--root`; this covers what it
  cannot -- that the boot runs it, over the real root, once.
  `--positive-control` deletes the service's descriptor from the copy,
  and the conversion checks go red. `ondemand_sweep.py`; two boots, about
  a minute.
- **`w16_install_test.py`** -- `w16 --install` and the Install Windows 3.x
  dialog in a guest booted from a fresh image, from `gen_win3_disks.py`'s
  synthetic floppy images and folder. The CLI: `--dry-run` plans every file
  and writes nothing; the install puts every file where that generator's
  table says (sizes, and two contents read back), decoys absent, and
  Program Manager's `RLn.GRP` groups beside them, each in PROGMAN.INI; WIN.INI
  kept, then made afresh with `--replace-ini`; a set missing CALC.EXE
  exits 1 naming it. The dialog: Retro Launcher's bar while drive C: has no
  Windows, the bar opening the dialog, Disks to Check to Done by its own
  log lines, the launcher's folders gaining `C:\WINDOWS` as Windows, and
  the bar gone once the launcher has the focus back. ~1.5 min under KVM.
  In `ondemand_sweep.py` (`w16install`).
- **`dup_scan.py`** -- where is the tree actually duplicating code?
  Normalises away comments, blanks, brace-only lines and whitespace,
  hashes a sliding window (`--window`, 6 by default), and reports every
  window occurring in two DIFFERENT files, each extended to its maximal
  run and claimed so one 40-line clone is reported once rather than as 35
  overlapping windows. Vendored `third_party/<port>/upstream/` is skipped -- that is somebody
  else's duplication. **It ALWAYS exits 0**: whether a block is worth
  extracting is a judgement call, and a check that failed the build on it
  would be wrong most of the time.

  **What it cannot see, said plainly because a clean run is the most
  misreadable kind of result**: the same idea written twice in different
  words, and an API read two different ways by its callers. The menubar
  sentinel in `docs/conventions/gui.md` -- six apps, three readings, one
  latent keyboard-only bug -- was found by COUNTING CALLERS of one
  function, not by this. A clean run means "no copy-paste", never "no
  duplication".

  First answer, 2026-09-17: 71 duplicated lines across 27 GUI files and
  75 across 88 `/bin` programs, i.e. almost nothing -- which is why the
  work that came out of that audit was a contract fix rather than an
  extraction. In `ondemand_sweep.py`.
