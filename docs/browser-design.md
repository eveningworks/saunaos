# A web browser

A staged plan for a Web Browser app: NetSurf's engine behind a toy-os
window. It answers "port or write our own, what does a port actually
need from this system, and in what order is it worth building?"

**STAGE 0 IS BUILT (2026-10-12)** -- NetSurf and its libraries are
vendored and link for toy-os as `/tests/nsmonkey`. **The choices were
made the same day**: port NetSurf
(GPL-2.0, C) from a PINNED MASTER COMMIT rather than its last release;
the W1 window (a tab strip over a toolbar, the Web Browser mockup
canvas); and the page view as a reusable `uui_webview` widget. The
stage markers on the headings are the authority.

## The finding that shapes the whole plan

**A browser engine is the largest parser of hostile input this system
will ever run, and the only engine that fits toy-os is GPL-2.0-only.**
Both facts point at the same boundary: the engine lives in ONE binary,
never in a shared library.

- **Licence.** toy-os is MIT. NetSurf's core is GPL-2.0 with no "or
  later"; its libraries (libcss, libdom, libhubbub, libparserutils,
  libwapcaplet, libnsgif, libnsbmp, libnsutils, libnspsl, libnslog)
  and Duktape are MIT. DOOM already settled how GPL code lives here:
  linked into exactly one binary, our backend outside the vendored
  directory (`docs/decisions/build.md`, the doomgeneric entry). The
  browser follows it. Everything it links is compatible: `libhttp.so`
  and toolkit code are MIT, and the vendored mbedTLS is
  `Apache-2.0 OR GPL-2.0-or-later`, so it can be taken under the GPL.
- **Hostile input.** HTML, CSS, images and fonts from anywhere. The
  thumbnailer already set the rule that the desktop never decodes
  someone else's file in-process (`/bin/thumb`). A browser tab is the
  same problem, larger.

**So `uui_webview` cannot be a libuapp widget that links the engine.**
That would put GPL code into every app that embeds a page (Help,
release notes) and run hostile parsers inside them. It is a widget
that shows a page RENDERED BY ANOTHER PROCESS -- the engine as a
content process, drawing into shared memory over `uchan`. That is
also what every real embedded web view does (below), and it is stage
5, after the browser works in-process.

## What real systems do

- **The big engines** (Chromium/Blink, Gecko, WebKit) are tens of
  millions of lines of C++ with JIT compilers, and all of them render in
  separate processes: Chromium's renderer processes, WebKit's
  `WebContent` process behind `WKWebView`, WebView2 running the Edge
  runtime out of process, and Android's WebView in a renderer process.
  An embeddable web view is a CLIENT of an engine process, not the
  engine. toy-os copies that shape (stage 5), not the size.
- **Hobby and small systems**: Haiku ported WebKit (WebPositive), a
  multi-year effort; SerenityOS wrote its own engine, LibWeb, which
  became Ladybird. NetSurf's own tree carries front ends for RISC OS,
  GTK, Qt, Windows, AmigaOS, Atari, BeOS/Haiku, a bare framebuffer, and
  `monkey` -- a headless one driven by text commands that prints what
  it would draw, which is this plan's first test oracle.
- **Why not the others.** Ladybird and litehtml are C++, and toy-os
  builds no C++ (no runtime, no unwinder); either would first need a
  C++ runtime port, a project of its own. Our own engine was weighed and
  set aside: parsing tag soup correctly (the HTML5 algorithm) and the
  CSS cascade are the parts that take years to get right, and NetSurf
  brings both plus CSS 2.1 layout and `display: flex` (3.11).

## Releases are slow; master is not (checked 2026-10-12)

NetSurf's last release is 3.11, from December 2023, and it is still what
Arch, Alpine and Flathub package. Master is alive: commits from three
developers through February 2026 (the GitHub mirror,
`netsurf-browser/netsurf`), including "Improve HTTP redirect handling"
and bounded-string fixes; libcss and libnsgif have commits as late as
August 2026. So the port vendors PINNED MASTER COMMITS -- each library at
its head on the day it was vendored -- the way mbedTLS is pinned, and
refreshes them deliberately with the vendoring tool (stage 0).
The risk is the bus factor of a three-person project, not abandonment.

What NetSurf does NOT do, and nobody should expect after the port: CSS
grid, modern JavaScript (Duktape is ES5, and NetSurf's DOM bindings are
partial), WebAssembly, video. Wikipedia, Hacker News, old.reddit,
documentation sites and most text-first sites render; GitHub's UI,
YouTube and Gmail do not.

