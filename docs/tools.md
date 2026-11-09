# tools/

The dev/build helper scripts — not compiled, not shipped as part of the
OS. **This file is the INDEX**: which tool answers which question, how
`tools/` is laid out, and every tool in one line (generated). The full
reference for each one -- what it does, why it exists, the traps it
encodes -- is in **`docs/tools/<folder>.md`**, one file per folder.

**The bar for adding one:** does it fix a rederive-from-scratch cost?
That is the reasoning that produced every tool below. Add freely when it
does — put it in the folder its role names (below), add its entry to
that folder's `docs/tools/` file, and run `tools/gen/gen_tools_index.py`;
`tools/checks/check_docs.py` verifies both.

**`tools/t` is the front door**: `tools/t` alone prints the commands and
the folders, `tools/t which vm` a tool's path, `tools/t vm start` runs
one by name, `tools/t test fm` every test with `fm` in its name, the way
its own runner runs it. `tools/t gate|gui|sweep|host|static` are the
runners.

## How tools/ is laid out

**A test's FOLDER names its runner** -- the shape of Linux kselftest's
per-directory lists, without the lists:

| Folder | Run by | |
|---|---|---|
| `tests/gate/` | `runners/preflight.sh` | every gate |
| `tests/gui/` | `runners/gui_regress.py` | a fresh image and guest per tool |
| `tests/sweep/` | `runners/ondemand_sweep.py` | on demand; a missing precondition is a skip |
| `tests/manual/` | nothing | by hand; each says why |
| `hostchecks/` | `runners/hostchecks.py` | `gate` ones in preflight, the rest in the sweep |
| `checks/` | `runners/checks.py` | every gate |

Everything else is by role: `lib/` (the harness, and anything another
tool imports), `dev/`, `bench/`, `build/` (what the Makefile and releases
call), `gen/`, `fetch/`, `runners/`. Data directories (`docker/`, `gdb/`,
`systemd/`, `w16/`) stay where their users expect them.

**A test says the rest in a `RUNNER = {...}` literal near its top**
(`covers` always; `vm`, `needs`, `parallel`, `shared_vm`, `args`, `gate`,
`manual` when they apply -- `tools/lib/runner_meta.py` has each key). It
is read with `ast.literal_eval`, so a tool is in its runner from the
moment it is saved, and `checks/check_runners.py` refuses a test outside
a runner's folder or without a header. The GUI suite's start order comes
from each tool's MEASURED median in the run history, not a hand-kept
table.

**Every script starts by finding `lib/`**, then imports `toolpath`, which
puts every tools/ folder on `sys.path` -- so `import harness` works from
any folder, and a basename must be unique across tools/:

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
    import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path

**Name a tool by `toolpath.tool("vm.py")`, never `os.path.join(HERE,
"vm.py")`** -- a path built from a folder breaks the moment the tool
moves; the basename lookup does not. `toolpath.REPO`/`toolpath.TOOLS`
replace `dirname(dirname(__file__))`, whose depth is now per folder.

## Which tool answers which question

| Question | Tool |
|---|---|
| Where is a tool? | `tools/t which NAME`, `tools/t list [PATTERN]` -- the folder index below |
| Is it MINE, or already broken? | `predates.py "<command>"` -- "it predates me" is a MEASUREMENT |
| When did this check last fail, or pass -- at which commit, on which host? | `test_history.py "<check>"` -- every local run recorded, logs kept |
| Can my test SEE the break? | `mutate.py --edit FILE OLD NEW -- <test>` -- restores and rebuilds itself |
| Does my window look the same as before -- every rect, every pixel? | `ui_snapshot.py boot OUT` before and after, then `compare` -- takes each twice to show the noise |
| Is it safe to commit? | `preflight.sh` (**stop your `vm.py` guest first**) |
| Has the on-demand half rotted? | `ondemand_sweep.py` -- everything in `tests/sweep/` and the non-gate host checks |
| Drive the BARE-METAL machine | `remote.py` (`exec`/`put`/`get`/`sync`/`flash`/`screenshot`/`shell`) |
| Let a machine PULL a build | `update_server.py` (a user service: `/dev` live, `/stable` = `--publish`ed) |
| Run the GUI TOOLS on it | `gui_regress.py --host <ip>`, over `remote_gui.py` |
| Drive a VM | `vm.py` (text in, text out), `qmp_test.py`, `gui_debug.py`, `gui_flow.py`, `shell_flow.py`, `serial_console.py`, `serial_capture.py`, `watch_vm.sh`, `run_release.sh` |
| Is it INTERMITTENT, and at what rate? | `boot_rate.py` (bare metal), `flake_hunt.py` (VM) |
| Test runners | `tools/t gate\|gui\|sweep\|host\|static`, `tools/t test PATTERN`; `boot_smoke_test.py`, `ktest_run.py`, `usertest_run.py`, `faulttest_run.py`, `gui_regress.py`, `hostchecks.py`, `checks.py`, `damage_sweep.py`, `damage_hunt.py`, `sanitize_run.py` (UBSAN both rings + KASAN, in a scratch copy) |
| Diagnose | `panic_resolve.py` (**never hand-roll `nm`**), `acpi_dump.py`, `aml_walk.py`, `QMPSession.hmp()` (**the one oracle the guest cannot fake**), `pixel_probe.py`, `iso_guard.py` |
| Does it actually SOUND right? | `audio_loopback_test.py` -- records the G6 back on line in; needs the cable patched in |
| Check an implementation against a FOREIGN one | `libc_diff.py`, `uimg_codec_hostcheck.py`, `usnd_hostcheck.py`, `midi_hostcheck.py` (vs FluidSynth), `hash_hostcheck.py`, `divti3_hostcheck.py`, `regex_hostcheck.py`, `umd_hostcheck.py`, `ugfx_text_hostcheck.py`, `utext_hostcheck.py`, `utween_hostcheck.py`, `term_scheme_hostcheck.py`, `teapot_hostcheck.py` (vs a float surface) |
| Measure | `idle_cpu.py` (quote DIFFERENCES only), `timer_bench.py` (a timer configuration, idle and under load), `sys_bench.py` (a syscall, a context switch, read/write by size), `loc.py`, `dup_scan.py` (copy-paste; a REPORT, never a gate), `ping_rtt.py`, `latency_under_io.py`, `fs_isolation.py`, `frame_balance.py` |
| Disk images, from the host | `seed_disk.py`, `install_grub.py` (also `boot_medium()`), `tfs3_writer.py`, `mkpart_test.py`, `fetch_wad.py`, `fetch_soundfont.py` |
| Generated data | `gen_version.sh`/`set_version.sh`, `gen_kconfig.sh`, `genttf.py`, `gen_kbs.py`, `gen_cursors.py`, `gen_icons.py`, `gen_imgdata.py`, `gen_audio.py`, `gen_music.py`, `gen_sf2.py`, `gen_mp3_tables.py`, `gen_signames.py`, `gen_bootwords.py`, `genrelocs.py`, `gen_syms.py`, `build_conf.py`, `gen_modalias.py`, `gen_decisions_index.py`, `gen_commands_index.py`, `gen_next_up.py`, `fetch_ca_bundle.py` |
| The repo itself | `backup_repo.sh` -- run before ANY change to the repo's identity or history |
| Wait for something, safely | `wait_for.sh` -- a PID or a file, never a process NAME, always bounded |

