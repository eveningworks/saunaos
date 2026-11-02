# `tools/runners/` -- Runners

One entry per tool in `tools/runners/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`hostchecks.py`** -- every `tools/hostchecks/` tool, found by folder,
  in parallel (host-only: nothing boots). `--gate` runs the ones whose
  RUNNER says `gate`, which is what `preflight.sh` calls; the rest run
  in the sweep. A missing oracle is a SKIP from the tool's `needs`, a
  `manual` one is listed and never run, `--positive-control` passes the
  flag to each and inverts the verdict (a control passes when its check
  went red; a tool with no control is a skip), `-k`, `--list`, `--logs`.
  Each check stays a standalone script.

- **`checks.py`** -- every `tools/checks/` rule in parallel, one line
  each and the whole output of a failing one; `-k`, `--list`. What
  preflight runs in place of a step per rule, so a new rule is in the
  gate the moment it is saved. Each rule keeps its own named waiver
  (CLAUDE.md's table).

- **`qemu_matrix.py`** -- runs the kernel test suite against SEVERAL
  QEMU versions in Docker (6.2, 7.2, 8.2 -- the last is what GitHub's
  runner has). Nothing is BUILT in the container: the ISO comes from
  the host's `make iso` and is mounted read-only, so what runs is
  byte-for-byte a normal build and there is no second toolchain to
  drift. **15 s per version** on a cached image (69 MB each), so ~45 s
  for all three -- cheap enough to run whenever "could this behave
  differently on another QEMU?" has a plausible yes: a driver, a poll
  loop, a timeout, a clocksource, DMA. **Run at a RELEASE and when
  asked** -- deliberately not part of `preflight.sh` (which must not
  start requiring Docker) and not an automatic per-change habit. A
  disagreement between versions is a REPORT: an older QEMU can be
  quirky rather than right, so it is the maintainer's call whether a
  finding is worth fixing, filing, or noting as a known limitation.
  **It exists because a whole class of bug is invisible on one QEMU.**
  A virtio-blk defect -- `virtqueue_poll()` spending a ~12 ms budget it
  believed was 5 s, then letting late completions desync the used ring
  -- never appeared on QEMU 11.1 and reproduced every time on 8.2.2,
  because which clocksource the kernel picks depends on the host. It
  was found through CI instead, at six pushes and several wrong
  diagnoses, on a loop costing ~90 s at best and sometimes failing for
  reasons of GitHub's own (one run died with `apt-get install` timing
  out after 8 minutes). This makes the second QEMU local and fast.
  **`--suite usertest` runs the ring-3 `/tests` suite instead of ktest,
  and `--runs N` turns a row into a RATE.** Both exist because the two
  suites drive the disk differently: ktest never hung on 8.2.2 while the
  ring-3 suite deadlocked the emulator 4 runs in 17 (see "An ATA command
  is done when the bus master says so" in `docs/decisions.md`). A hung
  run is reported as `HUNG`, its container killed BY NAME -- a timeout
  on the `docker run` client alone leaves the container and its QEMU
  running -- and each run's output and serial log are kept in `.qemu_matrix/`.
- **`preflight.sh`** -- one command running `make clean && make all &&
  make iso` + a NOGPL build + `runners/checks.py` (every static rule in
  `tools/checks/`, `check_deps.py` and `check_layout.py` among them) +
  `genttf.py --check` + `runners/hostchecks.py --gate` + every
  boot-free test in `tools/tests/gate/` (found by folder) +
  `boot_smoke_test.py` + `ktest_run.py` + `usertest_run.py`
  + a `git status --short` summary (`fs_switch_test.py` is NOT in it --
  that one needs a disk copy and a longer boot cycle, run it yourself
  after `kernel/fs/` changes), so
  "am I safe to deliver?" is one call instead of three run by hand.
  `--skip-clean` skips the initial `make clean`. Every run is recorded
  in the local test history (`test_history.py`) with its whole output
  and the step that failed, and every tool it runs names it as parent.
  **It refuses to run until this repository has a commit identity of its
  own.** That identity is per-repository, so a CLONE DOES NOT CARRY IT
  and a fresh checkout falls back to the global one -- the real name a
  history rewrite once removed from every commit here, with nothing in
  git warning before the first commit puts it back. What is required is
  that a LOCAL identity is set at all, not that it matches this
  project's own (`SaunaOS` / `noreply@saunaos.local`, or a cloud
  session's `Claude` / `noreply@anthropic.com`; it notes any other):
  demanding the exact value would refuse anyone working on
  a fork, who has every right to commit as themselves. The hazard is
  "you did not decide", not "you chose wrongly". In the gate rather than
  in a hook because `.git/hooks` is not cloned either.
  **It REFUSES to start while a `vm.py` guest is running**, and that
  guard is worth its four lines: `make iso` re-seeds `disk.img` while
  the guest holds a write lock on it, and the first thing to complain is
  `boot_smoke_test` with "qemu exited early" -- which reads as the
  kernel failing to boot, several minutes and a whole clean rebuild
  after the actual mistake. Run `python3 tools/lib/vm.py stop` first.
- **`gui_regress.py`** -- runs every tool in `tools/tests/gui/` (found by
  folder; a tool's RUNNER header says what it covers and, as `vm`, any
  guest flags it needs), each against
  its own copy of a FRESH image and its own VM, and prints one
  pass/fail table -- and records a row per tool, under the script's
  name, plus one for the run in the local test history
  (`test_history.py`), each with its output: `--logs` is no longer the
  only copy of a run that has scrolled away. This is the standard check
  after touching `apps/ui/`, `userland/`, or anything the WM draws.
  Tools are **STARTED longest-first** (`pick_order()`), because a
  parallel run cannot end before its slowest member does and `forcequit`
  (71s) used to sit eleventh of fourteen and finish alone -- that sort
  alone took a run from 1:56 to 1:30. The cost is each tool's MEASURED
  median over its last ten passes in `test_history.py`'s record (a
  hand-kept `COST_S` table until f662b8e3); a tool never measured here
  is assumed SLOW, so a new one cannot become the straggler by
  omission. The summary is printed toolkit first (`FIRST`), then by name.
  **THE IMAGE IS BUILT FRESH BY DEFAULT** (`fresh_disk.py`, ~20 s), not
  copied from `disk.img`: `make iso` syncs rather than reformats, so a
  window position, a setting or a launcher an earlier test or a `make
  run` left there reached every tool, and four failed that way on
  2026-09-30. `--disk PATH` uses an image as it is, `--no-fresh` uses
  `disk.img` as it is -- and both print what drifted onto it first.
  `-k NAME` for a subset, `--logs DIR` to keep each tool's full output,
  `--list` to see what's in it. The per-tool fresh image and fresh VM
  are the parts that matter: several tools write files, and every one
  of them expects an empty desktop -- a tool inheriting the previous
  one's state fails in ways that look exactly like real widget bugs.
  It runs **one tool per physical core at a time**, capped at 16
  (`-j N` to change, `-j1` for the old serial behaviour). Whether the
  SMT sibling helps depends on the host: the first 16-thread dev host
  was green and fastest at 16 jobs, its i7-11700 replacement turned 15
  tools red there and 3 at 8 -- the comment above `DEFAULT_JOBS` has the
  runs. Each tool gets its own **VM slot**:
  `vm.py --instance N` derives that VM's pidfile, serial socket, QMP
  port and VNC display from one number, and the slot is LEASED for as
  long as the VM lives rather than derived from the tool's position in
  the list. **`--first-slot N` leases from N up** -- the run lock is
  per checkout, so a suite in a worktree beside another checkout's
  tests (whose slots start at 0) needs it. Use `--instance` yourself any time you need a second
  headless VM alongside one that's already up -- **`--instance auto` takes the lowest FREE slot and
  prints which one**, the right thing when a `gui_regress.py` may be
  running in another terminal. It prints because a slot that differs
  per run must still be replayable (`--instance <that number>`), and
  it is a narrowing rather than a lock -- `port_guard.py` at the
  launch is what makes a residual clash loud instead of silent.
  **It does NOT fix CPU contention**: a tool run beside the full
  suite is port-safe but competes for cores, and an animating app
  can fail a settled-frame comparison under that load (measured --
  `gfxdemo` failed two checks with five guests up and passed 23/23
  alone). Slot 0 is the plain
  `.vm.pid`/`.vm.serial`/4445 every existing caller assumes.
  **WHERE THE WALL-CLOCK TIME ACTUALLY GOES, measured rather than
  assumed, because the obvious answers were both wrong.** A full run
  WAS ~425 tool-seconds across ~25 tools (it is ~408 now, see the end of
  this entry), so the wall clock was the SLOWEST SINGLE TOOL, not the
  total: at `-j4` it was 76s and at `-j8` 72s,
  because both are pinned by the same one tool. **KVM (`--kvm`) buys
  almost nothing either** -- 64s -- since what the slow tools spend
  their time on is WAITING for real timeouts, which no amount of guest
  CPU shortens. The lever that worked was cutting the floor itself:
  `forcequit_test.py` waits out the WM's not-responding timeout about
  ten times, so shortening that timeout for its run (`gui pingtimeout`,
  the 663d63b pass) took it from 72s to ~35s. A later pass made every
  tool wait on an OBSERVABLE rather than a fixed sleep -- `enter_gui()`
  polls the desktop ready instead of sleeping ~3s per tool, and
  `menubar`/`taskmgr` poll the app's own layout/log reports -- taking the
  sum to ~408 tool-seconds and leaving `notepad` (~41s) the floor. The
  general lesson is worth more than the seconds: **on a fan-out like
  this, look at the maximum, not just the sum** -- but once the max is
  cut, the sum over the job count starts to bind, which is why the two
  passes complemented each other and why the `-j` cap could then rise to
  12 for hosts with cores to spare.

  `damage_sweep.py` is deliberately NOT in it (much slower under
  `gui damage verify on`, and it has its own `--positive-control`
  protocol) -- run that separately.

  **A SECOND CONCURRENT RUN IS REFUSED**, with an flock under `build/`.
  Every tool takes a VM slot and slots start at 0 for every run, so two
  suites at once fight over the same pidfiles, serial sockets and QMP
  ports -- and it does not fail as a port clash. `port_guard` refuses
  some launches and the rest surface MINUTES later as `BrokenPipeError`
  or a screenshot that was never written, in whichever tool happened to
  be mid-command; seven tools "failed" that way in one run here, none of
  them at fault. An flock rather than a pidfile check, so a run killed
  with -9 leaves nothing to clean up.
- **`flake_hunt.py`** -- one GUI tool run N times, reporting which
  CHECKS failed and how often (`python3 tools/runners/flake_hunt.py menubar -n 6
  --keep /tmp/flake`). The sibling of `damage_hunt.py`: that one varies
  a SEED, this one varies nothing and asks whether a tool is
  intermittent. Reach for it the moment a tool fails once and passes on
  re-run -- a rate is the diagnosis, a verdict is not, and this repo has
  a recorded case of a real bug coming back clean six times before
  reproducing five times running. Scores a run that never printed a
  summary as `error`, not `pass`: a run that measured nothing must not
  look like a good one. **IT DOES NOT RESET `disk.img` BETWEEN
  RUNS, so any rate involving the filesystem is contaminated** -- a
  KTEST run leaves state behind, so runs 2..N inherit run 1's and the
  rate climbs for reasons that have nothing to do with the code.
  Measured: `ktest` reported fs checks failing 2-of-4 that way, and
  passed 3 of 3 when each run got its own `make clean-disk && make iso`.
  Until it does that itself, believe only its FIRST run for anything
  touching storage. Also the way to check a fix -- and to catch a
  fix that starts a DIFFERENT check failing, which is what happened when
  the menubar flake was fixed. **`flake_hunt.py ktest -n N` drives the
  IN-KERNEL suite** the same way, reporting each failure as
  `<suite>/<test>`: the 285 ktests run inside the live kernel, so a
  handful are sensitive to what else the machine is doing, and telling
  that from a regression needs a rate per ASSERTION rather than a
  verdict. One caveat unique to ktest -- a run leaves state on
  `disk.img`, so a test that leaks blocks fails the NEXT run's `fsck`
  checks; a rate that CLIMBS run over run is a dirty fixture, and
  `make clean-disk && make iso` between batches is the control. For one
  suite, booting once and repeating `vm.py exec "ktest <suite>"` is much
  faster, and is how the `heap-debug` flake's 1-in-15 rate was measured.
- **`ondemand_sweep.py`** -- runs everything in `tools/tests/sweep/` and
  every host check that is not a preflight `gate` one -- the tests
  **neither** `preflight.sh` nor `gui_regress.py` covers -- and reports
  which have rotted. Each tool's RUNNER header says how: `parallel`,
  `needs` (a precondition, so a missing IWAD is a SKIP with a reason),
  `shared_vm` (attach to a guest the sweep boots), `args`.

  **THE SHARED GUEST MUST HAVE A DESKTOP, AND THE SWEEP NOW CHECKS.**
  `stdin`, `ansi` and `console_bleed` each delete
  `/etc/services.d/toywm` to free the console, and until 2026-09-30 none
  put it back -- so every attaching tool after the first of them booted
  with no desktop and failed as `no provider named gui`, twenty lines
  that each looked like their own bug. All three copy the descriptor to
  `/var/tmp` and restore it at exit now, and the sweep boots the shared
  guest once up front and refuses to start without the service (`make
  iso` puts it back). **`--only` prefers an EXACT name**: `--only
  console` used to run `console_bleed` too. `fileop`'s header says `shared_vm`
  now; with none, every check read an empty answer.

  **The gap it fills is measured, not theoretical.** Two of those tools
  were found red by accident in one session, having failed for an
  unknown period: `fs_switch_test.py` (10 checks, because `df` and
  `stat` became `/bin` programs while it still parsed the ring-0
  output) and `init_test.py` (16 of 31, same class). Neither was a
  regression; both were rot, and nothing in the repo could notice.

  **A SKIP IS COUNTED SEPARATELY FROM A PASS.** `doom_test` exits 0
  with no IWAD and `hires_test` passes vacuously on a default-mode ISO
  -- counting either as a pass reports coverage that does not exist.
  Preconditions are checked before running, so those report as skips
  with a reason. The `hires` precondition reads the **built**
  `iso/boot/grub/grub.cfg`'s `multiboot2` line, not the repo-root
  template: the template still holds the `@KCMDLINE@` placeholder and
  documents `video=` in four comments, so a substring search over it is
  true on every checkout.

  **Serial by default (`-j1`)**, and that is not a conservative
  guess: nearly all of these take the shared VM slot, the physical
  console, or boot their own machine. This is not `gui_regress`.

  **A TOOL THAT CHANGES `disk.img` RUNS LAST** (`DIRTIES_IMAGE`).
  `console_bleed_test.py` deletes `/etc/services.d/toywm` and cannot put
  it back -- there is nothing in the guest to copy it from -- so every
  tool after it would boot a machine it did not configure, which is this
  file's own "a green line that means nothing" hazard pointed at itself.
  The sweep says at the end that `make iso` is needed rather than
  re-seeding the image on somebody's behalf.

  NEVER A GATE. Several need hardware, Docker or a fetched IWAD, and a
  check that cannot pass on a clean checkout is one people learn to
  ignore. Run it before a release, or when you want to know whether the
  on-demand half of this directory still works. `mkpart_test.py` is
  excluded because despite the name it is a WRITER that takes a
  disk-image argument, so running it bare is an argparse error rather
  than a result.

  **Five tools were missing from it for an unknown period** and were
  found the same way its own first run found two red ones -- by
  enumerating `tools/*_test.py` and subtracting what each runner names:
  `net_test.py`, `audio_test.py`, `cursor_ibeam_test.py`,
  `serial_backpressure_test.py` and `console_bleed_test.py`. Adding a
  tool to `tools/` does not add it here, and nothing checks that it
  did.
