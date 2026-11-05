# GUI tests that wait for the right thing

**Status: stages 1 and 2 BUILT** (`userland/apps/system/toywm/wm_debug_wait.c`
and `wm_debug.c`'s `gui input`; `gui_debug.py`'s `wait*()`, `input()`,
`tap()`; `tools/tests/gui/wait_test.py`, `input_test.py`); stages 3-4
PLANNED. Asked for on
2026-11-05 after a measurement of the GUI suite's flakes (below); the
alternative, a test runner inside the OS, was put after this because
the measurement did not point at the transport.

## The measurement

`tools/lib/test_history.py` held 7,719 GUI tool runs over 2026-10-24 ..
2026-11-05: 208 failures, of which **77 were FLAKY** -- the same tool
passed on the same code (commit and diff hash). Classified by what the
failing check said:

| Kind | Flakes | Seen | What it was |
|---|---|---|---|
| A. a window or app not up in time | 12 | parallel runs only | "no window titled ... within 15.0s", "the File Manager opens" |
| B. the transport or the harness | 5 | parallel runs only | `no provider named gui` (the WM not up yet), QMP refused, serial EOF |
| C. a layout read before it was reported | 12 | parallel runs only | `widgets []`, `row0 None`, `rects=None` |
| D. real-time input and animation | 14 | 9 parallel, 5 alone | Alt+Tab taps, holds, drags, Paint's spray, smooth scrolling |
| E. app state after an action | 35 | 23 parallel, 12 alone | Hex Viewer copy, Device Manager, Recent, Settings staging... |

A, B and C never failed in a run alone: they are contention, made into
failures by a test waiting on the wrong signal or on the host's clock.
D is the one the HOST cannot fix: it times a key's down and up with its
own sleeps, so under load a tap arrives as a hold. Part of E is the
same shape -- the action started work the app finishes later (a worker
thread, a spawn) -- and part is likely real bugs, which this does not
remove and should not hide.

## What other systems do

- **Android's Espresso** runs in the app's process and WAITS FOR IDLE
  before every action and assertion -- the main looper empty, and every
  registered IdlingResource (a network call, a worker) idle. It is the
  standard example of a UI test harness that is not flaky, against
  UIAutomator driving from outside.
- **Qt's QTest** retries an assertion until it holds or times out
  (`QTRY_VERIFY`), and synthesises input with explicit delays
  (`QTest::keyClick(..., delay)`).
- **Weston** tests through a protocol extension (`weston-test`): a test
  client asks the compositor to move the pointer or press a key and gets
  an event back when it has happened.
- **Playwright** auto-waits every action for the element to be attached,
  visible and stable, and finds elements by role and name.

What they share: **every step waits for a state, the app's own, never a
duration; and widgets are found by name.** Where the test code runs is
secondary. SaunaOS already finds widgets by name (`uui_describe`, the
widget map) and has one quiescence primitive (`gui sync`), so this
extends that rather than moving the harness into the guest.

## The constraint

A `gui` command crosses one kernel slot (`kernel/core/diag.c`,
`DIAG_WAIT_TICKS`, about two seconds) to the WM, which polls it once per
frame and must answer from that poll. **So a wait cannot block inside a
command**: it registers a condition and answers a TICKET, the WM
evaluates the condition every frame and on every ask (it sleeps between
frames when nothing moves; the ask wakes it), and the host asks after the
ticket -- the shape `gui sync` already has (a serial, then `gui state`).

**THE GUEST'S CLOCK IS NOT ITS PROGRESS**: under TCG it follows the
host's wall clock, so a starved guest gets no more time by being timed
by it. What helps is that a wait ENDS the moment its condition holds, so
its timeout can be generous (60 s) and cost nothing on a pass, while
the commonest real failure -- a program that died at start -- ends it at
once (`--pid`). A window is matched by TITLE, never by that pid: a
single-instance app's second copy hands over to the open window and exits,
which is a success.

## Stage 1: waits in the guest (A, B, C)

- **BUILT**: `gui wait window [--pid N] [--ms N] TITLE`, `gui wait gone
  [--ms N] TITLE`, `gui wait widget [--ms N] NAME TITLE` (in the window's
  widget map with a non-empty rect: a closed dialog sits there at 0x0) ->
  `{"ticket","state","ms"}`; `gui waited TICKET` -> `pending`, `done`,
  `timeout`, `exited` or `unknown`. A log condition was left to the host:
  the WM does not read the kernel log, and the host's log poll already
  waits on what it asserts.
- `gui sync <title>`: the round trip with THAT window's client too --
  today it asks only the focused window and the one under the pointer,
  so a window that is neither is never waited for.
- **BUILT**: `gui_debug.py`'s `wait()`, `wait_window()`, `wait_widget()`,
  `wait_gone()`, falling back to polling on a WM without `gui wait`;
  `spawn()` on `wait_window(pid=)` with a 60 s default; `sync(title=)`;
  `json()` waits up to 30 s for a `gui` provider not yet registered (B).
- **BUILT**: the A and C tools converted -- their tool-local 15 s window
  loops replaced, widget waits before widget reads, and the two polls only
  the app's log can answer (Browser's first load, System Update's dialog
  parts) given generous timeouts.