**Host tools, none required to build** (`docs/tools.md`): **`bear --
make all`** from a `make clean` regenerates `compile_commands.json` for
`clangd` -- use the LSP rather than guessing `userland/libs/ui/` ops tables.
**`ruff check tools/` and `shellcheck tools/*/*.sh`** before touching a
harness (`ruff.toml` pins `F` + `E9` on purpose; neither is in the gate).
`ccache` is wired in and matters on the gate, which starts with `make
clean`.

## Every tool, by folder

<!-- BEGIN folder index: generated by tools/gen/gen_tools_index.py -- do not edit -->

### `tools/runners/` -- what runs the tests: preflight, the GUI suite, the sweep, host checks, static rules

Full entries: [`docs/tools/runners.md`](tools/runners.md).

- `checks.py` -- Run the static checks: every tools/checks/ rule, in parallel, one verdict
- `flake_hunt.py` -- Run a GUI test tool N times and report which checks fail, and how often
- `gui_regress.py` -- run every GUI test tool, each on a fresh VM
- `hostchecks.py` -- Run the host checks: SaunaOS code built with the HOST's gcc, judged by
- `ondemand_sweep.py` -- Run the tools nothing else runs, and report which ones have rotted
- `preflight.sh` -- "am I safe to deliver?" in one command
- `qemu_matrix.py` -- Run a test suite against SEVERAL QEMU versions, in Docker

### `tools/tests/gate/` -- run by every preflight

Full entries: [`docs/tools/tests-gate.md`](tools/tests-gate.md).

- `boot_smoke_test.py` -- Fast, non-GUI boot verification: boots saunaos.iso headlessly in QEMU,
- `ktest_run.py` -- Runs the in-kernel test suite and turns it into a process exit code
- `remote_hang_test.py` -- tools/dev/remote.py cannot hang: every wait is bounded, against fakes that misbehave
- `sdk_test.py` -- Host-side: is the SDK on the disk the tree's, and can a program be built from it alone?
- `tfs3_writer_test.py` -- Host-side: does tfs3_writer.py's OVERWRITE hand every old block back?
- `todisk_test.py` -- Host-side: does tools/build/todisk.py lay a folder over a TFS3 volume, and only that?
- `usertest_run.py` -- Run the self-checking ring-3 diagnostics in /tests, as one pass/fail table

### `tools/tests/gui/` -- the GUI suite: a fresh image and guest each

Full entries: [`docs/tools/tests-gui.md`](tools/tests-gui.md).

