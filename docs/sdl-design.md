# SDL as the platform layer for ported programs

A staged plan for SDL on SaunaOS: one library that games and media
programs written for Linux, Windows or macOS already speak, so a port
is a recompile and not a new backend each time. ScummVM is the first
program it is for; DOSBox and most SDL games are the ones after.

**STAGES 0 AND 1 ARE BUILT (2026-11-17)** -- SDL 3.4.20 and sdl2-compat 2.32.74
are vendored, build as `/lib/libSDL3.so.0` and `/lib/libSDL2-2.0.so.0`,
and pass `/tests/sdl3_test` and `/tests/sdl2_test` with SDL's own dummy
video and disk audio drivers; an SDL program opens a real desktop
window, scaled and driven (`sdl_window_test.py`). Sound is next.
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
                                                   |    [stage 2] audio driver -> usnd_push
                                                   v
                                   libbase.so (udirs, uself)  libc.so (tolibc)
```

`libSDL3` links `libui` (the video driver) and `libbase` (the
personal folders, the program's own path, the scaler); the audio
driver will add `libmedia`. Both are zlib-licensed, so they ship in the base
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
is read when a window opens), window position, relative mouse and
pointer grab, message boxes.

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

### Stage 2 -- the audio driver, over usnd_push

`PRIVATEAUDIO_bootstrap`: SDL's device thread fills a buffer in
`usnd`'s format (stereo S16 at `USND_RATE`; SDL converts anything else)
and pushes it into the never-blocking single-producer queue DOOM's music
uses. Exit: a tone played and its samples seen at the sink.

### Stage 3 -- the rest a port reaches for

`SDL_GetPrefPath()` (an open question, below), the clipboard, opening a
URL in the Web Browser, `SDL_ShowMessageBox` as a SaunaOS dialog,
relative mouse mode and fullscreen, and SDL in the SDK so `cc`/`c++`
(on the machine and cross) find `<SDL3/SDL.h>` and `-lSDL3`.

### Stage 4 -- ScummVM

On its stock SDL backend, as a GPL optional component; the freeware
games (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the
Temptress, Drascula) through a downloader like DOOM's.

## Open questions

- **Where `SDL_GetPrefPath()` points.** SDL's Unix answer is
  `$HOME/.local/share/<org>/<app>/` (XDG). SaunaOS has no accounts and
  `HOME=/`; DOOM keeps its saves in `/var/games/doom`. Candidates:
  `/home/.local/share/...` (XDG's shape under the one user's home),
  `/var/games/<app>/` (DOOM's precedent, games only) or
  `/var/lib/<app>/`. Stage 0 answers "unsupported".
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
