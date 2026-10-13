# A web browser

A staged plan for a Web Browser app: NetSurf's engine behind a toy-os
window. It answers "port or write our own, what does a port actually
need from this system, and in what order is it worth building?"

**STAGES 0-2 AND MOST OF 3 ARE BUILT (2026-10-12)** -- NetSurf and its
libraries are vendored, lay out a set of test pages identically to the
same source built on the host -- read from files, over http and over
https -- and the Web Browser app (Start -> Internet) browses the web.
Stage 4 is next. **The choices were made the same day**: port NetSurf
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
  `/etc/ssl/certs`). It was HTTP/1.0 with `Connection: close`, GET
  only, with no caller headers -- stage 2 below made it what a browser
  needs.
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

### Stage 1 -- a page laid out headlessly -- DONE 2026-10-12

`/tests/nsmonkey` loads `file:` pages from `/tests/web` (ten of them, one
engine feature each: text, tables, floats, flex, forms, two charsets,
tag soup, CSS selectors, GIF/BMP images) with its resources at
`/usr/share/netsurf`, and **`tools/browser_hostcheck.py` finds every
page's layout IDENTICAL to the host build's** -- the title, the extent
and every draw call of a redraw. Breaking libparserutils' Windows-1252
table turned exactly the euro sign's text run red.

What it found, both fixed:

- **`CLOCK_REALTIME` in whole seconds stalled every page.** NetSurf's
  scheduler times 10 ms callbacks on `gettimeofday()`, so each step of a
  load waited up to a second. It reads the kernel's nanosecond UTC now
  (`QUERY_CLOCK`); a `file:` page loads in a few hundred milliseconds
  under TCG.
- **`vm.py` mangled non-ASCII text read off the guest** -- it decoded
  each serial read alone, splitting multi-byte characters, and framed
  `readfile` by characters rather than bytes. Five of the ten pages read
  back empty until that was fixed; the engine had been right.

### Stage 2 -- HTTP/1.1 in libhttp, and the fetcher -- BUILT 2026-10-12

