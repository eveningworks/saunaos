# GUI tests that wait for the right thing

**Status: PLANNED** -- four stages below, none built. Asked for on
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
evaluates the condition every frame against its own clock, and the host
asks after the ticket -- the shape `gui sync` already has (a serial, then
`gui state`). The deadline is the GUEST's, so a slow guest is given the
time its own clock says it had.

## Stage 1: waits in the guest (A, B, C)

- `gui wait window <title> [ms]`, `gui wait gone <title> [ms]`,
  `gui wait widget <title> <name> [ms]` (named in the window's widget
  map, with a non-empty rect), `gui wait log <text> [ms]` -> a ticket;
  `gui waited <ticket>` -> `pending`, `done <ms>` or `timeout`.
- `gui sync <title>`: the round trip with THAT window's client too --
  today it asks only the focused window and the one under the pointer,
  so a window that is neither is never waited for.
- `gui_debug.py`: `wait_window()`, `wait_widget()`, `wait_gone()`,
  `wait_log()` on top of them; `widgets()` waits for the window's client
  to have drawn since the last input. The boot handshake waits for the
  `gui` provider before the first command (B's `no provider named gui`).
- Convert the A and C tools to them.

## Stage 2: input timed by the guest (D)

- A timed injection queue in the WM, drained each frame by the guest's
  monotonic clock: `gui tap <key> [mods] [ms]`, `gui hold <key> <ms>`,
  `gui press <x> <y> [button] [ms]`; `gui drag` already has a paced form
  (`HZ`), which becomes the default.
- Each returns a ticket that completes when the last event is DELIVERED.
- Convert the D tools (switcher, pins, paint's spray, snap...).

## Stage 3: an app says when it is busy (E, the timing half)

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
