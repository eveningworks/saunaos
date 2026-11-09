# `tools/tests/gate/` -- Gate tests

One entry per tool in `tools/tests/gate/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by `tools/runners/preflight.sh`, every gate: the boot-free ones by a
loop over this folder, then `boot_smoke_test`, `ktest_run` and
`usertest_run` in that order. A new test here is in the gate the moment
it is saved.

- **`boot_smoke_test.py`** -- fast non-GUI boot check — see
  `docs/testing.md`

- **`ktest_run.py`** -- drives the in-kernel test suite over serial and
  turns it into an exit code -- what `make test` and CI run;
  `--virtio-disk PATH` attaches a second disk on virtio-blk, which then
  carries the filesystem while the IDE drive stays for the
  `[ata]`/`[atac]` suites, `--ahci-disk PATH` the same on an ICH9 AHCI
  controller -- a COPY of `disk.img`, whose shared PARTUUIDs plus AHCI's
  precedence make it the root, and the run FAILS if the root did not
  land on `ahci0` (control: the copy with
  `mkpart_test.regenerate_guids()` roots on IDE and fails), `--mem MIB`
  sizes the guest (256 by default, and the `mm` and `paging` above-4-GiB
  checks SKIP below 4096 -- `highmem_test.py` is the runner that refuses
  the skip), and `-v` prints the WHOLE transcript, boot messages
  included. **It stops the desktop before the suite** -- `service stop
  toywm`, after waiting for init to report it ready, because the
  winshare KTESTs need the compositor role and refuse to take it from a
  live desktop; nothing about that stop survives to the next boot, and
  the run FAILS if the role was still held when the suite ran, so a
  silently-skipped suite cannot read as a pass. **It stops `logd` too,
  and waits for the DHCP lease**, because the suite needs a QUIET DISK:
  the `fs` and `atac` tests compare disk usage and flush counts against
  themselves, and `logd` persisting the suite's own kernel-log lines
  landed between those reads on 4 fresh boots in 6, while the dhcp
  one-shot writes `/etc/resolv.conf` a few seconds into the boot. **And
  it runs `fsck repair` first, printing what it reclaimed**: a guest
  killed with a `batched` inode update still deferred leaks the blocks
  past it by design, every harness here kills its guest, and the suite's
  first fsck asserts clean -- so the precondition is established rather
  than inherited from whichever tool booted the image last

- **`usertest_run.py`** -- runs the self-checking ring-3 diagnostics in
  `/tests` (`libc_test`, `fpu_test`, `klineedit_test`, `newsyscalls_test`,
  `file_test`, `write_test`, `exit_test`, `random_test`, `memtest`,
  `guard_test`, `malloc_test`, `wrap_test`, `focusring_test`,
  `uvterm_test` -- the Terminal's grid as UTF-8 -- `imgshare_test` --
  two instances of a program share its code through the image cache,
  judged per file and by the second instance's SHARED vs private
  bytes -- and more) as one
  pass/fail table, asserting BOTH an
  exit code and required output. In `preflight.sh`. It fills a real gap:
  `make test` runs inside the kernel and `gui_regress.py` covers the
  windowed clients, so nothing ever ran a plain `/tests` binary except a
  person typing `run <name>`. **Read its `EXCLUDED` list before adding
  to it** -- a test that faults on purpose, blocks on the serial port,
  needs a desktop, or needs a parent to spawn it will fail in a way that
  says nothing about the code under test. `pipe_test` is the worked
  example: it exits 3 under `run` because its `waitpid` finds no parent,
  and passes fine under the KTEST that spawns it properly.
  **`--serial-log PATH` keeps the guest's serial output** (vm.py's own
  flag, passed through) -- the one record left when the EMULATOR hangs,
  since the table is only printed at the end.
  **`--transcript PATH` keeps every test's captured output, passed or
  not.** A `/tests` program's stdout and stderr go to the CONSOLE, which
  the harness reads over the debug console -- they are NOT on the serial
  log, so `--serial-log` alone never shows what a passing test printed.
  **Verdicts are read with the debug console's `readfile`, not `sh
  cat`** (`vm.parse_framed()`): one frame, taken by its declared length,
  with preemption off and the kernel log held off the wire while it goes
  out. `cat` crossed the wire in chunks and a log line between two of
  them tore verdict lines -- 4 full runs in 25 reported a passing test
  as failed. Any tool reading a file off the guest should do the same.
  **The frame's length is in BYTES** and the reply is decoded UTF-8, so
  `parse_framed()` measures in bytes, and `vm.py` decodes the socket
  INCREMENTALLY: the serial console hands over a byte or two per read,
  and decoding each read alone used to split every multi-byte character
  -- so a file holding any non-ASCII text came back mangled and never
  matched its frame (found by `browser_hostcheck.py`, 2026-10-12).
  **An expected exit code of `None` means SPAWN IT INSTEAD**, and judge
  it by what it printed. `run` uses the legacy loader, which has no
  scheduler slot, so anything reaching the window server is refused
  there -- `wrap_test` needs a real font (`ugfx_font_init()` goes
  through `SYS_WIN_REQUEST`) and measures nothing at all under `run`,
  where every width comes back 0 and every check passes for the wrong
  reason. A spawned test has no exit code to read, so its printed
  verdict carries the whole assertion, and it writes that verdict to
  `/tmp/<name>.out`: a spawned program's console output arrives while
  the harness is between commands, where it is dropped, so the harness
  waits on the ARTIFACT rather than on the timing. **That file is
  `UTEST_VERDICT_FILE`'s now, not each test's own** -- every self-checking
  `/tests` program reports through `userland/libs/base/utest.h`, which streams
  each line to the file as it is produced rather than buffering the
  transcript and writing it at the end, so a test that dies part-way
  leaves the lines it reached. **The table therefore states its expected
  strings only where a test DEVIATES from the harness's one epilogue**;
  `None` in both string columns means the default, which is what took it
  from a string per test to a handful of exceptions. `focusring_test` is
  the same shape and covers the eleven widgets that accept keyboard
  focus, drawing each into a plain surface and counting accent pixels
  -- **both ways**, absent unfocused and present focused, since a
  one-sided check passes on a control that rings itself
  unconditionally. Its load-bearing checks are the ROW ones: a ring
  round the whole box and a ring on the selected row both put accent
  pixels down, and only the height tells them apart.
- **`/bin/mkfiles`** (a guest program, not a host tool, but this is
  where anyone looks for it) -- fills a directory with N files to test
  the filesystem at scale: `mkfiles /big 5000`, `mkfiles /docs 100 512`,
  `mkfiles /docs 100 0-64000`, and `mkfiles --verify` to read them back.
  Content is derived from (file index, offset), so `--verify` proves
  every file still holds ITS OWN bytes -- a constant fill cannot detect
  two files sharing a block, since both read back the constant and look
  perfect. Sizes in a range are derived from the index too, so a verify
  reproduces them without being told a seed. **Pass `vm.py --timeout`**:
  the default is 30s and 5,000 files takes ~50s. It found the
  binary-write truncation the day it was written.
- **`remote_hang_test.py`** -- `remote.py` cannot hang, checked against
  fakes on localhost in about seven seconds (in `preflight.sh`): a TFTP
  server that only repeats a stale ACK (`put` must give up), one that
  resends one block forever (`get` must), a telnet peer that never stops
  talking (`read_until_line` must still time out), and `_mark_executable`
  handed a session must use it rather than open one. A give-up must be
  THE give-up -- a `RuntimeError` saying "no progress" -- since a
  `NameError` in the code under test ends the call too. `--against PATH`
  runs the same checks on another copy of `remote.py`; the pre-fix one
  (`git show 807338a4:tools/dev/remote.py`) fails the first four, three of
  them as real hangs the test's own 20 s limit ends. A fifth fake LOSES
  the ACK that ends a window and resends it faster than the client's
  timeout, as tftpd does: `get` must re-ACK on the duplicate and finish
  (`git show e56925d4:tools/dev/remote.py` fails it).
- **`tfs3_writer_test.py`** -- does the host seeder's OVERWRITE hand
  every block back? Host-only, no VM, about a second: it formats a
  scratch image, writes one file per block-map level (direct,
  single-indirect, double-indirect), rewrites each with different
  content of the same size, and asserts the allocation bitmap did not
  grow. **The sizes ARE the test.** A free path can be right for one
  level and wrong for the next, and the seed tree has exactly one file
  over the single-indirect ceiling (`/install/kernel.bin`, ~4.8 MB) --
  so a leak of its two double-indirect tables sat in the seeding path
  with every existing test green, reddening `ktest`'s `fsck` checks on
  any second gate run against one image. Triple-indirect is deliberately
  NOT covered: it starts past 4 GiB. To watch it go red, restore
  `delete_path()`'s old `img.free_block(node["ptrs"][12])` in place of
  the pointer walk -- the double-indirect case fails by exactly 2 and
  the other two stay green. Run by `preflight.sh`, because a leak here
  corrupts the gate's own fixture. **Its second half is HARD LINKS**: a
  seed tree whose `gcc` and `cc` are one host file must sync to one
  inode with two links, stay so on a re-sync, split into two files with
  their own bytes when the tree splits them (freeing nothing `cc` still
  reads), link again, and lose one name to a prune without losing a
  block. Seen red against the writer before it made links (6 checks).
  **Its third is the SHORT LAST GROUP**: a format's last group's bitmap
  bits past the volume's end must read in use, as the kernel's
  `t3_bbm_default()` writes them, or `fsck` on a freshly seeded image
  counts every one as "referenced but free". Seen red before the fix
  (8250 clear on the 96 MiB fixture).
- **`sdk_test.py`** -- host-only, a few seconds: `build/sysroot` is
  compared byte for byte with a fresh `stage_sdk.py` staging, and disk.img's
  `/usr/include` and `/usr/lib` with `build/sysroot`, both directions (a
  header the tree deleted that is still on the image fails); a program
  using stdio, malloc, setjmp, varargs, errno, unistd and a toolkit header
  is compiled `-nostdinc` against the sysroot ALONE and linked from its
  `/usr/lib`, and must be `ET_EXEC` with `_start` at the entry; on a scratch
  image, `sync`'s `exact` prune removes a dropped file and directory and
  keeps what is still seeded and what is outside the list.
  `--positive-control` tampers with a copy of the SDK (an edited
  `stdio.h`, a stale header, no `crt0.o`) and must fail. In `preflight.sh`.
- **`todisk_test.py`** -- its check, host-only and about a second: a
  scratch partitioned image, a folder with files from empty to over the
  direct-block ceiling, read back through `tfs3_writer`'s reader. Bytes
  arrive at the paths they have under the folder; a second run writes
  nothing; a changed file is replaced and an untouched one is not; a file
  taken out of the folder stays; `--dry-run` leaves the image identical;
  an image with no volume is refused; so is one a stand-in `qemu-system`
  process has open. `--positive-control` removes the QEMU guard and must
  turn exactly those two checks red. In `preflight.sh`.