**libhttp** (`userland/dynlib/uhttp.c`) is HTTP/1.1: a method and body,
caller headers, EVERY response header handed over raw as it arrives
(`on_header`), Content-Length and chunked framing, gzip inside either,
and KEPT-ALIVE connections in a caller-owned `struct uhttp_pool` -- six
idle per host, sixteen in all, closed after 30 s idle (curl's connection
cache, owned by its caller as a curl share handle is). A pooled
connection the server dropped is found out by the request that takes it
and retried once on a fresh one, for GET and HEAD only; a POST never
takes a pooled one. The certificate chain the server presented comes
back leaf first with each certificate's fault (`on_certs`,
`<utls.h>`'s `struct utls_chain`), and a refused one says why
(`cert_err`, and "the certificate has expired" in `err`).
`max_redirects` stays for wget (now GNU's default of 20) and update;
NetSurf passes 0 and follows them itself.

**The fetcher** (`userland/backends/netsurf/fetch_http.c`, shared by the
app and `/tests/nsmonkey`) is upstream's curl fetcher's shape over
libhttp: the same messages in the same order, Location, WWW-Authenticate
and Set-Cookie read on the way past, cookies and Basic credentials from
urldb added at start, and urldb's "proceed anyway" as an insecure fetch.
Each fetch is a detached THREAD (256 KiB stack, for the handshake) that
queues headers, data, the chain and its result; NetSurf's poll drains
the queue on the main thread, and the first message into an empty queue
wakes the app through `uapp_post()`, whose handler drains at once
through `fetch_fdset()`. The queue holds at most 512 KiB per fetch, so
a fast server cannot outrun layout into the heap. The fdset slot is
NULL. NetSurf runs six fetches at once and six per host
(`max_fetchers`, `max_fetchers_per_host`; its default was 24 threads).

**The app**: cookies and the global history are kept in
`/var/lib/browser` (`cookies`, `history`) -- the place every app's kept
state has, not the personal folders, which hold a person's documents
(Firefox and Chrome keep theirs in `~/.config`/`%APPDATA%` too). The
cache stays in memory: NetSurf's disk cache was left out in stage 0.
Typed words that are not an address are searched for on DuckDuckGo's
page that needs no JavaScript (NetSurf's own default engine; the app
builds the URL, so `--search-url` can point a test at a local server).
A typed host is https, except a dotted quad, localhost or an explicit
port other than 443 -- Chrome's exclusions from its upgrade -- and,
unlike Chrome's, there is no fallback to http when https does not
answer. The User-Agent is `Mozilla/5.0 (toy-os; x86_64) NetSurf/3.12`,
or the Linux form or a custom string (`browser.user_agent`, System
Settings > Network > Web Browser, read at each navigation).

What it took beyond the plan:

- **libhttp and the resolver were not thread-safe**: a `static` read
  buffer in the fetch and static DNS query/reply buffers in
  `uresolv_lookup()`. mbedTLS needed `MBEDTLS_THREADING_C` too -- PSA's
  key slots and RNG are global, however separate the sessions look.
- **Every handshake parsed the whole trust store**, about 150
  certificates, per connection. It is parsed once per process now.
- **NetSurf's fallback `uname()` says "windows"**, so its own UA string
  read `Mozilla/5.0 (windows) NetSurf/3.12`; the fetcher sends ours.
- **`tools/browser_hostcheck.py --scheme http|https`** lays the corpus out
  with the guest reading it from `tools/web_server.py` and the host from
  files: 12/12 identical both ways. The original pages each arrived in
  ONE chunk with no separate resource, so dropping a byte per chunk
  changed nothing; `net.html` (21 KB, its own stylesheet and image) is
  what turns a lost chunk red.
- **`tools/http_test.py`** checks libhttp through wget against the
  server's own connection count; `tools/browser_test.py` gained the
  network half (a typed host, a separate stylesheet in the pixels,
  cookies sent back and kept, a search, reuse, https and an expired
  certificate's "Privacy error" page).

### Stage 3 -- the Web Browser window -- BUILT 2026-10-12, its fonts 2026-10-13

`/bin/wm/apps/browser` (Start -> Internet -> Web Browser) lays out and
draws NetSurf's pages in a toy-os window: W1's toolbar row -- back,
forward, reload/stop, home, the address field, Downloads and the menu
(both greyed until stage 4) -- over the page, with tabs. Links, typed
addresses and paths, history, the wheel and the keyboard scroll, forms
take keys, a hovered link's address shows in the corner, and
`file:`/`about:`/`data:` pages load -- and `http:`/`https:` since stage 2.
`tools/browser_test.py` drives it in `gui_regress.py`.

How it is put together, in `userland/browser/` beside the app:

- **Text** (`ns_font.c`): Liberation Sans, Liberation Serif and DejaVu
  Sans Mono, each in regular, bold, italic and bold italic, through
  `lib/ttf.h`'s metrics to MEASURE and `ui/uglyph.h` to DRAW, walking a
  run the same way in both so it draws exactly as wide as it measured.
  Measuring never rasterises -- uglyph's cache is of rendered glyphs, and
  layout asks for widths thousands of times a page. NetSurf's DPI is
  set to CSS's 96, the same the point-to-pixel conversion uses.
- **Drawing** (`ns_plot.c`) onto the window's surface, every clip NetSurf
  asks for cut to the page view's; **bitmaps** (`ns_bitmap.c`) declared
  to NetSurf in ugfx's own 0xAARRGGBB layout, so a decoded image is
  blitted without a conversion pass, with one cached scaled copy.
- **Timers** (`ns_sched.c`) run from the app's tick, which is re-armed
  for the next one -- the app sleeps until NetSurf has something to do.
- **The page widget** (`ns_view.c`'s ops in `ns_window.c`) is the shape
  stage 5's `uui_webview` takes: everything the app learns about a page
  arrives through `nsapp_*()` hooks.

**THE TABS ARE THE TITLE BAR'S** (`uapp_set_tabs()`, the compositor's,
as Files has them) rather than the mockup's separate strip -- Chrome's
and Edge's arrangement, and one toy-os already draws.

**PNG, JPEG and QOI** (`userland/backends/netsurf/image_uimg.c`, the
same day) decode through `lib/uimg.h` rather than libpng and libjpeg,
which are not vendored; GIF and BMP stay with NetSurf's libnsgif and
libnsbmp, which animate. The shape is upstream `jpeg.c`'s -- the header
read when the bytes are in, the pixels decoded on the first draw into
`image_cache`, which may evict them -- plus a THREAD: an image over
64 KiB decoded is decoded off the main thread from a copy of its bytes,
its box empty until a main-thread timer sees the job finish and asks
for the redraw that takes the pixels. Smaller ones decode in place,
where a thread costs more than they do. `lib/uimg.h`'s PNG reader grew
every bit depth and Adam7 for it, which the Image Viewer and the
thumbnails gained too.

**The address field holds 8 KiB** (`NS_URL_MAX`), through
`uui_textbox_set_max()`: a field may now ask for more than the
toolkit's 127 bytes, its text then on the heap. Typing a long address
exposed a cost in the toolkit: the field re-measured its whole visible
slice once per character to find where to start drawing, which was
seconds per key past a few hundred characters under TCG. It is a binary
search now.

**A page's fonts are three families in four styles**, real faces rather
than an upright slanted in software (what FreeType and Skia fall back
to when a family has no italic). CSS's generic `serif`, `sans-serif` and
`monospace` pick the family; `cursive` and `fantasy` draw in sans. The
named families pages ask for most map onto the face that stands in for
them, as fontconfig's aliases do -- Arial and Helvetica to Liberation
Sans, Times New Roman and Georgia to Liberation Serif, Courier New and
Consolas to DejaVu Sans Mono -- and Liberation is metric-compatible with
Arial, Times New Roman and Courier New, so a page wraps where its author
saw it wrap. Each face is opened the first time a page asks for it, and
a missing one falls back upright, then regular, then sans. The italics
are styles of a face as `-bold` is (`font_face.h`), so the font pickers
list Liberation Serif once and never an italic.

### Stage 4 -- what makes it a browser -- IN PROGRESS

Chosen 2026-10-13 on the Browser stage 4 canvas: S1 (words in the
address field: a lock, a red "Not secure" chip, a "File" chip, a zoom
badge, a star), P1 (the lock's connection card: protocol, cipher,
certificate, cookies), D1 (a downloads flyout, its list a shared
widget), H1 (bookmarks and history in a sidebar), and the ☰ menu with
find in page floating at the page's top right, as Notepad's does.

**Built: the ☰ menu, zoom and find in page.** The menu is a bar-less
`uui_menubar` opened under its button (Terminal's). Zoom is NetSurf's
own scale, per tab, along Chrome's steps from 25% to 500% (Ctrl+=,
Ctrl+-, Ctrl+0). Find is NetSurf's search -- it finds, highlights every
match and scrolls to the current one -- with the bar's "N of M" counted
by a LINK-TIME HOOK rather than an edit to the vendored source: NetSurf
keeps its match list private, so the browser links with
`--wrap=content_textsearch_add_match` and counts each match a new
search adds (`userland/browser/ns_find.c`). NetSurf's search stops at
either end; the bar wraps, as Chrome's and Firefox's do. Two things are
upstream's and stay so: a match draws in the SELECTION's colours (no
yellow, no distinct current match), and a text box shows only its first
match highlighted, though every match is counted and stepped to.

**Built: the address field's states and the lock's card.** The field
is a frame (`userland/browser/ns_addr.c`) around the app's own text
field drawn bare, so the field keeps its focus, editing and edit menu;
the chip before it comes from NetSurf's page-info state (a lock for
https, a red "Not secure" for http or a refused certificate, "File",
"Web Browser" for the browser's own pages), and a zoom badge after it
shows off 100% and resets on a click. The card (`ns_card.c`) is drawn
in the window as an overlay, so a click outside closes it and still
reaches the page. Its facts come from three places: the protocol and
cipher from libhttp, recorded per host by the fetcher, since NetSurf
keeps neither; the certificate's names and dates decoded by
`utls_cert_describe()`; and the cookies from NetSurf's cookie store,
matched to the host as RFC 6265 sends them and cleared the same way.
**A FETCH OVER A POOLED CONNECTION MAKES NO HANDSHAKE**, so it brings no
certificate of its own: the fetcher hands NetSurf the chain that host
presented last, or the card of the second page from a site would have
none.

Downloads go through NetSurf's own download path, which takes the
response already arriving -- a second fetch through `uhttp_download`
would lose a POST's result and a download that needs the session's
cookie.

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

## The case against

It is the largest port this tree will carry, from a three-person
project with releases years apart, and it ends at the 2010s web:
anything built on modern JavaScript stays out of reach whichever
engine route is taken, short of a C++ runtime and a Ladybird port. What
it buys is a real browser for documents within a few stages, an
engine nobody here has to write, and a content-process boundary that
any later engine would sit behind unchanged.