## What already exists, measured (2026-10-12)

- **HTTPS**: `/lib/libhttp.so` (`<uhttp.h>`) over `/lib/libssl.so`
  (mbedTLS 3.6, TLS 1.3, chain verification against
  `/etc/ssl/certs`). **It is HTTP/1.0 with `Connection: close` and
  blocks**: one connection per request, GET only, no caller-supplied
  headers, and `on_headers` reports only the status and length. It
  decodes gzip and can follow redirects (`max_redirects`), though NetSurf
  wants redirects handed to it.
- **Sockets**: blocking only -- `<unistd.h>` says there is no
  `select`/`poll`, and a connect waits up to `SYS_NET_CONNECT_MS`. So a
  fetch is a THREAD (pthreads exist), and `uapp_post()` -- "the one call
  in Toykit a worker thread may make" -- wakes the main loop.
- **Text**: `ui/uglyph.h` rasterizes any code point of a .ttf at any
  size in ring 3, cached. **The shipped faces are Liberation Sans
  (regular, bold) and DejaVu/Vera mono only: no serif and no italic**,
  so pages would set Georgia and `<em>` in upright sans until faces are
  added (Liberation Serif and the italics, OFL).
- **Images**: `lib/uimg.h` decodes PNG, JPEG, GIF, BMP and QOI.
- **libc gaps** -- filled in stage 0, below. libparserutils has its own
  codecs for UTF-8, UTF-16 and the 8859/Windows-125x families, so an
  `iconv` that knows UTF-8, Latin-1 and Windows-1252 and refuses the
  rest is enough for NetSurf's `utils/utf8.c`.
- **Isolation**: `/bin/thumb` is the precedent for a separate process
  parsing hostile files; `lib/uchan.h` is the asynchronous channel
  between two ring-3 processes, over shared memory.

## The NetSurf seams a front end fills

- **`struct netsurf_table`** (`desktop/gui_table.h`): `misc`, `window`,
  `fetch`, `bitmap` and `layout` are mandatory; `download`,
  `clipboard`, `search` are needed for a usable browser; `file`, `utf8`,
  `llcache`, `search_web`, `corewindow` have defaults.
- **A fetcher per scheme** (`content/fetchers.h`, `fetcher_add()`):
  `setup` (URL, POST data, request headers), `start`, `abort`, `free`,
  `poll`, `fdset`. `poll` runs on the main thread; ours drains what the
  fetch threads queued and turns it into `fetch_send_callback()`
  messages -- headers ONE AT A TIME, data, finished, error, certificate
  chain. NetSurf keeps cookies (urldb), its cache (llcache), redirects
  and HTTP auth itself, which is why the fetcher must hand it RAW
  headers rather than a parsed summary.
- **Layout** (`gui_layout_table`): width, position-in-string and split
  for a font style, over `uglyph.h`. **Plotters**: rectangles, lines,
  polygons, paths, clips, bitmaps and text, onto `uui_canvas`.
- **Schedule**: `misc->schedule(ms, cb, p)` is a one-shot timer list;
  the main loop sleeps until the earliest one or a `uapp_post()`.

## Where it lives

```
userland/ports/netsurf/        NetSurf core at the pinned SHA (GPL-2.0)
userland/ports/libcss/ ...     each library, own directory (MIT)
userland/backends/netsurf/     OURS: platform switches and shims (stage 0),
                               then the fetcher, fonts, bitmaps, plotters
                               (outside the vendored tree, as
                               backends/doom/ is)
userland/tests/nsmonkey.c      the headless front end (stages 0-1)
/bin/wm/apps/browser           the window (stage 3), linking all of it
```

