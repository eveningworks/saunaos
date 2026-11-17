# SDL as the platform layer for ported programs

A staged plan for SDL on SaunaOS: one library that games and media
programs written for Linux, Windows or macOS already speak, so a port
is a recompile and not a new backend each time. ScummVM is the first
program it is for; DOSBox and most SDL games are the ones after.

**STAGE 0 IS BUILT (2026-11-17)** -- SDL 3.4.20 and sdl2-compat 2.32.74
are vendored, build as `/lib/libSDL3.so.0` and `/lib/libSDL2-2.0.so.0`,
and pass `/tests/sdl3_test` and `/tests/sdl2_test` with SDL's own dummy
video and disk audio drivers. **The choices were made the same day**:
SDL3 with sdl2-compat on top rather than SDL2 alone, and the video
driver over a STEP API in `uapp` rather than a second thread or the
raw window protocol. The stage markers on the headings are the
authority.

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
                                                   |    [stage 1] video driver -> uapp step API
                                                   |    [stage 2] audio driver -> usnd_push
                                                   v
                                   libbase.so (udirs, uself)  libc.so (tolibc)
```

`libSDL3` links only `libbase` today (the personal folders and the
program's own path); the video driver will add `libui`, the audio
driver `libmedia`. Both are zlib-licensed, so they ship in the base
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

### Stage 1 -- the video driver, over a uapp step API

- `uapp` gains the program-owns-the-loop shape: open a toplevel, pump
  events with a timeout, present a frame -- `uapp_run()` becomes a loop
  over the same three.
- `PRIVATE_bootstrap`: an SDL window is a uapp toplevel; SDL's
  framebuffer path (`CreateWindowFramebuffer`/`UpdateWindowFramebuffer`)
  presents its surface with damage. The software renderer draws into
  it. No OpenGL, so no GL contexts and no SDL_GPU.
- Input: key presses and releases by POSITION (`input_keys.h`) as SDL
  scancodes, the layout's characters as SDL text input (UTF-8); mouse
  motion, buttons and the wheel; resize; Close and Alt+F4 as SDL's
  close request; the title; showing and hiding the cursor.
- `display.scale` as SDL's pixel density.
- Exit: an SDL program in a window, drawn and driven, measured by
  pixels and injected input.

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
