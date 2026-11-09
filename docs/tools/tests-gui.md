# `tools/tests/gui/` -- GUI tests

One entry per tool in `tools/tests/gui/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by `tools/runners/gui_regress.py` (`tools/t gui`), each against a
fresh image and its own guest. A new tool here needs a `RUNNER =
{"covers": ...}` header, plus `"vm": [...]` when its guest must differ
from the default (a sound card, a USB mouse, `--reboot`).

- **`logview_test.py`** -- the Log Viewer on a FIXTURE log it is opened
  at (`logview <file>`): an error, a warning, an ordinary line, the
  error again and a program line, planted with `vm.py put` -- not
  `DebugConsole.write_lines()`, which types through `tosh -c echo`
  where a level's `<3>` is an input redirection and the file never
  appears. Checks the rows are DRAWN in severity colours (the error's
  Level cell has the theme's red, the warning's its amber, an ordinary
  row neither, from a settled screenshot), Errors shows exactly the two,
  the error reports `seen 2`, Only this message and Show the lines
  around it, and a Mute kept in `/etc/logview.conf` (read back with
  `cat`) and taken out again. In `gui_regress.py`. Controls
  (`mutate.py`): the severity drawn in text colour reddens the colour
  checks; a Mute that never writes the file reddens the `/etc` check.
  `/tests/ulogset_test` covers the line store underneath (the merge,
  repeats, the filter, a saved file read back).
- **`corners_test.py`** -- `desktop.corners` and `desktop.scrollbars`
  by pixel: About's Close button's corner pixel is its own edge under
  Square and the window behind it under Small, changed in the OPEN app;
  First-boot setup's layout list at rest shows the groove under Always
  and plain field under Automatically, and the bar's far column is the
  groove (an inset left a one-pixel gap there). In `gui_regress.py` as
  `corners`.
- **`fbs_test.py`** -- First-boot setup, on a guest whose image is
  pre-marked: `firstboot --boot` opens nothing, `fbs again` then makes it
  open; the window is fullscreen and a click across the taskbar's band
  opens no tray flyout (a closed overlay used to take that click under a
  fullscreen app -- the control restores it and sees the On-Screen
  Keyboard flyout open); a layout picked with the arrow key is saved
  (`config get`); an invalid computer name keeps Next on its step and a
  valid one lands in `system.hostname`; the last Next ends it
  `finished`, and Skip then Return ends a `fbs now` run `skipped`. The
  display scale STARTS at the recommendation: 1280 wide leaves 100%, and
  after a live switch to 1920x1080 a reopened wizard applies 125% (QEMU's
  EDID says 32 cm), which Skip keeps -- red with the preselection taken
  out. Geometry from `gui widgets`. In `gui_regress.py` as `fbs`.
- **`bootmgr_test.py`** -- the Boot Manager and System Settings > Boot
  menu write the real `grub.cfg`. The word list is DRAWN (a word the
  entry has has an accent checkbox, one it lacks a white one, by pixel);
  choosing an entry shows it; ticking a word and Save puts it in the file
  as `bootcfg` -- the independent reader -- sees it, with a `.bak`; a
  stray `}` typed into the Text view is a problem, Save opens the refusal
  and the file is unchanged; the Settings page's default dropdown moves
  `bootcfg`'s `*` and back. In `gui_regress.py`. Positive control: with
  the app's `ubootcfg_save()` call removed, "Save writes it" goes red.
- **`properties_test.py`** -- Properties on a picture and a folder. The
  hero is checked BY PIXEL (the stage is not the page grey, the picture
  area is not flat) since a window that logs its rows can still paint a
  blank hero; a section header opens its section; clearing Others > Read
  changes the mode as `stat` sees it; Checksum opened at the window's
  foot shows its button and compare field at once, a sum typed BEFORE
  computing compares as a match, the field is still shown after Compute,
  and Compute SHA-256 equals `sum -a sha256`; another app under Opens with lands in `/etc/mimeapps.conf`;
  a typed name renames the file (`ls`) and retitles the window; a
  folder's Contains settles on the true recursive count. In
  `gui_regress.py`. Positive control: with `ufileinfo_chmod()`'s
  `sys_chmod` call skipped, the mode check goes red.
- **`hover_sweep_test.py`** -- opens every Start-menu app and moves the
  pointer over a grid of points in every widget it names; no window or
  process may appear, and no app may log `uapp: BUG` (the toolkit refusing
  a duplicate widget id or a button with no `on_action`). HOVER ONLY: a
  widget report has no type, and a click on a toolbar is a command, so
  "a click opened something" is not a sweep's call. It closes each app by
  KILLING it, so System Update dies mid-check -- the case that froze the
  desktop before `group_defer_death()`. In `gui_regress.py`.
- **`crt_test.py`** -- the Terminal's screen effect (`userland/libs/ui/ucrt.c`):
  off by default (a blank band of the grid is the page's colour alone),
  Ctrl+Shift+E turns it on and the same band repeats a darker row at the
  scanline pitch the app derives from its font, off again is plain;
  Options > Screen effect > Curved > OK curves the window, puts the
  grid's corner on the bezel and writes `/etc/terminal.conf` (read back
  with `cat`); and a double-click where the banner's `tosh` SHOWS under
  the curve selects those 4 bytes -- the shown position from this
  tool's OWN model of the warp, so the app is not checking itself. It
  prints the effect's first-frame cost. Positive controls, both by
  `mutate.py`: dropping `point_at()`'s `ucrt_source_point()` call
  reddens the double-click (`selbytes 0`), and dropping the scanline
  gain reddens the pitch check. In `gui_regress.py`.
- **`crashview_test.py`** -- the crash report viewer (Crash Reports'
  viewer face over `base/ucrash.c`): a Notepad killed with SIGSEGV
  leaves a report; `open` on it opens the viewer (`Handles=.crash`);
  the viewer's logged frames are checked against THIS tool's own
  reading -- the report's raw header and `readelf` over the same
  build's binaries -- frame 0 is the header's rip, every name and
  offset is the host's symbol table's, every later frame follows a
  call instruction in the host's copy of the file, and the chain holds
  `uapp_run` then `main`; the document is drawn, `crashlog` prints the
  same frames, and Enter in the list opens the viewer. Positive
  control: `mutate.py` making `after_call()` accept anything reddens
  the call check (three stale addresses got in). In `gui_regress.py`.
- **`crashtest_test.py`** -- the fault paths and what the user is told:
  the app enumerates the kernel's fault kinds, kernel faults
  are refused while disarmed, a ring-3 crash kills the app WITHOUT taking
  the desktop with it, the crash notice names it, Details opens the
  dialog and All reports the list -- then "crash the desktop" kills toywm
  and the test checks the kernel HELD the last frame (its log line, and
  no "console restored") and the restarted desktop says so. It ends on a
  restarted desktop. In `gui_regress.py`, which is only safe because the kernel
  half is disarmed unless `faultinject` is on the command line.
- **`player_test.py`** -- the Audio Player, on a machine with **no
  sound device**, which is the default boot and therefore the
  configuration nearly everything sees. That is the point: an audio app
  is most likely to be broken exactly where `usnd_init()` fails, and
  `audio_test.py` -- which boots its own AC97 -- can never see it. It
  asks whether the window opens as a ring-3 client and reports the
  missing device rather than dying, whether the sounds directory is
  listed by PROBING (the count checked against what the host seeded, not
  against the guest's own opinion), whether the transport and both
  scales have ink where the app says they are, and whether `uui_scale`
  takes a real pointer -- clicking the left and right ends of the volume
  track must move the reported volume, which is the new widget's only
  test with a pointer in it. Since the redesign it also asks that the
  stage's controls have ink against the STAGE beside them (the window
  corner is light chrome, and would count an empty dark stage as ink),
  that the MP3's title and artist come from its ID3 tag, that Next does
  nothing at the last track without repeat and wraps with it, that
  Ctrl+L hides the playlist and the stage widens, and that F11 makes the
  window the screen. `poll_logs()` ACCUMULATES: a check reads only what
  was logged after its own action, or the earlier opens pass it. It can
  see nothing about sound (auto-advance needs a card: checked by hand).
  In `gui_regress.py`.
- **`video_test.py`** -- the Video Player on a machine with no sound
  device, in `gui_regress.py`: the window lists `/usr/share/videos` by
  probing, plays the file it was handed, and the picture is DRAWN AND
  MOVING (two unsettled photographs a second apart differ inside the
  picture's rect -- `QMPSession.screenshot(stable=False)`, since a
  playing video never settles) and HOLDS STILL when paused (the same two
  photographs identical); Right seeks ten seconds, a playlist row plays
  the AVI, Ctrl+S saves a frame, hovering the seek bar brings a preview,
  F11/Esc. **The paused check found a real bug under TCG**: a decoder
  running behind the clock kept "catching up" behind a pause, so the
  picture changed while paused -- invisible on KVM, where the decoder is
  never behind. Positive control: a widget that never draws the frame
  fails exactly the two drawn-and-moving checks.
- **`theme_test.py`** -- themes (`userland/libs/ui/utheme_file.h`) and the
  Theme Editor, in `gui_regress.py`. A live `config set desktop.theme`
  must turn a focused title bar from Light's blue to Dark's and High
  contrast's yellow by PIXEL, with a desktop pixel beside it unchanged
  (the control), and darken an open app's page; then the editor: a colour
  typed into its picker changes the preview's title text, Save as writes
  `/etc/themes/testred.theme`, and Apply makes it the desktop's -- the
  setting, the bundled settings (Paper's round corners and light taskbar)
  and the REAL title bar drawn red. Its toolbar buttons are found by
  their Create-green icons: the toolbar's items are not in the layout
  log. Types lower case only: the harness's Shift does not reach the
  guest's text fields. Puts every setting back and deletes its theme.
- **`charmap_test.py`** -- the Character Map, in `gui_regress.py`: it
  opens on the interface's font with the names loaded and the grid DRAWN
  (a cell with "!" has ink, one past the last has none); "rightwards
  arrow" selects U+2192 -- an exact name wins over U+0362, which sorts
  first -- and the selected cell and detail tile draw it; Copy, then
  PASTE INTO THE SEARCH BOX finds the same character alone, for the
  arrow (UTF-8) and for e-acute (one Latin-1 byte); a double-click adds
  to the text line and Backspace removes it; "G" on the focused Block
  list shows Greek; the Fonts page's card switch redraws the size
  ladder; "Use for terminals" writes `system.font.mono`, put back
  after. **It caught the grid drawing nothing after any search** (a
  refill cleared the cell callback): the drawn-after-search check is
  red on that build.
- **`cursor_theme_test.py`** -- cursor themes end to end: the
  theme loads completely, switching it changes the drawn pointer, the
  size setting scales it by the right MAGNITUDE, returning to normal is
  pixel-exact, and a theme that does not exist still leaves a working
  pointer. Every IMAGE theme (found from `data/cursors/`) loads all
  shapes, paints its OWN opaque colours on the software sprite -- counted
  as exact screen-pixel matches against its arrow QOI, with the mask
  default as the control that must show none -- and loads its 2x
  renderings at `large` (the loader's own report; area cannot tell a
  2x file from a doubled 1x). At `display.scale` 125% every image is its
  1.25x rendering and the ink grows ~1.56x -- 1.0 would be the 1x set,
  4.0 the @2x. It needs a guest WITHOUT a cursor plane,
  since the plane is invisible to `screendump`, and checks that it has
  one. Read its docstring before editing: the built-in fallback
  means "a cursor is on screen" proves nothing, so every check asserts a
  load count or a pixel difference. It also sets both settings
  explicitly at the start -- they persist to the disk image, so
  inheriting them makes every measurement relative to a silently wrong
  baseline. In `gui_regress.py`.
- **`cursor_shapes_test.py`** -- the three gesture shapes, each at its
  first caller: `hand` over a link in Help, `move` while a window is held
  by its title bar, `not-allowed` while a desktop icon is dragged over a
  title bar (which takes no drop). Asserts the RESOLVED shape (`gui state
  --json` cursor.shape), each beside a control that must not show it --
  the prose by the link, the title bar after the release, the desktop
  and a client's content during the same drag. Presses only where no
  window edge is within a margin: a press on a resize zone resizes the
  window instead. In `gui_regress.py`.
- **`leave_test.py`** -- the Leave page (`userland/apps/system/toywm/leave_page.c`):
  Start's Shutdown and Restart rows open it with their own action
  focused, the desktop dimmed under it (a pixel against the frame
  before), Left/Right and Esc; then Notepad with typed text and
  Calculator open, Shut down asks both to close -- **the control is the
  pair**: Calculator must be reported closed and its window gone,
  Notepad still open -- Cancel returns to Notepad, "Shut down anyway"
  acts, and "Restart into" lists the boot entries and restarts into the
  one picked; last, Notepad with its Options dialog open is still asked
  and the page moves on rather than hanging in its closing phase. **Every
  final action is a dry run** (`gui leave dry on` makes it a `leave:
  would ...` log line), so nothing powers off. In `gui_regress.py`.
- **`dialog_test.py`** -- verifies the confirm dialog's buttons by
  PIXEL VALUE: hover moves the hovered button and leaves its neighbour
  alone, a press dragged off doesn't commit, No closes it. Opened with
  the desktop's Delete on a file it makes (Start's power rows open the
  Leave page now). Three traps
  it encodes: hover needs the REAL cursor parked (use
  `DebugConsole.warp_cursor()` -- `gui move` holds for one WM iteration
  only, and `QMPSession.goto()` is open-loop and undershoots a large
  jump); don't sample the pixel under the cursor sprite; and take the
  button rects from `gui dialog --json`, not by scanning a row for
  THEME_BUTTON_BG, which only ever worked for a Yes/No dialog and
  cannot measure the wider "Force Quit"/"Wait" one.
- **`uidemo_test.py`** -- drives UI Demo's widgets and asserts on its
  log (27 checks: click selection, cancel paths, keyboard navigation,
  Tab/Shift-Tab focus cycling, Space activating a focused button,
  wheel-scrolls-without-selecting, the dropdown popup's open/commit/
  dismiss/Esc, and keyboard focus). Exits non-zero on a failed check.
  Run it after touching anything in `apps/ui/`. Geometry comes from the
  app's own `uidemo: layout ...` lines rather than from re-deriving row
  offsets in Python -- the Python copy drifts silently the moment a row
  is added to the app, which is exactly what happened when the dropdown
  and listbox rows landed mid-file.
- **`calculator_client_test.py`** -- drives the RING-3 Calculator
  (`userland/apps/calculator.c`) and asserts on it (8 checks). Worth reading
  for two techniques: it uses **no OCR** -- every check is a round trip
  (a state change must alter the display's pixels, and returning to the
  same logical state must restore them EXACTLY), which proves rendering
  and arithmetic together and also catches a right number drawn in the
  wrong place; and its last check presses a button, drags OFF it and
  releases, which must NOT commit. That one matters because a client
  acting on button-down passes every other check and fails only that.
  Geometry is derived from the window's reported content size, not
  hardcoded, so it survives a font-size change.
- **`uterm_test.py`** -- drives the RING-3 Terminal, which is a real
  TERMINAL EMULATOR now: a pty with `/bin/tosh` on the far end.

  **Its `u` block is UTF-8 end to end** (`--only utf8` runs just it;
  docs/utf8-design.md, step 2): a folder `Kesä` made over the debug
  console is listed by `ls` in five cells, not `KesÃ¤/`'s six (ink per
  CELL, the sixth required empty); a double-click selects the word whole
  (`selbytes` 5); `cd Kesä` with the ä TYPED on the Finnish layout through
  QMP -- the real keyboard path, never `gui key`, which injects a
  character no layout translated -- lands there, read back as `pwd`'s
  bytes by `sum -a sha256` (the serial console mangles non-ASCII output);
  and Polish AltGr+a, Romanian AltGr+s and Estonian's dead caron then s
  reach `cat` as C4 85, C8 99 and C5 A1. All seven are red on the step-1
  build (fc4f6bea); the typing ones were what found a pty master's bytes
  being encoded a second time as keys.

  **Its `b` block is the one-bar chrome** -- the Session panel by F9 and
  by its button, find (with a query that cannot match as the control,
  and the cursor field proving the typing never reached the shell), the
  ☰ and ▾ menus, a new tab starting in the last tab's folder, and the
  Options window. It reads the bar's buttons from `tbbtn` lines the app
  logs itself (the toolbar describes buttons only when it overflows),
  and it does NOT call `prime_layout()`: with the menu bar hidden, that
  helper's two F10s show the bar and then OPEN a menu, which swallows
  every key after it.

  **Its TAB checks open their own window**, because `run()` ends by
  closing the first one with Alt+F4 -- three of them passed against
  nothing on the first run, reading STALE log lines from the window that
  had been there. The load-bearing one is the SWITCH, not the count: a
  Terminal that drew a strip and pointed both tabs at ONE session passes
  every count and every pixel check, so a marker is typed into the
  second tab and the first is required not to have it. Confirmed by
  control. Its key
  check is worth copying elsewhere: it distinguishes a BUILTIN (`echo
  hi`, handled inside the shell with no spawn) from an EXTERNAL program
  (`lscpu`, dozens of lines that can only appear if it was spawned and
  its stdout piped back) by INK VOLUME. A terminal that echoed commands
  but never captured output passes every other check and fails that one.

  **The check the whole TTY layer exists for is here**: a spinning job is
  started in the window, `Ctrl-C` is sent as the byte `0x03`, and the job
  must die while the SHELL SURVIVES -- the second half is load-bearing,
  since a Ctrl-C that killed the shell too would satisfy the first. Its
  positive control is the best evidence in the repo that this is ONE
  implementation and not two that agree: disabling `signal_char()` in
  `kernel/tty/ldisc.c` reddens this check AND `ctrlc_test.py`'s
  physical-keyboard ones, from the same three lines.

  **`Ctrl-Z` and `fg` are checked here too, and for the sibling reason.**
  0x1A on the same pty, recognised by the same discipline function, so
  what this proves is not the suspension -- `jobs_test.py` does that
  against the physical console -- but that a WINDOW is a terminal in the
  same sense: same foreground group, same job table, same shell binary.
  A difference between the two would mean the tty layer had failed at its
  one job.

  **And the check that says a FULL-SCREEN program works here**: it runs
  `/bin/edit` in the window, types, saves with F2 and exits with F3, then
  reads the file back through a completely different path. Two assertions
  beside it are what make that mean something, because a file gets
  written whatever the screen did with the escape sequences: the CARET
  must be near cell 5 (an editor whose `ESC[1;6H` was printed rather than
  obeyed would have a caret hundreds of cells along), and the status bar
  must be a long unbroken RUN of the reverse-video colour. **Counting
  bar-coloured PIXELS does not work and was tried** -- glyphs are drawn
  in the same grey, so a frame with no bar at all scored 4304. A run
  discriminates: a glyph is a few pixels wide, a bar is hundreds.

  Two premises here went stale in the GOOD direction when the Terminal
  became an emulator, and both are recorded in the file. A bare `cat` now
  WAITS for input instead of returning at once -- it has a real terminal
  to inherit, where before it got a closed pipe -- so the check types
  Ctrl-D to end it, as a person would. And there is no prompt WIDGET to
  report a position for any more, so the layout log reports the
  emulator's CURSOR, which is what would be wrong if `\r` or overwrite
  were mishandled.
- **`notepad_client_test.py`** -- drives the RING-3 Notepad. `--only
  utf8` runs its UTF-8 block: the `/tests/utf8` corpus by pixels, editing
  by the saved bytes, and (step 2) Polish AltGr+a, Romanian AltGr+s and
  Estonian's dead caron then s typed through QMP on their layouts and
  saved as C4 85, C8 99 and C5 A1. The rest asserts
  tabs (New opens one and does not ask, Ctrl-W closes a clean one back to
  the tab it came from and asks for a modified one), the edit history
  (undoing everything typed makes the title CLEAN again -- dirty is a
  position in the history -- and redo dirties it), find's match count,
  and a full round trip: type, save, verify the bytes on disk via `cat` (a
  completely independent path -- the editor claiming success proves
  nothing), clear, reopen, and require the rendered text to match pixel
  for pixel. Two traps it encodes: `ls`'s output on this console is
  interleaved with kernel log lines, so parsing it needs a strict
  entry-shaped regex rather than `split()[-1]`; and a reference
  screenshot must park the caret first, since `load_file()` resets the
  cursor to 0 and a caret bar is a real pixel difference. Last (or
  alone, `--only second-close`): a window close while Notepad is asking.
  Under a TAB's prompt (its one exception) it is held and the session
  written -- two dirty tabs, Ctrl-W on the second, Alt+F4, Don't Save:
  that tab goes and the close goes on to ask about the FILE's tab; under
  a tab's Save As chooser it switches no tab and asks nothing new; with
  Options open and the window minimized, a single close is ignored and
  brings it forward -- unminimized, Options in front; `gui endtask` (End
  Task) asks Notepad's two windows once each, its dialog first, raises
  nothing, and Notepad exits; a held close given up with Cancel leaves
  File > Exit to write the session with both tabs; and a minimized dirty
  Notepad closed from the taskbar comes forward with its prompt (the
  client's WIN_REQ_ACTIVATE_OWN). Tab 2 is saved by its
  FULL PATH first, so the session checks can see a tab that was closed,
  and the cleanup removes exactly that file.
- **`clipboard_test.py`** -- drives the system TEXT clipboard across two
  apps, which is the claim that makes it a system clipboard rather than
  a feature of one: text copied in Notepad pastes into the GUI Terminal,
  which shares no code with it.

  **Four checks, in an order where each leaves what the next needs.** A
  paste with NOTHING on the clipboard runs first, while that is still
  true, and must change the document not at all -- the check that a
  `kind` field earns, since without one a file path would paste as a
  line of text. Then a round trip through the DISK rather than through
  pixels: copy, paste, save, and read the file back with `sh cat`, an
  independent path that shares nothing with the editor that wrote it,
  requiring the phrase TWICE. Then the cross-app paste, asserted as a
  BAND -- the change must be contiguous and near the top, where a shell
  prompt is, so a paste that scattered ink down the grid fails while
  "something changed" would pass. Then an oversized copy: Select All in
  `pci.ids` (1.6 MB) asks for far more than `WIN_CLIP_BYTES`, and the
  clipboard must still hold what the round trip put there -- a
  truncating clipboard would replace it with the first 64 KB of
  pci.ids, which is the failure the refusal exists to prevent.

  **A trap it encodes: counting dark pixels is backwards over the
  Terminal**, whose grid is white on black -- there every pixel is
  "ink" and text REDUCES the count, which reported a paste that had
  plainly landed as nothing having happened. Anything over a dark app
  compares two shots instead, which does not care which way round the
  colours are.

  Positive control: making `uclip_set_text()` return 0 without sending
  reddens four of the five checks and leaves the empty-clipboard one
  green, which is the right split.
- **`engineless_test.py`** -- Help and System Update with NO Web page
  engine (`docs/software-design.md`, stage 3): `/bin/webcontent` moved
  away (and back in a `finally` -- removing the component would need a
  server to add it back), each app must SAY its pages are uui_markdown's
  (`help: pages drawn by ...`, `sysupdate: notes drawn by ...`, logged
  where each decides at start), Help must report the overview's links as
  uui_markdown does and start no `webcontent`, and both pages must be
  DRAWN (ink in the widget's rect). The control is in the same run: with
  the engine back, a new Help says the opposite. Red with
  `uui_webview_available()` mutated to 1 (3 of 12 fail).
- **`editmenu_test.py`** -- the clipboard keys and the shared
  right-click edit menu (`ui/uui_editmenu.h`) in text fields that
  declare nothing about either. It drives UI Demo's field and reads it
  back from the app's own `key N text=` and `edit text=` lines, not
  pixels. It checks the keys, including the CUA trio, then each menu row
  (Select All, Delete, Paste, Undo). It also checks the read-only
  scrollback's three-row menu, whose copy pastes into the one-line field
  as the FIRST LINE only, and the Menu key in Help's search. Menu rows
  are found by INDEX in `uui_editmenu.c`'s table, separators included.
  It assumes a fresh VM, because a second UI Demo confuses
  `window_settled()`. Positive controls: breaking the core's Ctrl-V
  turns the four paste checks red, breaking the menu's Paste turns Paste
  and Undo red, breaking the Menu key turns its check red, and dropping
  the single-line rule turns the read-only paste check red.
- **`uiclient_test.py`** -- drives `userland/tests/uiclient.c`, the ring-3
  client that renders real text with `userland/libs/ui/ugfx.c`, and asserts on
  it (8 checks: text actually rendered, the button drew, a click and a
  key each repaint, the unchanged label comes back identical, the close
  handshake works). Two things it encodes: "text was rendered" is
  asserted as INK COVERAGE in a band rather than a single-pixel sample
  (a glyph run puts a countable number of non-background pixels in its
  rows; a blank window and a solid fill are both distinguishable that
  way), and **a client's `stdout` goes to the owning Terminal's
  scrollback, not the serial console** -- so `DebugConsole.logs()`
  can't see a client's own log lines even though a shell-spawned
  process's are visible. Run it after touching `userland/libs/ui/ugfx.c` or
  the font-sharing path.
- **`winclient_test.py`** -- drives `userland/tests/winclient.c`, the ring-3
  client that owns a real window on the desktop, and asserts the
  windowing protocol end to end (8 checks: the window appears in the
  WM's own list at the requested size, the client's pixels reach the
  screen, a key and a click each route to it and make it redraw, the
  window behind it does NOT change, the close handshake completes, the
  desktop survives). Geometry comes from `gui windows` and content from
  PIXEL VALUES with a control point, per `docs/gui-guidelines.md`. Run
  it after touching `userland/apps/system/toywm/wm_client.c`, `kernel/proc/win_role.c`,
  or anything in `abi/win_proto.h`.
- **`sched_gui_test.py`** -- proves the desktop stays ALIVE while a
  ring-3 process runs, the end-to-end counterpart to
  `kernel/proc/sched_test.c`'s KTESTs. The trick it encodes: the `gui`
  debug commands are dispatched from inside `wm_run()`, so a frozen WM
  cannot answer one -- which makes "did the WM answer?" a direct
  liveness test with no screenshot to interpret. Every sample is paired
  with the WM's own `proc_pid` (`gui state --json`) so only samples
  overlapping a genuinely live process count; OVERLAP is the claim, not
  speed. Run it after touching the scheduler, `wm_run()`'s loop, or
  anything about process spawning. Both it and the KTESTs were checked
  as positive controls with the change disabled (0 overlapping samples
  there, versus a continuously responsive desktop) -- do that again
  before trusting a clean run, same reasoning as `damage_sweep.py`'s
  `--positive-control`.
- **`gpl_switch_test.py`** -- the GPL switch, `software.show_gpl`
  (`docs/software-design.md`, stage 4), through the compositor's own
  reports (`gui menu --json`'s apps, `gui icons --json`): on, the Start
  menu lists DOOM and the Web Browser and a DOOM launcher copied to the
  desktop is an icon; off -- with NOTHING in `/usr/wm/applications`
  changing, so the reload must follow the setting -- DOOM leaves both
  and the Browser (MIT, though it needs the GPL engine) stays; the Games
  folder is Minesweeper alone and its row opens Minesweeper (the
  positional-rows trap); DOOM's files stay; on again, it returns. Red
  with the setting's reload trigger in `wm.c` mutated out (3 fail).
  `appentry_test` (`/tests`, usertest) holds `uappentry_each()` to the
  same switch, and its lookups to NOT following it.
- **`gfxdemo_test.py`** -- drives the Shapes demo (`userland/apps/demos/gfxdemo/gfxdemo.c`)
  and asserts on its log + its pixels. Run it after touching
  `kernel/lib/geom.c`/`fixed.c`, `uui_canvas`, `ugfx_tex.c`,
  `uui_toolbar`, or anything a ring-3 client draws with. Beyond the
  cube and the 2D scene it checks the teapot is a solid whose texture
  reaches the pixels (the checker's grey, which the blue untextured
  teapot never has), and RESIZES the window: the toolbar folds into
  View when narrow, a menu row commits, Esc shuts the menu, the
  minimum keeps the scenes and speed controls, and the scene scales
  (no ink on the canvas's outermost pixels). It puts the window back at
  the size it found, because uapp remembers geometry per app. Its
  layout reader resets on each `layout canvas` line: the block is
  re-sent whole, and only on a change. Three of its checks encode reasoning worth
  reusing: the window is proved to be a ring-3 client from `gui windows
  --json`'s `client_pid` rather than from how it looks; "it rotates" is
  paired with "it stops dead at speed 0", because either half alone
  proves almost nothing; and the AA toggle is checked by COUNTING
  DISTINCT COLOURS in the canvas (468 with, 5 without) rather than by
  sampling a point, since a curve moves and a fixed sample point
  doesn't follow it.
- **`animation_test.py`** -- window animations (`userland/apps/system/toywm/wm_anim.c`)
  and `desktop.animations`. Opens, minimizes, restores and closes
  Calculator with the setting on and then off. For each change: `gui
  state --json`'s `anims` is >= 1 right after and 0 once settled, and a
  frame captured mid-ghost differs inside the window's rect from BOTH
  the frame before the change and the settled frame -- so the ghost is
  something in between, not one of them. Off: `anims` stays 0 and the
  first frame after the change already equals the settled one. The
  close is the client being killed, which is the case the ghost exists
  to outlive. Puts the setting back to `on` in a `finally`. Then GLIDE
  PACING: with the floating taskbar on, maximize/restore with animations
  on and off, and the app's new-size present (`gui state`'s
  `maximize_ms`, timed inside the WM) must not trail by the 150 ms glide
  -- an unpaced glide starved the client. Checks first that the taskbar
  glided at all, or the comparison proves nothing.
- **`shadow_test.py`** -- drop shadows (`userland/apps/system/toywm/wm_shadow.c`) and
  `desktop.shadows`. Places Notepad and Calculator over the flat lower
  wallpaper band, focuses one, and reads LUMINANCE just below and
  beside their edges: the focused window darkens the desktop and the
  darkening fades with distance; the inactive window darkens it less;
  Notepad's File menu (a popup window) darkens what is below it; with
  the setting off the pixel beside the edge matches a control pixel
  60 px out, and that control itself has not moved. Puts the setting
  back to `on` in a `finally`.
- **`glass_test.py`** -- transparency (`userland/apps/system/toywm/wm_glass.c`) and
  every `desktop.transparency*` setting, by PIXEL VALUE against a
  control taken with the setting off or the scene changed: the Clear
  taskbar at 50% sits twice as far from its own colour as at 75%,
  whatever is beneath, and floating at 40% shows no shadow band inside
  its edges; under the Start menu, a window's edge is a sharp
  step through Clear and spread out through Frosted, Start's own glass
  setting holds while every other surface is Frosted, and moving the
  window away changes nothing through Wallpaper (but does through
  Frosted, the control); the desktop's menu and Notepad's File menu (a
  `WIN_POPUP_GLASS` client, opened upward over the wallpaper) darken
  toward it when on; window bodies stay white or not by mode, and
  "moving windows" is checked mid-drag with a REAL button held over QMP,
  because an injected `gui drag` releases at once. Runs under the
  damage verifier throughout -- frosted glass is part of its rect's
  damage -- and puts every setting back in a `finally`. Its positive
  controls (dropping the damage growth, the menu painter, or the
  popup flag) each redden exactly the matching check. When the verifier
  does report a bug, the run prints WHICH PHASE it first appeared in
  (taskbar, start menu, desktop menu, app menu with glass on, windows,
  simd tiers) and the last `wm:` context lines (the window over it, the
  pixel before and after) -- the verdict alone was all a flake left.
- **`occlusion_test.py`** -- the compositor's VISIBLE REGIONS
  (`userland/apps/system/toywm/wm_render.c`): two Notepads, A under B. With B
  maximized over all of A, a burst of frames whose damage lands on A
  must show A CULLED and never drawn -- read from `gui compositor
  --json`'s `windows` counters (`culled_total >= 1` is the control that
  the burst reached A; `drawn_total <= frames` is the check). Then B,
  restored and set over A's title bar with every window Clear glass,
  must show A through its body: a pixel against the same pixel with A
  elsewhere, and pure white with transparency off. Runs under the
  damage verifier, whose reference render culls nothing, so a window
  wrongly left out is a reported miss. Positive controls (`mutate.py`):
  disabling the cull reddens the counters; counting a see-through
  window as opaque reddens the verifier -- NOT the pixel, which the
  previous frame's pixels in the back buffer can still satisfy. Also two
  damage checks: a desktop-menu submenu closed by hover leaves no shadow
  band past its far edge (darker while open is the control); the
  calendar, closed by opening the network flyout, leaves its rect
  wallpaper again; so does the network flyout dismissed by a real click
  outside with the pointer still -- by pixels with the verifier OFF, and
  by a scene repaint within `CLOSE_FRAME_S` of a click made just after a
  clock tick and its frame (`DebugConsole.wait_clock_tick()` within
  `TICK_WAIT_S`, then `wait_scene_repaint()`; a missed wait fails the
  check), three closes running, because the
  tray clock's once-a-second frame clears a close that asked for no
  frame within a second and a settled picture cannot tell them apart
  (the remote flyout too where the guest shows its tray item); and
  re-setting `system.timezone` to its own value --
  every client re-presents an identical frame -- counts as
  `presents_unchanged`, with no present charged a full frame
  (`presents_full`, not the global full-frame count, which debug
  commands move).
  Last, `gui compositor`'s per-stage split (`userland/apps/system/toywm/wm_prof.c`)
  must add up to the bucket's mean frame time, with the present among
  the stages -- a split folded into nothing reads all zeros.
- **`smooth_scroll_test.py`** -- smooth scrolling (`ui/uui_scrollanim.h`)
  in the File Manager's icon grid, and the `desktop.smooth_scroll`
  setting. One wheel notch with the setting ON must draw SEVERAL frames
  at distinct positions, spread over time, with shrinking steps
  (ease-out); with it OFF, exactly one. The positions are the app's own
  `files: layout cellgrid` lines read back through `dmesg`, because the
  app emits one per drawn frame that differs from the last and the
  console's live buffer misses a 20 ms burst. A thumb drag must land in
  one frame -- a drag never glides. It writes the setting to
  /etc/desktop.conf and puts it back to `on` in a `finally`, since a
  setting left behind changes the machine for every later tool.
- **`scrollbar_test.py`** -- scrollbar BEHAVIOUR, against the ring-3
  Notepad's OVERLAY bar: the full thumb at rest under Always (the
  default), thin at rest under Automatically (set, then restored), wide
  under the pointer, the thumb
  doesn't jump when grabbed anywhere on it, a drag is reversible, the
  trough pages, and the strip is wide enough to hit. The pointer is
  parked on the strip at the end AWAY from the thumb, so its sprite
  cannot split the thumb's run. It measures the THUMB'S PIXELS (anything
  darker than the groove, so a column scan gives its exact top and
  height) rather than reading text, and takes the strip's rect from
  Notepad's own `notepad: layout scrollbar` line. Written after the
  ring-3 Notepad shipped a bar that scrolled -- so every other check
  passed -- while jumping to put the thumb's top under the cursor,
  making it grabbable only by its top edge. The spec it enforces is
  `docs/gui-guidelines.md`'s "Scrollbars: what a real one does"; run it
  after touching either `apps/ui/ui_scrollbar.c` or
  `userland/libs/ui/uui_scrollbar.c`.
- **A tool that PARKS the real cursor must un-park it.**
  `DebugConsole.warp_cursor()` is the right way to hold a hover -- `gui
  move` lasts one WM iteration -- but the cursor then STAYS there, and a
  menu opened later finds the pointer already inside it. That turned one
  check red 5/5 while its partner ("a click outside dismisses the menu")
  stayed green for the wrong reason: the menu had never opened. Park,
  measure, un-park; `menubar_test.py`'s `hover()`/`unpark()` pair is the
  worked example. This is what the long-standing menubar flake turned
  out to be -- see `docs/decisions.md`.
- **`menubar_test.py`** -- the menu bar, its nested submenus and the
  status bar (`userland/libs/ui/uui_menubar.*`, `uui_statusbar.*`), driven
  through the ring-3 Notepad. 22 checks: the popup is DRAWN (not merely
  responsive), a title opens on press while an item commits on release,
  a press dragged off commits nothing, a click outside dismisses AND is
  swallowed rather than reaching the text, submenus open on hover and
  are placed to the right, Esc closes one level, disabled and checkable
  items behave, and the status bar's indicator tracks the cursor while
  its message pane does not. Two positive controls are recorded in its
  docstring, and they are the reusable part: commit-on-press reddens
  exactly the drag-off check, and a dismissing click that falls through
  leaves "the menu closed" GREEN and reddens only the caret measurement.
  Run it after touching either widget.
- **`popup_test.py`** -- a menu LEAVES its window: popup surfaces
  (`WIN_REQ_POPUP`, `userland/apps/system/toywm/wm_client.c`, `ui/uui_popup.h`),
  driven through Notepad shrunk to its minimum. 18 checks: the
  compositor lists the open menu as a popup entry of Notepad's, its rect
  leaves the parent's content, the popup background is on screen at a
  point the parent does not own (sampled as NOT that colour before the
  menu opened), a row beyond the parent's edge still commits, a press on
  another window closes the menu and does NOT raise that window, a press
  on the desktop dismisses through the compositor and the client agrees
  (`WIN_EV_POPUP_DONE`), and near the taskbar the menu flips above its
  title and stays clear of the panel. Two positive controls in its
  docstring: a provider that refuses every surface reddens the surface
  checks and nothing else (the menu still works in-window, so a check
  green under it was measuring the menu); a grab that delivers every
  press reddens the three "another window" checks. Run it after
  touching `wm_client.c`'s popup path, `uapp_surface.c`'s surface table or
  `uui_menubar.c`'s open/close paths.
- **`start_menu_test.py`** -- the Start menu's FOLDERS, search field and
  keyboard (`userland/apps/system/toywm/start_menu.c`, and `wm_overlay.h`'s `key` op
  that makes typing into an overlay possible at all). 23 checks: the
  menu reports two columns with the app rows starting right of the
  sidebar, clicking a folder shows exactly that folder's apps (compared
  against `gui menu --json`'s own app-to-folder list, with the selection
  asserted SEPARATELY from the contents -- the pair is what tells "the
  highlight moved but the list did not" from the reverse), typing with
  no click into the field filters and the reported query is what was
  typed, Enter launches the highlighted result into a real window, Esc
  clears the query before it closes anything, and the arrows move the
  result and the folder. The pixel half: typing CHANGES the search
  field's own rect while a sidebar action row stays byte-identical --
  "it responds" is not "it is drawn", and the neighbour is half the
  assertion. Since the Kickoff layout it also asserts the field spans
  the header beside its settings button, and that a description the row
  shows whole raises NO tooltip. It also covers what PERSISTS: a pin made
  through the row's own context menu is read back from
  `/etc/start-menu.conf` through an independent path (`sh cat`), and
  then the guest is REBOOTED and asked again -- the only version of that
  check worth having. **It is the one tool that reboots its guest**, so
  `gui_regress.py` launches it with `--reboot`: the default
  `-no-reboot` answers a guest reboot by ending QEMU, and the tool's
  next command would then fail as a dead socket somewhere unrelated.
  It also covers the field as a TEXT FIELD: a key inserts at a moved
  caret, Shift+Right selects and typing replaces, a click places the
  caret, the pointer is the I-beam over the field only, and raw frames
  show the caret blink -- after turning `desktop.caret_blink` back ON,
  which the harness turns off for every tool (`_steady_caret`). The
  frames are taken on a 0.375 s GRID until two differ: a fixed sleep
  after each shot made the loop's period the screendump's cost plus the
  sleep, ~1 s under load, which is the blink's own period -- every frame
  in one half of it (5 runs in 8 with the period forced to 1 s).
  It also covers the DESCRIPTION: a line too long for the strip is
  marked `..` and the tooltip carries the whole of it, with the three
  halves asserted apart because they fail apart -- not up immediately,
  up after the delay with the full text, and a click that still reaches
  the row underneath. Four positive controls in its docstring: refusing
  printable characters reddens the seven search/Enter checks and nothing
  else; a pane that ignores the selected folder reddens the contents
  check while the selection check stays green; a pin that never reaches
  the disk reddens the four persistence checks and leaves the same-boot
  one green. **The tooltip's DELAY is not checked** (removed 2026-09-29
  at the maintainer's request): asking "no tooltip yet" inside half a
  second raced the console under load and failed while the tooltip was
  fine; that it appears, with the full text, and is drawn, still is.
  **Recent's freeze is read from `recent_shown`** in `gui menu --json`,
  the order the menu showed as it closed: a launch closes the menu a
  flash after the click, so reading the rows after it raced that close
  (red with `open=False` under a loaded suite). Removing the freeze
  reddens exactly that check. Run it after
  touching `start_menu.c`, `start_store.c`, `wm_tooltip.c`, the overlay
  table or a `.desktop` `Category=`.
- **`start_settings_test.py`** -- the Start menu's settings, the
  Desktop > Start page (`desktop.start_*`, read once per OPEN, so every
  check reopens the menu). Detailed, Compact and Grid against each
  other: one column vs several, shorter rows showing more, the SAME card
  footprint in all three, and a grid cell's rect actually inked (in a
  list that spot is a blank pill end). The grid's two-level keys. Power
  buttons as the exact footer tail, with search finding Exit to shell
  only while it is offered -- the pair, so an empty result means
  something. Opens on All Apps, Recent and the last folder chosen by
  hand. Recent off: the folder gone AND `run.*` gone from
  `/etc/start-menu.conf` (read with `sh cat`, after asserting a launch
  had put it there), and a launch while off unrecorded. Hover on opens a
  rested-on folder, off does not. Positive controls in its docstring.
- **`trash_gui_test.py`** -- the Recycle Bin through the GUI: the
  desktop's Delete ("Move") and Shift+Delete, the File Manager's Delete
  and Shift+Delete, `trash:/` as a folder with no "..", Restore on the
  bin's command bar, and Back out of it. Every outcome is read back
  through `/bin/trash list` and `ls`, never from the app that did it.
  Reuses `filemanager_test.py`'s layout reader. Its positive control
  routes both Deletes back to a permanent remove: the four "into the
  bin" checks redden.
- **`undo_gui_test.py`** -- the File Manager's Undo and Redo
  (`base/ufileundo.h`): Delete with confirmation off, the toast's Undo
  button (found from the `files: layout toast` line, which carries the
  button's rect and label), Ctrl+Y and Ctrl+Z, a rename and a new folder
  undone -- the folder into the bin, not for good. Read back with `ls`
  and `/bin/trash list`. Its positive control makes Undo a no-op: four
  checks redden. In-place rename selects the STEM, so the test types
  "cherry", not "cherry.txt".
- **`search_gui_test.py`** -- the File Manager's search into subfolders
  (`fm_search.c` over `base/uwalk.h`) against a fixture whose expected
  set is written by hand: typing filters only the folder, Enter lists
  the matches below it (three levels deep, a matching folder) and NOT
  one under a hidden folder, a result opens at its real path, and the
  strip's "This folder" goes back to the filter. **Wait for the layout
  BEFORE the "search done" log line**: the frame after the last result
  is the one to read, and an idle window draws no other -- reading the
  log first swallowed it. Its positive control stops the walk from
  descending: the subfolder check reddens.
- **`sizes_gui_test.py`** -- View > Folder sizes (`fm_sizes.c` over
  `base/uwalk.h`): folders made with `mkfiles` at known sizes, one with a
  subfolder, read back from the app's `files: size <bytes> <path>`
  lines (logged once per folder, on the main thread). Its positive
  control counts only a folder's top level: the subfolder check reddens
  (3000 instead of 4000).
- **`tabs_gui_test.py`** -- the File Manager's tabs in the title bar
  (`WIN_REQ_TABS`, drawn by toywm; `fm_tabs.c` decides). Clicks land
  on the rects `gui windows --json` exports per window (`tabs[]` with
  each close box, `new_tab`, `active_tab`), never on a formula. Each
  tab's own Back history is checked with a step only that tab took --
  a history shared between tabs would stay put. Its positive control
  stops a tab from restoring its history: that check reddens.
- **`launch_test.py`** -- running programs and scripts (`base/ulaunch.h`,
  `ui/uui_runask.h`, `/bin/wm/system/runask`, Terminal's `-e`) from the
  File Manager and through `/bin/open`: the "Run hello.sh?" card and
  each of its answers, the Terminal held open with the exit status and
  closed by Ctrl+D, "Always do this" kept in `/etc/mimeapps.conf`,
  Shift+Enter, the context menu's first row, a file without an execute
  bit, an ELF program, an app (no card) and `open` on a script. **Each
  act is proved by its EFFECT** -- the script touches a marker, uterm
  logs `<path> finished, status 0`, `stat` reads the mode -- never by
  the card having closed. Two traps it encodes: **a layout block is
  logged only when it CHANGES**, so a second identical card logs nothing
  and the rects are read from the LAST block rather than after a clear;
  and **a card a failed step left open takes the next step's Enter**
  (its default button is Run in Terminal), which once turned a broken
  Shift+Enter green -- every step closes stray cards first. `/bin/open`
  is started with `gui spawn`, not `sh`: from `sh` runask's stderr, and
  so its layout report, is the shell's. Its positive controls drop the
  Always save and the Shift+Enter handler: each reddens its own check
  and only that one.
- **`pins_gui_test.py`** -- Pin to Places (`base/upins.h`) through the
  File Manager's context menu, read back from `/etc/places.conf` with
  `cat` and from the side column's rows by PATH. **The target is
  `/usr/share/sounds`, no personal folder** (`base/udirs.h`): a lookup by
  path would find the standard place first. Its positive control
  stops the app listing pins: the row check reddens. Check 4 pins
  `/home/pin-gone` through `/etc/places.conf` BEFORE the app starts (a
  pin is listed only while its folder exists), deletes it under the
  running app and clicks the row; its control is the tree click
  ignoring `fm_goto()`'s refusal, which lands the pane in the deleted
  folder.
- **`fm_places_menu_test.py`** -- the File Manager's left-pane menus
  (`userland/apps/apps/files/fm_places.c`): one menu per kind of row, read from the
  app's `files: placemenu <kind> <path> rows a|b|-|c` line AND the popup
  laid out; Move down, Rename pin and Unpin read back from
  `/etc/places.conf`; Hide and Show hidden places from files.conf's
  `hidden_places=`; Properties on System opens the VOLUME page
  (`ui/uui_volinfo.h`) whose ring is sampled all round its middle circle
  in the window's pixels (`properties: layout ring x y d`, content-
  relative); the details pane at `/` logs `files: details volume /
  button Open in Disks`; Places as ONE list -- a default unpinned and
  restored, a row DRAGGED above Home and a folder dragged from the pane
  into a gap, each read back from `/etc/places.conf`. **A row is found
  by PATH in the layout's `treerow` lines, a heading by its `-`**; the
  dragged folder is `0pin`, first under `..` in any sort order.
  `--positive-control` aims Properties at `/home`, a folder, which gets
  the file page and no ring: that check reddens. Then a VOLUME dragged
  above System (the `dev:` lines and the tree) and one dragged into Places
  landing nowhere. The drag checks were shown red by `mutate.py` -- every
  gap refused in `places_can_insert()`, and volumes made undraggable in
  `group_of()`.
- **`fm_thumb_drag_test.py`** -- the icons view's scrollbar after a
  click: `/bin`, click the first icon after `..` (`wm`), then drag the
  thumb. It must scroll (`layout scroll`) with no file in flight
  (`layout drag`). **The first cell is `..`**, which no drag carries,
  so clicking it proves nothing. Its positive control drops the
  `press_row` reset ahead of `ic_press()`'s scrollbar branch: the
  folder goes with the pointer and the grid stays put.
- **`archive_gui_test.py`** -- archives in the File Manager
  (`fm_archive.c`, `fm_compress.c`, over `base/uarchive.h`). A `.zip` and
  a `.tar.gz` are written by Python's `zipfile` and `tarfile` on the
  host and put on the guest with `vm.py put` **BEFORE the test takes the
  debug console**: put talks over the same socket, and with the test
  holding it the file never arrived and nothing said so. For each:
  the derived folders, opening a folder in place, Up to the archive's
  own folder, Extract all against the sizes Python wrote, and opening a
  member through /tmp/zip-open. Then Compress to ZIP from the context
  menu (named in place, then opened back as a folder), Compress... as
  `.tar.gz` from the keyboard, and on an archive FILE Extract here (into
  `zt`, selected) and Extract to... (its chooser opens; Esc cancels).
  A layout that never arrives is read as an empty one, so a broken build
  FAILS its checks rather than crashing the run. Two traps it encodes: a real folder
  lists `..` as row 0, so a right-click aimed by row index is off by
  one without it; and closing the Notepad a member opened in does not
  promise where focus lands, so the test clicks the pane before typing
  again. Its positive control -- extraction not making parent folders
  -- reddens Extract all for both formats. Whether another archiver can read what SaunaOS WRITES is
  `archive_hostcheck.py`'s question, not this one.
- **`paint_test.py`** -- Paint (`apps/apps/paint/paint.c`, `apps/apps/paint/paint_tools.c`)
  judged by PIXELS, the screen's and then the saved file's: a brush drag
  paints black with the pixel above it still white; the palette's red
  and the fill tool flood around the stroke, which stays black; Ctrl+Z
  and Ctrl+Y on screen; a right-click with the pencil paints the SECOND
  colour (the button the toolkit does not route); the rail's pencil
  button; Ctrl+S on an untitled picture saves where it is told
  (`imginfo` says PNG 800x600); a fresh Paint on that file shows the
  stroke, the fill and the dot; Ctrl++ zooms to 150% with the picture
  rect; the brush kinds -- the highlighter keeping black black and white
  exactly its yellow, a soft stroke's in-between colours, the spray
  adding dots while held still. Its positive controls -- the fill
  painting the second colour; the highlighter blending "over"; the spray
  with no timer -- redden the fill, highlighter and spray checks. In
  `gui_regress.py`.
- **`rename_gui_test.py`** -- Rename many (`base/urename.h`,
  `ui/uui_renamer.h`): Ctrl+A then F2 opens "Rename 3 items", Enter
  takes the default `<folder>-##` pattern, one Ctrl+Z puts every name
  back, and Esc renames nothing. The fixture is a CHAIN -- berry.txt
  becomes rn-02.txt while rn-02.txt is still there -- and each file
  carries its own content, which is what is read back with `cat`, so a
  rename that overwrote would still show the right NAMES. Its positive
  control renames straight to the new names, skipping the temporary
  pass: the rename check reddens.
- **`fm_group_test.py`** -- View > Group by in the File Manager's details
  view (`ui/uui_fileview.h`): a saved `group=type` bands the fixture as
  Folders, Pictures, Music and sounds, Documents, in that order; `..` is
  left out while grouped; Group by > Size and > Name through the View
  menu's submenu, written to files.conf; None takes the captions away and
  brings `..` back -- the negative control. Read from the app's `files:
  layout groups <by> <cap|cap|...>` line, never from pixels. In
  `gui_regress.py`.
- **`fm_quick_test.py`** -- Quick access (`fm_quick.c`, `quick:/`): first
  in Places; its three bands (Pinned folders, Recent files, Drives); a
  file opened with `open` arrives in the Recent band without a reload;
  Down, Enter on a pinned folder goes there; F2 renames nothing (the
  rows are places, not files). In `gui_regress.py`.
- **`fm_back_test.py`** -- Back at once after a double click
  (`fm_history.c`): six round trips over three folders, each Alt+Left
  sent with no wait, must land back where they left; then one more Back
  must still reach the folder the window opened on. A move reaching the
  history only at the next tick failed this (8 in 20 at 0.1 s by hand).
  In `gui_regress.py`.
- **`fm_columns_test.py`** -- the details columns (`uui_table`'s choosable
  layout through `ui/uui_fileview.h`): a divider dragged resizes Size and
  the width is written to files.conf's `left_cols`; the header's
  checklist (right-click) hides Size with Down, Enter -- Name is locked
  and skipped; Recent keeps its own columns, untouched. Read from the
  table's `left.columns` describe line. In `gui_regress.py`.
- **`fm_conflict_test.py`** -- Replace or skip (`ui/uui_compare.h`): two
  conflicts in one copy, dated so they disagree; the card's incoming
  side carries the newer and larger badges and the existing side
  neither; its "Do the same for the rest" box ticks on a click; Keep
  newer for the rest replaces one file and keeps the other, with no
  second card -- read back with `cat`. In `gui_regress.py`.
- **`fm_search_filter_test.py`** -- the search strip's filters
  (`base/ufind.h`, `fm_search.c`): Ctrl+F opens the strip with its four
  chips before anything is typed; Type > Pictures narrows the folder;
  Enter with a filter alone searches below for it; Contents finds a text
  file by its line 4 and NOT a GIF with the phrase appended (binary);
  the results show Name | Match first; Size > Empty searches again and
  finds nothing (the negative control); Esc drops every filter. **What a
  search found is read from the app's `files: search hit <line> <path>`
  lines before `search done`, and the columns from the frames the search
  drew**: the window is idle once the walk ends, and a frame asked for
  then can be an older one still queued. Its positive control (the walk
  ignoring the filter, the binary test removed) reddens four checks. In
  `gui_regress.py`.
- **`fm_progress_test.py`** -- the Operations panel (`ui/uui_transfers.h`
  over `fm_jobs.c`): F5 on a folder of twelve copies of `first-boot.mpg`
  (~100 MB, so a copy lasts long enough to pause under TCG; 50 MB
  finished first on the HP Z1 G8) shows one
  running row whose total is EVERY byte (the counting pass, compared
  exactly against `ls -l`); the bytes grow and a rate is measured; Pause
  holds them for 1.5 s -- measured from a frame taken AFTER the pause
  settles, since the chunk being written when it was pressed still
  lands; two more copies wait as rows and one is cancelled from its row;
  Resume goes on (or finishes between polls -- a dead Resume stays
  paused); at the end the copies are whole, the queued one ran
  and the cancelled one did not. Rows are the panel's `ops.state <row>
  <state> <got> <total> <rate>` lines. Its positive control (a Pause
  that never waits) reddens five checks. In `gui_regress.py`.
- **`recent_gui_test.py`** -- Recent (`base/urecent.h`, `fm_recent.c`):
  `open` on a .txt and a .jpg records both in /var/lib/recent under the
  desktop entries' names (Notepad, Image Viewer); Places has the row;
  `recent:/` lists newest first in two day bands (the layout's
  `captions` count, from a line three days old); Delete takes a row off
  the list and leaves the file; Open folder selects the file in its
  folder; Clear list empties it. **The three-day-old line is written on
  the host and `vm.py put` BEFORE the test takes the console** -- the
  guest's echo cannot write the tab the format needs. Its positive
  controls: every row in one band reddens the bands check, and Delete
  left as Delete reddens the forget check (a confirm dialog comes up and
  the row stays).
- **`filedialog_test.py`** -- the shared file chooser as an OWNED window
  (`WIN_REQ_DIALOG`, `ui/uui_filedialog.h`, `uapp_window_open()`),
  driven through Notepad, Image Viewer and Audio Player. 17 checks: the
  compositor lists a second toplevel with `dialog`/`owner`/`modal` set
  and real chrome, the strip gives it no taskbar button of its own, a
  press on the owner leaves the dialog on top AND cannot drag the owner,
  a Places row changes the directory the view reports, a typed path
  commits and the app loads that file, a breadcrumb segment and a typed
  breadcrumb path both navigate, Places rows are clicked at the rects the
  widget reports (`place_rows()`, since a device row is taller than a
  place), Places' scrollbar thumb DRAGS with the real pointer
  (`drag_real()`; the strip overflowing is checked first, and making a
  press on the bar arm nothing turns it red), Escape cancels and the
  owner gets the focus back, killing the owner takes the dialog with it, and both
  other apps open the same chooser.

  **Read its docstring before trusting the modality checks.** Two
  earlier versions of them stayed GREEN with the block removed
  entirely -- the obvious press point (the centre of the owner's title
  bar) is under the chooser, which is wider than the window that opened
  it, and the next one along is the app ICON, where a press opens the
  window menu rather than starting a drag. `clear_grab()` picks a point
  that is neither, and `place_owner()` pins the window first so such a
  point exists at all; without the pin the cascade can put the whole
  title bar under the dialog and the check fails as a fixture problem
  that reads exactly like a regression. Three positive controls are
  recorded there, including the one that found this.
- **`input_backlog_test.py`** -- input a slow client cannot take at once
  WAITS in the compositor (`wm_client.c`'s line) and arrives whole: a
  200-key burst typed at `/tests/hangclient --slow` (40 ms over every
  key) is logged back by the client and compared byte for byte, the
  window's `backlog` (`gui windows`) must have been non-zero mid-burst --
  a client that kept up would pass the first check vacuously -- and
  `dropped` 0. Then, with `gui evline 64` and the ping timeout raised past
  the wait, a HUNG `hangclient` flooded past the inbox and the cap is
  marked not responding at once, which only the backlog can have done.
  Injected keys drain one per frame and send a press alone, so the flood
  is 300 keys and the wait 10 s. Shown red by sending input straight to
  the inbox again: 106 of 200 characters arrived. In `gui_regress.py`.
- **`forcequit_test.py`** -- not-responding detection and force quit
  (TWP's ping/pong, `scheduler_kill()`, the dialog, and the slot
  reaping). 15 checks. The design point it encodes: a client that
  REFUSES to close and one that is WEDGED are identical to a plain
  timeout, so `winclient` (declines, keeps answering) and
  `userland/tests/hangclient.c` (stops pumping on `h`) are tested
  against each other -- neither half means much alone. Its positive
  control reddens exactly one check and leaves the winclient ones green,
  which is worth reading before trusting them.
- **`caret_blink_test.py`** -- the text caret (`ui/uui_caret.h`) in
  Notepad: captures across two seconds differ ONLY in a caret-sized band;
  ten seconds after the last key they are identical and show the caret;
  `desktop.caret_blink off` holds it solid while typing; and a window
  that lost focus mid-blink does not spin (its CPU time from `ps` over
  four seconds). The last one is the bug the first build shipped with --
  the flip's repaint drew no caret and the loop asked again forever.
  **`gui_debug.enter_gui()` turns blinking OFF for every other tool**,
  because a blink defeats a settled-frame comparison (clipboard_test's
  "a paste with nothing on the clipboard leaves the document alone"
  failed on the phase); this tool does not call it and sets the setting
  itself. In `gui_regress.py`.
- **`half_frame_test.py`** -- no HALF-PAINTED window reaches the screen.
  The File Manager on `/bin` in icons view, scrolled with the real wheel
  while QMP dumps frames as fast as it can; a frame whose icon pane is
  mostly the window's plain grey (236,236,236) was composited while the
  client was repainting it. **The pane moving is the control** -- frames
  that differ from the one before -- since a scroll that never happened
  would pass on a still picture. Guards the buffer handback
  (`WIN_EV_BUF_RELEASE`, `docs/decisions/gui.md`). **It captures until
  40 frames have MOVED** (up to 960), not a fixed count: the power is
  in the moved frames, and a fixed 240 fell under the control's floor
  beside fifteen other guests. Positive control: `surf_back()` drawing
  into the front buffer, 85 of 241 half-painted -- merely ignoring the
  busy flags no longer fires with three buffers. In `gui_regress.py`.
- **`idle_desktop_test.py`** -- with nobody touching it, does the screen
  SIT STILL? Two regions (the icon column, an empty patch) must be
  pixel-identical across eight captures. Written after a blinking console
  cursor appeared on top of a desktop icon and every one of the 23 GUI
  tools passed, for a structural reason worth knowing: they all DRIVE the
  desktop and assert on what changed, so nothing was asking the opposite
  question. It is the cheapest check for a whole class of bug -- a second
  owner writing to the framebuffer. **The taskbar is excluded and its
  clock is the control**: it must CHANGE, which is what proves the
  capture pipeline can see motion at all (without it, a harness handing
  back one cached frame would report a beautifully steady desktop). In
  `gui_regress.py`.

  **It was the only tool that did not wait for the desktop**, and that
  cost a real investigation: it connected and asked `gui state --json`
  straight away, so a boot that had not finished starting `toywm`
  failed it two seconds in with `no window manager running`. Measured
  with `predates.py` at **1 run in 6 on HEAD and 3 in 6 on a busier
  tree** -- pre-existing, and at those counts the two rates are not
  distinguishable, which is exactly why the fix is the poll rather than
  an argument about whose change it was. It calls `wait_for_desktop()`
  now (6 of 6 after), and deliberately NOT `enter_gui()`: that also
  turns on the per-frame layout log, and a tool measuring what an idle
  desktop does is the last one that should be given more to log.
- **`icons_test.py`** -- application icons from a `.qoi` file to the
  screen (9 checks), with the host as the oracle again: Pillow decodes
  the same file, scales it the same way, composites it over the sampled
  wallpaper, and the guest has to agree. Covers all three draw sites
  (desktop, Start menu row, taskbar button -- the last matched through
  the entry's `AppId=`), the missing-file fallback, and that the cache is
  a CACHE (`gui icons` reports the count; twelve forced repaints must not
  change it). **Read its docstring before editing the corner check**: the
  first version asserted the corner was "not the tile colour", which a
  plain `ugfx_blit()` satisfies by writing black, and only the positive
  control found that. In `gui_regress.py`.
- **`drag_image_test.py`** -- the compositor's drag image (`wm_dnd.c`,
  `WIN_REQ_DRAG_FEEDBACK`): a file held over a folder in its own File
  Manager window says "Move to sub" (the source's own answer), over the
  desktop's background "Move to Desktop" and with Ctrl "Copy to
  Desktop", over Notepad "Cannot drop here" (Notepad's answer); the
  chip's box holds text-coloured pixels in a real frame; the release on
  its own folder leaves the file. Reads `gui dnd --json`. In
  `gui_regress.py` as `dragimg`.
- **`desktop_drop_test.py`** -- dropping onto desktop icons, in
  `gui_regress.py`: each icon's art by type (`gui icons --json` reports
  `art`, the draw loop's own lookup), thumbnails (`thumb`, `peeks`: a
  .qoi, a .jpg and a folder holding a .bmp come out drawn as themselves
  from `/bin/thumb`'s cache; a text file does not), and HELD real-pointer drags that
  stop on a target and ask (`target`, and the top-level `drop` label):
  a folder lights and takes the file, a plain file is no target (the
  control), Notepad refuses a .qoi and nothing moves, the Recycle Bin
  takes one and turns `trash-full`, Image Viewer opens one. Then the
  extras: the first move's Undo card (`gui state --json`'s `notice`),
  Ctrl+Z taking it back, no card for the second operation, New > Text
  document opening into rename, Sort by > Size leading with the biggest
  file (and by name not, the control), Auto arrange putting a dropped
  icon back (and without it the drop moving it, the control), and Show
  desktop icons off hiding every icon. Positive
  controls: folders never targets turns the three folder checks red; a
  cache the compositor can never read turns the thumbnail check red.
- **`desktop_many_test.py`** -- a desktop past its old 64-icon table:
  75 files in `/home/Desktop` must all be icons (`gui icons --json`); a
  rubber band over the columns holding only test files -- worked out
  from the icons' own rects, not an assumed layout -- must select every
  icon it covers; and Delete, then the dialog's Move, must put every one
  in the Recycle Bin, a selection whose paths are past `SPAWN_ARGS_MAX`
  and so need `base/uargbatch.h`'s several spawns. Its positive control
  is the build before: 64 icons in all.
- **`folder_options_test.py`** -- the desktop following the File
  Manager's Folder Options (`/etc/files.conf`, `base/ufolderopts.h`) and
  the one double-click time (`system.double_click`, `base/udclick.h`), in
  `gui_regress.py`: extensions off and on in `gui icons` names (a folder
  keeps its name), a dot-file appearing only with `hidden=on`, thumbnails
  off, a 1 MB cap that leaves the 1.03 MB `orbit.avi` an icon while a
  small picture stays a thumbnail (and 2 MB lets it in), Delete asking
  by default (`gui dialog`) and not with `confirm_delete=off`, one click
  opening a folder only in single-click mode (with the link hand,
  `cursor.shape` 7), presses ~450 ms apart a double click at Very slow
  and not at Very fast, and System Settings' Test folder opening on a
  double click (`settings: dclick test opened`; the page is wheeled with
  the REAL pointer, which is where a wheel goes). Positive control:
  breaking the confirm gate and the tick conversion turned it red.
- **`trash_limits_test.py`** -- the Recycle Bin's settings, in
  `gui_regress.py`: `desktop.show_trash` hiding and showing the bin's
  icon and bringing back a deleted entry, a hand-made item dated 2020
  deleted by the desktop's own `trash expire` within seconds of
  `system.trash.max_age 30` while one deleted now stays (and `trash
  expire` with no limit deleting nothing), and the bin's "Recycle Bin
  settings..." row opening System Settings on the page with its usage
  card (`settings: trash usage`). The SIZE limit's trimming is not
  covered: its smallest step is 5% of the volume.
- **`desktop_menu_test.py`** -- the desktop's menus, glass, Rename and
  Properties (26 checks; the last three are `desktop.menu_spacing`: the
  rows shrink in order, the card is DRAWN shorter -- the comfortable
  card's last row is wallpaper under the compact one -- and Notepad's
  File menu shrinks with it), from the WM's own geometry (`gui ctxmenu
  --json` now reports each row's rect, `disabled`, and a second submenu
  level as `sub2`; `gui icons --json` reports `hovered`/`renaming`).
  Paste greys and un-greys, Open > reaches every desktop app through its
  categories, the keyboard opens and commits and Esc closes one level,
  the icon menu's strip and Rename's greying, rename by F2 (a folder on
  disk, a launcher's `Name=`), Alt+Enter, the hover wash with a control
  icon, the GLASS check -- two icons over different wallpaper, each
  matching its own blend, which a solid fill cannot -- and an app
  popup's compositor-cut corner. **It establishes an empty clipboard by
  spending a cut**: the clipboard outlives a run, and a second run on
  the same guest inherited the first one's copy. In `gui_regress.py`.
- **`taskbar_menu_test.py`** -- the taskbar's empty-strip menu and the
  Start button's (14 checks), through `gui taskbar --json` and `gui
  ctxmenu --json` (whose rows now carry `checked`). The rows in order;
  the tray opening NOTHING and a window button still opening its
  window's menu, which is the control a strip handler that swallowed the
  buttons would fail; a Combine choice as a ROUND TRIP -- the setting
  written, the taskbar adopting it, the reopened submenu's tick moved --
  then put back; Floating panel both ways; the Start button's rows and
  Leave >; a tool row opening Task Manager; Leave > Shut down opening
  the Leave page dry and Esc backing out. Positive controls: the strip
  handler returning early reddens every menu check and leaves the two
  controls green; a choice that writes nothing reddens checks 2 and 3.
  In `gui_regress.py`.
- **`mines_test.py`** -- Minesweeper (29 checks), and the protocol
  property it was built to prove: **a secondary click reaching a ring-3
  client**. The check that matters is a PAIR -- a right-click on the
  BOARD must flag a cell and leave no context menu open, and a
  right-click on the TITLE BAR must still open the window menu; either
  half alone passes under a half-broken split. Game rules are asserted
  against the client's own `mines: state ...` log line rather than
  pixels (a screenshot cannot tell a flood fill that opened 51 cells
  from one that opened 3), and geometry comes from its `mines: layout
  ...` lines, menu rows included, so nothing here re-derives the app's
  sizing. Two things its docstring records, both learned the expensive
  way. **The flag's pixel check compares the SAME cell before and
  after** -- the first version compared the flagged cell against a
  different one and passed for the wrong reason, because the cell it
  flagged happened to be revealed and a revealed cell differs from a
  covered one whether or not a flag ever drew. And **the clock is
  asserted to ADVANCE, never to reach N after N seconds**: the guest's
  timer runs at whatever rate TCG manages, measured here at well under
  half real time under load. Both positive controls were run -- disabling
  the WM's content forwarding reddens four checks and nothing else;
  narrowing the first-click safe zone from the 3x3 neighbourhood to the
  clicked cell reddens exactly one, on the right assertion. **It plays
  a game to a WIN** from the minefield the client logs (layout log
  only), checks the "Solved in" note is drawn -- POLLED, because the
  win's effects run in stages with still gaps between them and a
  settled frame taken in one is not a finished win -- and that a new
  process reads the best time back; removing the note's fill reddens
  that one check. **Its clicks are PACED** -- one, then the state line
  it produces -- because `gui click`s sent back to back are lost
  (`docs/bugs.md`). In `gui_regress.py`.
- **`calendar_test.py`** -- the tray clock's calendar popup: opening,
  the grid, paging, `locale.week.start`, `locale.week.numbers` (the
  column's own pixels, not the flag) and the "Date & time settings..."
  link (System Settings must report the Date & time page). **The grid is checked
  against the HOST's `datetime`**, which shares no code with the guest's
  `cal_days_from_civil()` -- a test that re-derived the first column the
  same way the code does would agree with a wrong implementation. **Every
  open/close check is paired with the panel's own pixels**, because
  `gui calendar --json` reporting `open: true` is exactly what a popup
  that draws nothing also reports (this repo's "it responds is not it is
  drawn" trap). The comparison box is the panel BELOW its clock card --
  the card ticks every second, and a box containing a clock could never
  settle. **The year boundary is exercised** (paging back
  past January), since "month - 1" with no wrap is the obvious bug, and
  **the week checks restore what they found**, since a written setting
  outlives the run and changes the machine for every later tool.
  Three positive controls were run: disabling today's highlight reddens
  one check, ignoring the setting reddens three, and removing the year
  wrap reddens two -- each on the right assertion and nothing else. In
  `gui_regress.py`.
- **`clock_settings_test.py`** -- System Settings' Date & time page:
  the Change... dialog, `/bin/time -s`, and the lock while network time
  is on. **The oracle is the kernel's `config get clock.utc`, never the
  dialog**: Set must move it by exactly the hour the dialog was told,
  which a Set that never reached `SYS_SETTIME` fails (that positive
  control was run, and reddens exactly that check). `time -s` then puts
  the ORIGINAL LOCAL time back under Helsinki, which only lands on the
  original UTC if the local-to-UTC conversion is right. The NTP lock is
  checked by clicking Change... and requiring that no dialog appears;
  `system.ntp.server` is pointed at 127.0.0.1 first, so turning NTP on
  sends nothing off the machine. Restores `system.ntp`,
  `system.ntp.server`, `system.timezone` and the clock. A dialog
  publishes no widget map, so its controls come from its
  `settings.clock: layout` lines. It also checks
  `desktop.clock_seconds`: on, the taskbar clock's pixels change within
  a second; off, they hold still for 2.5 s (tried twice, for a minute
  rolling over) -- the control that ignores the setting reddens that
  half. In `gui_regress.py`.
- **`volume_test.py`** -- the taskbar's volume flyout: the slider, mute,
  the wheel and the output-device list. **The slider's real assertion is
  `config get volume`, not the popup's own reading**: the write is
  DEBOUNCED, so a build that moved the number on screen and never
  flushed it would pass every check phrased against the popup. **The
  wheel check has a negative half** -- a notch over the tray icon moves
  the level, the same notch over the desktop must NOT, which is what
  fails if the handler forgets to ask where the pointer is and then eats
  every scroll in every app. The pointer is WARPED
  (`DebugConsole.warp_cursor`), never `gui move`: an injected position
  survives one `wm_run()` iteration and the wheel would arrive with the
  cursor back where it was. Open/close is paired with the panel's own
  pixels, same reason as the calendar's. `gui_regress.py` boots it with
  `--audio both` (see `EXTRA_VM_ARGS`), because the device rows are
  empty and unwritable on a machine with no sound card -- it says so and
  skips them rather than failing, so the rows going missing on a machine
  that HAS a card still reads as a failure.
- **`tray_press_test.py`** -- the notification area's feedback: a
  rounded fill while the pointer rests on an item, a different one while
  it is held, and neither latching. Driven on the CLOCK, the one tray
  item that is registered whatever hardware the guest has. Every
  assertion is a pixel value read from the item's left padding strip --
  **a band the glyphs never reach, so the seconds ticking underneath
  cannot move it**, which is what lets an ordinary pixel comparison work
  on a control that redraws once a second. The cursor is PARKED on the
  item (`DebugConsole.warp_cursor`, never `gui move`): the strip must
  change on hover while `tray_pressed` stays -1, and change AGAIN when
  held. The direction of the wash is READ, not assumed
  (`uui_state_bg()` darkens a light panel and lightens a dark one). It
  also asserts the fill is ROUNDED -- the pixel at the fill's own corner
  (`btn_y` from the report) must sit nearer the panel's colour than its
  interior -- that a drag off the item disarms and a re-entry re-arms,
  and that nothing latches: the calendar the press opened is still up
  while the clock, the pointer gone, is back at rest. It asserted
  hover stayed DEAD until the 2026-09-29 redesign gave the tray a
  hover (`docs/decisions.md`). Skips cleanly with no Pillow. In
  `gui_regress.py`.
- **`taskbar_style_test.py`** -- the taskbar's independent layout
  settings (`desktop.taskbar.buttons`, `_align`, `desktop.start.position`,
  `desktop.taskbar.float`) and its two themes, each checked in the WM's
  report (`gui taskbar --json`) AND in a pixel the report could not
  fake: the defaults fill the band and the screen's corner is the strip;
  floating's band grows by the gap, the corner pixel is NOT the strip,
  and maximizing the topmost window DEFLATES it; icons centred with
  Start are 48px, the one under the pointer is reported hovered AND
  drawn lit, and (peek off) its tooltip names the window; Start left
  goes to x=0 with the icons still centred, and that floats; icons from
  the left pack straight after Start; a centred Start over left buttons
  sits alone in the middle; labelled buttons centre with their labels
  whole; light draws `#ECECEC`; an icons-only GROUP carries a count
  badge in the strip's ink while its window has the focus, a single
  window none, and a labelled group reads "Notepad (2)" (`gui taskbar
  --json` reports each button's `badge`). **The gap probe is placed from the
  SCREEN's edge, not from the reported panel** -- its first version
  derived it from the panel, so a wrong report moved the probe off the
  screen and crashed the tool instead of failing a check. Positive
  control: forcing `taskbar_floating()` to 0 reddens the floating
  checks, and dropping the hover fill reddens "drawn lit" while the JSON
  hover check stays green -- which is why both exist; no badge reddens
  the badge check alone, and the app id for a group's name reddens the
  label check alone. Unsets every taskbar setting on the way out. In
  `gui_regress.py`.
- **`quick_test.py`** -- Quick Settings (`userland/apps/system/toywm/wm_quick.h`)
  against `gui quick --json`: the tray's group button opens six tiles;
  the network tile, on, is drawn in the accent (a pixel); Do not disturb
  turns `desktop.notifications.dnd` and the bell on; Transparency turns
  `desktop.transparency` on; the on-screen keyboard tile opens and closes
  its overlay; a click a quarter along the volume slider sets
  `system.volume` to a quarter; the Keyboard tile, with two layouts,
  switches to the next; the network chevron shows the network flyout as
  a page (`gui state`'s overlays) with Back, Back returns to the tiles,
  and a click outside closes the page and the card; Esc closes the card.
  A hovered whole tile keeps its right-hand outline (the wash once
  covered it; a pixel inside must change, so the hover was drawn), and
  on the keyboard-layout page (and the remote-desktop one, which needs
  a live session and SKIPs without) Back ends above the first row
  (`gui layout --json`'s row `top`, `gui remote --json`'s `list_y`).
  Needs a sound device for the slider (`vm.py --audio-wav /dev/null`; it
  SKIPs without one). The flyouts' own tools (`volume`, `brightness`,
  `network_tray`, `keyboard_layouts`, `osk`, `occlusion`) call
  `gui_debug.separate_tray_icons()` first, so a click on an icon opens
  its flyout rather than Quick Settings. Positive controls: the transparency tile doing
  nothing reddens its check alone; the page not followed when it closes
  itself reddens "a click outside closes both". In `gui_regress.py`.
- **`scale_test.py`** -- the display scale (`display.scale`, fontd's
  beacon, `ugfx_px()`): at 150% the taskbar is 72 px (48 at 100%) and
  DRAWN so (a pixel 60 rows up is the strip's colour), an open Notepad
  grows by half and stays above the taskbar, its title bar by about half;
  Settings' sample (`scale_sample`, opened with `settings display.scale`)
  has 72 rows of its taskbar's colour; back at 100% the taskbar is 48,
  the window its old size (or two thirds of a height the work area cut)
  and the title bar exactly what it was. Positive controls (both at
  once): `read_height()` without `ugfx_px()` reddens the 150% taskbar
  checks; `wm_scale_changed()` doing nothing reddens the window's growth
  and its round trip. In `gui_regress.py`.
- **`pins_test.py`** -- pinned taskbar apps (`wm_taskbar.h`,
  `start_store.h`'s `taskbar` list) against `gui taskbar --json`'s
  `pins`: the shipped pins stand in order as icon-wide buttons, each
  icon drawn (a pixel off the strip's colour); a click on a pin starts
  the app and its window's button stands at the pin's x, flagged
  `pinned`; an unpinned window comes after the pins; a drag puts the
  Terminal pin first and `/etc/start-menu.conf`'s `taskbar` line says
  so; an unpinned window dragged to the front stays after the pins
  WHILE HELD (the drop alone cannot show it; SKIPped on bare metal,
  which injects no press without its release); a window's button menu
  pins it, a pin's menu unpins it (and from the file); closing the app
  gives the pin back in its place. Starts from an empty desktop and the
  shipped pins (`gui pins set`), and puts them back. Positive controls:
  `resolve_pins()` never marking a pin running reddens the two
  "in its place" checks; `apply_drag()` without its side clamp reddens
  "... while held" alone. In `gui_regress.py`.
- **`notify_test.py`** -- notifications (`userland/apps/system/toywm/wm_notify.h`,
  `ui/unotify.h`, `/bin/notify`) against `gui notify --json` and `gui
  state`'s `notice`: a notification puts up its card and an unread row;
  **`notify --wait` prints the label of the action clicked on the card**
  (the answer reaching the sender), and the answered row leaves the
  history; under Do Not Disturb a normal one has no card but is kept and
  a critical one shows; an app set `off` in `/etc/notify.conf` keeps
  nothing, `quiet` keeps without a card, `-t` is never kept; the bell
  opens the history and clears the badge, a row's x removes it, the
  switch writes `desktop.notifications.dnd`, Clear all empties it; and
  Settings' Apps card writes `app.Backup=off` when Off is chosen.
  Notifications are sent as `sh spawn /bin/tosh -c notify ...` -- the
  kernel shell's `spawn` keeps quotes. Positive controls: a show under Do
  Not Disturb reddens the two DND checks, and no WIN_EV_NOTIFY push
  reddens the action check. In `gui_regress.py`; N/A on bare metal (it
  drives the kernel shell's `spawn`, `write` and `append`).
- **`snap_test.py`** -- window snapping (`userland/apps/system/toywm/wm_snap.h`,
  `wm_snaplayout.h`) against `gui windows --json`'s `snap` and `saved`,
  `gui switcher` and `gui snaplayout`: a drag to the left edge fills the
  left half (0,0, the work area's height) and Snap Assist lists the rest
  in the right half; a click there snaps Notepad right; dragged off,
  Super+Right'd back, maximized from a half and restored, and dragged
  down off the top, a window is **back at its own size** (the check a
  restore reading the half's rect fails); Super+Left/Up/Down walk half,
  quarter, half, quarter; the bottom-right corner gives that quarter;
  Calculator (not resizable) does not snap; resting on Maximize opens
  Snap layouts, a hovered zone turns a wallpaper pixel toward the accent,
  a click lays the window in the middle third and Assist offers the left
  third, then the right; Super+Z opens it with a zone lit. Double-clicks
  go as two `gui click`s, the first unsettled. Positive controls: the
  restore rect retaken on every snap reddens the four own-size checks
  alone; the preview at alpha 0 reddens the accent check alone. In
  `gui_regress.py`.
- **`switcher_test.py`** -- Alt+Tab (`userland/apps/system/toywm/wm_switcher.h`)
  against `gui switcher --json`: **two quick taps come back to the first
  window** (most recent first -- a z-order or taskbar-order walk passes
  one tap and fails the second), and the tap reports the card not drawn;
  held, the card draws with the focused window first and the pick
  second, its thumbnails read as pictures from the screen; Tab,
  Shift+Tab and Right move the pick and Alt's release switches to it; Esc
  changes nothing; a click on an entry switches; a minimized window is
  listed last and a switch restores it; Alt+` lists only the focused
  app's windows. Keys go as `gui key 0x09 alt` -- a bare `9` is the
  character. Positive control: `build()` walking windows[] bottom-up
  reddens the four order checks and nothing else. In `gui_regress.py`.
- **`taskbar_peek_test.py`** -- the taskbar's window preview
  (`desktop.taskbar.peek`, `userland/apps/system/toywm/wm_peek.h`) against `gui peek
  --json`: resting on a button opens a card over it; **the thumbnail is
  a picture** -- 49 samples of its rect must be neither near-black nor
  flat, which is the check the first build failed (every thumbnail
  black, card otherwise perfect); moving along the strip switches at
  once and leaving for the desktop closes it; clicking the entry
  activates the window; in `highlight` a wallpaper pixel darkens by a
  quarter while one inside the lifted window keeps its value; the x
  closes the window; a GROUP closes one x after another -- the card
  stays up holding the window left, and the last x takes it down;
  `off` opens nothing. Positive controls: restoring the alpha-weighted
  scale reddens exactly the thumbnail check, and an x that closes the
  card again reddens exactly the stays-up check. In `gui_regress.py`.
- **`taskbar_drag_test.py`** -- dragging taskbar buttons: mid-drag the
  reported row has the dragged button under the pointer and the one it
  passed in its old slot, and a screenshot has it drawn lifted; the drop
  commits the order and activates nothing; the glide is measured by the
  WM's `glide_frames` COUNTER (a 150 ms glide fits between two samples of
  a `gliding` flag -- the first version sampled one and read False), and
  with `desktop.animations` off the counter does not move; a plain click
  still activates, on release; a new window lands at the end; a file
  dragged off the desktop and rested on a button raises its window, and
  is let go over the empty strip where nothing takes a drop. Positive
  controls, both run: without `apply_drag()` six checks go red; with the
  glide made a jump, exactly the glide check does. In `gui_regress.py`.
- **`brightness_test.py`** -- the taskbar's brightness flyout, driven
  on a machine with NO backlight, which is every QEMU adapter. It
  asserts the degraded path honestly: the tray item exists, the panel
  is painted and repainted away, the sentence it shows is the
  registry's own (`config set brightness` refuses with the same words),
  and the slider and the wheel WRITE NOTHING -- checked against `config
  get`, since a build that skipped the availability check would push a
  value the registry refuses and show a level the hardware never took.
  It also drives `desktop.tray.brightness` through all three values:
  `auto` must HIDE the sun on this backlight-less guest, `never` must
  too, and `always` must show it anyway. An absence is weak evidence, so
  it is taken twice -- the compositor's `tray_hidden` AND `gui taskbar
  --json`'s `tray_x` moving by the icon's width, which comes from the
  same walk that draws the strip and so cannot report a tray narrower
  than it painted.
  **It PINS the setting to `always` for everything else it does**: on
  the default `auto` there is no sun in a QEMU tray to click, and checks
  1-5 would have nothing to drive. It puts `auto` back on the way out,
  since an extra tray item shifts the taskbar geometry other tools
  measure. Its setting writes go through the console's own `sh`, never
  `vm.py exec` -- two readers on one serial socket steal each other's
  replies and the write silently does not happen, which reads exactly
  like the feature being broken.
  The positive half (a real panel dimming) runs on the bare-metal
  laptop by hand: `config set brightness 40` reads the PWM back. In
  `gui_regress.py`.
- **`keyboard_layouts_test.py`** -- several keyboard layouts end to end:
  `system.keyboard.layouts` refuses a layout with no file, switching the
  active layout writes nothing to `/etc`, the tray item hides with one
  layout, Super+Space walks with Super HELD and commits on its release
  (a tap is the next layout, Esc cancels), a click switches, and the
  Settings page's preview CHANGES with the selection while the Add
  button beside it does not. Try it must make the previewed layout
  active and give the old one back; Move up plus Apply must store the
  order. **Super is held with `QMPSession.key_down()`** -- `send-key`
  releases everything at once and can only test the tap. In
  `gui_regress.py` as `kblayouts`; its controls (release not switching,
  Try it not switching) each turn exactly their own check red.
- **`network_tray_test.py`** -- the taskbar's network item: the icon's
  state, the card behind it, its adapter switch, `/bin/netctl`'s
  `down`/`up`/`renew` through netd, and the visibility setting. The
  switch is checked by its OUTCOME -- the kernel's `admin_down` through
  `gui network` (the address is KEPT), "switched off" in `netctl`'s own
  listing, and a ping that gets no reply -- so a netd that answered
  "accepted" and did nothing (the positive control) reddens them.
  **Its strongest check is the one that does not ask the compositor**:
  the interface name and address the panel reports must also appear in
  `/bin/netctl`'s output, which walks the same `QUERY_NETDEV` class
  through a completely different program -- a compositor reporting its
  own view back to a test proves only that it is self-consistent.
  **Connecting** is driven for real: QMP's `set_link` cuts the guest's
  wire and `netctl address DEV none` takes the address off, so netd
  asks and nobody answers. The tray item must show several DIFFERENT
  pictures over unsettled screenshots (an animation never settles), one
  with `desktop.animations = off`, and one again past 30 s while netd
  still asks behind its 169.254 address; with the wire back it must bind
  WITHOUT a Renew. Positive controls: netd treating 169.254 as set by
  hand, and a tray that never advances a frame.
  It also pins the rule the widget exists for: `link_known` is
  THREE-valued, a driver that cannot answer is not a driver saying
  "down", and the e1000 in a default QEMU guest is exactly that case --
  so `connected` must follow the ADDRESS. The positive control for
  that (deriving `connected` from the link flag instead) reddens those
  two checks and nothing else. Then: the panel is PAINTED rather than
  merely flagged open, a second click on the icon closes it, a click
  outside dismisses it and restores the pixels underneath, the volume
  flyout and the Super key close it through the overlay table's `close`
  op, and `never`/`auto` hide and show it with `gui taskbar --json`'s
  `tray_x` moving by the icon's width. In `gui_regress.py`.
- **`modeset_test.py`** -- a runtime resolution change under a live
  desktop, on the default adapter. **The oracle is the DEVICE**: a QMP
  screendump's own pixel size is QEMU's scanout geometry, and it must
  equal both the requested mode and what `gui state` reports -- the
  two disagreeing is the stale-`gfx_width()` bug the work replaces.
  Then the Start menu must paint at the new size, a maximized Notepad
  must fill the new screen with its pixels in the corner outside the
  old mode, an unlisted mode must be refused with the screen untouched,
  and the boot mode must come back. It restores the setting, because a
  stored resolution is applied at the next boot. In `gui_regress.py`.
- **`live_wallpaper_test.py`** -- live wallpapers: the background client
  (`userland/apps/system/toywm/wm_background.c`), read through `gui background`, a JSON
  line with the pid, program, frame count and whether it is paused. It
  checks that the background MOVES between two raw captures while the
  taskbar strip, its control, does not, and that the moving picture is
  not the still one. **The pause is checked against its own resume**: a
  screensaver (`gui idle start`) must stop the frame counter and its
  stop must restart it, because a counter that never moved would pass
  "stopped" on its own. It also kills the client to check the fallback
  to the picture and the restart, plays `rain.gif` (a Live choice) through the
  GIF player, reads a plain colour back from pixels, and shows that the same
  program started by hand is an ordinary window. Every setting it
  touches is restored. In `gui_regress.py` as `livewall`.
- **`wallpaper_mode_test.py`** -- `desktop.wallpaper.mode`, at a screen
  mode where the setting can actually be seen. **It sets 1024x768
  because at the default it would measure nothing**: both stock
  wallpapers are exactly 1280x720 and so is the boot mode, and at a
  matching aspect ratio `fit` and `fill` produce the SAME pixels -- so a
  check taken there passes whether the setting works or not. Its first
  check asserts that premise rather than assuming it, so a future
  wallpaper that breaks it says so here instead of quietly making the
  tool pointless.

  **The pair is what makes it evidence.** One check requires a MODE
  change to log no decode (`uui_image_draw()` re-fits the same decoded
  source every frame, so re-decoding would spend a whole JPEG to reach
  identical pixels); on its own that passes just as well for a broken
  log line or an absent wallpaper. The next changes the NAME and
  requires a decode to appear, from the same log in the same run.
  Neither half means anything alone. The round trip is the third: `fit`
  then `fill` must restore the original bytes, which "the pixels
  changed" cannot tell from "the pixels changed to something else".
  Every setting it touches is restored, the resolution included. In
  `gui_regress.py`.
- **`imgview_test.py`** -- JPEG decoding all the way to a screen,
  and the only one of the three decoder checks that can see a
  pixel. Its oracle is the host: `data/wallpapers/aurora.jpg` is
  1280x720 and so is the screen, so the default wallpaper must match
  libjpeg's decode of that file PIXEL FOR PIXEL, with nothing resampled
  in between -- and the control beside it is that the same samples must
  NOT match the other wallpaper. Then Image Viewer: it lists a directory
  by PROBING its files, decodes, reports where the picture landed, and
  the fit modes are asserted by geometry and by pixels (the letterbox
  bars are the stage's radial ground, so points mirrored about its
  middle column match -- which a cropped picture breaks; the bars are on
  whichever axis the window's shape leaves them). The second picture is
  chosen by clicking the filmstrip's `strip.cell 1`, where the widget
  reports it. Then the redesign's controls: wheel zoom, drag-to-pan
  windowed and full screen, rotate, Properties, the slideshow, and
  "leaving full screen restores the window's rect". **A PAN CHECK PARKS
  THE POINTER OFF THE COMPARED BOX**: with the cursor sprite inside it
  the two frames always differed, and the windowed check passed for a
  pan that never moved (positive controls: the imgview guard, and the
  WM's mid-press leave). **The rect after full screen is WAITED FOR**:
  the WM adopts a size when the client presents at it, so a read straight
  after Esc can still be screen-sized, and the resize checks restored
  to that. They restore with `gui resize`, never a grip drag. Its
  `poll_logs()` is CUMULATIVE: take `len()` before an action and slice.
  Finally "Set as wallpaper" is followed across a process boundary --
  the viewer writes `/etc/desktop.conf`, the desktop notices through the
  filesystem generation counter, and the background becomes the other
  image. Both positive controls in its docstring were run: a swapped
  Cb/Cr reddens exactly the four pixel comparisons and no layout check.
  It restores the original wallpaper on the way out, and its comment
  says why that matters -- leaving "no wallpaper" behind fails the NEXT
  run's first check, which cost a confusing red run. Last (or alone,
  `--only close-chooser`): closing the viewer while its Open chooser is
  up closes both -- an app with no `on_close` closes, question or not.
  In `gui_regress.py`.

  **A GIF PLAYS and a BMP shows**: both are copied in with `vm.py put`
  (inside `DebugConsole.detached()`, since QEMU serves one client per
  serial socket and `put` needs it to start tftpd). The GIF's three
  solid frames must cycle IN ORDER from RAW screendumps -- an animation
  never settles -- at a mean interval under the viewer's 500 ms idle
  tick, so the frame timer is what drives it; then Right to the BMP,
  whose quadrants must be its colours the right way up and stay put.
- **`blank_window_test.py`** -- opens EVERY app in the registry and
  requires its window to contain more than a flat fill. Reads the app
  list from the KERNEL (`gui apps`), so an app added tomorrow is covered
  without editing it. Exists because UI Demo shipped completely blank
  and a 35-check suite passed it: every check asserted on the app's LOG,
  and the widgets were live, hit-testable and simply never painted. In
  `gui_regress.py`.
- **`caption_corner_test.py`** -- a maximized window's close button
  owns the screen's top-right corner (`title_btn_at()`). Probes the
  corner and the top row, then clicks the corner with the REAL pointer
  -- press arms, release commits -- because a probe alone could agree
  with a copy of the hit test rather than the click. Controls: a
  restored window's corner is its frame, and the column below the title
  bar is content. In `gui_regress.py`.
- **`window_geometry_test.py`** -- windows come back where you left
  them (`userland/apps/system/toywm/wm_geometry.c`). Most of it drives the RESTORE half
  from a hand-written `/etc/windows.conf` rather than from a previous
  run, on purpose: a round-trip test alone passes if both halves are
  broken the same way -- save the wrong rect, restore the wrong rect,
  get it back. Writing a known geometry and demanding exactly that one
  cannot be satisfied by a symmetric bug. The SAVE half is covered
  separately by dragging a window and reading the file. A restored
  window's FIRST frame (`gui windows`' `first_frame`) must already be
  its restored content size: drawn before the queued resize was read,
  Settings first appeared at its default 896x704 and then jumped. The
  same holds for an app whose OWN default is taller than the work area
  (the Audio Player's 898x698 on 1280x720): fitted before its first
  frame, not shrunk after it.

  **Its checks were written twice, and the first set was nearly
  worthless**: with restore disabled entirely, five of six still passed,
  because "the window is on screen" and "reopening matches" are both
  true of default placement. They assert exact sizes that can only have
  come from the file now -- 4 of 6 go red under that control, and the
  drag check goes red under the save control. Worth reading before
  adding a check here. **The state and its switches** (2026-10-29): a
  window maximized writes `,max` after its OWN rect and reopens
  maximized, then restores to that size; one snapped writes `,snap:` and
  reopens in its half; a drag's end on a plain window is in the file
  before any close; `desktop.windows.remember off` writes nothing and
  restores nothing, and on again restores the kept line; Forget Saved
  Position (Alt+Space) drops the line and the close does not write it
  back. Positive controls: no `,max` suffix reddens the five state
  checks, Forget not marking the window reddens its two, and no save at
  a drag's end reddens that check alone. In `gui_regress.py`. **On bare
  metal** the state checks pass; the older restore checks assume QEMU's
  1280x720 screen and font, and fail on the ASUS's 1920x1080.
- **`osk_test.py`** -- the on-screen keyboard (`userland/apps/system/toywm/osk.c`): it
  opens floating, its bar drags it (and a moved keyboard still types),
  Dock and Float switch it, and its close button closes it.
  **Its load-bearing check is a ROUND TRIP THROUGH THE FILESYSTEM**: it
  types `mkdir /<name>` into the Terminal with keycap clicks and then
  asks the shell whether the directory exists, which covers the
  hit-test, `wm_client_send_key()`, the client, the line editor and the
  disk in one assertion. Ink rising in a Notepad window would not --
  a blinking caret moves those pixels, which is the trap CLAUDE.md
  names. **Shift and Ctrl get DISCRIMINATING checks** because both fail
  silently: `mkdir /Zz` distinguishes a modifier that stuck (`ZZ`) from
  one that never armed (`zz`), and the Ctrl check types `xyz`, presses
  Ctrl-C and then a real command -- if Ctrl were sent as the modifier
  BIT rather than folded to a control code, a literal `c` would land in
  the line and nothing would be created. Its control is breaking that
  fold, which reddens two checks. **Warp the cursor, never `click_at`
  alone** -- the WM accelerates an injected delta, so an open-loop move
  misses the tray item and reads as a dead control. **It tests the guest
  already on its slot** (the runner's fresh image) and boots its own from
  a `disk.img` copy only when there is none; it used to stop the
  runner's and boot `disk.img` every time. Run by `gui_regress.py`.
- **`fullscreen_test.py`** -- the fullscreen state and the display lease
  (`docs/scanout-design.md`), on virtio-gpu because the lease needs a
  cursor plane. Spawns `/tests/fsclient`, a write-only client that goes
  fullscreen at once, and asserts the reported state and rect, the
  taskbar rows in the client's colour, `gui fb --json` naming the
  lessee, frames still flowing while the compositor's present count
  stands still (the compositor is not presenting, so they can only be
  the client's own flips), the Start menu taking the lease and giving
  it back, F11 and Alt+F4. Its positive control was the overlay rule
  dropped from the policy, which left the lease standing under the
  Start menu -- one check red, the right one. After the lease returns
  it also counts WM loop iterations over `IDLE_WINDOW_S` (`gui
  latency`'s `work`) and holds them to `IDLE_RATIO` times the same count
  taken under the lease before any overlay opened: the closed menu's
  frame is still owed and no leased frame draws, so a render gate that
  kept asking for it spins -- the mutate.py control (the lease exception
  dropped from `overlay_close_frame()`) reddens that check. It also
  reads the window's `first_frame` from `gui windows --json`: fsclient
  asks for fullscreen in `on_open`, so uapp holds its first frame for
  the proposal and the window must APPEAR at the screen's size -- a
  first frame at the desc size is the flash every screensaver used to
  open with.
- **`compositor_death_test.py`** -- the compositor death path (the ring-3 GUI milestone's
  R7, 10 checks). Killing the compositor must not panic the kernel, and
  must not take the desktop with it. Two things it encodes. **The
  teardown is conditional on that compositor BEING the desktop** -- while
  the ring-0 WM is registered it still owns the screen, so a stand-in
  compositor leaving is a second consumer going away, not a desktop
  dying; the first version tore down live windows and `compositor_test`
  caught it as UI Demo going silent. And its positive control reddens
  exactly TWO of the ten, because the other eight are regression cover
  for stage 4a's role-clear path rather than tests of R7 -- read that
  before trusting a green run. In `gui_regress.py`.
- **`frame_pacing_test.py`** -- the compositor composes at most one
  frame per refresh (`userland/apps/system/toywm/wm.c`, "FRAMES ARE PACED TO THE
  REFRESH"): a Calculator window shaken by its title bar with FAST
  relative QMP motion (no sleep; `warp_cursor()` never reaches the cap)
  composes no more frames a second than the monitor's EDID refresh
  (`lsdisplay`) plus a margin; the SAME drag after `gui pacing off`
  composes more, which is the positive control built in; and after a
  quiet moment a single press moves no `paced` counter. It runs under
  QEMU, which cannot say where the beam is; the vblank pacing is checked
  on hardware by `vblank_pacing.py`. A mutant that delays every frame
  fails the first and third. In `gui_regress.py` as `pacing`.
- **`compositor_test.py`** -- the ring-3 GUI milestone stage 2's raw input path to a
  registered ring-3 compositor (`userland/tests/compclient.c`), 16
  checks. Its design point: every injected input is asserted TWICE, once
  in the compositor's log and once in UI Demo's, because "the compositor
  received the click" is equally satisfied by an implementation that
  stole the input stream outright -- and stage 2's whole shape is that
  both paths run at once. Run it after touching `userland/apps/system/toywm/wm.c`'s loop,
  `win_role.c`'s compositor registration, or the `WIN_EV_RAW_*`
  events. In `gui_regress.py`.
- **`screen_surface_test.py`** -- a ring-3 compositor's SCREEN surface
  (the ring-3 GUI milestone stage 4b): the back buffer, the clip rect, the damage box, the
  blit, the publish path and R2's verify diff, driven through
  `userland/tests/screenclient.c`. 14 checks. Run it after touching
  `userland/libs/ui/ugfx.[ch]`, `SYS_SBRK`, or `kernel/include/kernel/
  uaddr.h`. Two things it encodes. Every geometric check asserts an
  EXACT number, not "it changed" -- a damage box that covers only the
  last rect passes any did-it-change test, and a clipped blit that
  offsets its destination but not its SOURCE draws the right count of
  pixels in the right box with the wrong contents. And its first check
  is a real gate on the ring-3 HEAP: the client reports geometry only
  if sbrk handed over a full screen of back buffer.

  **And a race worth knowing about, because it is the shape of the
  tool's one intermittent.** The client prints `screenclient: step <k>`
  and that step's reply as two SEPARATE writes. `one()` used to sweep the
  log once and scan the section after the marker, so a sweep landing
  between the two reported "no reply" for a client that was working
  perfectly -- measured 2 runs in 4 once boot got slightly slower, with
  every other check in the tool passing. It polls now (6 in 6). A marker
  is not a guarantee that what follows it has arrived. In
  `gui_regress.py`.
- **`desktop_entries_test.py`** -- the `.desktop` entry system: the
  `ShowIn=` key and live reload, 13 checks. Its reusable lesson is in the
  ShowIn checks: they assert an entry is **LOADED but filtered** (`gui
  apps` versus `gui menu`), never just "absent from the menu" -- the
  first version asserted only absence and passed with the filter
  disabled outright, because `write` truncates and the check was racing
  the transient invalid file. In `gui_regress.py`.
- **`settings_gallery_test.py`** -- System Settings' cursor theme
  gallery (5 checks), from the app's own `settings: gallery` lines: a
  card per theme with a painter, the Amber tile carrying amber ink while
  Classic's carries none (the control), a click STAGING without writing,
  the arrow keys moving it like a radio group, and Apply writing it to
  `/etc/saunaos.conf` (read with `cat`) with the compositor's own `cursor:
  theme "amber"` line. **The card rects are CONTENT-relative**; the tool
  adds the window's content origin. Then the FONT galleries (3 checks): a
  card per face named by family, Liberation Sans's and DejaVu's Interface
  tiles inked AND different (one face painted everywhere fails), and the
  proportional face's Monospace card carrying the warning ink while
  DejaVu's carries none. **To scroll, warp the REAL pointer first** -- an
  injected `gui move` lasts one compositor iteration and the wheel goes
  where the pointer is; the card lines are re-logged after a scroll, so
  read the fresh ones. Then the WINDOW galleries and the Screen monitor
  (6 checks): each drag setting's Outline card carries the dashed white
  outline and its Window card none, a click stages `outline` (and Reset
  takes it back, or closing asks to discard), the shadows On card
  darkens the desktop under its windows and Off not at all, See-through
  None's bodies are solid and All's are not, and Display > Screen's
  monitor is captioned with the mode on screen and holds the desktop in
  miniature. **A card's miniature is found by its DESKTOP colour's
  bounding box**, so the card's white margin is never counted. Then
  round 3 (8 checks): Placement's cards show the wallpaper (not the plain
  desktop colour) and, the built-in pictures being the screen's shape,
  the same in both, with the line saying so; Icon size's folder grows
  Small < Medium < Large; Menu spacing shows more rows the denser it is
  (rows counted only on the menu's light ground); Corners, after ticking
  Show advanced settings (**scroll to it first: it ends the page**),
  keeps more title colour in its corners the squarer the preset;
  Transparency's sample taskbar is solid with effects off and lets the
  wallpaper through with them on; and the Date dropdown's Region default
  matches the preview line's date. **A page too short to scroll
  re-reports no cards**: the opening report is used. In
  `gui_regress.py`.
- **`settings_test.py`** -- the ring-3 System Settings app and, through
  it, the settings registry. Run it after touching
  `kernel/lib/setting.c`, `SYS_SETTING`/`SYS_SYSINFO`, or
  `uui_sidebar`/`uui_setting_row`/`uui_switch`/`uui_segmented`/
  `uui_radio_list`/`uui_spinbox`/`uui_dialog`/`uui_layout`'s `hidden`
  handling. ~100 checks, including Reset, the Apply / Discard / Cancel
  question on leaving a changed page, and the search box. **A section
  that stages a change Applies or Resets it before navigating**, or the
  next sidebar click opens that question instead of a page. (It was `cpanel_test.py` until the app was renamed on
  2026-08-19 -- Control Panel is Windows' name, and this shows exactly
  the SETTINGS registry.) Two things it encodes. A change is verified by reading the
  BYTES ON DISK through the console's own `sh cat`, not by believing the
  app -- and note `/bin/config get` does NOT work for this, because a
  spawned program's stdout goes to its parent's pipe rather than the
  kernel log (only stderr is readable from outside). And its
  hidden-page check measures the SAME RECT in both states: the first
  version compared the widget's band against the WHOLE page's ink, a
  baseline so much larger that a positive control (making the layout
  ignore `hidden` again) reddened nothing at all. Recorded numbers:
  55% of the shown ink survives when hidden works, 98% when it does not.

  It also drives the TIMEZONE DROPDOWN, which is where three things are
  covered that nothing else reaches: a choice's display name (the app
  logs `settings: choice <n> <name> raw <token> shown <display>`, since
  a screendump cannot tell a missing display name from a value that
  reads like one), HOVER as pixels, and keyboard type-ahead. Three
  fixture rules that cost a run each. The timezone is set BEFORE the app
  starts, because the app caches a setting's value when it builds a
  page. The page is reached by navigating AWAY and BACK, because the
  layout log is deduped per frame and re-opening the page already shown
  logs nothing at all. And the row hovered must not be the SELECTED row
  -- selection correctly outranks hover, so hovering the current city
  measures nothing and reads as a dead hover.

  **NOTHING IS AIMED AT WHERE IT WOULD BE UNSCROLLED.** Three rules,
  each of which cost a check that measured nothing. A SIDEBAR ROW is
  clicked through `open_row()`, which scrolls it into view and derives
  its current position from the app's reported offset: the row dump is
  taken once, at the top, so row 30 of 31 is reported at a y below the
  window -- and a click there lands on the TASKBAR, whose button for
  this window MINIMIZES it, after which the app draws nothing, reports
  nothing, and every later check reads the state it had before. A
  CONTROL is scrolled fully inside the PAGE VIEWPORT by `reveal()`,
  which is the page rect and not the window: a control whose lower half
  hangs past the scroll view is drawn clipped and a press there is
  clipped away too, which is why the mouse-speed spinbox stepped up and
  never down. And a PAGE is confirmed open by its own `settings: page`
  report before its controls or its pixels are read, since an empty
  control list means either a page with no controls or a page that never
  opened. `click()` refuses a point outside the content area outright,
  with a check naming the coordinate, so the whole class fails loudly.

  It also drives the SCREENSAVER PAGE, whose option rows are not
  registry settings at all: they are synthesised from the chosen saver's
  descriptor (`userland/libs/base/usaver.h`) and written to a file of its own,
  so the checks name the SAVER the rows claim to belong to rather than
  counting them -- "there are controls" is satisfied by the two registry
  settings alone. Two things it encodes. The saver is chosen BY NAME
  through a scan, because the popup is a `uui_listbox` with no
  `describe` and a row pitch worked out once from a font size is the
  constant this repo has re-measured three times; and the rows are read
  from the window `pick_saver()` already drained, because the layout
  report is deduped per frame and the rebuilt page is described exactly
  once. Asking for "any saver but this one" picked `blank`, which ships
  with no descriptor and correctly shows nothing -- the swap check now
  names a saver that has options.
- **`screenshot_test.py`** -- screen capture end to end: `/bin/screenshot`,
  `--pointer`, `-w`, and the Screenshot OVERLAY through PrtSc -- Screen
  mode equal to the screen as it was (the overlay, its pill and its
  taskbar button absent, the focus where it was), a dragged and then
  handle-resized region cropped to the pixel, the Window target
  following the pointer, the crosshair over open ground in Region (the
  pill as its control), the compositor's card with Open / Copy / Folder
  / Save as and that Save as end to end (the chooser over a preview, a
  PNG copy beside the kept original), Esc saving nothing, the copy toggle said on the card, and no
  launch left a zombie. The cursor box and the taskbar clock (it shows
  seconds) are left out of every comparison.
- **`screenshot_options_test.py`** -- what Screenshot's options DO to the
  next capture: a folder, a `<mode>-<n>` template and PNG name the file
  (`ls`, `imginfo`), card off and flash on reach the compositor (its
  `wm: notice` line), Shift+PrtSc and Alt+PrtSc save with no overlay
  (Alt's at the focused window's size, named by `<app>`), the pill's
  popover keeps a toggle, and the Options window moves Print Screen in
  `/etc/shortcuts.conf`. **The option files are PUT from the host
  (`vm.py put`), never echoed**: a template's `<mode>` is a redirection
  to tosh, so `write_lines()` stopped at that line and the app looked
  as if it ignored its options. Its positive control drops the notice's
  answered last piece and the key writer: the flash and both key checks
  redden.

  **The assertion that matters is a ROUND TRIP, not "a file appeared".**
  A capture that wrote a well-formed image of the WRONG THING passes
  every did-it-produce-a-file check there is, and both ways it could be
  wrong are real: the compositor's back buffer can be a frame old, and
  its alpha byte is zero, which every alpha-aware consumer reads as
  fully transparent (that one shipped, as a black preview). So the file
  is pulled OFF the guest's TFS3 volume with `tfs3_writer.py read` and
  compared pixel by pixel against a QMP screendump, by **Pillow** -- a
  decoder sharing no code with ours.

  **The cursor box is its own control.** Outside it the two images must
  match exactly; INSIDE it they must differ, or the comparison is
  passing because the capture is a copy of the screendump rather than
  because it is correct. `--pointer` is then checked as a COUNT OF
  DISTINCT COLOURS in that box (a cursor is an antialiased arrow: three
  wallpaper shades become twenty-six), not as "the files differ", which
  two captures of a moving desktop would satisfy with no cursor in
  either.

  It asks QEMU which image the guest has (`info block`) rather than
  assuming `disk.img`, because `gui_regress.py` boots a per-tool copy
  and a hardcoded path would read a file the guest never wrote.
- **`screensaver_test.py`** -- the idle clock in the compositor, the
  two settings, and the savers it spawns. A saver is a PROCESS, so
  `gui idle` reports its pid and the checks ask for that rather than
  guessing from pixels; the window it owns has to reach the screen's
  size before anything is asserted about it, because a pid is not a
  window and a window is not yet a fullscreen one. Both input paths are
  driven separately -- a key and a pointer move arrive at different
  places in the frame loop, and a clock reset wired to only one would
  look correct from whichever the test happened to use. The blank saver
  is checked as PIXELS, since "a window opened" is not "the screen is
  dark". **The lever is `gui idle start`**, which skips the WAIT and
  nothing else: the shortest timeout a person can configure is one
  minute, and it still refuses while the timeout is zero, so "off means
  off" stays testable. It puts both settings back, including after a
  failed check.

  **And it checks that a saver's OPTIONS reach its pixels**, which is
  the half neither `/tests/usaver_test` (does the file parse) nor
  `settings_test.py` (does the app write it) can see: a descriptor wired
  up and a `#define` still in place look identical from both. The tint
  is asserted as a COMPARISON BETWEEN CHANNELS -- amber is redder than
  it is blue, ice the other way -- because an absolute value would need
  a threshold picked from one machine, and a saver ignoring the option
  draws white, where the two are equal. The star count is asserted at
  the two ends of its declared range, twenty times apart, which no
  frame-to-frame variation in a moving field can cover. The conf file is
  written with `tosh -c` and its redirection, UNQUOTED: the kernel
  shell's `spawn` passes a quoted word through with its quotes, so tosh
  re-lexes it as one word and runs a command called "echo colour=amber".

  **And that the Desktop cube wraps the desktop it captured.** The
  oracle is the dimmed BACKDROP, because it is the capture and it stays
  on screen: with the solid held in the middle (speed and spin 0), the
  taskbar strip of a saver frame must equal a screenshot taken just
  before, box-filtered, scaled back up and dimmed in `cube.c`'s own
  integer arithmetic (`cube_backdrop()`) -- so anything the capture had
  that the desktop did not, the saver's own taskbar button included,
  shows there. Unlit, the faces must carry the desktop's mean colour,
  which tells them from the grey fallback checker only while the
  wallpaper is coloured, so that precondition is a check of its own. The
  intro's three phases are asserted from the saver's log, in order. Its
  positive control (the capture dropped, `g_have_shot = 0 &&`) reddens
  four checks.
  **The cube's damage** is checked with its own control. The same fast
  run with `damage=off` must leave the bottom band as the backdrop and
  log no hit. With it on, hits are logged, the band fills with the pile,
  and a worn cube shatters and refolds. A band that differs for another
  reason would pass the second check without the first.
- **`thumbcache_test.py`** -- the File Manager's thumbnail RATE, and the
  disk cache under it (7 checks). **THE ASSERTION IS A RATE, NOT A
  DURATION**: decoding used to be two per 500 ms tick, so four a second
  was that design's CEILING by construction, and a measured rate above
  it is something it could not have produced however fast the host is --
  where a wall-clock bound would be a bet on the emulator. Measured at
  24/s on TCG; the bar is 8. It reads the app's own drain report
  (`files: N thumbnail(s) in M ms, C from the cache`) off the WIRE
  through `DebugConsole.logs()`, **never `sh dmesg`** -- the kernel ring
  holds a few hundred lines and any tool that has turned the layout log
  on fills it in about a second, so the first version of this check
  asked dmesg and found NOTHING with the app working perfectly. It is
  its own tool rather than a section in `filemanager_test.py` because it
  needs to START A FRESH APP TWICE, once cold and once against a
  populated cache: a second process is what proves the DISK was read
  rather than an in-memory table. Two fixture traps it paid for:
  `/etc/files.conf` remembers the view mode, so it removes that file to
  get the icons view rather than inheriting whatever ran last; and
  SaunaOS's `touch` does NOT move an existing file's mtime, so the
  staleness check copies a different icon over one instead -- the first
  version went red against working code because the input never reached
  the branch. **And a second must pass between writing the fixture and
  the first look**: freshness is STRICTLY newer at one-second resolution
  (`uthumb.c`'s `cache_fresh()`), so an entry written in its source's
  second re-decodes once. The old "not older" rule tied on a same-second
  rewrite and served it stale, which was this tool's intermittent red.
  In `gui_regress.py`.
- **`bad_present_test.py`** -- a refused replacement of the buffer ON
  SCREEN keeps the last good frame and the compositor alive. Drives
  `/tests/badpresent`, a raw-protocol client (no uapp, which never
  re-presents its front buffer): it presents buffer 0 at generation 1,
  then the same buffer at generation 2 with a size its object cannot
  hold. Asserts toywm's PID is unchanged -- init restarts a crashed
  desktop, so "it answers" alone passes on the broken build -- and the
  window still shows generation 1. Control: `mutate.py` putting
  `unmap_buf()` back before the open restarts toywm. In `gui_regress.py`.
- **`focus_state_test.py`** -- the focus a CLIENT is told matches
  the focus the WM routes keys by, and minimize/restore keeps a
  maximized window maximized. Focus is read from the client's side --
  uapp logs `uapp: pid N focus F` on every change -- so a check names
  the pid that must, and must not, hear an event: minimizing the focused
  window, and closing the front window above a minimized one. State
  goes through the taskbar and the window menu (maximize, minimize,
  restore, then Restore to the original rect). Its control is
  `mutate.py` restoring to WIN_NORMAL and disabling `wm_focus_sync()`:
  6 checks go red. In `gui_regress.py`.
- **`above_test.py`** -- Always on Top. Notepad, then Calculator over
  it, pinned with Ctrl+Super+T; a click on Notepad then focuses it while
  Calculator stays STACKED above (`gui windows`' `stack`), covers the
  overlap pixel for pixel as it did while focused, and takes the probe
  there. The taskbar button's pin is amber where `gui taskbar` puts it,
  the window menu's row is ticked, and choosing it unpins -- after which
  the overlap changes, the in-test control. `--keep DIR` keeps the
  screenshots. Its control is `mutate.py` drawing in activation order
  (`draw_one_window(k, ...)`): the pixel check goes red. The w16 side
  (HWND_TOPMOST, a program's control-menu item) is in `w16_gui_test.py`'s
  dialog.exe checks. In `gui_regress.py`.
- **`notepad_options_test.py`** -- Notepad Options, asserted by what
  the options DO: OK writes `/etc/notepad.conf` (only the keys that
  moved); the status bar going off gives the text its height; Tab types
  spaces to an 8-column stop and Save trims the trailing ones, read back
  with `cat`; the saved file is ONE entry in `/var/lib/notepad/recent`;
  closing and reopening Notepad brings the tab back; Preview Always +
  Below puts the Markdown pane under the text. Opens Options by keys
  (F10, Right, `o` -- the unique letter in Edit) and clicks the rows the
  window's own layout log names (`options: layout <name>`). Control:
  `mutate.py` dropping spaces-for-Tab and the reopen turns exactly
  those two red. In `gui_regress.py`.
- **`window_identity_test.py`** -- a window held across frames stays
  the SAME window when `windows[]` is renumbered under it: the window
  menu across a close below it (its Close must close its own window) and
  across its own window's close (dismissed), a held title-bar drag
  across a close below it (`gui state`'s `dragging` follows, and the
  release moves that window only), and the shortcut inhibitor across a
  RAISE of another window into the focused slot (Super+E must fire).
  Windows are told apart by ICON NAME -- a client's `app` is empty and
  Notepad's title is its document's. The drag needs a REAL held button
  (`QMPSession.mouse_down` after `warp_cursor`); `gui drag` cannot hold
  one across a close. Red on the build before the fix (6 of 11). In
  `gui_regress.py`.
- **`shortcut_test.py`** -- global keyboard shortcuts: the default
  bindings, the Super gesture, and rebinding one through System Settings
  (15 checks). It reaches the Shortcuts page by TYPING "shortcuts" into
  the Settings search box, which needs no pixel; a fixed click after a
  blind scroll was what failed two of its checks for weeks. **It asserts
  on what the compositor LAUNCHED**, from its
  own `wm: shortcut -> <command>` line, not on a window appearing -- a
  window is also what a Start-menu click gives you, and this is about the
  key path. Four traps it pins, each of which has already bitten:
  **the command paths drift from `data/wm/applications/`** (two of four
  were wrong first time, and the symptom was `pid -1` in a log nobody
  read, so the first check lists each one on the disk); **Super held over
  another key must not open the Start menu**, or every Super shortcut
  opens it on the way out; **capture needs the compositor to stand down**,
  checked by recording a combination that IS already bound and asserting
  no file manager appeared; and **the rebinding is end to end** -- Apply,
  then press the NEW key -- because `usetting_get()` returns 1 for
  success and reading it as a syscall result made every binding fall
  through to its fallback, so a rebinding was stored, reloaded and
  ignored while every cheaper check passed. It leaves
  `/etc/shortcuts.conf` removed, so the machine is as it was found. In
  `gui_regress.py`.
- **`help_test.py`** -- the Help app (`userland/apps/apps/help/help.c` over
  `uui_markdown`'s links): opens on the Getting started overview; a code
  span naming a page is REPORTED as a link and DRAWN in the accent (the
  page's title is the control that must have none); a click follows it,
  Alt+Left/Alt+Right walk history, the toolbar's Home returns. Search is
  checked as FULL TEXT: the query is a word in ping's body but not its
  name or title, verified against the host's copy of the page, and the
  page Enter opens must contain it. A category heading opens a generated
  page of links, and Right/Left expand and collapse it -- the tree is
  lazy, so a dead toggle leaves the row count unchanged. With links
  turned off it goes red on seven checks. In `gui_regress.py`.
- **`sndformat_test.py`** -- a sound card's Format panel
  (`ui/uui_sndformat.h`) in System Settings (Sound > Output) and Device
  Manager, on QEMU's HDA (`gui_regress.py` boots it with `--audio hda`).
  The controls by their layout-log names, DRAWN (ink in their rects),
  and what they SAVE read back from `/etc/sound-cards.conf` through the
  shell. A row that went away is seen as no ink where it was; Device
  Manager's own `format 0|1` on its `selected` line says whether the
  panel is up. Then the Test card (`ui/uui_sndtest.h`): Left starts a
  test, the left meter lights in at least one of a dozen frames (a chime
  is under half of each second) and the right one never does, Left
  again stops it. **`--wav PATH`** -- the guest's `vm.py --audio-wav` --
  also reads the recording past the frames written before the click
  (QEMU fills the header only on exit, so it is measured off the file
  length) and wants the chime on the left channel and silence on the
  right: the oracle the meters cannot fake. `gui_regress.py` passes no
  recording.
- **`netadapter_test.py`** -- a network card's ADAPTER SETTINGS end to
  end on QEMU's e1000: `netctl link` refuses a setting the driver lacks
  (EEE) and changes nothing, applies and saves interrupt moderation, and
  `defaults` takes the key back out; then System Settings opened on
  Network > Adapters (`settings adapters`) shows the one row the e1000
  offers and NOT the other three, choosing High there applies and saves
  it, and Restore defaults undoes it. **Judged through the kernel log's
  `net: <card>: ... moderation N` line (what the core accepted) and the
  bytes of `/etc/net.conf`, never the app's own report.** The e1000 is
  the useful fixture precisely because it offers one setting of four: a
  panel that drew every row for every card fails here (the positive
  control: showing the EEE row unconditionally turns it red). Device
  Manager's half of the same panel is checked in `devmgr_test.py`: only
  the offered row, and a notch of scroll CUTS the panel (ui/uui_clip.h)
  rather than hiding it -- red with the old whole-slot rule.
- **`devmgr_test.py`** -- the Device Manager (`userland/apps/system/devmgr/devmgr.c`
  over `base/udevice.c`): the tree is DRAWN with its icons, "By connection"
  keeps the selection, and Disable/Enable on the network card round-trip
  through the confirm dialog -- and a window close while that dialog asks
  is refused (uapp's default holds a close while a question is open in
  the window). **Whether a device is bound is read from
  `devctl list`, never from the app's own report** -- the app says what it
  believes. The icon check counts saturated pixels AND the columns they
  span, since a saturated selection wash would pass a bare count. It
  checks that a desktop context menu opened over the tree takes the
  pointer (no row lights beside it; red on the WM before 2026-09-29), with
  a hover beside the menu as the control that the measurement can see a
  highlight at all. It closes a Device Manager already open first: the
  app is single-instance,
  so a leftover window takes the spawn with the layout log still off. It
  never ticks "Keep disabled", so it leaves `/etc/devices.conf` as found.
  It also checks the pane as a page of sections (the app's `pane` report)
  against `devctl show`, the disable/enable cycle in `devctl events`
  (Taken, Given back, Driven, in order), the tree filter (Ctrl+F, the
  card's driver name: two rows; Esc: all back), Copy details, Unload and
  Load of the module-driven NIC through the pane's own buttons (`devctl`
  the judge), and that a disk and a monitor are listed. **A slot's
  controls are reported only while the slot is wholly in view**, so it
  scrolls the pane to them (`reveal()`) as a person would. The filter and
  event checks fired on a never-hiding filter and on a claim that
  recorded no event. In `gui_regress.py`.
- **`sysupdate_channels_test.py`** -- System Update's CHANNELS, against
  `update_server.py`'s handler serving three in-process (Release a
  fixture one version OLDER, with one file different): an address naming
  `/stable` is split into the server and its channel (read from
  `/etc/update.conf`); the check lands on Main of three; Change
  channel... opens the list -- Dev included on a -dev build -- and its
  rows are DRAWN (the first row's rect changed against a screenshot
  before it, the title bar the control); choosing Release asks first,
  Wait leaves the file alone with the view waiting, and Go back installs
  it, `sum` reading the older file. Each row's build must carry its TIME
  and a newer/this build/older badge (Release older). CHANGE SERVER... is
  a dialog: Test finds the three channels without touching
  `/etc/update.conf`, says so for an address nothing answers, Use refuses
  `ftp://` with the dialog left up (PIXELS: the result line against the
  same box with no dialog, since the layout log repeats nothing
  unchanged), and a recent row plus Use saves and checks. Puts the guest
  back on main after. In `gui_regress.py`.
- **`sysupdate_test.py`** -- the System Update window
  (`userland/apps/system/sysupdate/sysupdate.c`) against `update_server.py`'s handler
  in-process on an ephemeral port, THROTTLED so an install has a middle:
  it checks by itself and counts the two files damaged beforehand; the
  file list is drawn; mid-install the `uui_progress` bar is PART-filled
  (accent pixels across its reported rect, so a bar that jumps from
  empty to full fails) and the fetched row is tinted; it ends on
  "installed", `sum` agrees with the manifest, and the bar is full.
  **What's new**: the fixture serves notes with the guest's own build in
  the middle; the app must count the two notes above it (`sysupdate:
  notes`), open on that tab and DRAW them (more ink rows than the
  one-line placeholder; `mutate.py` forcing the placeholder reddens
  exactly the two drawing checks), then reach Files by the slot the
  strip reports, and still show the notes after the install. The
  damaged files are neither a library (that waits for a restart) nor
  anything running (a desktop restarted mid-test would find it
  truncated). In `gui_regress.py`.
- **`taskmgr_test.py`** -- the ring-3 Task Manager's three pages: the
  Processes table in its Grouped, Tree and List views (the ORDER each
  gives, read from the app's `taskmgr: order` line), sorting, folding a
  parent, the filter, Stop/Continue checked through `ps`, Force Quit's
  arm-then-commit, the Performance device list and Ethernet's connection
  log, and stopping and starting `ntpd` checked through `service list`.
  **Two checks count LOG LINES PER CLICK** -- one SIGSTOP, one init
  request -- because a button names itself on press, motion and release
  and the first build acted on all of them (four signals, four requests,
  three of which timed out); removing the commit-only test reddens both.
  An ARMED Force Quit must be dropped when folding moves the selection
  to the parent (checked on the app's `disarmed` line, never by clicking
  Confirm, so a broken build cannot kill the desktop); "Go to service"
  must land on the service; and a remembered 600 px size must leave
  Name its room and Force Quit inside the window.
  **Memory that adds up**: `meminfo`'s four kinds must sum to its
  "used" (within 2%; true by construction, so it guards the printing,
  not the accounting -- the `mm` KTEST on private/shared does that);
  Processes must list the Kernel, Graphics and Shared memory rows, a
  kind row must select and Force Quit must do nothing to it; the PID
  sort must keep the kind rows LAST in both directions; and the
  Performance page's composition bar must fill edge to edge in
  `meminfo`'s proportions, read again just before (each part within
  4 px), from the bar's own `memcomp.seg` describe lines.
  Its resize check asserts the
  table grew by ROUGHLY WHAT THE WINDOW GREW BY, not merely that it
  changed -- by as much as the screen leaves room for, since the window
  opens wider than it did -- the bug it was written after grew the width correctly and
  the height by 16 px against 300, so "it changed" was satisfied. On its
  first run it found a pre-existing bug in `uui_listbox` (see the
  widget-`hit` trap in the widget section above).

  **IT IS ALSO THE ONLY PLACE IN THE SUITE THAT USES THE KEYBOARD ON A
  TABLE, and it was not, for most of its life.** Every check in it drove
  by MOUSE -- rows, headers, buttons -- so the app's entire key handler
  could be absent and all of them passed, which is exactly what happened:
  Task Manager declared neither `uapp_desc.focus` nor `on_key`, so the
  table's arrows, Home/End, paging and type-ahead had never worked there.
  Two checks cover it now, and the second is the load-bearing one: an
  arrow must reach the table at all, and a typed letter must select by
  NAME rather than by the PID column. **Ask which input device a tool's
  checks use** -- a suite that only clicks cannot see a dead keyboard.
- **`single_instance_test.py`** -- one copy of an app, and relaunching
  it raising the copy that exists (`WIN_REQ_ACTIVATE`,
  `UAPP_SINGLE_INSTANCE`; 9 checks). Run it after touching TWP's create
  path, `wm_client.c`'s window list or `uapp_run()`'s startup. Two of
  its checks are worth copying: the multi-instance CONTROL (UI Demo
  declares no app id, so two windows is the right answer there, and an
  over-eager match reddens exactly that check), and identifying the
  raised window by **`client_pid`, not by title** -- with the raise
  disabled a brand-new window is frontmost too, so the title-only
  version of that check stayed green through the positive control.
- **`resize_edges_test.py`** -- a window resizes from all EIGHT edges
  and corners, and each one shows the right cursor. 8 cases, each
  naming the dimensions that must CHANGE *and* the ones that must be
  ANCHORED -- the second half is what has teeth, because pinning the
  wrong corner moves a window without resizing it and looks almost
  right in a screenshot. **IT RESETS THE WINDOW BEFORE EVERY CASE**,
  after the first version dragged it into the screen corner and then
  reported the WM as broken when the clamps correctly refused to grow
  it further; the reset is asserted, so a fixture failure says so
  rather than masquerading as a bug. It also takes the TOPMOST window
  of its title rather than `DebugConsole.window()`'s lowest, which is
  what a re-run against a guest that still has one hands you -- reading
  one window's geometry while clicking another's reddens the whole
  board with nothing wrong at all. Two checks beyond the drags: the
  TITLE BAR still drags end to end while the band just past its ends
  resizes (the guard for a corner zone growing back into the bar, which
  is what a "resize cursor far from the corner" report means), and the
  outside band ENDS -- asserted from both sides, since "nothing resizes
  out here" also passes when the band is broken outright.
  `--positive-control` prints every before/after geometry.
- **`resize_stride_test.py`** -- a resized window is composited at the
  size it was DRAWN at. 4 checks. **THE ASSERTION IS TWO NUMBERS, NOT
  PIXELS**: a shear is a stride disagreement, and reading the numbers
  beats hunting a diagonal in a PNG. It originally compared the kernel's
  record of each buffer against the compositor's; stage 6b deleted the
  kernel's record, so it now checks that the compositor's content size
  tracks every one-pixel step -- which it can only know from the frame
  -- with the buffer's GENERATION moving as the control that the buffer
  was really replaced. **It steps by ONE pixel and that is
  load-bearing**: a buffer's length is page-rounded, so a larger step
  takes a different path through the client's buffer handling.
  `--control` prints the edit that reddens it.
- **`uapp_test.py`** -- the TWP resize handshake and focus events, via
  `winclient` (which contains no resize code -- it sets
  `.flags = UAPP_RESIZABLE` and nothing else, so what is under test is
  Toykit's and TWS's). Also the DEFAULT TICK: `/tests/tickclient` has
  an `on_tick` and no `tick_ms`, and must count about 30 ticks a second
  -- the old polling loop gave about 15,000. Its focus check is a ROUND TRIP:
  capture a Terminal's content focused, take focus away and require it
  to CHANGE, give focus back and require it to match the first capture
  EXACTLY -- "it changed" alone is satisfied by almost anything.
- **`doom_data_test.py`** -- DOOM's front end (`userland/apps/apps/doom/`): with
  no IWAD the game-data card, a download, the launcher page, and F1's key
  sheet over a level. **The download never leaves the machine**: the
  guest's `/etc/doom.conf` gets `mirror=` aimed at a server the tool runs
  on loopback, serving `data/doom/doom1.wad` and (when `fetch_wad.py
  --freedoom` has run) Freedoom's zip behind a 302 to a 1100-character
  URL, GitHub's shape. The licence step is checked on the way: id's is
agreed to once (a second Download does not ask), Freedoom's is a notice,
and both are found in `/usr/share/licenses` afterwards. **The control
runs first**: a `doom1.wad` with one
  byte flipped must be refused on its SHA-256 and leave nothing in
  `/usr/share/doom`. Then the real file, its hash read back with `sum`
  outside the app, the sheet by pixel against the frame before it, F1 on
  the sheet reaching DOOM's own help (`doom: menu open`), and the pause
  under it (`key sheet closed, the game was paused`). Without the host's
  WAD only the card and the control run, and it says so. In
  `gui_regress.py`.
- **`keyup_test.py`** -- key RELEASES reaching a ring-3 client, across
  all five layers that carry one: the driver's key event stream, the
  kernel's raw-event push, the compositor's raw-input queue, the WM's
  routing and Toykit's `on_key_up`, via `winclient`, which keeps a
  model of what is currently HELD -- nothing else in the tree does,
  because every real app acts on the press. It also checks that Ctrl
  and Alt reach a window as BITS (Alt+a is one 'a', Ctrl+1 is '1') and
  the physical-key stream (`on_phys_key`: positions, both edges, no
  repeat while held).

  **Its load-bearing check is the one that holds a key down.**
  `QMPSession.send_key()` wraps QMP's `send-key`, which presses and
  releases in one go, so a test built on it cannot tell a working
  release path from a guest that invented the release itself.
  `key_down()`/`key_up()` (added to `qmp_test.py` for this) carry the
  edges separately, so "the client still believes W is down a second
  later, and reports it up only when told" becomes assertable. It also
  checks that a MODIFIER reaches a client at all -- Ctrl produces no
  character, so before this it reached one by no path whatsoever -- and
  that a release carries what the PRESS produced rather than what the
  key would produce now (press W, press Shift, release W must report
  `'w'`).

  **One check passes vacuously under the obvious positive control**, and
  the docstring says so: 2b asserts that no release arrives while a key
  is down, which a build delivering no releases at all satisfies
  perfectly. It is worth having -- it catches a release synthesised from
  a timeout -- but only paired with 2c.
- **`mouse_buttons_test.py`** -- that all five pointer buttons reach a
  ring-3 client, and that a SHORT press reaches it too. Run by
  `gui_regress.py`, on a guest with a USB mouse (`--usb xhci+mouse`).

  **The check that matters is the TAP**, `QMPSession.tap()`: a press and
  a release with no wait between them. `click()` sleeps 100ms, which
  gives a level-sampling input path every chance to look and pass --
  and the input path here sampled a level at three stages, so a tap
  whose two edges landed between two looks cancelled out and the click
  never happened (docs/decisions.md, "A pointer button is an edge, not a
  level"). Check 2 -- the same button HELD -- is what passes on the
  broken build, and the pair is what makes the difference legible.

  **Two harness traps it paid for.** A down and an up in ONE
  `input-send-event` send NOTHING: QEMU's PS/2 mouse accumulates button
  state and only queues a packet at the batch's sync, so the two net out
  -- caught because the primary-button tap went missing on a build where
  clicking plainly worked. And the two commands are PIPELINED, both
  written before either reply is read, because a round trip between them
  is long enough for the guest to look.

  **ITS POSITIVE CONTROL DOES NOT REDDEN, AND THAT IS MEASURED RATHER
  THAN AN OVERSIGHT.** With the fix reverted this tool passed all ten
  checks on a PS/2 guest and again on a USB one: the PS/2 path
  interrupts per packet and the idle loop that polls wakes on those
  interrupts, and a USB guest is paced by the endpoint's polling
  interval, so under emulation something always looks in between. The
  deterministic control for the lost edge is the KTEST `input`/"a press
  and its release both survive one polling pass"; this tool's job is the
  end-to-end claim -- five buttons by name, and the thumb pair
  navigating the File Manager -- not the race.
- **`hover_test.py`** -- that a hover change REPAINTS rather than only
  recording damage. Run by `gui_regress.py`.

  **It counts frames, not pixels, and that is the whole point.** The
  obvious test -- warp the cursor onto a menu row and photograph it --
  cannot see this bug: the tray clock forces a repaint once a second, so
  a settled screenshot finds the highlight correctly placed, having
  arrived up to a second late. That IS the symptom, and settling
  launders it away. `gui state`'s `scene repaints` counter answers the
  real question.

  It asserts BOTH directions: eight hover changes must repaint, and
  eight moves with no overlay open must not -- without the second half a
  WM that repainted on every move would pass.

  Its moves are INJECTED rather than warped, against CLAUDE.md's usual
  advice, because it tests whether a change caused a FRAME rather than
  whether a state survives a capture -- and an injected move is one
  console round trip against a warp's several hundred milliseconds,
  which is what keeps the tray clock out of the measurement. The first
  version's "moving within one row must not repaint" check was wrong for
  the matching reason: an injected position snaps back to the real
  pointer next iteration, so each move changed the hover twice.
- **`browser_test.py`** -- the Web Browser app in `gui_regress.py`: it
  opens on its start page and reports it loaded; the page's header band
  is DRAWN in its own blue with white text on it, against a control rect
  of bare body grey; a click on its Text card loads that test page, not
  an error page (the start page is the engine's own file and the test
  pages are not); focused, the address bar's 2 px border stays whole
  across the field, with a white control row inside it; a typed address
  is followed and names the tab; Back
  returns; a click on `/tests/web/links.html`'s block link (a known band,
  so no guessed coordinates) follows it; the wheel scrolls; Ctrl+T and
  Ctrl+W open and close a tab. **The window is found by its title's
  SUFFIX after the page loads** -- "Web Browser" becomes "Start - Web
  Browser" as soon as the page names itself, and a fast load renamed it
  before `spawn()`'s exact match looked. Shown red by swapping red and
  blue in the plotters' colour conversion. `/tests/web/svg.html` reads
  SVG and opacity in pixels: a ring's inner contour is a HOLE, two
  same-way squares fill their overlap (nonzero, not even-odd), a curved
  stroke is drawn, a checkbox at opacity 0 leaves its green label
  green while one at full opacity draws and a plain box at opacity 0
  still does; `/tests/web/overflow.html`'s zero-height `overflow: hidden
  auto` menu of 300 items leaves the page no taller than the view -- shown
  red by the libcss patch never reading the second keyword; and the
  opacity check by the patch's test changed to `opacity == -1`
  and the path fill switched to even-odd, each turning only its own
  check red.

  **Menu, zoom and find**: the ☰ menu reports its rows (the menu's own
  describe op) and its Zoom in is clicked by that rect: the extent grows
  at 110% and Ctrl+0 returns the SAME extent. Ctrl+F "line" on
  `text.html` is 1 of 8, the page's pixels change under the highlights,
  Enter steps, Shift+Enter wraps to 8 of 8, and Esc (then Home) leaves
  the page pixel-identical to before the search. Shown red by counting
  no matches and by dropping the scale call: exactly the three count
  checks and the zoom check failed.

  **Out of memory**: `/tests/web/oom.html` doubles a string it keeps
  until the engine's `RLIMIT_DATA`; the page must END with the view's
  `nomem` set, the kernel's `reached its data limit` line in `dmesg`, no
  `no frame for user page` line, and the next page loading in a new
  process. Shown red by dropping webcontent's `setrlimit()`: four checks
  failed -- without the limit the page stays up at its heap's ceiling.

  **The sandbox** (over the network): on `fileref.html`, served over
  http, the page's engine shows its four promises in `ps --sandbox`; the
  page's own image over http is drawn (the control) while the same file
  as a `file:` URL is not, and the app logged refusing it; and `dmesg`
  holds no refusal of an `/etc` path for that engine -- the check that
  found the locale and settings files a ring-3 sandbox must unveil.

  **The address field and the lock's card**: the start page's chip says
  File (drawn ink in the reported rect); a zoomed page shows its badge
  and a click on it resets to 100%; an http page's chip is red (Not
  secure) and its card counts the cookie the server set; an https page
  is secure and its card has a TLS version and a chain -- still after a
  second page over a POOLED connection, the case with no handshake. Clear
  is checked last, in a reopened window after the jar has been read: the
  card then counts 0 and the server is no longer SENT the cookie, read
  from a new address so the answer is not the cache's.

  **Popups and the pointer**: a click on the page closes the Downloads
  flyout, opening the menu closes the flyout, a click outside closes the
  menu; the compositor's RESOLVED pointer shape (`cursor_shape()`, in
  `WM_CURSOR_*` numbering, where TEXT is 4) is the I-beam over a word
  and the arrow over blank page beside it. Shown red by restoring the
  router's old gate and by never asking for the I-beam: exactly those
  two checks failed.

  **Bookmarks and history**: Ctrl+D fills the address field's star (the
  ACCENT, matched closely -- a loose match took the unfilled star's grey
  outline for blue, and passed with adding bookmarks broken); the sidebar
  opens on Ctrl+B with the page in it and the page view narrowed, a row
  goes to its page, Ctrl+H lists history, its search narrows to one
  entry, Backspace edits the search (Ctrl+H is the same code), the row's
  remove button deletes, Ctrl+B again closes, and the bookmark is in its
  file after the window closes. Shown red by never adding a bookmark.

  **Downloads** (the network half): an attachment opens the flyout and
  lands in `/home/Downloads` with the server's bytes (`sum`'s CRC-32
  against `download_bytes()`), a second of the same name is
  "data (1).bin", a connection that breaks a moment after it started and
  a Cancel clicked on the row's reported rect each leave nothing behind,
  and Clear finished empties the list. Shown red by skipping the rename
  of the finished `.part`: exactly the two file checks failed.

  **Fonts**: `/tests/web/fonts.html` draws one word per 48 px row in
  sans, serif, mono and each one's italic, and the INK of each pair
  that must differ (sans/serif, upright/italic, regular/bold) differs,
  while two controls match to the pixel: a row repeated, and
  "Times New Roman" against `serif` (the alias). A broken face choice
  that draws every row alike fails the differences; noise or a moved
  row fails the controls. Shown red by never choosing an italic in
  `ns_font.c`: exactly the four slant checks failed, at 0 px.

  **The network half** (skipped on bare metal, where 10.0.2.2 is nobody)
  runs against `web_server.py`: a typed `10.0.2.2:PORT/net.html` is
  fetched over http and its SEPARATELY fetched stylesheet's red is in
  the pixels, with none lower down; a cookie the server set comes back
  on the next page and is in `/var/lib/browser/cookies` after the window
  closes; words become a search, pointed at the same server with the
  app's `--search-url`; the server saw fewer connections than requests;
  and with the test CA fetched into `/etc/ssl/certs` by wget, https loads
  and an expired certificate stops at NetSurf's "Privacy error" page.
  The app is spawned with `--weak-entropy`, as the guest has no
  virtio-rng. Shown red by never sending the Cookie header and by never
  pooling. The same half types a 233-byte address, past a toolkit
  field's default 127, and checks its LENGTH (the app's layout line
  carries `len`, and at most 72 bytes of the address itself, since a
  160-byte log line cut by vsnprintf() loses its newline); shown red by
  dropping the field's `uui_textbox_set_max()`.

  **What outlives a content process** (`docs/browser-design.md`, after
  stage 5): a page's process is killed and Back goes back, in a new
  process, scrolled where the page was left, and Forward returns; the
  window closed and opened again restores the tab, scroll and both
  directions from `/var/lib/browser/session.tsv` (`forget_session()`
  deletes it before any check that wants the start page). An HTTP Basic
  page on `web_server.py`'s `/auth/` opens the app's sign-in, a wrong
  password asks again, the right one loads it, a second tab is let in
  without asking (the login is webnet's, not the content process's),
  the lock card's Clear forgets it, and Cancel shows the server's own
  401 page. A window spawned with `--upgrade-local` tries a typed
  `10.0.2.2:PORT` over https first and falls back to http (the test
  server speaks only http), remembers the host, and never falls back
  from an https:// typed out. Back shown red by making it do nothing;
  the scroll restore went red on two real faults while it was built.

  **`/tests/web/photos.html`** checks PNG and JPEG by pixels -- each
  quadrant of a progressive JPEG and each band of a 2-bit palette Adam7
  PNG its own colour, with the page beside them white -- decoded in a
  thread, as both are over `image_uimg.c`'s in-place threshold. Shown
  red by never asking for the redraw when the decode finishes.
- **`w16_gui_test.py`** -- w16 running the project's own samples in
  toywm, by pixel. `gdi.exe` puts a string-table string, a bitmap
  resource three ways (SetDIBitsToDevice; a memory DC and BitBlt SRCCOPY;
  NOTSRCCOPY, every colour inverted), a pen line that stops short of its
  end, a brush rectangle in a pen edge, text through the global and
  local heaps, a square its timer draws after three ticks, its caret
  (a 2-pixel bar inverting the white) and a metafile it records itself
  (a brush, a rectangle, a BitBlt from its memory DC) and plays back,
  and a (200,100,50) brush -- solid, and with `colors=16` equal to the
  dither measured on 3.11 -- each at a fixed place compared with
  tolerance 0; Alt+Space opens the control
  menu, X maximizes (the border goes, the caption reaches the corner)
  and Alt+Space, Enter restores; `fault.exe`'s click opens the
  Application Error box, its crash report names the fault and its
  `.last` file says it stopped. GDITEST.INI written on drive C: first
  turns gdi.exe's magenta square on and gets its own Written=yes;
  `scale=2` in gdi.exe's own settings opens it at twice the size, and
  `frame=desktop` gives hello.exe toywm's frame round a client of the
  size it asked for. With scanlines in its own settings (`screen_effect`)
  gdi.exe at 2x keeps its caption band pixel for pixel and darkens the
  second screen line of each 3.1 row in its client; Alt+Space, E opens the
  Screen effect cascade, O saves `effect=off` (through hlpbroker) and the
  client is plain again -- red when the effect covers the whole window. Retro Launcher runs a tile on a double
  click, `retrolauncher FILE` runs a program with no launcher window, and closing
  it records `ended`. A program's calls window (`retrolauncher --calls`) lists
  fault.exe's one missing call under two chips, every import with Show
  all, one row after a search, and copies the rows on Ctrl+C; gdi.exe's
  opens on every import, and the details panel's button opens it.
  `scale=1.5` opens gdi.exe at exactly 1.5 times, with `method=pixels`
  leaving only pure colours across the magenta square's edge and
  `method=smooth` blending them. `dialog.exe`: Alt+F opens its menu bar's File menu, its Checked item
  unticks itself, F2 (an accelerator) opens its dialog, whose edit field
  keeps what is typed (read back through DIALOG.INI), a right click's
  MessageBox answers No by mnemonic, and Help > About is ShellAbout's
  box; breaking GetDlgItemInt or TranslateAccelerator turns exactly its
  check red. `wincalls.exe` writes the answers of calls that need no window
  to WINCALLS.INI (huge blocks and a GlobalReAlloc that must move them,
  GetDIBits, bitmap bits, INT 2Fh, a flood fill on a 1-bit bitmap, an OLE
  server's Release on revoke among them); `opendlg.exe` runs the Open dialog on its own template, its hook
  recording which of Read Only and Help show and ticking Read Only before
  OK; `popup.exe`'s screen-sized popup covers SaunaOS's whole screen, its
  640 x 480 centred on black (its green corner at the centred origin),
  and a click destroys it and leaves the main window; `place.exe`'s popup
  (`placed_popup`) opens at its program's 40,30 from the main window's
  corner and is drawn there, a click's GetWindowRect-then-MoveWindow moves
  it to 50,30, a drag of its caption keeps it attached and tells the
  program, and a drag of the main window carries it -- a GetWindowRect
  answering 0,0, a w16 deaf to the drag, and toywm not following each turn
  their own checks red. Taking out the flood fill's 1-bit border turns exactly its check red.
  Flipping the DIB's rows or swapping P and S in the ROP3 rule turns
  exactly the matching check red, as do ignoring the INI value, a
  launcher that never spawns and a calls list that ignores "missing"
  (`mutate.py`). Since 2026-10-20 also: the pointer keeps the arrow while
  hello.exe's scroll-bar thumb is dragged onto its text; drive D: mapped
  to a folder gets hello.exe's D:\HELLO.TXT; nine folders list and a
  folder's right-click Move down is saved; `retrolauncher FILE` runs
  hello.exe at once and cards fault.exe (a call missing) and a DOS
  `.exe`, `--details` names the File Manager's rows, and the File
  Manager's right-click on fault.exe lists Show its calls; the details
  panel scrolls above its pinned Run in a short window; the Options
  window grows and shrinks to a floor; and a dialog caption's black line
  leaves white at both ends. Each of those goes red with its fix taken
  out. Options > Sound lists 3.11's seven events, a Play cell plays
  SaunaOS's chime (its `play ROW PATH` log line, so a guest with no sound
  card still answers), and plays nothing once Off is chosen, before OK.
  `w16probe.exe` (2026-10-28, Control Panel's Fonts, Date/Time and
  Keyboard): an extended-selection list counts and lists its selection,
  a variable owner-drawn list keeps each row's height, GetRasterizerCaps,
  AddFontResource, DOS's set date and time on Windows' own clock (a
  30th of February refused), and GetFileResource on gdi.exe's bitmap; a
  physical 1 held 1.75 s in dialog.exe's Number box types `111` at
  WIN.INI's KeyboardDelay=3 and KeyboardSpeed=0 (QMP's key_down, since
  only a physical press repeats). A list with no multiple selection, one
  row height for all, and SaunaOS's repeats passed through each turn
  their check red.
  `fpu.exe` (the x87 and WIN87EM): 1/0 and sqrt(-1) each reach its
  handler once as 83h and 81h, a ninth load as 8Ah -- under KVM; TCG's
  x87 has no stack fault, and the test asks QEMU which it is -- masked,
  BX=8 reports the zero divide, and sqrt(2) * 1e9 through FISTP is exact;
  WIN87EM answering 84h for a zero divide turns it red. `shapes.exe`:
  Ellipse's rows, RoundRect's corners and a pie's radius sit where real
  3.11 drew them; the ellipse's radii one pixel larger turn it red.
  `cursor.exe`: its own class cursor is drawn as a client image
  (`gui state`'s cursor shape 14 and `image`), its inverting pixels
  black over white and white over black, the black and white ones
  unchanged; SetClassWord(GCW_HCURSOR) switches it from the next move,
  SetCursor(NULL) hides the pointer, and bits the program GlobalLocks and
  rewrites show at its next SetCursor. The inversion broken
  (`wm_render.c`'s XOR) or the re-read skipped (`w16_cursor_refresh()`)
  each turn their check red. `retro_round` (2026-11-01, Retro Launcher's
  missing calls, trace, INI file and drives): `missing=zero` answers
  fault.exe's OpenComm 0 and the `.last` record names it; the panel's
  Missing calls row and footer ("1 missing call / answered 0 --
  which?"), whose link scrolls to the calls; the run card's "Run,
  skipping them"; the split Run button's arrow menu above the button, Run
  with trace's record and the footer's Open the trace (Log Viewer, once
  the crash card has gone from over it); the INI section's None yet, Make
  one... and first lines; a CD-ROM drive's sidebar row and the program in
  its subfolder; `label_d` in w16's trace; Options' When a call is
  missing and Drives' Type and Label saved on OK -- and no footer control
  outside the panel or over another (the overflow rule, from the
  launcher's layout lines). Each of its checks went red under a
  `mutate.py` break of what it covers (the setting ignored, the footer's
  count, the link scrolling to 0, the card's button, the arrow 8 pixels
  off, the menu left below, the trace line, the stop's call, Log Viewer
  not spawned, the INI name, the file not made, the line count, the
  drive row, the walk not descending, the label ignored, the Options
  rows and the save; the calls button 200 pixels wider turns both
  overflow checks red). `mdi` (2026-11-03, `samples/mdi.asm`, mdi16.c):
  the last document made is active (its caption navy, the first's white),
  Ctrl+F6 activates the first, Window > Maximize gives the frame "MDI test
  - [One]" and its bar the document's box and Restore, Restore takes them
  away, and Run Hello's WinExec opens hello.exe as a second w16; with the
  bar's items and TranslateMDISysAccel broken, the Ctrl+F6, title and bar
  checks went red, and with the compositor refusing the launch, Run
  Hello's. Document Two (vertical bar) and a captioned document's frame: its scroll bar lies inside the frame and starts under the caption, its move outline is drawn over the other documents while held, a move lands where the pointer took it, a size by one edge moves only that edge, and WM_NCCALCSIZE answers the client area (2026-11-08, each red on the code before its fix). `w16probe.exe` also records SPI_ICONHORIZONTALSPACING's default as 3.11's 75. `groups` (grp16.c): `w16 --groups` makes
  nothing without a PROGMAN.EXE on drive C:, then RL1.GRP for the samples'
  folder, listed once in PROGMAN.INI however often it runs.
  `fullwin.exe` (`fullwin`, the Windows Tutorial's Windows Basics in one
  program): FrameRgn 1 by 1 and 2 by 2, an icon's dark magenta and dark
  grey as the VGA showed them and a group's 16-colour picture over its
  8-colour one, a second WS_OVERLAPPEDWINDOW painted and copied off the
  screen with no wait between, maximized, hidden and SW_SHOWNORMALed back
  to its rect, a journal hook's last click before its unhook's post, and
  no shadow beside the second window; check 2 adds Restore's four-row
  arrows. Each turns red with only its own fix removed (2026-11-05). It also checks the screen DC over the second window (`screen16.c`): a bar shows over its caption and client and on onto the main window in screen coordinates, a strip inverted twice is as it was, and a repaint takes the bar off the client. A drag from the second window let go over the main window is the main window's WM_LBUTTONUP (`input16.c`'s `top_under()`). Made taller, a window with no CS_VREDRAW repaints only the uncovered strip. DrawFocusRect's dots lie on the screen's checkerboard. `ownact.exe` (`ownact`): a click on an owner gives it the focus as 3.11 did, the program's ShowWindow(SW_SHOW) on its already-shown owned window gives that window the focus while the program has it, and a hello.exe focused meanwhile loses nothing to it. Each went red on the code before its fix (2026-11-08).
  `mci.exe` (`mci`, mci16.c): SaunaOS's orbit.avi opened as an AVIVideo
  device by alias, its length in frames, in ms and through
  mciSendCommand's MCI_STATUS, drawn where MCI_PUT put it, played from
  frame 140 with notify to MM_MCINOTIFY's success, then -- the
  destination moved first -- sought while stopped, its frame drawn there
  (red with the seek's wake removed); an unknown device's error and text;
  `where ... window` giving the display window's place and size, not the
  destination's; a binary MCI_OPEN of `type!element`; MCI_WINDOW's text
  read from its own offset and answered as the window's caption; and a
  play with both wait and notify told once. Each of those four turns red
  with only its own fix removed (2026-11-05).
  `--only NAME,...` runs just those checks
  (`shapes`, `own_cursor`, ... -- `main()`'s list).
  In `gui_regress.py`.
  **Run it on a fresh image**: a second run in the same guest fails the
  two scale checks (toywm remembers gdi.exe's earlier window sizes) and
  the calls-window checks (cause not established) -- 48 of 48 pass on a
  fresh one (2026-10-19). It does NOT yet cover child windows, the EDIT,
  list boxes, files or the common dialogs, which only Windows 3.11's
  Notepad exercises today (a roadmap item: a NASM sample that edits and
  saves).
- **`pager_test.py`** -- `userland/libs/base/upager.c` under BOTH its front
  ends, by driving a GUI Terminal. `/bin/less` had no test at all until
  its paging moved into a library for `/bin/doc` to share.

  **The load-bearing check is the STATUS BAR.** Everything else passes
  with the pager replaced by `cat`: text appears, the screen changes
  when a key is typed (that key echoes at the prompt), the window
  survives. What only a running pager produces is a reverse-video band
  across the bottom row. Measured with a positive control that makes
  `upager_run()` dump instead of paging: the bar check goes red for
  both front ends and the other five stay green, which is exactly why
  the docstring names it.

  Two things the measurement had to get right, and the first version
  got both wrong. **The background is uniform too**, so "the widest run
  of one colour" is satisfied by an empty row -- the background is
  found as the commonest colour in the band and excluded. And it is
  COVERAGE rather than a run: reverse video paints the bar in the
  foreground colour and its text in the background one, so the longest
  unbroken run across a padded bar is one character wide.

  **It picks the LONGEST page on the host** for the `doc` half. The
  first version used `doc ls`, which renders to 25 lines and fits one
  screen -- so "space turns the page" was asking a correct pager to do
  something it must not.
- **`sync_test.py`** -- `gui sync` and `settle()`: the counter demo's
  Add clicked twenty times, each followed by `settle(0)` (the sync, no
  grace -- which would hide an early answer) and an UNSETTLED
  screendump of its label, which must differ from the last every time --
  an answer before the frame, or none, shows the old label now and then.
  Prints settle()'s median and worst wait. `--positive-control` settles
  the old way with no grace and must see a stale label (4 and 5 of 20 on
  2026-10-29). In `gui_regress.py`.
- **`input_test.py`** -- `gui input`, a sequence the WM times: an Alt+Tab
  tap and a 400 ms hold told apart by the switcher's own measure
  (`last_ms`, whether its card drew); a title-bar drag that waits with the
  button down moves the window WHILE THE REAL POINTER MOVES (an idle loop
  sleeps through the wait and would pass without the held-button
  re-delivery); a sequence ending with a button down, and a second while
  one runs, refused. Seen red with the re-delivery and the waits each
  disabled (`mutate.py`). In `gui_regress.py`.
- **`wait_test.py`** -- `gui wait`/`gui waited` and `gui sync TITLE`
  (`docs/gui-test-sync-design.md`): a window wait ends `done` only once
  the window has presented; a `--pid` that exits ends it `exited` at once,
  not at the timeout; a widget that never comes ends `timeout` at the time
  asked; a widget wait and a `gone` wait end `done`; `gui sync TITLE` asks
  a client that is neither focused nor under the pointer. Seen red with
  the exit check and the titled sync each disabled (`mutate.py`). In
  `gui_regress.py`.
- **`scroll_test.py`** -- scrolling as a person does it, read from the
  scrollbar thumb's PIXELS with the pointer parked off the bar: the view
  opens with its thumb at the top, the wheel down moves the thumb down and
  the wheel up moves it back, a click in the trough below the thumb pages
  down and above pages up, dragging the thumb moves the view with the
  pointer. Run on the Retro Launcher's program grid (60 programs: a folder
  with more rows than fit) and the Log Viewer's list. It exists because
  `uui_grid`, `uui_medialist` and `uui_loglist` drew the thumb MIRRORED
  (the painter counts from the bottom; they passed a position from the top)
  and nothing looked until a folder of hundreds of programs gave the grid a
  bar. **A new scrolling view is added here before it is called done**
  (`userland/CLAUDE.md` rule 10): the Retro Launcher's Details table is
  checked here too, the Hex Viewer's byte grid (`uui_hexview`, on a
  550-row font licence -- on a 20,000-row binary the wheel's 36 rows move
  the thumb a few pixels, under the check's threshold; `key_mods="ctrl"`,
  since its Home/End are the row's), and Disks' pending list (`uui_steplist`) through
  `check_view()` from `disks_test.py`, which has the second disk a queue
  needs. `check_view(warp=True)` parks the REAL pointer: the wheel goes to
  the widget under it and `gui move` lasts one compositor iteration, so a
  view away from the window's middle never hears the wheel without it;
  `top_inset` skips a table's header, where a long thumb's centre would
  never read as "at the top". Its controls, seen red with
  `mutate.py`: the grid's draw without its flip, the log list's hit test
  without its flip, the hex grid's bar given its top mirrored. In `gui_regress.py`.
- **`retro_list_test.py`** -- Retro Launcher's list as a person uses it,
  on the sample programs: `view=details` in `/etc/retrolauncher.conf`
  opens the table; the search box filters by name and by FILE name (the
  query `.EXE` is in no title, description or module, so it reaches only
  the file-name term -- a first query, `wincalls`, matched the module and
  let the control stay green), Esc clears it; a click on a header sorts and
  a second reverses, judged from the log line the app writes (first and
  last program with their call counts), not pixels; the selection stays on
  its program through a re-sort and a view switch; the arrow keys move both
  views; a double click runs; the choices reach the conf and a second launch
  opens as left. Controls, both seen red with `mutate.py`: Missing compared
  as Calls, the file-name term of `matches()` removed. In `gui_regress.py`.
- **`fm_bigdir_test.py`** -- the File Manager on a folder of 600 files
  (`mkfiles`, from inside the guest): its own `rows` report says all 600
  (601 with the `..` row), the last file can be chosen and marked (the
  mark bitmap grew with the listing), and the status line says "600
  items". The File Manager once listed 256 and said "(more)" -- a copied
  Windows folder looked as if it had lost most of its files. Its control,
  seen red with `mutate.py`: capping the listing at 256 in
  `uui_fileview.c`'s reload. In `gui_regress.py`.
- **`hlpview_test.py`** -- Help Viewer on `hlpgen.sample()`, put into the
  guest with `vm.py put` before the console is taken: the link colour is
  in the reported link rects and nowhere else (pixels), the pointer is the
  hand over a link, the text cursor beside it and the arrow over nothing,
  a click follows a jump, Alt+Left/Right walk the history, a pop-up opens
  and Esc closes it, zoom stops at both limits and Ctrl+0 resets it, the
  right-click menu greys Copy and Back and not Select All, Forward or
  Contents (by the rows' own contrast), Ctrl+A Ctrl+C reaches Notepad
  through the real clipboard with a phrase-compressed paragraph intact,
  and Find opens the first hit. Links come from the widget's own
  `view.link` layout lines; the menu's rows from `ctxmenu.item`. Notepad
  takes the CONTROL CODES (`0x16`, `0x13`), not `mods="ctrl"` -- the
  viewer takes either, which hid it for one run. Its controls, both seen
  red with `mutate.py`: removing the zoom clamp, and drawing links in the
  text colour. In `gui_regress.py`.
- **`hexview_test.py`** -- the Hex Viewer on a generated ELF (the
  hostcheck's, with 16 KiB of NUMBERED rows after it, so no two rows draw
  alike), put into the guest with `vm.py put`: it opens and names the
  format; scrolled six rows by the wheel, the top row's offset column is
  pixel-identical to what row six showed (rows drawn at their offsets);
  a selection changes a byte's colour in the hex AND the text column and
  not a byte outside it; the inspector's values at the entry, Big
  swapping them, As an offset + Go to; a structure-tree click selecting
  exactly the interpreter's bytes; Find in hex and text; Copy as hex
  pasted back into Find finding its source; Go to absolute and relative;
  32 per row; Copy as hex greyed by its own pixels and the right-click
  rows by their contrast; Save as writing under `/home` and refused
  under `/tmp` with the app alive (the sandbox); File > Open in the
  sandbox. The menu bar is the control that must not change; the pointer
  is parked on the status bar before each capture. Its controls, both
  seen red with `mutate.py`: the text column's selection band dropped,
  and a tree click that selects nothing. In `gui_regress.py`.

### `tools/tests/gui/idle_io_test.py` -- does an idle machine stay off the DISK?

Boots a guest with an HDA card (so snddrv and soundd run, as on the
laptops), turns on the `cachesim` boot word with `bootcfg` and one
reboot, waits for boot's own I/O to settle, and measures idle block
reads and writes per second over 30 s. Fails over a budget (15 reads,
10 writes a second), naming the heaviest process and path in the
window from `cachesim -t`. In `gui_regress.py` (`idleio`, which boots
the guest with `EXTRA_VM_ARGS`); by hand it boots its own.

    python3 tools/tests/gui/idle_io_test.py [--instance N] [--window S] [--max-reads R] [--max-writes W]

**Why it exists.** soundd re-read its config 50 times a second on any
machine running snddrv -- 95% of the laptop's idle disk reads -- and no
functional test could see it, because everything still worked
(b93b62c9). QEMU never showed it either: `vm.py` boots without a sound
card, and snddrv runs only on a PCI one. **A guest that lacks the
hardware a bug needs passes for that reason**, so the test asserts
snddrv is running rather than assuming it.

**THE PROBE IS NOT FREE.** Each `cachesim` spawn loads `/bin/cachesim`
from the disk -- 79 block pages, more than the whole idle machine in a
second -- so the test measures one probe's cost back to back, takes it
off every interval, and never names the probe's binary. The first
version did not, never "settled", and reported 7.8 reads/s for a
machine doing 2.4. Measured idle with HDA: ~2.4 reads/s, ~0 writes/s;
with soundd's reload forced every pass (the positive control), 51.7/s
naming soundd and `/etc/sound.conf`.