Generated sources (libcss's property tables, libhubbub's entities,
NetSurf's `Messages`) are produced ON THE HOST by the vendoring tool and
committed with the source, so the build runs no Perl and no generator.

## The stages

### Stage 0 -- vendoring, and the libc it needs -- DONE 2026-10-12

`tools/netsurf_vendor.py` clones NetSurf and nine libraries at pinned
commits, builds upstream's headless `monkey` front end on the host, and
copies exactly what that build compiled -- read off the compiler's
dependency files -- into `userland/ports/`, with each port's
`sources.mk` naming its translation units. The toy-os Makefile compiles
those 588 units with `-nostdinc`, and `/tests/nsmonkey` links. **On a
VM it starts, registers its tables, opens a window and begins a `file:`
fetch, then shuts down cleanly** when its input ends; loading a page to
completion is stage 1's harness.

What it took, and where each piece lives:

- **tolibc** grew what was POSIX and missing -- `iconv`, `inet_pton`/
  `inet_ntop`, `realpath`, `scandir`, the POSIX clocks, `strtof`, the
  float forms of the exact maths -- and three things C itself requires
  that were simply absent (`EXIT_FAILURE`, `EDOM`/`EILSEQ`, a defined
  `gettimeofday`). `docs/libc-design.md`'s stage 10 has the list and
  what stays absent and why; `userland/tests/libc6_test.c` checks it.
- **`userland/backends/netsurf/`** holds what is NetSurf's problem, not
  the C library's: the platform switches in place of `utils/config.h`,
  a zlib-shaped shim over `lib/uinflate.h`, forwarders for libdom's
  binding headers, and two upstream rough edges -- `content/fetch.c`
  includes the curl fetcher's header in a curl-less build, and
  `desktop/gui_factory.c`'s default socket opener calls `socket()`.
  Its README lists each file and why.
- **Not taken**: monkey's `main.c` (its loop waits in `select()`; ours is
  `userland/tests/nsmonkey.c`), the disk cache (`fs_backing_store.c`),
  libnsutils' `pread()` wrapper that only the disk cache calls, and
  libdom's XML binding (expat). The host build HID the curl-header
  problem, because libcurl's headers are installed there -- which is
  what `-nostdinc` on the toy-os side exists to catch.

### Stage 1 -- a page laid out headlessly -- PLANNED

`/tests/nsmonkey`, NetSurf's `monkey` front end: `file:` and `data:` URLs
only, no network, no window. It prints what it would plot. **The
oracle is the SAME pinned source built for the host**:
`tools/browser_hostcheck.py` runs both on a fixed corpus of saved pages
and diffs the plot streams, so a difference is a port or libc bug
rather than an engine one -- the `*_hostcheck.py` pattern. Proves the
engine before any GUI or network work.

### Stage 2 -- HTTP/1.1 in libhttp, and the fetcher -- PLANNED

`libhttp` grows what NetSurf (and `wget`, `update`) want: a method and
body, caller headers, every response header delivered as it arrives,
HTTP/1.1 chunked bodies, and kept-alive connections pooled per host --
a page with thirty images otherwise pays thirty TLS handshakes in a
row. The certificate chain, and the reason it failed, are reported
rather than flattened into `err`. The fetcher runs each fetch in a
thread and queues results for `poll`. Tests serve a corpus from a
loopback server (http and https with a test CA), so nothing leaves the
machine.

### Stage 3 -- the Web Browser window -- PLANNED

W1 from the mockups: tab strip over a toolbar (back, forward, reload,
home, the address field with the lock and a bookmark star, Downloads,
the menu), the page, a hovered link's URL in the bottom-left corner.
Plotters onto `uui_canvas`, text through `uglyph.h` with Liberation
Serif and the italic faces added, images through handlers over
`uimg.h` (NetSurf's own libnsgif for animation). A `.desktop` entry and
a Start-menu icon.

### Stage 4 -- what makes it a browser -- PLANNED

The lock's connection card (protocol, cipher, certificate, cookies),
the Downloads flyout through `uhttp_download` into
`/home/Downloads`, find in page (`uui_findbar`), zoom, bookmarks and
history (NetSurf's hotlist and global history are core windows), and
a plain-http page saying "Not secure".

### Stage 5 -- `uui_webview`: the engine as a content process -- PLANNED

The engine moves into a content process per view; the widget forwards
input and composites what it drew from shared memory, over `uchan`. The
browser becomes the first client, then Help and release notes.
The licence and the crash boundary are the same line (the finding
above). A crashed page shows a "this page stopped" card in the view,
not a crash of the app.

### Stage 6 -- JavaScript -- PLANNED

Duktape, with NetSurf's bindings generated on the host by `nsgenbind`
in the vendoring tool. Off by default until it is measured on the
corpus, with a setting to turn it on.

## Open questions

- The memory budget: what a Wikipedia article costs in NetSurf's
  caches, measured in stage 3, against the RAM toy-os is usually run
  with.
- Where the store path (cookies, cache, history) lives under the
  personal folders (`lib/udirs.h`).
- The address bar's search engine, and the User-Agent string.

## The case against

It is the largest port this tree will carry, from a three-person
project with releases years apart, and it ends at the 2010s web:
anything built on modern JavaScript stays out of reach whichever
engine route is taken, short of a C++ runtime and a Ladybird port. What
it buys is a real browser for documents within a few stages, an
engine nobody here has to write, and a content-process boundary that
any later engine would sit behind unchanged.