- `above_test.py` -- Always on Top: stacked over the rest, focus unchanged
- `animation_test.py` -- Window animations (userland/apps/system/toywm/wm_anim.c) and `desktop.animations`
- `archive_gui_test.py` -- Archives in the File Manager: a .zip and a .tar.gz as folders, and
- `bad_present_test.py` -- a refused buffer replacement keeps the
- `blank_window_test.py` -- no app opens a blank window
- `bootmgr_test.py` -- The Boot Manager and System Settings > Boot menu edit the real grub.cfg
- `brightness_test.py` -- The taskbar's brightness flyout: the slider, the wheel, and the
- `browser_test.py` -- The Web Browser: NetSurf's pages drawn in a SaunaOS window, and driven
- `calculator_client_test.py` -- Drive the RING-3 Calculator (userland/apps/apps/calculator/calculator.c) and assert on it
- `calendar_test.py` -- The taskbar clock's calendar popup: opening, the grid, and week_start
- `caption_corner_test.py` -- a maximized window's close button owns
- `caret_blink_test.py` -- The text caret blinks, stops solid, and costs nothing once it stops
- `charmap_test.py` -- Character Map (userland/apps/apps/charmap/charmap.c)
- `clipboard_test.py` -- Drive the SYSTEM TEXT CLIPBOARD across two apps and assert on it
- `clock_settings_test.py` -- System Settings' Date & time page: setting the clock by hand, and its lock
- `compositor_death_test.py` -- The compositor death path -- The GUI in ring 3's R7
- `compositor_test.py` -- Drive userland/tests/compclient.c -- The GUI in ring 3, stage 2's raw input
- `corners_test.py` -- Corners (desktop.corners) and the always-visible scrollbar
- `crashtest_test.py` -- Crash Test: the fault paths, and the gate in front of the dangerous half
- `crashview_test.py` -- The crash report viewer (Crash Reports' viewer face, base/ucrash.c): a
- `crt_test.py` -- The Terminal's screen effect (userland/libs/ui/ucrt.c): off, toggled, set
- `cursor_shapes_test.py` -- The three gesture shapes: hand over a link, move, and not-allowed
- `cursor_theme_test.py` -- Cursor themes: loading, switching, sizing and the malformed-file floor
- `desktop_drop_test.py` -- dropping onto desktop icons, and their art
- `desktop_entries_test.py` -- Drive the .desktop entry system: the ShowIn= key, and live reload
- `desktop_many_test.py` -- a desktop with more icons than its old table
- `desktop_menu_test.py` -- the desktop's menus, its glass, Rename and Properties
- `devmgr_test.py` -- The Device Manager: its tree is drawn, its views switch, and Disable works
- `dialog_test.py` -- verify the confirm dialog's Yes/No buttons
- `doom_data_test.py` -- DOOM's game data and key sheet: the downloader, the launcher, F1
- `drag_image_test.py` -- The compositor's drag image: one image wherever the pointer is, saying
- `editmenu_test.py` -- Clipboard keys and the shared right-click edit menu, in a text field
- `fbs_test.py` -- First-boot setup: shown when it should be, saves what is chosen, ends
- `filedialog_test.py` -- the shared file chooser, as a window
- `fm_back_test.py` -- Back straight after a double click: the File Manager's history records
- `fm_bigdir_test.py` -- The File Manager lists a folder of any size: 600 files, not the first 256
- `fm_columns_test.py` -- The File Manager's details columns, sized, chosen and saved -- uui_table's
- `fm_conflict_test.py` -- The File Manager's Replace-or-skip card (ui/uui_compare.h): two files of
- `fm_group_test.py` -- The File Manager's View > Group by: the details view in bands under
- `fm_places_menu_test.py` -- The File Manager's left pane menus (userland/apps/apps/files/fm_places.c) and the
- `fm_progress_test.py` -- The File Manager's Operations panel (ui/uui_transfers.h over fm_jobs.c):
- `fm_quick_test.py` -- Quick access, the File Manager's start page (fm_quick.c): the
- `fm_search_filter_test.py` -- The File Manager's search filters (base/ufind.h): the strip's type,
- `fm_thumb_drag_test.py` -- The File Manager's icon view: dragging the scrollbar's thumb after
- `focus_state_test.py` -- clients hear the focus the WM routes by,
- `folder_options_test.py` -- the desktop follows Folder Options, and
- `forcequit_test.py` -- not-responding detection and force quit
- `frame_pacing_test.py` -- The compositor composes at most one frame per refresh, and a lone
- `fullscreen_test.py` -- the fullscreen state and the display lease
- `gfxdemo_test.py` -- drive the Shapes demo and assert it really draws
- `glass_test.py` -- Transparency (userland/apps/system/toywm/wm_glass.c): the taskbar, Start, menus and
- `half_frame_test.py` -- no half-painted window reaches the screen
- `help_test.py` -- The Help app: contents, links, history and full-text search
- `hexview_test.py` -- Hex Viewer (/bin/wm/apps/hexview: ui/uui_hexview, ui/uui_byteinspect, lib/ubinmap) on a GENERATED ELF
- `hlpview_test.py` -- Help Viewer (/bin/wm/apps/hlpview, lib/uhlp, ui/uui_helpview) on a GENERATED help file
- `hover_sweep_test.py` -- Hover over every named widget in every Start-menu app: nothing may happen
- `hover_test.py` -- A hover change must REPAINT, not just record damage
- `icons_test.py` -- application icons, from a .qoi file to the screen
- `idle_desktop_test.py` -- nothing paints over an IDLE desktop
- `idle_io_test.py` -- an idle machine stays off the disk
- `imgview_test.py` -- the JPEG decoder, all the way to the screen
- `input_backlog_test.py` -- Input a slow client cannot take at once WAITS for it; nothing typed is lost
- `input_test.py` -- `gui input`: a sequence of keys and pointer steps the window manager times, not the host
- `keyboard_layouts_test.py` -- keyboard_layouts_test -- several keyboard layouts: the list setting,
- `keyup_test.py` -- key RELEASES reach a ring-3 client, end to end
- `launch_test.py` -- Running programs and scripts from the File Manager, the desktop and
- `leave_test.py` -- The Leave page: Restart / Shut down / Exit to shell, and apps asked to close
- `live_wallpaper_test.py` -- Live wallpapers: the background client, what it draws, when it stops
- `logview_test.py` -- The Log Viewer, on a fixture log it is opened at (`logview <file>`)
- `menubar_test.py` -- the menu bar and status bar, against ring-3 Notepad
- `mines_test.py` -- Drive Minesweeper (userland/apps/apps/mines/mines.c) and assert on it
- `modeset_test.py` -- A runtime resolution change under a live desktop
- `mouse_buttons_test.py` -- a pointer button is an EDGE, not a level
- `netadapter_test.py` -- Drives a network card's ADAPTER SETTINGS end to end on QEMU's e1000
- `network_tray_test.py` -- The taskbar's network item: the icon's state, the panel, and the
- `notepad_client_test.py` -- Drive the RING-3 Notepad (userland/apps/apps/notepad/notepad.c) and assert on it
- `notepad_options_test.py` -- Notepad Options: saved, applied, remembered
- `notify_test.py` -- notifications (userland/apps/system/toywm/wm_notify.h,
- `occlusion_test.py` -- Visible regions (userland/apps/system/toywm/wm_render.c): a window covered by an
- `osk_test.py` -- the on-screen keyboard (userland/apps/system/toywm/osk.c)
- `pager_test.py` -- The shared pager: it draws, it turns a page, and it puts the terminal back
- `paint_test.py` -- Paint, end to end, read back from the PICTURE
- `pins_gui_test.py` -- Pin to Places in the File Manager (base/upins.h): a folder's context
- `pins_test.py` -- pinned taskbar apps (userland/apps/system/toywm/wm_taskbar.h,
- `player_test.py` -- the Audio Player, on a machine with NO SOUND
- `popup_test.py` -- a menu leaves its window: popup surfaces
- `properties_test.py` -- Properties: the facts it shows, the sections it opens, and the edits it makes
- `quick_test.py` -- Quick Settings (userland/apps/system/toywm/wm_quick.h): the
- `recent_gui_test.py` -- Recent in the File Manager (base/urecent.h, fm_recent.c), checked
- `rename_gui_test.py` -- Rename many in the File Manager (base/urename.h, ui/uui_renamer.h),
- `resize_edges_test.py` -- Resize a window from all EIGHT edges and corners, not just the grip
- `resize_stride_test.py` -- a resized window is composited at the
- `retro_list_test.py` -- Retro Launcher's list: the Details view, the search box and the order, as a person uses them
- `scale_test.py` -- the display scale (`display.scale`, fontd's
- `sched_gui_test.py` -- Prove the desktop stays ALIVE while a ring-3 process runs
- `screen_surface_test.py` -- Drive userland/tests/screenclient.c -- The GUI in ring 3, stage 4b's ring-3
- `screensaver_test.py` -- The screensaver: the idle clock, the two settings, and the savers
- `screenshot_options_test.py` -- Screenshot's options: what each one DOES to the next capture, read back
- `screenshot_test.py` -- Screen capture: /bin/screenshot, the Screenshot app, and the region band
- `scroll_test.py` -- Scrolling, as a person does it: the wheel goes the right way, the scrollbar thumb follows the view, a click in
- `scrollbar_test.py` -- scrollbar BEHAVIOUR, against the ring-3 Notepad
- `search_gui_test.py` -- Search into subfolders in the File Manager (fm_search.c over
- `settings_gallery_test.py` -- System Settings' galleries and previews
- `settings_test.py` -- Drives the RING-3 System Settings app (userland/apps/system/settings/settings.c)
- `shadow_test.py` -- Drop shadows under windows and menus (userland/apps/system/toywm/wm_shadow.c), and
- `shortcut_test.py` -- Global keyboard shortcuts: the bindings, the Super gesture, and rebinding
- `single_instance_test.py` -- Single-instance apps: WIN_REQ_ACTIVATE and UAPP_SINGLE_INSTANCE
- `sizes_gui_test.py` -- Folder sizes in the File Manager (View > Folder sizes; fm_sizes.c over
- `smooth_scroll_test.py` -- Smooth scrolling (ui/uui_scrollanim.h) in the File Manager's icon
- `snap_test.py` -- window snapping (userland/apps/system/toywm/wm_snap.h): edge and
- `sndformat_test.py` -- a sound card's Format panel (ui/uui_sndformat.h)
- `start_menu_test.py` -- the Start menu's folders, search and keyboard
- `start_settings_test.py` -- the Start menu's settings (Desktop > Start)
- `switcher_test.py` -- Alt+Tab (userland/apps/system/toywm/wm_switcher.h): the
- `sync_test.py` -- `gui sync` and settle(): after a click and settle(), the frame the click caused is ALREADY on the screen
- `sysupdate_channels_test.py` -- System Update's channels: one server address, the server's own list
- `sysupdate_test.py` -- System Update: it finds what changed, shows the install, and installs it
- `tabs_gui_test.py` -- Tabs in the File Manager's title bar: drawn by the compositor
- `taskbar_drag_test.py` -- dragging taskbar buttons to reorder
- `taskbar_menu_test.py` -- the taskbar's empty-strip menu and the Start button's
- `taskbar_peek_test.py` -- the taskbar's window preview
- `taskbar_style_test.py` -- the taskbar's independent layout
- `taskmgr_test.py` -- Drive the ring-3 Task Manager: its three pages, the table, and its verbs
- `theme_test.py` -- themes (userland/libs/ui/utheme_file.h), the
- `thumbcache_test.py` -- The File Manager's thumbnail RATE, and the disk cache under it
- `trash_gui_test.py` -- The Recycle Bin in the File Manager: Delete, Shift+Delete, the bin's
- `trash_limits_test.py` -- the Recycle Bin's settings: shown or not,
- `tray_press_test.py` -- The notification area's feedback: a rounded fill on hover, a
- `uapp_test.py` -- the TWP resize handshake, end to end
- `uiclient_test.py` -- Drive the text-rendering ring-3 client (userland/tests/uiclient.c)
- `uidemo_test.py` -- drive UI Demo's widgets and assert on its log
- `undo_gui_test.py` -- Undo and Redo in the File Manager (base/ufileundo.h), each checked
- `uterm_test.py` -- Drive the RING-3 Terminal (userland/apps/apps/terminal/terminal.c) and assert on it
- `video_test.py` -- the Video Player, on a machine with NO SOUND
- `volume_test.py` -- The taskbar's volume flyout: the slider, mute, the wheel and the
- `w16_gui_test.py` -- w16 running Win16 programs in toywm (docs/w16-design.md), checked by pixel
- `wait_test.py` -- `gui wait` / `gui waited` and `gui sync TITLE`: conditions the window manager decides
- `wallpaper_mode_test.py` -- `desktop.wallpaper.mode`: does fit differ from fill, and is the
- `winclient_test.py` -- Drive the ring-3 client window (userland/tests/winclient.c) and assert on it
- `window_geometry_test.py` -- windows come back where you left them
- `window_identity_test.py` -- a held window stays the SAME window

### `tools/tests/sweep/` -- on demand; a missing precondition is a skip

Full entries: [`docs/tools/tests-sweep.md`](tools/tests-sweep.md).

- `ahci_test.py` -- Boot SaunaOS with its filesystem on a SATA drive behind an AHCI HBA
- `ansi_cursor_test.py` -- ANSI cursor movement and erasing, checked as PIXELS on the console
- `audio_loopback_test.py` -- Measure what the G6 actually PLAYS, by recording it back on line in
- `audio_rate_test.py` -- a card's rate follows what plays, judged
- `audio_test.py` -- AC97 and the PCM stream, end to end, judged
- `bigdisk_test.py` -- a disk past 2 TiB, through each 64-bit driver
- `boot_entry_test.py` -- A one-shot GRUB entry is taken ONCE, and GRUB is what ends it
- `boot_migrate_test.py` -- A machine with the old /boot/boot layout moves up to /boot by itself,
- `boot_splash_test.py` -- The boot splash: the logo from the framebuffer's first moment to the
- `bootcfg_test.py` -- `bootcfg` against a real /boot: edits land, refusals change nothing,
- `component_test.py` -- /bin/component end to end: remove an optional component, update past it, add it back
- `console_bleed_test.py` -- The kernel console must not paint over the desktop -- and must not lose the text
- `console_shell_test.py` -- A `text` boot reaches a RING-3 shell prompt, and the kernel shell is
- `cross_gcc_test.py` -- The host's x86_64-saunaos-gcc builds a program that runs on SaunaOS
- `cross_gxx_test.py` -- The host's x86_64-saunaos-g++ builds a C++ program that runs on SaunaOS
- `ctrlc_test.py` -- Ctrl-C at the physical console interrupts the foreground JOB, and
- `cursor_ibeam_test.py` -- Named pointer shapes: the I-beam, the busy pointer, and the clamp
- `dash_gap.py` -- What the dash port still needs from tolibc, measured rather than listed
- `dash_test.py` -- Does the vendored dash actually BEHAVE like a shell?
- `debug_tty_test.py` -- the serial debug console is a terminal
- `devclaim_test.py` -- a ring-3 process takes the sound card off
- `device_count_test.py` -- More devices than the old fixed tables held: PCI past 32, USB past 8
- `disc_desktop_test.py` -- Disc images on the desktop: an .iso mounted as a CD drive from the File
- `diskmark_test.py` -- Drive Disk Mark (userland/apps/apps/diskmark/diskmark.c) and assert on it
- `disks_resize_test.py` -- Disks' Resize dialog (R1) on a guest of its own
- `disks_test.py` -- Disks (userland/apps/apps/disks/disks.c) on a guest of its own
- `doc_test.py` -- /bin/doc: the pages reach the machine, and the command finds them
- `doom_sound_test.py` -- DOOM's sound effects and music, judged on
- `doom_test.py` -- DOOM runs, draws, animates and takes input
- `dup_scan.py` -- Where is this tree actually duplicating code? -- measure, don't guess
- `fat32_grow_test.py` -- `part resize` grows a FILLED FAT32 volume, both ways, and shrinks it
- `fat32_test.py` -- FAT32 and the mount table, checked against an INDEPENDENT implementation
- `faulttest_run.py` -- Runs the ring-3 diagnostics that FAULT ON PURPOSE, and asserts on what
- `filemanager_test.py` -- Drive the File Manager (userland/apps/apps/files/files.c) and assert on it
- `fileop_test.py` -- lib/ufileop through /bin/cp, /bin/mv and /bin/rm
- `fm_network_test.py` -- The File Manager's Network rows: Sign in, Add network share, and a share that does not answer
- `font_test.py` -- Runtime fonts: loading a TTF from disk, switching faces live, and
- `frame_balance.py` -- Does a process's teardown balance? -- physical frame accounting
- `fs_isolation.py` -- does I/O on one path slow the filesystem down on another?
- `fs_switch_test.py` -- prove live filesystem switching end-to-end
- `fstorture_test.py` -- Filesystem torture, then power cuts: TFS3, FAT32 and ramfs against a model
- `grep_test.py` -- /bin/grep, driven through a real ring-3 shell
- `guictl_test.py` -- /bin/guictl -- the window manager's diagnostics, asked from ring 3
- `hdacodec_test.py` -- a ring-3 driver reads the codec graph and
- `highmem_consume.py` -- Consume past 4 GiB from ring 3, and require the allocator's books to balance
- `highmem_test.py` -- The frame allocator on a guest with more than 4 GiB: managed, idle, zoned
- `hires_test.py` -- A desktop above 1280x720, and a client window that can actually fill it
- `http_test.py` -- Drive libhttp's HTTP/1.1 half through /bin/wget against a server here
- `https_test.py` -- Drive /bin/wget's HTTPS path against a TLS server on this machine
- `hwdata_test.py` -- Drive `hwdata update` against a server on this machine
- `init_test.py` -- init as pid 1: the target, service supervision, adoption and reaping
- `install_test.py` -- install_test -- does SaunaOS install itself onto another disk, and does
- `installer_test.py` -- the Install SaunaOS window, on the live ISO,
- `iso9660_test.py` -- iso9660_test.py -- disc images mounted from a file, checked against xorriso
- `jobs_test.py` -- Ctrl-Z suspends a job, `jobs` lists it, and `fg` brings it back
- `kbd_test.py` -- `kbd` reports what the keyboard actually did, on both input drivers
- `kdebug_test.py` -- The kernel debugger: a GDB stub over COM3 or the network, end to end
- `keyboard_paths_test.py` -- The same keys do the same thing on PS/2 and on virtio-input
- `latency_under_io.py` -- what heavy disk I/O, or a busy CPU, does to desktop latency
- `libc_diff.py` -- tolibc's formatter, number parsers and maths against glibc, case by case
- `live_boot_test.py` -- the ISO boots and is USABLE with no disk
- `logrotate_test.py` -- `logd`'s per-boot retention: /var/log/boot/<n>.log and `log -p N`
- `ls_test.py` -- /bin/ls: format flags, ordering, colour, and a directory past the page
- `many_files_test.py` -- apps given more than their old tables held
- `mem_stress.py` -- Several memory hogs at once: does the machine survive, and is the memory right?
- `mixer_test.py` -- the per-application volume, from the tray
- `module_test.py` -- Loadable kernel modules, end to end, in a running guest
- `msi_test.py` -- the Local APIC, and the xHCI delivering through
- `multidisk_test.py` -- two disks on two drivers, and root= choosing
- `native_cc_test.py` -- GCC ON SaunaOS: C and C++ compiled on a guest run as the host's build does
- `native_make_test.py` -- GNU make ON SaunaOS: a Makefile builds, rebuilds and cleans as the host's make does
- `net_test.py` -- the network stack, on both NICs, judged on the HOST
- `netheal_test.py` -- `/bin/netheal`: reboot (twice at most) when a machine comes up with no network
- `ntp_test.py` -- network time, end to end, against a LOCAL server
- `nvme_test.py` -- the root filesystem on an NVMe SSD, and a 4K namespace beside it
- `oom_test.py` -- Run a small guest out of memory: the BIGGEST process is ended, not the one that asked
- `panic_store_test.py` -- The panic store: a kernel panic survives the warm reset that follows it
- `part_test.py` -- `part delete` against a table a FOREIGN tool wrote
- `partition_test.py` -- Boot SaunaOS with its filesystem INSIDE an MBR or GPT partition
- `ping_rtt.py` -- the compositor <-> client round trip, in microseconds
- `poweroff_test.py` -- Prove the machine stops through ITS OWN ACPI tables, not a hardcoded port
- `remote_test.py` -- telnetd, tftpd and tools/dev/remote.py, end to end against a QEMU guest
- `rename_migrate_test.py` -- A machine installed as toy-os keeps its settings and log when it boots
- `rootdisk_test.py` -- every SATA drive, and which disk is the root
- `sanitize_run.py` -- Build SaunaOS with UBSAN=1 and/or KASAN=1 and list every site that fires
- `screentrial_test.py` -- A screen size tried at a restart: GRUB sets it once, screentrial asks,
- `sector4k_test.py` -- a disk with 4096-byte sectors, end to end in QEMU
- `serial_backpressure_test.py` -- a stalled COM1 reader must not
- `shutdown_sync_test.py` -- A write made just before `reboot` must survive it
- `shutdown_test.py` -- init stops the services in REVERSE order on reboot
- `smb_test.py` -- Network shares in a guest: /bin/smb, `mount -t smb`, and saved shares under /net
- `soundd_test.py` -- the sound daemon: TWO programs audible at once,
- `stdin_test.py` -- Blocking stdin (fd 0) and the standalone ring-3 shell, /bin/tosh
- `sum_test.py` -- /bin/sum -- the command, in a guest, against the HOST's own digests
- `taskbar_test.py` -- open enough windows to overflow the taskbar,
- `terminal_probe.py` -- The GUI Terminal, asked what it actually does: keys, paging, scrolling
- `tfs3_grow_test.py` -- `part resize` grows a FILLED tfs3 volume twice, then shrinks it
- `tfs3_v1_test.py` -- Prove the kernel still mounts and uses a TFS3 **v1** image
- `trash_test.py` -- lib/utrash through /bin/trash: the Recycle Bin, checked as text
- `uefi_boot_test.py` -- A UEFI boot off the disk's own loader, and the screen size it can reach
- `update_test.py` -- /bin/update end to end: fetch, verify, install live, and stage for boot
- `usb_audio_test.py` -- USB Audio Class 1.0 playback, end to end,
- `usb_desktop_test.py` -- USB sticks on the desktop: the tray's USB drives flyout, the File
- `usb_multi_test.py` -- USB on TWO xHCI controllers, each driven as its own bus
- `usb_storage_test.py` -- USB sticks on QEMU: bulk-only transport, quick removal, a pull, a re-plug
- `usb_test.py` -- USB: an xHCI controller, and a HID boot keyboard and mouse
- `utf8migrate_boot_test.py` -- The utf8migrate SERVICE across two boots, judged on the host
- `virtio_boot_test.py` -- Boot SaunaOS with NO IDE controller and its filesystem on virtio-blk
- `virtio_gpu_test.py` -- the virtio GPU, end to end
- `virtio_input_test.py` -- virtio keyboard, mouse and tablet
- `vnc_test.py` -- /bin/remoted's VNC server, end to end, from a VNC client written here
- `w16_install_test.py` -- `w16 --install` and its dialog, in a guest, from disks with no Microsoft file
- `w16_sound_test.py` -- a Win16 program's sounds, judged on the HOST

### `tools/tests/manual/` -- by hand only -- each says why

Full entries: [`docs/tools/tests-manual.md`](tools/tests-manual.md).

- `damage_hunt.py` -- run damage_sweep.py over MANY seeds, one VM each
- `damage_sweep.py` -- exercise the WM against its damage invariant
- `kvm_soak.py` -- run the desktop under KVM, across FRESH BOOTS,
- `libvnc_check.py` -- /bin/remoted against a FOREIGN VNC client: libvncclient, what Remmina uses

### `tools/hostchecks/` -- our C built with the host gcc, judged by a foreign oracle

Full entries: [`docs/tools/hostchecks.md`](tools/hostchecks.md).

- `archive_hostcheck.py` -- Check lib/uzip and lib/utar against Python's zipfile, tarfile and gzip, on the HOST
- `browser_hostcheck.py` -- Lay out the same pages with NetSurf on SaunaOS and on the host, and compare
- `caret_hostcheck.py` -- Check userland/libs/ui/uui_caret.c -- the one blink every text caret shares --
- `components_hostcheck.py` -- The optional components' build half, on the HOST: the check, the staging, the manifest tags
- `cpu16_hostcheck.py` -- Check w16's 16-bit CPU interpreter against the real CPU, on the host
- `des_hostcheck.py` -- Check userland/libs/base/udes.c against OpenSSL's DES, on the HOST
- `divti3_hostcheck.py` -- Check tolibc's 128-bit division helpers against Python bignums, on the HOST
- `exfat_hostcheck.py` -- SaunaOS's exFAT, judged by the host: fsck.exfat and an independent reader
- `filemanager_harness_hostcheck.py` -- filemanager_test.py's own harness, checked on the host with no guest
- `hash_hostcheck.py` -- Check /lib/libhash.so's algorithms against hashlib and zlib, on the HOST
- `heap_hostcheck.py` -- Fuzz kernel/lib/heap_core.c on the HOST, with its own invariants checked
- `hid_parse_hostcheck.py` -- Check kernel/lib/hid_parse.c against REAL report descriptors, on the host
- `indeo3_hostcheck.py` -- Check userland/libs/media/uvid_indeo3.c (Indeo Video 3) against FFmpeg, on the HOST
- `midi_hostcheck.py` -- Check the MIDI codec and SoundFont synth on the HOST, against FluidSynth
- `ora_hostcheck.py` -- Check lib/uora (OpenRaster) against Python's zipfile, ElementTree and Pillow, on the HOST
- `regex_hostcheck.py` -- tolibc's regex against glibc's, on the HOST
- `settings_harness_hostcheck.py` -- settings_test.py's own wait and geometry logic, on the host, no guest
- `smb_hostcheck.py` -- Check the SMB client (/lib/libsmb.so's protocol) against a real Samba, on the HOST
- `teapot_hostcheck.py` -- Check Shapes' teapot mesh (userland/apps/demos/gfxdemo/teapot.c) and the depth-
- `term_scheme_hostcheck.py` -- Check the shipped Terminal colour schemes against what the loader does
- `tree_lock_hostcheck.py` -- The test lock's PreToolUse hook refuses every way a source file can be
- `uadpcm_hostcheck.py` -- Check userland/libs/media/uadpcm.c (MS and IMA ADPCM) against ffmpeg, on the HOST
- `ubinmap_hostcheck.py` -- Check lib/ubinmap (the Hex Viewer's structure decoders) and lib/ubyteval (its inspector) on the HOST
- `ucolour_hostcheck.py` -- Check userland/libs/base/ucolour.c's HSL and HSV conversions against
- `ucrt_hostcheck.py` -- Check userland/libs/ui/ucrt.c -- the CRT effect -- with the host gcc, against
- `ufatimg_hostcheck.py` -- Check base/ufatimg.c -- FAT12/FAT16 disk images read from memory
- `ugfx_fill_hostcheck.py` -- Check ugfx's anti-aliased fills against Pillow, on the HOST
- `ugfx_pix_hostcheck.py` -- Check ugfx_pix's SIMD tiers BYTE FOR BYTE against its C table, on the host
- `ugfx_text_hostcheck.py` -- Check the toolkit's text measurement and uui_textbox's geometry on the HOST
- `uhlp_hostcheck.py` -- Check userland/libs/base/uhlp.c (the Windows Help file reader) on the HOST
- `uhlpmac_hostcheck.py` -- Check userland/libs/base/uhlpmac.c (the WinHelp macro language) on the HOST
- `uimg_codec_hostcheck.py` -- Check uimg's QOI, PNG, BMP and GIF codecs against Pillow, zlib and ImageMagick, on the HOST
- `uimg_hostcheck.py` -- Check userland/libs/media/uimg_jpeg.c against libjpeg, on the HOST, over many files
- `umd_hostcheck.py` -- Check userland/libs/base/umd.c against every page it will ever render, on the HOST
- `umsexpand_hostcheck.py` -- Check base/umsexpand.c -- the SZDD and KWAJ expander -- three ways
- `upixel_hostcheck.py` -- Check userland/libs/media/upixel.c and upixundo.c -- the paint tools and their
- `uregion_hostcheck.py` -- Check userland/libs/base/uregion.c -- the compositor's visible-region
- `urgn_hostcheck.py` -- Check userland/libs/base/urgn.c -- the growable region w16's GDI regions and
- `usnd_hostcheck.py` -- Check userland/libs/media/usnd_mp3.c against ffmpeg and usnd_flac.c against flac, on the HOST
- `usolid_hostcheck.py` -- Check usolid_slice() (userland/libs/ui/usolid.c) -- the Desktop cube's
- `utext_hostcheck.py` -- Check userland/libs/ui/utext.c's wrap accounting against a naive oracle, on the HOST
- `utween_hostcheck.py` -- Check userland/libs/base/utween.c -- the desktop's one easing tween -- with
- `uvid_hostcheck.py` -- Check userland/libs/media/uvid*.c (MPEG-1 video, the program stream) and
- `uwinsetup_hostcheck.py` -- Check base/uwinsetup.c -- Windows 3.x installed from its disks
- `uwmf_draw_hostcheck.py` -- Check userland/libs/ui/uwmf_draw.c (a metafile played into a box) on the HOST, against stub drawing calls
- `uwmf_hostcheck.py` -- Check lib/uwmf's metafile decoder against an independent writer, and against hostile files
- `w16_hostcheck.py` -- Check w16's NE parser against a second reader, and against hostile files
- `winfnt_hostcheck.py` -- Check userland/libs/media/winfnt.c against FreeType's winfnt driver, on the HOST

### `tools/checks/` -- static rules over the source, all in preflight

Full entries: [`docs/tools/checks.md`](tools/checks.md).

- `check_chains.py` -- the write_dec/write_hex chains are FROZEN
- `check_components.py` -- Refuse a program directory the build would misread
- `check_config_size.py` -- No shipped config file may exceed the sanity bound; no descriptor text its ABI field
- `check_control_states.py` -- Refuse an app whose menu bar or toolbar never says when a command cannot act
- `check_copy_user.py` -- a vmm_copy_*_user() result tested as if it
- `check_deps.py` -- prove the build's header dependency tracking works
- `check_dispatch.py` -- Refuse a dispatch chain that has grown big enough to want a table
- `check_docs.py` -- the documentation rules that rotted before
- `check_drivers.py` -- Refuse a driver that declares itself to nothing, and a driver file
- `check_font_state.py` -- Refuse a function that SELECTS a font and does not put back the one it found
- `check_initcalls.py` -- an init that is both an INITCALL and a
- `check_key_routing.py` -- Refuse a GUI app that declares a key-taking widget and routes no keys
- `check_layout.py` -- Verify disk.img's directory layout matches docs/filesystem-layout.md
- `check_licenses.py` -- Every vendored port, shipped font, palette and notice file is named in LICENSE
- `check_optional_components.py` -- The optional components in data/components/ are well-formed, disjoint and GPL-complete
- `check_runners.py` -- A test no runner runs: one outside tools/tests/ and hostchecks/, or without a RUNNER header
- `check_signals.py` -- Every signal abi/signal_abi.h defines has a name in kernel/lib/ksignal.c
- `check_syscalls.py` -- No two syscalls share a number
- `check_test_sleeps.py` -- a test tool may not gain a FIXED SLEEP
- `check_text_loops.py` -- Fail the build on a BYTE of a string measured or drawn as a CHARACTER
- `check_text_measure.py` -- Fail the build on a character COUNT used as a text WIDTH
- `check_tool_commands.py` -- Every guest command a tool drives still exists
- `check_widget_ops.py` -- Refuse a `struct uui_widget_ops` table with a slot it needs left NULL

### `tools/lib/` -- the harness: vm.py, qmp_test, gui_debug, harness, toolpath...

Full entries: [`docs/tools/lib.md`](tools/lib.md).

- `components.py` -- The optional components (docs/software-design.md), read and checked on the host
- `fresh_disk.py` -- A pristine disk image, and whether an existing one has drifted
- `gui_debug.py` -- talk to SaunaOS's serial debug console, and to
- `gui_flow.py` -- named, composable QMP click-flows on top of
- `harness.py` -- What every test tool here had written for itself: a results table, a
- `hostcheck.py` -- The build half every `*_hostcheck.py` had written for itself
- `iso_guard.py` -- Refuse to boot a stale saunaos.iso
- `mkpart_test.py` -- writes a synthetic legacy MBR or GPT
- `panic_resolve.py` -- Resolve a pasted panic into function names and source lines
- `pixel_probe.py` -- Read exact pixel values out of a screenshot, and compare them across
- `port_guard.py` -- Refuse to start a guest on a QMP or VNC port another guest already holds
- `preconditions.py` -- Can a test run on this machine at all? -- the `needs` keys of a RUNNER header
- `preflight_stamp.py` -- A record that preflight passed on THIS tree, and a check against it
- `private_tmp.py` -- A private TMPDIR for one test tool, deleted when it returns
- `qmp_test.py` -- Reusable helpers for driving SaunaOS in headless QEMU over QMP
- `remote_gui.py` -- Driving the BARE-METAL machine with the tools written for a VM
- `runner_meta.py` -- What runs each test, read from the test itself
- `serial_boot.py` -- Boot a guest whose serial console is a TCP server, and drive it
- `serial_console.py` -- Boot a guest with COM1 as a SOCKET, and drive it as text in / text out
- `shell_flow.py` -- a named, composable QMP flow for the PHYSICAL
- `test_history.py` -- A local record of every test run: when did this check last fail, or pass?
- `toolpath.py` -- Where everything in tools/ lives, for scripts that live in its folders
- `tree_lock.py` -- The TEST LOCK: nothing rebuilds or edits the tree while a test is using it
- `vm.py` -- Manage a headless SaunaOS VM and talk to it in TEXT

### `tools/dev/` -- working on the OS: remote.py, update/web/samba servers, mutate, predates

Full entries: [`docs/tools/dev.md`](tools/dev.md).

- `acpi_dump.py` -- Pull an ACPI table out of a running guest, and prove it arrived intact
- `aml_walk.py` -- Walk a DSDT dump on the host and say how its PCI interrupt routing is
- `backup_repo.sh` -- Take a complete, restore-tested backup of the GitHub repository
- `code_identity.py` -- Do two builds contain the same code? The check after a source-tree move
- `kdebug_bridge.py` -- Carry GDB to the kernel debugger's NETWORK transport
- `loc.py` -- how big is this project, honestly
- `mutate.py` -- A positive control, done safely: break the code on purpose, show the test goes red
- `predates.py` -- Did this failure exist before my changes? Measure it, don't guess
- `remote.py` -- Drive a SaunaOS machine over the network: run commands, move files
- `samba_server.py` -- A throwaway Samba server in Docker, for testing the SMB client
- `serial_capture.py` -- read a running VM's serial console RAW
- `tree_move.py` -- Move source paths and rewrite every reference to them, from a mapping table
- `ui_push.py` -- Send an edited window file (.ui) to a running guest and restart its program
- `ui_snapshot.py` -- Every rect and every pixel of a window, to compare two builds of it
- `update_server.py` -- Serve SaunaOS builds to machines running /bin/update, on three channels
- `wait_for.sh` -- Wait for a PID to exit, or for a file to appear -- with a MANDATORY
- `watch_vm.sh` -- Attach a VIEW-ONLY VNC viewer to a headless SaunaOS VM, so a test run
- `web_server.py` -- A web server on THIS machine for testing libhttp and the Web Browser

### `tools/bench/` -- measurements with numbers and no verdict

Full entries: [`docs/tools/bench.md`](tools/bench.md).

- `boot_rate.py` -- Reboot a bare-metal machine N times and count how often something appears
- `host_latency.py` -- Holds this HOST's CPUs out of their deep idle states while a guest's
- `idle_cpu.py` -- how much CPU does SaunaOS burn doing NOTHING?
- `sys_bench.py` -- What a system call, a context switch and a read or write cost, on one build
- `timer_bench.py` -- Measure what a timer configuration costs and buys, on one booted build
- `vblank_pacing.py` -- Whether toywm's frames land on the vblank, measured on a machine whose
- `vnc_latency.py` -- How long a VNC viewer waits for the screen to answer -- a number for

### `tools/build/` -- what the Makefile and releases call: seed_disk, tfs3_writer, versions

Full entries: [`docs/tools/build.md`](tools/build.md).

- `build_conf.py` -- Resolve build.conf: which drivers are modules, and how the build is tuned
- `build_native_make.sh` -- GNU make that RUNS ON SaunaOS
- `build_native_toolchain.sh` -- GCC and binutils that RUN ON SaunaOS
- `build_toolchain.sh` -- the x86_64-saunaos cross toolchain
- `check_lib_layers.py` -- Refuse a shared library that uses a symbol from a library to its RIGHT
- `gen_kconfig.sh` -- Writes the kernel's build-time timer configuration as a header:
- `gen_version.sh` -- Regenerates kernel/include/api/version.h from VERSION (repo root, plain
- `install_grub.py` -- put GRUB, and the kernel, ON disk.img
- `ovmf.py` -- QEMU's UEFI firmware (OVMF), found and wired up once
- `release_assets.sh` -- The assets of a GitHub Release, made only from a build that IS the tag
- `run_release.sh` -- Launches a downloaded SaunaOS release with the device and display
- `seed_disk.py` -- format-aware seeding front-end
- `set_version.sh` -- Sets VERSION (repo root, plain text, one line) to a new value. This
- `stage_components.py` -- Stage the optional components into /usr/share/components, checked, with their sizes
- `stage_sdk.py` -- Stage the SDK -- the headers and libraries a program for SaunaOS is built against
- `strip_staged.py` -- Strip the DWARF from every program and shared library staged for an image
- `tfs3_writer.py` -- host-side TFS3 v1 read/write tool
- `todisk.py` -- Lay the files in todisk/ over a disk image's TFS3 volume

### `tools/gen/` -- generators of checked-in data and headers

Full entries: [`docs/tools/gen.md`](tools/gen.md).

- `gen_audio.py` -- the WAV files that ship with SaunaOS
- `gen_bootwords.py` -- build/gen/bootwords.h: every kernel boot word, from docs/boot-flags.md
- `gen_commands_index.py` -- regenerate docs/commands/README.md's index
- `gen_cursors.py` -- Generate the cursor themes into data/cursors/<theme>/
- `gen_decisions_index.py` -- regenerate docs/decisions.md from docs/decisions/
- `gen_icons.py` -- Draw the application icons into data/icons/*.qoi
- `gen_imgdata.py` -- Generate the image data this repo ships: the test vectors and the wallpapers
- `gen_kbs.py` -- generates /usr/share/kbs/<layout> keyboard-layout data
- `gen_libm_tables.py` -- tolibc's maths constants, to the last bit
- `gen_modalias.py` -- Write /lib/modules/modules.alias from the built .ko files, and refuse
- `gen_mp3_tables.py` -- regenerate userland/libs/media/usnd_mp3_tables.h
- `gen_music.py` -- the music files that ship with SaunaOS: MP3s, and
- `gen_ne.py` -- build a Win16 (NE) executable from one NASM source
- `gen_next_up.py` -- regenerate the "Next up" section of docs/roadmap.md
- `gen_saver_thumbs.py` -- Draw each screensaver's gallery picture: data/wm/savers/<name>.jpg
- `gen_sf2.py` -- the built-in General MIDI SoundFont, toy-gm.sf2
- `gen_signames.py` -- signames.c for the dash port, from SAUNAOS's signal numbers
- `gen_syms.py` -- Bake the kernel's function symbol table into the image, for panics
- `gen_toolkit_index.py` -- Generate docs/toolkit.md: every ring-3 toolkit header, one line each
- `gen_tools_index.py` -- Regenerate docs/tools.md's folder index: every tool, by folder, in one line each
- `gen_unicode_names.py` -- the Unicode NAMES and BLOCKS of every
- `gen_video.py` -- the video files that ship with SaunaOS, and the
- `gen_w16_calls.py` -- the pages that say what each Windows call w16
- `gen_w16_ordinals.py` -- the Win16 API's ordinals and names, for w16
- `gen_wcwidth.py` -- the East Asian Wide table a grid of cells
- `gen_win3_disks.py` -- A Windows 3.x SETUP DISK SET with no Microsoft file in it
- `genrelocs.py` -- Build the kernel's own relocation table, for kernel ASLR (Memory protection hardening)
- `genttf.py` -- Bakes a real TrueType font into static anti-aliased bitmap fonts for
- `hlpgen.py` -- Writes small WinHelp 3.1 (.HLP) files, so the reader has input that is not Microsoft's
- `icon_art.py` -- The COLOUR icons -- apps, folders, file types, drives and the Start
- `mscompress.py` -- COMPRESS.EXE's two formats, WRITTEN -- SZDD and KWAJ (methods 0-3)

### `tools/fetch/` -- downloads, each behind its licence

Full entries: [`docs/tools/fetch.md`](tools/fetch.md).

- `fetch_ca_bundle.py` -- Fetch Mozilla's CA root bundle into data/etc/ssl/certs/
- `fetch_extras.py` -- the optional, differently-licensed material
- `fetch_soundfont.py` -- put a full General MIDI SoundFont where the
- `fetch_toolchain.py` -- the GCC, binutils, GMP, MPFR, MPC and GNU make source tarballs
- `fetch_video.py` -- a real film for the Video Player: Big Buck Bunny
- `fetch_wad.py` -- put a Doom IWAD where the build will seed it
- `netsurf_vendor.py` -- Vendor NetSurf and its libraries into userland/third_party/<port>/upstream/, at pinned commits

<!-- END folder index -->

---

## Host tools this repo expects (not in `tools/`)

Installed on the maintainer's machine rather than checked in. Nothing
here is required to BUILD SaunaOS — `make all`, `make iso` and
`preflight.sh` work without every one of them — but each removes a
rederive-from-scratch cost, which is the same bar `tools/` holds itself
to. Arch package names; all are in the official repos.

- **`bear`** — generates `compile_commands.json` from the real build:
  `bear -- make all` (start from `make clean`, or it only captures what
  actually recompiled). Gitignored, because it holds machine-specific
  absolute paths.

  **This is what makes `clangd` work on this repo**, and it earns its
  keep on a codebase of this shape: `userland/libs/ui/` alone is a couple of
  dozen widgets whose ops tables and helper signatures are easy to guess
  wrong.
  Writing `uui_tree.c` without it cost a build cycle to five wrong
  guesses in one file — `uui_scrollbar_draw`'s arity, three `uui_widget_ops`
  function-pointer types, and `KEY_UP` where this kernel spells it
  `KEY_ARROW_UP`. Regenerate it after adding a source directory.

- **`ccache`** — wired into the Makefile as `CC = $(CCACHE) gcc`, which
  falls back to plain `gcc` when it is not installed, so a checkout
  without it builds identically. It pays for itself on the GATE rather
  than on an ordinary edit: `preflight.sh` and `make verify` both start
  with `make clean`, so every run is a full rebuild of a tree that
  mostly did not change. Measured here: **2.37s → 0.40s** for
  `make clean && make all`. It hashes preprocessed source plus flags, so
  a CFLAGS change correctly MISSES the cache — which matters, because
  the `.d` files do not track flags at all (CLAUDE.md).

- **`ruff`** — `ruff check tools/`, configured by `ruff.toml` at the
  repo root. Deliberately narrow (`F` + `E9`): the default ruleset
  reports ~320 findings here and essentially all are style. What is
  selected is the class this repo actually gets bitten by — **harness
  bugs that report a healthy system as broken**. Not in `preflight.sh`,
  for the same reason Docker is not: the gate must not start requiring
  a tool a checkout may not have.

- **`shellcheck`** — `shellcheck tools/*/*.sh`. Small surface (six
  scripts) and directly relevant: the repo has already been bitten by a
  `pkill ...; rm ...` chain aborting under `errexit` because `pkill`
  exits 1 when nothing matched, which shellcheck flags directly. It
  found one real thing on first run: an unguarded `cd` in
  `preflight.sh`, which would have run the whole gate — `make clean`
  included — in the caller's directory if it ever failed.

- **`clang-tidy` / `scan-build`** — a second opinion on the C, usable
  only once `bear` has produced the compilation database. Building the
  kernel with `clang` occasionally is worth it for the same reason: a
  different compiler's warnings find real bugs a single toolchain hides.

- **`graft`** — the code index behind `graft ask`/`graft grep` and the
  `saunaos-search` agent; `graft/` is gitignored, so each checkout builds
  its own. **Build it WITHOUT `userland/third_party/<port>/upstream/`**, or every query is
  answered with vendored NetSurf code, and `--in userland/` cannot
  filter it out because the ports sit inside it. graft has no exclude,
  so it is an allow-list, remembered for later rebuilds; a new
  `userland/` folder is not indexed until it is added:

  ```
  graft build --only-dir kernel --only-dir tools \
      $(for d in userland/*/; do d=${d%/}; \
          [ "$d" = userland/third_party ] || printf -- '--only-dir %s ' "$d"; done) \
      $(for g in userland/third_party/*/glue; do printf -- '--only-dir %s ' "$g"; done) .
  ```

  **Its hooks are deliberately NOT in `.claude/settings.json`** -- only
  the `graft`/`trail` permissions are. Its per-message suggestions match
  on words and mostly named unrelated code (cb1754bb). **An upgrade
  rewrites `settings.json`**, adding the hooks, a status line and more
  permissions; check `git status` after one and commit nothing but the
  permissions.

- **`docker`** — `qemu_matrix.py` needs it, and nothing else does.

- **`gperf`, `perl`, `pkg-config`, `patch`, expat** — `netsurf_vendor.py`
  needs them to run upstream's generators, apply our patches and link
  libdom's XML binding on the host, and nothing else does. The generated
  sources are committed, so the BUILD needs none of them.

Deliberately NOT used: `gcovr`/`lcov` (coverage needs runtime support a
freestanding kernel does not have) and `valgrind` (same reason).
