# `tools/tests/manual/` -- Tests run by hand

One entry per tool in `tools/tests/manual/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by nothing: each tool's `RUNNER` header carries a `manual` reason.
`tools/t NAME` runs one.

- **`libvnc_check.py`** -- `/bin/remoted` against a FOREIGN client: a
  probe built on Debian's libvncclient (what Remmina uses) in Docker,
  connected to a guest that is already up (a `vm.py` one with
  `--hostfwd tcp::15903-:5900`, or `--host` a laptop). It logs in, takes
  frames and prints `RESULT size=... updates=N tls=0|1`. `--tls` demands
  VeNCrypt; `--ca remote.crt` (fetched with `remote.py get
  /etc/remote.crt`) pins the server's certificate, which GnuTLS then
  checks against the address dialled -- the case Remmina hits. A
  different certificate as `--ca` must FAIL ("hasn't got a known
  issuer"), and did. Needs Docker and, the first time, deb.debian.org;
  in no runner (`ondemand_sweep.py`'s exclusions).
- **`kvm_soak.py`** -- the desktop under KVM, across FRESH BOOTS, failing
  on the symptoms that appear only there: a WM frame over a threshold, a
  file that exists but will not read, an incomplete cursor-theme load, a
  varying desktop entry count. Exists because **every other test here
  runs TCG**, and on 2026-08-17 that hid three real bugs at once -- a
  lost-wakeup race in the ATA driver (5s frozen desktop per disk read),
  a 54-read desktop reload (40ms TCG / 2.5s KVM), and a non-re-entrant
  filesystem that silently lost cursor shapes on ~1 boot in 3. Fresh
  boots per round because that last one is intermittent and one clean
  run says nothing. Not in `gui_regress.py` (needs `/dev/kvm`, boots its
  own VMs); run it after touching the disk driver, the filesystem, the
  VFS, the scheduler's preemption handling, or anything the WM reads
  from disk -- and whenever a user reports something this environment
  cannot reproduce. SKIPS loudly without KVM rather than passing
  quietly. **Its workload CHURNS `/usr/wm/applications` on purpose**: the
  desktop only re-reads when that directory changed and a cached read
  never reaches the drive, so without it the tool does almost no disk
  I/O -- verified by disarming the VFS preemption guard entirely and
  still getting four clean rounds. **It also switches the cursor theme
  once per page** between two real themes: the WM reloads a theme only
  when it changes (pushed config), so without that the incomplete-load
  check saw no loads and passed on nothing.
  **ITS SENSITIVITY IS MEASURED, AND IT IS LOW: with every mount lock
  made a no-op, 2 rounds of 6 went red** (System Settings page-faulting
  on what it read); the rest passed. On one vCPU two contexts meet
  inside a backend only when a holder sleeps in a disk wait, which is
  timing. So **use `-n 10` for locking work** (~98% to see a red round
  at that rate), and never read a 3-round pass as proof. It was broken
  for weeks by three harness faults at once -- a sidebar filter left
  over from the tree, a `settings: set` count that missed lines glued
  to a JSON reply, and a QMP client left open so the next round's port
  sat in TIME_WAIT -- and said nothing more useful than "did not come
  up" (fixed 2026-09-24; it now says why).
- **`damage_sweep.py`'s positive control injects its miss.** `gui damage
  shrink <n>` (a test lever in `wm_render.c`) insets every
  `wm_damage_window_rect()` by 48 px for the next `n` rendered frames;
  `--positive-control` shrinks four frames before a drag -- with
  `gui pacing off` around it and the lever cleared after, since a paced
  compositor folds an injected drag into a frame or two and the shrunk
  frames outlived it, surfacing later as a phantom Start-menu "bug" --
  and expects the
  verifier to report the border the window vacated. Shrink rather than
  drop, because a frame whose damage is all dropped has none, and a frame
  with no damage is a full repaint -- correct by construction. Until
  2026-09-18 the control only EXPECTED a violation, which passed for as
  long as the WM had a real one -- two, as it turned out (a focus change's
  shadow and the Start button's lit state) -- and the day both were fixed
  it reported the harness as "not checking anything". **It fires since 2026-10-07**: at 32 px the inset was net of the
  28 px shadow margin and took only the shadow's nearly transparent
  outermost 4 px, so nothing visible was left stale; 48 bites 20 px into
  the moved window's old body.
- **`damage_sweep.py`** -- drives a fixed sequence of window
  interactions (open, raise, drag, minimize/restore, resize by the grip,
  overlays, close) with `gui damage verify on`, and reports every
  distinct damage violation with the interaction that produced it.
  `--random N --seed S` walks the same interactions in orders nobody
  thought to list.

  **IT ALSO DRIVES THE TRAY'S VOLUME PANEL, AND THAT PART NEEDS A GUEST
  WITH SOUND HARDWARE.** That panel is the one overlay whose geometry
  changes with NO input: its rows come from the registered sound devices
  and from the per-application roster, so it grows and shrinks under an
  open panel and has to cover what the larger one drew. On a guest with
  no sound card no application row ever appears and the steps test
  nothing -- so it prints `NOT COVERED:` for them instead of passing
  quietly, and says the flags to fix it (`vm.py --audio-wav <path>
  --audio both start`). Added 2026-09-22 after the maintainer
  photographed a sliver left behind by exactly this on bare metal;
  nothing in the sweep had touched the tray before. **That sliver does NOT reproduce
  under QEMU at all** -- it needed a tray icon coming and going, which
  the laptop has and a guest does not -- and it was found and FIXED on
  the hardware instead (`docs/decisions.md`, "An overlay's damage is
  checked where it MOVES"). The coverage stays: a panel resizing under
  an open popup is worth exercising whether or not it is broken today.

  **THE VERIFIER IS MORE SENSITIVE THAN A PERSON WATCHING, AND ON BARE
  METAL IT CATCHES THIS ONE.** Driving it by hand on the laptop --
  `guictl damage verify on`, open the tray volume panel, leave it --
  reports 3 violations in 20 seconds of IDLE, where the maintainer
  watching the same screen saw nothing that time. So a quiet screen is
  not evidence and the verifier is: reach for `guictl damage verify on`
  plus `dmesg` before concluding a damage fault is gone. It is not free
  -- every frame renders twice while it is on -- so turn it off again.

  **IT REPORTS THE FIRST DIFFERING PIXEL'S COLOURS, and that line says
  what KIND of fault it is.** `wm:   first px 0x2c7385 -> 0x2d7487`: a
  wallpaper colour turning into chrome means a pass did not DRAW
  something; a shift of a point or two per channel means the same
  content was BLENDED a different number of times, which is a
  completely different hunt. It also prints the cursor position and the
  owning window on the two lines ABOVE the report, so grepping for
  `DAMAGE BUG` alone throws most of the evidence away -- grep
  `"first px|cursor now|diff is in|DAMAGE BUG"`.

  **TURN SHADOWS OFF BEFORE READING THOSE COLOURS** (`config set
  shadows off`). A shadow reaches further than what casts it, so its
  faint outer pixel is the FIRST in scan order and the reported colours
  are a one-or-two-point blend difference -- which reads as "composited
  twice" when the actual fault is a whole panel missing. That misread
  cost a round on the tray-panel bug. With shadows off the same fault
  reported a border colour against wallpaper, which is a different
  investigation entirely. Turn them back on afterwards.

  **AND IT LOGS TWO THINGS THAT ONLY APPEAR WHILE IT IS ON**: an
  overlay opening or closing (`wm: overlay <name> opened`), because an
  overlay that transitions BETWEEN the two render passes makes them
  disagree about a whole panel for an innocent reason; and a tray
  popup's anchor lookup falling back (`tray_item_rect(N) FAILED`),
  because that silently moves the panel somewhere else. Both are gated
  on verification so a tooltip's ordinary hover traffic does not drown
  the log.

  **Read the two counts in its summary.** A *violation* is a real missed
  damage declaration and fails the run; a *report the WM declared void*
  is one the compositor itself could not conclude anything from, printed
  so it stays visible (`-v`) but not counted. Before that distinction
  existed the tool reported 22 violations on a desktop with no damage bug
  in it at all -- every one of them the verifier comparing two renders of
  a CLIENT window whose content had moved underneath it, which a
  compositor with no buffer-release protocol cannot prevent. See
  `docs/decisions.md`.

  **Its positive control is an injected defect, not a flag** --
  `--positive-control` only inverts the exit code. Remove a
  `wm_damage_rect()` call in something the WM draws itself (the taskbar
  strip in `wm_client.c`'s `on_window_created()` is a good one) and
  rebuild; three checks should redden, all at the taskbar's y. Injecting
  it in CLIENT content proves nothing, since that is masked out by
  design.
- **`damage_hunt.py`** -- `damage_sweep.py` over MANY seeds, a fresh
  disk copy and its own `vm.py --instance` slot each, as one pass/fail
  table; non-zero if any seed violated the invariant. One seed is one
  ORDERING, and this bug family lives in orderings, so "does any of a
  batch fail" is the question worth asking -- and the four-line shell
  loop that answers it had been written from scratch twice, getting the
  slot/`--sock`/`--qmp-port` triple wrong each time. **A seed reports
  `pass`, `fail` or `error`, and the third one is load-bearing** -- a
  sweep that crashed (a guest too slow to accept a QMP connection at
  high `-j`, a serial socket dropping mid-run) measured NOTHING, and
  this tool used to score exactly that as a PASS. `-j` still defaults
  to 1, but for a plainer reason than before: each slot boots its own
  guest, and four booting at once is enough to lose two of them. Not in
  `gui_regress.py`, same reason `damage_sweep.py` isn't. The earlier
  "parallel VMs report a violation `-j 1` doesn't" claim is retired --
  it failed to reproduce six times, and the mechanism that made `-j 2`
  special was this tool putting 9 GB of tmpfs behind each slot (see the
  next bullet).
- **A copy of `disk.img` must stay SPARSE, and `shutil.copyfile` does
  not.** The image is ~4 MB of data in a 9 GB sparse file, so a
  hole-filling copy costs 9 GB -- of RAM, when the destination is
  `/tmp` on a tmpfs. That silently turned `damage_hunt.py -j N` into
  "N x 9 GB of host memory pressure" and killed `-j 4` outright with
  ENOSPC. Use `cp --reflink=auto --sparse=always` (what the tool does
  now); `cp --reflink=auto` is already what this file recommends
  elsewhere for the same file.
