# SDL as the platform layer for ported programs

A staged plan for SDL on SaunaOS: one library that games and media
programs written for Linux, Windows or macOS already speak, so a port
is a recompile and not a new backend each time. ScummVM is the first
program it is for; DOSBox and most SDL games are the ones after.

**STAGES 0 TO 4 ARE BUILT (2026-11-17)** -- ScummVM runs on it (stage 4,
`make scummvm`). SDL 3.4.20 and sdl2-compat 2.32.74
are vendored, build as `/lib/libSDL3.so.0` and `/lib/libSDL2-2.0.so.0`,
and pass `/tests/sdl3_test` and `/tests/sdl2_test` with SDL's own dummy
video and disk audio drivers; an SDL program opens a real desktop
window, scaled and driven (`sdl_window_test.py`), and SDL programs play
sound through usnd (`sdl_audio_test.py`).
**The choices were made the same day**:
SDL3 with sdl2-compat on top rather than SDL2 alone, and the video
driver over a STEP API in `uapp` rather than a second thread or the
raw window protocol; and sharp bilinear scaling for a window that is
not scale-aware, from a canvas of four. The stage markers on the
headings are the authority.

## The finding that shapes the plan

**SDL3 has a supported way in for a platform it does not know: the
"private platform".** Built with `-DSDL_PLATFORM_PRIVATE`, SDL reads
`SDL_build_config_private.h` in place of a CMake-generated config,
registers a video driver named `PRIVATE_bootstrap` and an audio driver
named `PRIVATEAUDIO_bootstrap` when they exist, and accepts private
time, filesystem and thread modules (`SDL_*_PRIVATE`). It is how
console ports live outside SDL's tree. So everything SaunaOS-shaped is
our code in `userland/third_party/sdl3/glue/`, the vendored tree is
upstream's, and moving to a newer SDL is a re-run of
`tools/fetch/sdl_vendor.py` -- one small patch apart (below).

The rest of SDL is portable C over POSIX, and tolibc already had nearly
all of it: 162 of SDL's 167 source files compiled on the first try,
and the link found no C-library function missing.

## What real systems do

- **SDL itself** keeps one driver per platform per subsystem
  (`src/video/<platform>`, `src/audio/<platform>`) over shared generic
  code; threads, timers and dynamic loading come from POSIX on any
  Unix. Haiku's driver is in SDL's tree; SerenityOS and Redox carried
  theirs as patches to a port.
- **SDL2 to SDL3.** SDL2 is in maintenance; SDL3 is where work happens.
  **sdl2-compat** is SDL2's whole API carried out by SDL3, which it
  `dlopen()`s at start -- Linux distributions have begun shipping it in
  place of SDL2. SaunaOS follows: one SDL to port, and both APIs.
- **How a toolkit is driven from someone else's loop.** X11's
  `XPending`/`XNextEvent`, Win32's `PeekMessage`, Wayland's
  `wl_display_dispatch_pending` -- each lets the PROGRAM own the loop
  and ask for events when it wants them. SDL programs are written that
  way (`SDL_PollEvent` in their own `while`); `uapp_run()` is the
  opposite shape, taking the loop and calling the app back. Haiku's
  SDL driver bridges the same mismatch with a second thread for
  `BApplication`. SaunaOS adds the step shape to `uapp` instead
  (stage 1), which any later port with its own loop can use too.

## Where it lives

```
  ported program (ScummVM, ...)          an SDL3 program
        |  SDL2 API                            |  SDL3 API
  /lib/libSDL2-2.0.so.0  (sdl2-compat) --dlopen--> /lib/libSDL3.so.0
                                                   |  generic SDL
                                                   |  sdl3/glue/: config, time, filesystem,
                                                   |    video driver -> uapp step API, uapp_window per SDL window
                                                   |    audio driver -> usnd_push (libmedia)
                                                   v
                                   libbase.so (udirs, uself)  libc.so (tolibc)
```