## Stage 2: input timed by the guest (D)

**What is wrong today, measured in the code**: an Alt+Tab TAP is two
`gui` commands -- `gui key 0x09 alt`, then `gui key <alt> up` -- so the
time between the press and the release is the host's: two serial round
trips, and under load more than the switcher's 100 ms, when the tap
becomes a hold and the card draws (`switcher_test.py`). Holds and drags
over QMP (`pins`, `paint`'s spray, `drag_image`) time themselves with
host sleeps the same way.

- `gui input STEP; STEP; ...` -- one command, the steps timed by the WM:
  `key CODE [mods]`, `keyup CODE`, `tap CODE [mods]`, `wait MS`,
  `move X Y`, `press [BTN]`, `release [BTN]`, `click X Y [BTN]`. Each
  step is due at the sum of the `wait`s before it and is fed into the
  existing injection queues at that time; the loop's sleep is bounded by
  the next due step, as a paced `gui drag`'s already is
  (`wm_debug_paced_due_ns()`).
- **A HELD BUTTON IS RE-DELIVERED EVERY PASS** while a `wait` runs, the
  paced drag's trap: between steps the loop reads the REAL mouse, whose
  button is up, so a hold would otherwise be a click.
- It answers a `gui wait` ticket that is `done` once the last step is
  DELIVERED, not merely queued.
- **BUILT** as above, with one finding: an IDLE loop sleeps through a
  `wait` -- nothing wakes it, so no pass reads the real mouse -- and a
  hold survives without the re-delivery; the re-delivery is what keeps it
  when something else (real motion, an animating client) wakes the loop.
  `input_test.py` moves the real pointer during its held drag so its
  check can tell the two apart (green without the re-delivery otherwise).
- **BUILT**: `gui_debug.py`'s `input(steps)` and `tap()`; `switcher`'s
  taps, `pins`' held drag and `paint`'s spray converted. `snap` and
  `drag_image` are not yet.
- **NOT a cure for the animation checks in D** (`smooth_scroll`,
  `network_tray`'s icon, `gfxdemo`, `live_wallpaper`): those sample
  frames with host screendumps. They want the WM's own frame record (a
  per-window count of presents and their content) -- a separate item.

## Stage 3: diagnosis first -- what E actually was

**Read one by one (2026-11-05), the 35 E flakes were mostly not app
state at all.**

- **7 were not flakes**: `hexview`'s six were one evening's runs while
  that test was being written (the source hash stayed while the test file
  changed), and one `theme` was a `mutate.py` positive control --
  `test_history.py` records a run's code, not that `mutate.py` had broken
  it. A classification from the history has to drop both kinds first.
- **The host's deadlines again, behind a guest PROGRAM**: `devmgr` (4,
  `sh devctl` read back empty), `fm_bigdir` (`sh mkfiles 600`), `bootmgr`
  (`bootcfg`), `archive_gui` (a size read through `sh`), and `notify` (a
  card from a spawned shell and `notify`, polled for 5 s). A `sh` reply
  that missed the console's 6 s came back EMPTY, marked the console out of
  step and said nothing, so the check failed on nothing and its log
  could not say why. **BUILT**: `send()` gives a `sh` command 60 s (it
  ends at the prompt), and PRINTS a reply cut short
  (`gui_debug: no prompt N s after ...`); `notify`'s spawn polls 30 s.
- **Known, already in `docs/bugs.md`**: `recent_gui` (3, the same three
  checks every time, cause not established) and `settings` (2).
- **Fixed by stages 1-2 or since**: `start_menu`'s Recent order (a test
  race, fixed 2026-10-31), `switcher` and `pins` (now `gui input`).
- **Not yet read**: `occlusion`, `undo_gui`, `scale`, `quick`,
  `window_geometry`, `scroll`, `hover_sweep`, `fm_back` -- one each.

So the idle-tracking protocol below is NOT built: no flake read so far
needed it, and every one explained was a host deadline. It stays the
answer if one of the unread eight turns out to be work an app finishes
after its frame.

### Planned only if needed: an app says when it is busy

- uapp counts BUSY: posts queued from worker threads (`uapp_post`), a
  `uapp_busy_begin()/end()` pair for work an app starts (a file
  operation, a child it waits on). A client answers `gui sync` only once
  it is idle, and says so.
- Mark the work the E tools wait on: the File Manager's operations, Hex
  Viewer's clipboard, Device Manager's `devctl`.
- What stays red after this is a candidate real bug, and goes to
  `docs/bugs.md` with its rate.

## Stage 4: measure again

`gui_regress.py` run as many times as the measurement above took to
see the flakes (the same 12-day classification, by `test_history.py`),
with every A-D tool converted. The success measure is the A-D count,
not "the suite is green": E's real bugs are the point of having tests.

## Not now: a runner inside the OS

Running the tests in the guest removes B and the serial round trips,
but not A, C or D -- those need the waits and timing above wherever the
test runs -- and it costs Python (there is none in the guest) and puts
the runner in the process tree it is testing. Revisit if B grows once
stages 1-3 are in.
