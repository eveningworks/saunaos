# GUI unit tests: the toolkit and its apps tested in-process, KTEST-style

A staged plan for a FAST test layer under `gui_regress.py`: widgets and
then whole apps driven in memory, with no guest, no compositor and no QMP
-- what KTESTs are to the kernel. **PLANNED (the maintainer's pick,
2026-11-17) to follow SDL stage 4, ahead of the toolkit's C++ headers.** Nothing here is built yet.

## The measurement that decided it

Measured 2026-11-17 on the dev host (`test_history.py`, the last ten
passing runs of each tool):

- `gui_regress.py` runs **151 tools**; a full run is **~16.5 min** at 8
  guests (990-1020 s over the last five).
- The tools' medians sum to **8,068 s** of guest time. The median tool
  takes **34 s**; twelve take over two minutes (`paint`, `w16_gui`,
  `sdl_window`, `screensaver`, `uterm`, `scroll`, `settings`, ...).
- **Booting is NOT where it goes.** A boot to a settled desktop is ~12 s
  under TCG (11.0 s to the prompt, 0.6 s to the desktop), and restoring
  a `savevm` snapshot instead is ~8 s -- saving ~3 s a tool, about a
  minute of wall time. Snapshots were measured and set aside.
- The runner already starts the longest tools first
  (`gui_regress.py`'s `pick_order`) at one guest per physical core.

The time is INSIDE the tools: serial round trips to the debug console,
waits for a frame, screenshots and their pixel reads, all under
emulation. Much of what they assert is a widget's own logic -- a hit
test, a layout, focus, an edit, a scroll position, a colour -- which
needs none of that.

## What real systems do

- **Qt**: QTest drives widgets with simulated clicks and keys in-process,
  on the `offscreen` platform plugin (`QT_QPA_PLATFORM=offscreen`) --
  the whole app, no display.
- **GTK**: tests run against a headless backend (Broadway, or a virtual
  Wayland compositor) in CI.
- **Flutter**: widget tests pump frames with no device; integration
  tests are a separate, slower layer.
- **Android**: Robolectric runs UI code on the JVM, no emulator; Espresso
  is the on-device layer.
- **Chromium**: `views_unittests` beside `interactive_ui_tests`, which
  need a real display and run serially.

The shape is the same everywhere: a broad, fast in-process layer, and a
small end-to-end layer kept for what only the real stack can show.
SaunaOS follows it. `gui_regress.py` stays, for the compositor, the
protocol, real input (QMP keys, the pointer lock), several windows and
pixels on the real screen.

## What exists to build on

- **20 `/tests` programs already drive widgets in-process**
  (`droprows_test`, `focusring_test`, `sbar_test`, `table_tree_test`,
  ...), reporting through `base/utest.h` -- but each in the guest, and
  each building its own scaffolding.
- **Host checks compile toolkit sources with the host gcc** and sweep
  them in seconds: `ugfx_text_hostcheck.py` (the real `ugfx_text.c` and
  `uui_textbox.c`), `ugfx_fill_`, `ugfx_pix_`, `utext_`, `caret_`.

## Stage 1: a widget harness on the host

A test builds a widget tree -- the same `struct uui_item` arrays and
layouts an app declares -- into an IN-MEMORY `ugfx_surface`, and drives
it by widget NAME through the real router (`ui/uui_route.h`): press,
motion, release, key, wheel, a frame. It asserts state (`selected`,
`checked`, a text, a scroll offset) and pixel values at a named widget's
rect. Compiled with the host gcc against the real toolkit sources and a
deterministic font, so the whole set runs in seconds with no QEMU.

- The harness: `uitest_window(items, layout)`, `uitest_click("name")`,
  `uitest_key(...)`, `uitest_frame()`, `uitest_pixel("name", dx, dy)`,
  reporting in `utest.h`'s shape.
- A `hostchecks/` runner, `gate`, so `preflight.sh` runs it.
- Port first: the checks `gui_regress` tools make that are pure widget
  logic, starting with the slowest tools' (`scroll_test`'s scrolling
  rules, `settings_test`'s rows). A ported check leaves the GUI tool.

## Stage 2: whole apps headless -- an offscreen `uapp`

Qt's offscreen shape: a `uapp` backend that draws into memory and takes
its events from the test instead of `wmchan`, so an app's REAL code --
its `main`, its `uapp_desc`, its handlers -- runs in-process. A stand-in
for the compositor's half: window creation, a present, a resize, the
widget map, popups and dialogs. In the guest first (an app's files and
settings are there), as a `/tests` runner; on the host where the app
allows.

## Stage 3: shrink the end-to-end layer

Measure again by the same method. Each `gui_regress` tool keeps only
what needs the real stack; the rest has moved down. The target is a
suite whose per-change subset fits in minutes, with the full run kept
for releases and the toolkit/compositor changes `gui_regress.py`'s scope
rule already names.

## The case against

- **A stand-in can lie**: the offscreen `uapp` and the host build are
  not the machine. Every behaviour moved down keeps one end-to-end check
  above it.
- **Two builds of the toolkit** (the guest's and the host's) can drift;
  the host checks already pay this and have caught real bugs because of
  it -- not a reason to stop, a reason to keep the host build in the gate.