`libSDL3` links `libui` (the video driver) and `libbase` (the
personal folders, the program's own path, the scaler) and `libmedia`
(the audio driver). Both are zlib-licensed, so they ship in the base
image rather than in a GPL optional component; a GPL program built on
them (ScummVM) is the component.

## The stages

### Stage 0 -- vendored, built, proven headless -- BUILT 2026-11-17

- `tools/fetch/sdl_vendor.py` downloads both release tarballs, checks
  their SHA-256, and copies what this configuration compiles (no host
  CMake needed).
- `sdl3/glue/SDL_build_config_private.h` -- what tolibc has, as SDL's
  `HAVE_*` claims; SDL's dummy and disk drivers; pthreads, Unix timers,
  `dlopen`, POSIX file operations.
- `SDL_saunaos_time.c` -- local time's UTC offset from tolibc's
  `tz_offset_seconds()`, because SDL's Unix module reads `struct tm`'s
  `tm_gmtoff`, which tolibc has not got (roadmap).
- `SDL_saunaos_filesystem.c` -- the program's own path from the kernel
  (`base/uself.h`, QUERY_PROCPATH; there is no `/proc/self/exe`), and
  SDL's user folders as `base/udirs.h`'s personal folders.
- **The one patch**, `sdl3/patches/0001-...`: on a private platform SDL's
  threads neither block signals nor set a priority. SaunaOS's signal
  mask belongs to the PROCESS, so SDL's per-thread masking of SIGINT,
  SIGTERM and SIGHUP would have blocked them for the whole program.
- **Tests.** `sdl3_test` reads back what it made: the software
  renderer's pixels, the audio thread's samples from the disk driver's
  file, a thread's result, a timer's event, the glue's paths, and
  Tokyo's offset (a zone that is not UTC). `sdl2_test` is sdl2-compat's
  `dlopen` of SDL3 and an SDL2 window surface read back. Both were seen
  red: the glue mutated, and `-Bsymbolic` missing.

**Traps found on the way**, each also at the line it concerns:

- **`-Bsymbolic` on `libSDL3`.** SDL3 calls its own API by public names,
  and `libSDL2` -- first in the search order -- exports the same names
  with SDL2's meanings. Without it SDL3's video init called
  sdl2-compat's `SDL_InitSubSystem`, read SDL2's `0` as failure, and
  failed with no error. Upstream avoids it on Linux through its dynamic
  API layer, which is off on a private platform.
- **GCC predefines `__linux` as well as `__linux__`**, and SDL reads all
  three spellings; every one is undefined for SDL and its programs.
- **SDL takes `pthread_*` from the PROGRAM** (they are in
  `libc_nonshared.a`, not `libc.so`). A program linked the way `/bin`
  programs are (`TOOLKIT_LINK`) exports them; a narrower link line made
  sdl2-compat's `dlopen()` of SDL3 fail as unresolved.

### Stage 1 -- the video driver, over a uapp step API -- BUILT 2026-11-17

- **uapp's step API**: `uapp_open()`, `uapp_pump(a, timeout_ms)` and
  `uapp_close()` -- `uapp_run()` taken apart, and `uapp_run()` now a
  loop over the same calls. Drawing is unchanged: `on_draw` paints what
  is dirty inside the pump. The pump returns how many events it handled
  (SDL's wait reports "something arrived" from that, not from the
  clock) or -1 once the app is over, and checks the compositor is alive
  even when it does not wait, so a game that only polls hears the
  desktop die: the driver turns that into `SDL_EVENT_QUIT`.
- **No desktop, no driver** (`uapp_has_desktop()`): SDL goes on to its
  next driver, X11's and Wayland's rule without a display. SDL's dummy
  driver is not one it falls back to -- it starts only when named --
  so with no desktop `SDL_Init(VIDEO)` fails, as on Linux without
  `$DISPLAY`.
- **An SDL window is a `uapp_window` of a no-toplevel app**
  (`UAPP_NO_TOPLEVEL`, w16's shape), not the toplevel: a toplevel cannot
  be closed and opened again, SDL windows are, and there may be several.
  Each has its own taskbar button. A hidden SDL window has no uapp
  window at all; showing it opens one.
- **Coordinates are points** (macOS's model): a 640x400 SDL window is a
  640x400 x `display.scale` uapp window, the desktop's mode is reported
  in points with that pixel density, and the pointer arrives in points
  by the window's actual size. A window that asked for
  `SDL_WINDOW_HIGH_PIXEL_DENSITY` draws the pixels; one that did not
  draws points and is scaled **sharp bilinear** (`base/upixscale.h`:
  nearest-neighbour to the next whole factor, then bilinear down) --
  the maintainer's pick from the "SDL window scaling at 125%" canvas,
  over unscaled, smooth and nearest. A whole factor is pure
  duplication; 640x400 -> 800x500 measured 0.9 ms a frame on the dev
  host.
- **Input**: keys by POSITION (`on_phys_key`'s evdev codes through
  SDL's own Linux scancode table), the typed character as SDL text while
  the program has text input on, all five buttons -- chords included,
  which needed the WM to track a press as a SET of buttons
  (`docs/conventions/gui.md`, "A chord is one press") -- the wheel, the
  pointer leaving (`uapp_window_desc.on_leave`, new), focus (focus lost
  releases every key, uapp's rule). The title bar's close is
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED`: a request the program decides.
- **Fullscreen** is the desktop's own mode. A FIXED window going
  fullscreen now hears the screen's size (`on_resize`, which used to be
  a resizable window's alone) and gets its own size back after --
  `uapp_window_set_fullscreen()` keeps it, since a fixed window declines
  every other resize. SDL sizes the window to the mode itself.
- A window that asked for high pixel density draws at the window's REAL
  size: points times the scale does not round-trip (1002 px -> 802
  points -> 1003 px), and a framebuffer a pixel off would be resampled
  every frame.
- **The cursor**: the desktop's shapes for SDL's system cursors, and
  hidden (`SDL_HideCursor`) -- `uapp_window_set_cursor()`, new, because
  `uapp_set_cursor()` reached only a toplevel. A window opened later
  takes SDL's current cursor, not the arrow.
- `SDL_UpdateWindowSurface()` presents at once (a `uapp_pump(a, 0)`),
  not at the program's next pump -- except from an SDL event watcher,
  which runs INSIDE the driver's pump: a pump from there would hand SDL
  the next events in the middle of this one, so the driver declines it
  and the outer pump paints.
- **SDL_Init leaves the locale alone** (`UAPP_NO_SETLOCALE`, GTK's
  `gtk_disable_setlocale()`): a C program starts in "C" until it calls
  `setlocale()` itself, and a ported one counts on it -- the region's
  decimal comma reaches it through `localeconv()` today, and would reach
  `printf`/`strtod` once tolibc honours `LC_NUMERIC` (roadmap).
- **`SDL_SetWindowHitTest`** on a BORDERLESS window: a press on a
  draggable or resize area starts the move or resize
  (`uapp_window_move_start()`) and the program hears no press -- X11's
  and Windows' rule. A framed window's hit test is not asked: toywm
  moves only a window that draws its own frame.
- **Tests**: `tools/tests/gui/sdl_window_test.py` over
  `/tests/sdlwin_test` -- pixels at 100% and 125% (an edge on a pixel
  edge unblended, one inside a pixel blended: plain bilinear and nearest
  each fail one check, both seen red), clicks in points, the right
  button, a chord (left held, right pressed and released), the pointer
  leaving, the wheel, a real key's scancode and text, fullscreen in and
  out at both scales, close; and after a review of the stage, a chord
  with the middle, a chord across two windows, a drag off the edge, the
  nested watcher, the locale and the hit-test move. Every check was seen
  red. `compositor_death_test.py` checks an SDL program waiting in 16 ms
  slices hears the desktop die.
  `/tests/pixscale_test` checks the scaler's arithmetic.

**Not yet**: custom cursor images (`uapp_cursor_image()` exists for a
toplevel), following a `display.scale` change while running (the scale
is read when a window opens), window position. (Relative mouse and
message boxes are stage 3's.)

**The desktop dying** reaches a program through `uapp_pump()`'s liveness
check, once a park's length apart whatever the timeout -- SDL's own wait
loop pumps with 0 on every pass anyway (`SDL_WaitEventTimeout_Device`),
but a program on the step API waiting 16 ms at a time would never have
sat through the silent park the check used to wait for.

**Traps found on the way**:

- **An edge that lands on a pixel edge is not blended**, by design: a
  test picture split at the half (80 of 160) shows nothing at 1.25x.
  The test's second edge is at 121 for that reason.
- **`gui key` injects a CHARACTER, never a key by position**: a test of
  a game's keys uses QMP's `send-key`, the keyboard's own path.
- **`SDL_UCS4ToUTF8()` writes no terminator.**
- **A MOUSE DOWN names the button pressed, an UP those still held** --
  `abi/win_proto.h` said so only for the UP; a client keeping a button
  set adds on a down and replaces on an up.

### Stage 2 -- the audio driver, over usnd_push -- BUILT 2026-11-17

`PRIVATEAUDIO_bootstrap` (`glue/SDL_saunaos_audio.c`): the device is
`usnd`'s format, always -- stereo S16 at `USND_RATE` -- and SDL converts
whatever a program asks for in front of it, as SDL does for PipeWire's
and PulseAudio's one native format. SDL's device thread pushes each
buffer into the never-blocking single-producer queue DOOM's music uses
(`usnd_push`), waiting first until no more than ~64 ms is queued ahead
of usnd's mixer, so a sound effect does not wait behind the whole
queue. The device is opened at `SDL_OpenAudioDevice`, not at
`SDL_Init`: a program that never plays does not take the card. No card
and no soundd is a failed open, as on Linux with nothing to connect to.
`libSDL3` now links `libmedia`. Playback only: usnd has no capture.

- **Tests**: `tools/tests/sweep/sdl_audio_test.py` -- two boots with an
  AC97, QEMU's wav recording of what the CARD emitted measured on the
  host (`audio_test.py`'s oracle): `/tests/sdltone_test` (SDL3, 1 kHz as
  mono float at 44.1 kHz into an `SDL_AudioStream`) and
  `/tests/sdl2tone_test` (SDL2's callback API through sdl2-compat --
  ScummVM's shape -- 750 Hz as mono S16 at 22.05 kHz). Each must open
  the `saunaos` driver, play to its end, at its own pitch, for ~1.5 s,
  with no whole-chunk hole.
- **The holes it found, and their cause.** Through soundd the pushed
  tone first reached the card with one-chunk (10.7 ms) holes, 4-8 per
  1.5 s, straight to the card it looked clean. A run of probes (soundd's
  view of each chunk, SDL's buffers, usnd's mixer, what usnd wrote)
  located them: **usnd's mixer read the pushed queue in two runs when it
  wrapped and added BOTH at the start of the pass** (`add_voice()` had no
  offset) -- the runs summed over each other and the rest of the pass
  was silence, once per 8192 pushed frames. DOOM's music takes the same
  path. Fixed with `add_voice_at()`; the tone is 1000.0 Hz with no hole.
  Straight to the card the wrap rarely split a pass, which is why it
  looked like soundd's.
- **Two more defects found on the way**, each real on its own: **soundd
  took a client's PARTIAL chunk** -- starved only when `hw_pos == wr_pos`,
  so a writer part-way into a chunk had the rest played as silence and
  its `hw_pos` moved past `wr_pos`; and **usnd padded a short pushed
  source with silence whenever the sink had room**, which soundd's deep
  ring always had -- it now mixes a pushed source as far as it has
  arrived, padding only for the device sink or the other voices.
- **Ruled out on the way**, by measurement: the driver's queue running
  dry, its depth, lock contention, TCG speed (KVM the same), and PACING.
  A PipeWire/PulseAudio-shaped pacing change (usnd woken by a push,
  soundd held to a 128 ms lead) was built on the pacing theory and
  dropped once the cause was found: nothing needed it. Lower latency for
  games is its own change, measured on the hardware.

### Stage 3 -- the rest a port reaches for -- BUILT 2026-11-17

The maintainer's picks from the "SDL message box options" canvas and
the question round of the same day:

- **`SDL_GetPrefPath()`** is `$XDG_DATA_HOME/<org>/<app>/`, else
  `/home/.local/share/<org>/<app>/` -- SDL's Unix answer under the one
  user's home, made if missing (an empty org is left out). Saves are the
  person's data and travel with `/home`; `/var/lib` is system state
  (browser, diskmark) and `/var/games` would fit games only.
- **The clipboard** is `media/uclip.h`'s TEXT, as SDL's Windows driver
  hears only `CF_UNICODETEXT`; another program's copy is
  `SDL_EVENT_CLIPBOARD_UPDATE`, one shared-memory read of the serial per
  pump. No primary selection: SaunaOS has none.
- **`SDL_OpenURL()`** is `base/uopen.h`'s `uopen_url()` -- xdg-open's
  job: http: and https: in the Web Browser (through `uapp_spawn()` while
  the driver is up, so the app reaps it), file: in whatever opens the file
  it names. Any other scheme is refused. `glue/SDL_saunaos_url.c`
  replaces SDL's dummy, which `sdl_vendor.py` leaves out.
- **`SDL_ShowMessageBox()`** is `ui/uui_msgbox.h` (M1 on the canvas): a
  window of its own, MODAL over the SDL window that asked -- which needed
  an unowned dialog to be able to own one (`wm_can_own()`; a window of a
  `UAPP_NO_TOPLEVEL` app is a toplevel in all but name, and
  `uapp_window_desc.owner` names it). Win32's MessageBox, KMessageBox
  and SDL's own X11 box are separate windows too, and SDL allows a box
  with no window at all -- the bootstrap's hook opens a uapp of its own
  for it. The message is a `ui/uui_para.h` paragraph: selectable, and
  Ctrl+C copies all of it with nothing selected, as MessageBox does. The
  box pumps the app while it waits, so it is refused from inside an SDL
  event watcher (`SAUNAOS_InPump()`).
- **Relative mouse mode** is a POINTER LOCK, Wayland's shape
  (zwp_locked_pointer_v1 with zwp_relative_pointer_v1) rather than X11's
  warp-to-centre: `WIN_REQ_POINTER_LOCK` from the client, and the
  compositor (`toywm/wm_ptrlock.c`) asks the kernel to HOLD the pointer
  (`WIN_REQ_RAW_MOTION`) -- a relative device's motion then becomes
  `WIN_EV_RAW_REL` instead of moving it, forwarded as `WIN_EV_MOUSE_REL`.
  Held in the KERNEL, not warped back each frame, so a click lands where
  the lock is however fast the mouse moved, and an edge stops nothing.
  In force only while its window has the focus and none of the
  compositor's own overlays is up: Alt+Tab, the Start menu, a flyout or
  another window suspends it and their going resumes it -- the way out
  every desktop gives. A lock's motion at a text VT is dropped, and it
  reaches the compositor before a click in the same poll (motion, then
  the button, as an evdev frame orders them). An absolute device (a tablet) still moves the
  pointer and adds no motion. `SDL_WarpMouseInWindow` is
  `uapp_window_set_pointer()`.
- **Fullscreen** came with stage 1.
- **Tests**: `tools/tests/gui/sdl_stage3_test.py` over
  `/tests/sdlmisc_test` -- the pref path written and read back, the
  clipboard across two SDL programs, both URL kinds and a refused one,
  four message boxes (owner and modality, the badge's colour by pixel,
  Ctrl+C, a click, Esc, Return, a close), a box before `SDL_Init`, and the
  lock: held pointer, summed motion, a click, suspended by another
  window, resumed, released. A KTEST checks the kernel's hold. Each part
  seen red: the kernel not holding, `uui_para_key()` declining, the
  badge in the accent.

**SDL in the SDK -- BUILT the same day.** `<SDL3/...>` and `<SDL2/...>`
in `/usr/include`, `-lSDL3`/`-lSDL2` through `/usr/lib`'s linker scripts,
`sdl3.pc`/`sdl2.pc` in `/usr/lib/pkgconfig` and `sdl2-config` (in the
sysroot's `usr/bin`, and the image's `/usr/bin` when it carries a
compiler). Their include flag is `-I=/usr/include/SDL2`: GCC reads `=` as
the sysroot, so the cross compiler and the machine's take one answer.
It needed dynamic linking first -- sdl2-compat `dlopen()`s `libSDL3.so.0`
-- which became the compiler's DEFAULT (`docs/cc-design.md`, "Dynamic by
default"). `cross_gcc_test.py` builds an SDL3 program with `pkg-config`
and an SDL2 one with `sdl2-config` and runs both on a guest.

### Stage 4 -- ScummVM -- BUILT 2026-11-17

ScummVM 2026.4.0 on its stock SDL2 backend (sdl2-compat over SDL3),
with four engines -- sky, queen, lure, drascula -- the four freeware
games' (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the
Temptress, Drascula). The maintainer's picks, from the "ScummVM games
options" canvas and two question rounds the same day:

- **Fetched, as GNU make is** -- the source is 212 MiB, so it is the
  `scummvm` row in `tools/fetch/fetch_extras.py` (licences shown,
  `EXTRAS=1`), pinned in `tools/fetch/fetch_scummvm.py`; our change is
  `userland/third_party/scummvm/patches/` (one line of `configure`:
  SaunaOS is a POSIX host) and `tools/build/build_scummvm.sh`, which
  cross-builds it with `x86_64-saunaos-g++` -- the first port built by
  the cross compiler rather than the tree's gcc -- into
  `toolchain/scummvm/usr`. `make scummvm` runs it (about a minute on
  the dev host); the seed step carries that tree to the image while it
  exists, never under `NOGPL=1`. 20 MB stripped, 10 MB of data beside it.
- **zlib, libogg and libvorbis are linked in statically** (its zip
  themes, compressed saves, Drascula's Ogg music); they reach the SDK
  when a second port needs them (the roadmap). No MP3, FLAC, PNG,
  FreeType, network or OpenGL: nothing these four games need.
- **The game data lives in `/usr/share/games/<id>/`** (Debian's
  shape, as DOOM's `/usr/share/doom`); ScummVM's settings and saves go
  where its POSIX backend puts them, `$XDG_CONFIG_HOME/scummvm` and
  `$XDG_DATA_HOME/scummvm/saves` -- which `/etc/environment` now sets
  to `/home/.config` and `/home/.local/share` (HOME stays `/`), the
  image ships those folders, and the front makes them on a machine
  updated from before: ScummVM makes only its own folder beneath them.
- **The front is DOOM's card (G2)** -- `/bin/wm/apps/scummvm`, the
  Start menu's ScummVM: with a game here it registers any new one
  (`scummvm --add --path=DIR`, so ScummVM writes its own list) and
  becomes ScummVM's launcher (`execv`) with no window of its own; with
  none, or as Free Adventure Games (`--games`), it shows the card,
  several games at once, and does the same once its window has closed --
  never inside the event loop, since each `--add` is a ScummVM run of
  its own. A game is offered to ScummVM ONCE (`added_<id>` in
  `/etc/scummvm-games.conf`), so one removed from ScummVM's list stays
  removed. Its left button opens ScummVM's launcher, whose Add Game is
  the way in for a game a person owns.
- **A ScummVM already open keeps a game another one added**: it writes
  its whole list back at a setting change and at its close, which used
  to delete a game the front `--add`ed meanwhile (marked offered, so
  never asked again). Our config patch (`patches/0002`) has
  `flushToDisk()` take in any game section on disk that the open one
  lacks and did not remove or rename itself; the front also leaves a
  game unmarked when another ScummVM ran during its add, and unmarked,
  once per machine, every game on disk that the list lacks (the
  `relisted` key) -- a game lost before the fix comes back.
- **The licence is a `uui_para`**: selectable, Ctrl+A / Ctrl+C, and the
  right-click Copy / Select All through `uapp_popup_menu()`.
- **The card is shared, and DOOM moved onto it**: `base/udatafetch.h`
  (the table, the SHA-256-pinned fetch, the unpack into `<id>.part` and
  the rename, the licence saved) and `ui/uui_datacard.h` (the rows --
  radio or checkboxes --, each licence, the progress). The transfer is
  passed in as `uhttp_download`: libhttp is not a toolkit library.
- **Every game is pinned by the `.sha256` ScummVM publishes beside it**,
  and fetched from `downloads.scummvm.org` (`mirror=` in
  `/etc/scummvm-games.conf` stands in). Their licence is one freeware
  text -- share it unchanged and free -- so it is a notice to read, not
  an agreement; each game's own readme is saved to `/usr/share/licenses`.
- **The component** is `scummvm` (GPL-3.0-or-later, `Built=if-present`):
  the program, its data, the front and its two Start-menu entries, which
  carry freedesktop's `TryExec=/usr/bin/scummvm` so a build without it
  seeds neither.
- **What building it found**: tolibc's `<string.h>` defined `memcpy` and
  three others as function-like macros, which a hosted compiler does not
  need and C++ forbids (ScummVM's `Std::memcpy` broke) -- now
  freestanding C only; and `execl`/`execlp`/`execle` were missing.
- **Tests**: `tools/tests/gui/scummvm_data_test.py` -- the card, a
  tampered zip refused on its SHA-256 with nothing left behind (seen
  red with the check removed), Lure downloaded from a loopback mirror,
  registered in `scummvm.ini` under `XDG_CONFIG_HOME`, its licence
  saved, the front handing over the next time, and `scummvm lure`
  drawing the game. `doom_data_test.py` passed unchanged on the shared
  card.

## Open questions

- **Per-thread signal masks** (`pthread_sigmask`), which would retire
  the patch -- a `docs/signals-design.md` question, not an SDL one.

## The case against

- **Size**: `libSDL3.so.0` is about 1.6 MB on the disk and
  `libSDL2-2.0.so.0` 0.5 MB (stripped), mapped only by programs that
  use them.
- **A second way to draw**: an SDL program draws with SDL's software
  renderer into one surface rather than with SaunaOS's widgets -- right
  for a game or an emulator, wrong for an app, which should stay a
  `uapp` program.
- **Against a native ScummVM backend** (one `OSystem` class, as DOOM's
  doomgeneric glue): smaller for ScummVM alone, but every later port
  would need its own; SDL is that work once.
