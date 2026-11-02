# `tools/checks/` -- Static checks

One entry per tool in `tools/checks/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by `tools/runners/checks.py` (`tools/t static`) as one preflight
step, in parallel. A new rule is a script here that exits non-zero
naming what it found; it is in the gate the moment it is saved.

- **`check_runners.py`** -- refuses a test no runner runs: a
  `*_test.py`/`*_hostcheck.py` outside `tests/` and `hostchecks/`, a test
  with no `RUNNER` header or no `covers`, a `tests/manual/` tool with no
  `manual` reason, an unknown RUNNER key (a misspelt `parallel` would
  silently run a tool alone), and two tools claiming one `name`. It
  replaced `check_tool_coverage.py` when the runners stopped keeping
  lists: with the folder naming the runner, "named by no runner" can only
  mean "outside a runner's folder".

- **`check_dispatch.py`** -- fails the build on an `if/else` chain or
  `switch` over ~20 branches that should be a table (CLAUDE.md's
  table; `dispatch-ok: <reason>` waives one in place). Deliberately
  dumb: it counts `else if` per brace depth. The count is PER FUNCTION
  -- flushed each time the depth returns to 0 -- since 2026-09-30; it
  was per FILE before, so a new five-branch function in
  `fs_syscalls.c` failed a chain that began in another function. Two
  chains at one depth inside one function still merge, which is the
  safe direction.
- **`check_docs.py`** -- the documentation rules a script can check,
  because the ones that rotted before were the ones nobody checked. A
  pointer to the DELETED changelog, a milestone heading that reintroduces
  a number or a target version, a DUPLICATED roadmap entry, a roadmap item
  that WRAPS onto a second line or runs past 140 characters, a
  roadmap-details HEADING that names no roadmap item or bug any more, a
  stale decisions index, a link to a doc that does not exist, a tool
  in `tools/` that CLAUDE.md never mentions, and a command whose
  `lib/uargs.h` table names a usage form, option or command its
  `docs/commands` page does not. The one-line rule is checked
  rather than stated for the usual reason: the roadmap reached 2,939
  lines by accumulating a paragraph per item, and nothing noticed. The duplicate check earns its
  place on its own -- two of this repo's own roadmap edits duplicated an
  entry and a third silently deleted three. The heading check earns its
  place the same way: the pairing by TITLE is what sends a reader from a
  one-line item to its long form, and the two files are edited apart, so
  an item reworded in place leaves its heading stranded. Eight had
  drifted when the check was added -- two section renames and six items
  reworded or ticked. The tool check enforces a
  rule this file already stated and nothing verified; it is deliberately
  a NAME check, so it says a tool is mentioned, not that what is written
  about it is still true. It does NOT flag `Milestone N` in prose
  (historical, and `docs/roadmap-details.md` ends with a legend for
  those); the noise would be what stopped anyone running it. In
  `preflight.sh` and CI.

  **It also checks that every command has a page** in `docs/commands/`
  -- every `/bin` program and every `dispatch()` builtin -- and that
  every page documents something that exists. `docs/roadmap.md` had
  wanted this since the man-pages milestone was written, and it is the
  half of a per-command docs folder that makes it stay true: a command
  shipping undocumented is not found by anyone reading the docs, it is
  found by someone typing `help` and meeting a name nothing explains.
  Where a program declares a `cmd_usage()` string the page must carry it
  verbatim, so a flag added to the program and not to the page fails the
  build; the PROSE is deliberately unchecked, since that is the part
  only a person can write.

  **BOTH FORMS OF THAT STRING COUNT, and for a while only one did.** The
  match was a string LITERAL, so the four programs passing a named
  `USAGE` constant -- `mount`, `umount`, `grep`, `mkpart` -- were skipped
  in silence, indistinguishable in the output from a program with no
  usage at all. `mount`'s source argument then grew a whole new form (a
  device name) with nothing comparing the page against the program,
  which is exactly what this check exists to catch. A named constant is
  resolved now, including one built from several adjacent literals, and
  the page's four-space code-block indent is accepted on either side.

  **And every tool must be named in `docs/tools.md`.** When only
  CLAUDE.md's list was checked, `multidisk_test.py` passed the gate while
  missing from the full reference; that list is gone from CLAUDE.md now
  (it cost always-loaded context, and each runner's `--list` -- or
  `preflight.sh`'s header comment -- is the live answer), so this file is the one the check reads. The `/bin` list comes from the SEED TREE
  rather than from `userland/bin/*.c`, because the Makefile renames some
  programs on the way in and the name on disk is the name people type --
  which also means this check is skipped in a checkout that has not run
  `make iso`. Exemptions are named with reasons in
  `COMMAND_PAGE_EXEMPT`.
- **`check_deps.py`** -- proves the build's header dependency tracking
  is actually live: touches one header per build directory (discovered
  from `build/`, not listed, so a new source directory is covered as
  soon as it's been built once), asks `make all -n` what it would
  rebuild, and fails on any directory that answers "nothing". Restores
  mtimes, so a run changes nothing. Exists because that tracking broke
  silently for the whole of `kernel/` when objects moved directories
  and nothing noticed -- a clean build can't observe a stale `.o`, so
  neither could CI. In `preflight.sh` and CI.
- **`check_layout.py`** -- verifies `disk.img`'s directory structure
  matches `docs/filesystem-layout.md`'s table, which is the source of
  truth for where things live on the OS's own filesystem. Runs in
  `preflight.sh` and CI. Fails in both directions (an undocumented
  directory on the image, or a documented-as-present one missing), and
  understands the table's "Created by" column -- a `build`-created
  directory must exist on a freshly built image, a `boot`-created one
  needn't until the OS has run. **Read that doc before adding a
  directory, a config file, or any new seeded data**: it also records
  the budgets (64-byte caller-side path buffers everywhere; the
  256-record table on TFS2 images only -- TFS3, the only
  since TFS3's inode layer, has ~590k inodes) and the `sync`-never-deletes
  trap that makes moving a seeded file need an explicit cleanup. It
  also WARNS (never fails) about orphans -- a file in a seeded directory
  that `seed/sync/` no longer places there, i.e. exactly that trap
  having already happened -- and prints the `delete` commands to fix it.
  That check found all four ring-3 GUI apps still sitting in `/tests`
  months after they moved to `/bin`, each frozen at the build that put
  them there. **It fired again in 2026-08-16, and that time the orphans
  were LOAD-BEARING**: six stale binaries at `/bin/<name>` were what
  eight GUI test tools had been spawning, long after seeding moved them
  to `/bin/wm/{system,apps,demos}/`. Deleting the orphans (the remedy
  the tool prints) turned the suite red instantly. The right repair is
  to point the tools at the CURRENT path, not to keep the stale copy --
  but the lesson generalises: **an orphan the tool reports may be
  something you are still using, so re-run `gui_regress.py` after acting
  on that warning**, and treat a test that still works after a file
  moved as evidence it is testing the old copy.
- **`check_layout.py`** -- see CLAUDE.md's `docs/` section: verifies the built
  image's directories against `docs/filesystem-layout.md`, and warns
  about orphaned seeded files. Runs in `preflight.sh` and CI.
- **`check_licenses.py`** -- every directory under `userland/ports/` and
  every font in `data/fonts/` must carry a license file **and** be named
  in `LICENSE`; so must every Terminal colour scheme (as another
  project's palette, or in the list of original ones -- naming EVERY one
  forces the question for a new one) and every notice file under
  `data/licenses/`. Palettes were added after three shipped unrecorded:
  data, not code, so nothing asked.

  **The inventory had drifted twice before this existed.**
  `userland/ports/doom/` is GPL-2-or-later source vendored into an MIT
  repository and `LICENSE` did not mention it at all; the font section
  described "two complete third-party font files" when there were five,
  with `vera-mono.ttf` and both bold faces unlisted and Vera a third
  license family. Neither was a violation on its own — every per-file
  notice was present, which is what the licenses actually require —
  but a reader of `LICENSE` could not learn that GPL code was in the
  tree.

  **What it cannot check, and says so in the file:** whether the license
  NAMED is the license the code is really under. Nothing static can read
  a directory and know it is GPL-2-or-later rather than GPL-3. That is a
  human reading the vendored license file, which is why the entries in
  `LICENSE` quote the version language rather than paraphrasing it.

  **Each port's licence is also DECLARED in the Makefile**
  (`PORT_LICENSE_<name>`, SPDX ids joined by OR), and this holds every
  declaration against the port's licence file by family: an id whose
  family's wording the file lacks fails, a file carrying the GPL's
  preamble declared as anything else fails, and a GPL-only port must name
  its programs (`PORT_PROGRAMS_<name>`). `NOGPL=1` decides from those
  lines, so a wrong one would ship GPL code in a "GPL-free" build.
- **`check_tool_commands.py`** -- static check that every guest command
  a tool drives still exists, against the same authority `check_docs.py`
  uses (the seeded `/bin` tree plus both shells' builtins).

  **It found a real one on its first run**: `kvm_soak.py` drove
  `delete`, which is `rm` now -- so its "force a reload by deleting"
  half had been a no-op and it had been leaving `zz*.desktop` litter on
  `disk.img`, which is the dirty-fixture class that makes *other* tools
  fail.

  **Its limits are stated in the file, because an oversold check is
  worse than none.** It cannot see a command whose OUTPUT changed,
  which is the rot that actually bit `fs_switch_test` -- the name `df`
  stayed valid the whole time. `ondemand_sweep.py` catches that by
  running things; this is the cheap subset that costs a second.
- **`check_config_size.py`** -- no shipped `data/etc/**` or
  `data/usr/share/services/**` file may exceed `ETC_CONFIG_FILE_MAX`,
  the 1 MiB sanity bound a config file is read up to (whole, at its own
  size). Run by `preflight.sh`.

  **The failure it prevents names the wrong thing.** A config file over
  the bound is refused, so its keys are not seen -- and what init then
  reports is `dhcp has no Exec=, will not start it`, which reads as a
  broken service. It was a 4 KiB buffer until 2026-10-29, when a long
  comment was enough; comments still count.

  It reads the limit FROM the header (a number or a plain product, like
  `(1024u * 1024u)`) rather than repeating it, since a limit copied into
  a checker drifts from the code it checks. It deliberately does not
  cover files written at RUNTIME (`resolv.conf`, the settings files) --
  those are refused at load, and logged once, rather than truncated.

  **It also checks the per-field text caps**, which is a different
  failure from the file being too big: a `Description=`/`Label=` in
  `data/etc/settings.d`, or a `Label.`/`Desc.`/`Unit.` in a
  `data/wm/savers/*.saver`, longer than the fixed array it is copied
  into arrives TRUNCATED MID-WORD with the file itself far under every
  size limit here. The saver caps come from `userland/lib/usaver.h` and
  the settings ones from the ABI, each read from its own header.

  **And a settings descriptor's NAME**: the file name is the setting's
  `<namespace>.<name>`, and either half over `SETTING_ABI_NS_MAX` or
  `SETTING_ABI_NAME_MAX` never matches its text -- the row shows with no
  label, choices or `Widget=`.

  **Everything here walks `git ls-files`**, so a file that has not been
  `git add`ed is invisible to it -- which is how a positive control on
  a brand-new descriptor directory changed nothing at all and looked
  like a checker that did not work.
- **`check_signals.py`** -- EVERY SIGNAL `abi/signal_abi.h` DEFINES HAS
  A NAME in `kernel/lib/ksignal.c`'s `SIGNALS` table, which is what
  `signal_name()` reads in both rings (`kill -NAME`, the shell's "killed
  by SIGx", `strace`). The table is a hand-kept copy of the header and
  fell five behind it -- ILL, ABRT, FPE, PIPE, TTOU -- so a pipeline
  whose reader went away printed `killed by SIG?`. Reads the header with
  `gen_signames.py`'s parser, so dash's table and this one agree on what
  counts as a signal. Run by `preflight.sh`.
- **`check_syscalls.py`** -- NO TWO ROWS OF `abi/syscall_rows.h` RESOLVE TO
  THE SAME NUMBER. The table is built with designated initializers, so a
  row's index IS its syscall number -- a good property with one hole: C
  lets the same index be written twice and silently keeps the LAST one.
  That happened on 2026-09-12. `SYS_SIGPROCMASK` was given 108 by
  reading the bottom of `abi/syscall_abi.h` and adding one, but the
  numbers there are NOT in file order and 108 was already `SYS_FORK`.
  The build was clean, the new syscalls worked, and `fork()` dispatched
  to `sigprocmask()` -- so every fork returned 0 and every caller
  believed it was the child. The only symptom was `fork_test` dying with
  no fault and no log; the only thing that showed it was `strace`
  printing the wrong name for the call. It reads the row NAMES out of
  the table and their values out of the header, so nothing guesses which
  `SYS_*` constants are numbers -- having a row is the definition, and
  the flags and limits sharing the prefix are simply not rows. In
  `preflight.sh`.
- **`check_text_loops.py`** -- fails the build on a BYTE of a string
  measured or drawn as a CHARACTER: `ugfx_char_advance(x)` /
  `ugfx_draw_char(.., x, ..)` with anything but a character literal,
  `ugfx_cp_advance()` / `ugfx_draw_cp()` / `ugfx_kern()` given a
  subscript (`s[i]`), and `ugfx_text_width_n(x, 1)`. Strings are UTF-8
  (`docs/utf8-design.md`), so such a loop draws `é` as `Ã©`, and it
  compiles and looks right on every ASCII string anyone tests with --
  the reason it is a ratchet rather than a review note. Step with
  `ugfx_text_decode()` instead. Waiver `text-loop-ok: <reason>`, whose
  one legitimate reason is a CODE PAGE (w16's Windows-1252, where a byte
  is a character). `--positive-control` runs it over planted lines of
  every shape and a literal that must pass. A subscript followed by a
  MEMBER (`row[c].ch`, a terminal cell's codepoint) is a struct field,
  not a byte, and is not flagged; `row[c].name[0]` still is.
- **`check_text_measure.py`** -- fails the build on a character COUNT
  used as a text WIDTH. `ugfx_char_w()` is the WIDEST advance in the
  face, so `n * ugfx_char_w()` measured a string correctly only while
  the interface face was monospace; the day it became `liberation-sans`
  it became the width of the widest possible string of that length, and
  ~23 sites across the toolkit, the WM and four apps were wrong at once
  -- a Start menu 1.8x too wide, labels cut early, a file-view column
  pitch (and its selection highlight) 1.7x over, and a text field whose
  caret and click landed on a grid the glyphs were not on.

  **The discriminator is a NAME versus a LITERAL**, and that is the
  whole reason the check is usable: a variable paired with the cell
  width is nearly always a count of characters in real data (a
  `strlen`, a `width_chars` field, a `MAX_CHARS` budget), while a
  literal is a layout unit -- `char_w / 2` of padding, a two-cell
  gutter -- which `ugfx.h` explicitly permits and which there are ~76
  of. Flagging those too would have meant 76 waivers and a check nobody
  reads. It tracks aliases (`int cw = ugfx_char_w()`) and scopes them
  per function, which is load-bearing rather than tidy: an `int w =
  ugfx_char_w()` in one function otherwise makes every `w` in the file
  look like a cell width, and it reported a callback's own parameter as
  a bug until it did.

  Its honest limit: a LITERAL count used as a text pitch
  (`14 * ugfx_char_w()` for a filename column) reads as a layout unit
  and is not flagged. Two of those existed and both are fixed by hand.
  Waive with `text-measure-ok: <reason>`; the legitimate reason is a
  monospace bracket, where dividing by the cell is the right answer.
  `--positive-control` proves the matcher fires. In `preflight.sh`.
- **`check_widget_ops.py`** -- refuses a `struct uui_widget_ops` table
  with a slot it needs left NULL, and it exists because FOUR widgets
  shipped with short tables on one day (`uui_dropdown`, `uui_checkbox`,
  `uui_textview`, `uui_textbox`), every function they needed already
  written. Two rules: a table with `draw` needs `natural_size` and
  `set_geometry` (a layout cannot place what it cannot measure), and a
  table with `press` needs `release` (`uui_route.c` names a widget to
  its app only when it has one). Both failures are SILENT and surface in
  a different file from their cause -- one of them presented as
  "`uui_layout` stops after four children" and cost most of a session,
  when `uui_layout_run()` has no early exit at all. Waive in place with
  a `widget-ops-ok: <reason>` comment, the same mechanism
  `check_dispatch.py` uses. In `preflight.sh`.

  **It also refuses a widget index derived from the array's own
  LENGTH** -- `#define WIDGET_TREE_SPLIT (g_widget_count - 3)`, and the
  chained `(WIDGET_TREE_SPLIT - 1)` form. Appending one widget shifted
  three of those at once in the File Manager, so hiding the tree hid the
  tree splitter and hiding the pane splitter hid the CONTEXT MENU: a
  right-click that stopped working in single-pane view, diagnosed from a
  screenshot rather than from any test. Look the widget up by its id.

  **And a widget that DRAWS A SCROLLBAR must not route `hit` through
  its row hit.** `_hit(...) >= 0` answers "which row", which excludes
  the bar column -- and `uui_route.c` gates press AND wheel on that
  slot, so the bar cannot be dragged and the wheel is dead anywhere
  that is not a row. It reads as an unimplemented scrollbar rather than
  as a routing bug, because the rows themselves work. Five widgets
  shipped it: `uui_listbox`, `uui_table` and `uui_fileview` each fixed
  it and left a comment, and `uui_sidebar` and `uui_tree` still had it
  in 2026-09-03 -- three comments having failed to stop it twice is
  what earned a check. Answer `uui_hit(x, y, w, h, ...)` there; `>= 0`
  stays right for a widget with no bar.
- **`check_key_routing.py`** -- its sibling, and the inverse question:
  `check_widget_ops.py` catches an ops slot nobody filled, this catches
  a filled slot nobody can REACH. An app whose `struct uapp_desc` names
  a widget whose ops table has a `.key` must declare `.focus` or
  `.on_key`, because those are the only two doors in `uapp.c`. Its
  second rule is the same shape from the pointer's side: an app that
  declares `.widgets` and calls `uui_menubar_press()` is hand-routing a
  menu bar the router should own, so a popup row's click also lands on
  the widget beneath (Image Viewer and Player shipped that way). It
  exists because type-ahead was added to `uui_table`, tested, and
  shipped doing nothing in Task Manager -- which declared neither, so no
  key had ever reached the widget and its arrows and paging had been
  dead since the app was written. Nothing caught it because every check
  in `taskmgr_test.py` drives by MOUSE. **The key-capable set is
  DERIVED** from the `.key` slots in `userland/ui/*.c` rather than
  listed, so a new widget is covered the day it gains a key handler --
  and `uui_menubar_ops`, which has no `.key` and is reached through
  `uui_router_overlay_key()`, correctly does not count. **It reads the
  code, not the comments**, which its own positive control caught: with
  `.on_key = on_key,` commented out the first version still matched the
  text and reported the app as routed. Waive with a `key-routing-ok:
  <reason>` comment; `diskmark.c` is the one waiver, because an OPEN
  popup takes keys through the overlay path regardless. In
  `preflight.sh`.
- **`check_font_state.py`** -- refuses a function in `userland/` that
  calls `ugfx_set_font()` and does not put back the font it found. The
  selected font is the PROCESS's, so what a widget leaves selected is
  what the next widget measures in: Help Viewer's topic widget drew in
  its zoomed face and returned, and the magnifier zoomed the toolbar,
  the tree and the status bar (2026-10-21). It accepts the toolkit's own
  idioms -- `was = ugfx_set_font(f)` ... `ugfx_set_font(was)`,
  `saved = ugfx_font_current()`, a push helper that `return`s the call,
  ending on `ugfx_set_font(NULL)` -- and a saved variable only under the
  toolkit's names for one, since `f = font_for(...); ugfx_set_font(f)` is
  the selection. Waive a function with `font-ok: <whose job the restore
  is>` inside it. `--list` shows how each function restores;
  `--positive-control` removes the restores from the widget that shipped
  the bug and must come back refused. In `preflight.sh`.
- **`check_control_states.py`** -- refuses an app whose menu bar or
  toolbar never says when a command cannot act. Every
  `uui_toolbar_init()` / `uui_menubar_init()` given its items needs an
  `item_flags` assignment in the same file, one per bar (a context menu
  built later, `uui_menubar_init(&ctx, 0, 0)`, is not counted); the
  waiver is `flags-ok: <why every command always applies>`. It exists
  because Help Viewer's magnifier stayed clickable after it could do
  nothing, and the audit that followed found Theme Editor's Save and
  Revert, Device Manager's device commands and Crash Reports' list
  commands offered with nothing to act on. It checks that the callback
  EXISTS; that it answers for every command that can fail to apply is for
  a person (`docs/conventions/gui.md`). `--positive-control` removes Help
  Viewer's `item_flags` and must come back refused. In `preflight.sh`.
- **`check_drivers.py`** -- refuses a driver that declares itself to
  nothing. Every `.c` under `kernel/drivers/`, and any file anywhere
  that calls a device class registry, must carry a `DRIVER_DECLARE(...)`
  or a `driver-none: <reason>` comment; `*_test.c` is exempt, since a
  KTEST beside a driver is not a second driver. It exists because THREE
  drivers were absent from `lsdrv` when it was written -- `i8042` (the
  PS/2 keyboard and mouse, the input path every default boot uses),
  `virtio_input.c` and `virtio_rng.c` -- and nothing was looking.

  **The half it CANNOT check is the half that mattered most**, and that
  is why the declaration moved into the `.drivers` linker section in the
  same change: `DRIVER_REGISTER` used to be a call inside `init()`, and
  `e1000_init()` returns at its "no card on this bus" check before
  reaching it -- so a build containing the driver listed no driver, and
  no amount of grepping for the call would have said so. Data cannot be
  skipped by a return. **It strips comments before matching**, which its
  own positive control needed: `kernel/core/kernel.c` merely NAMES
  `net_register()` in a comment, and matching that sends the reader at
  the wrong file. In `preflight.sh`.


  **A TRAILING `// dispatch-ok:` ON THE SWITCH'S OWN LINE COUNTS.** It
  did not, until a construct that had been "waived" that way for months
  grew past the limit and was reported -- the checker only looked at the
  lines ABOVE, where a trailing comment can never be. A waiver mechanism
  that silently does not waive is worse than none: it reads as
  protection at the call site and provides none.

  **IT SKIPS `userland/ports/`.** The rule is about how code in this
  project is structured and its remedies are both EDITS -- rewrite the
  chain, or waive it with a comment -- and vendored source is the one
  place an edit is forbidden by policy. Doom's `p_spec.c` has a
  72-branch switch written in 1993; it is not a finding, for the same
  reason the Makefile turns `-Wall` off for that directory.
- **`check_initcalls.py`** -- an init that is both an `INITCALL` and a
  hand call (it would run twice), an `INITCALL` at a level
  `kernel_main()` never walks (it would never run), or a `PCI_DRIVER`
  probe called by hand (the bus already calls it). Static, over the
  source, because the in-kernel `initcall` KTEST can see "never ran"
  at boot but not "ran twice". Run by `preflight.sh`.
- **`check_chains.py`** -- a file that gained a `vga_write_dec`/`_hex`
  or `klog_write_dec`/`_hex` chain call beyond its frozen count, or a
  file that started one. The 109 that exist stay (62 in
  `apps/shell_sys.c`, which the roadmap wants in ring 3); the point is
  that `*_printf` is what new code writes. The baseline is a ratchet:
  the script prints the lower number when a file drops. Run by
  `preflight.sh`.
- **`check_test_sleeps.py`** -- a `tools/*_test.py` that gained a
  `time.sleep(` beyond its frozen count, or started one: the guessed wait
  is this repo's commonest flake. A ratchet over 1606 sleeps in 197 tools
  (2026-10-29), its numbers in `tools/checks/check_test_sleeps.baseline`, which
  only goes down -- the script prints the lower line when a file drops;
  `--write` regenerates it. A sleep that IS the test (time passing, a
  rate) carries `# sleep-ok: <why>` on its line or the one above.
  Positive control: an added sleep fails it, the waiver passes it. Run by
  `preflight.sh`.
- **`check_copy_user.py`** -- a `vmm_copy_*_user()` result compared
  with `< 0`. The helpers return 1/0 and never negative, so that branch
  is dead and the handler runs on an unfilled buffer; twelve socket
  syscalls shipped that way with the contract stated in the header
  above them. Static, over `kernel/` and `apps/`. Run by `preflight.sh`.
