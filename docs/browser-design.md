# A web browser

A staged plan for a Web Browser app: NetSurf's engine behind a toy-os
window. It answers "port or write our own, what does a port actually
need from this system, and in what order is it worth building?"

**STAGES 0-2 AND MOST OF 3 ARE BUILT (2026-10-12)** -- NetSurf and its
libraries are vendored, lay out a set of test pages identically to the
same source built on the host -- read from files, over http and over
https -- and the Web Browser app (Start -> Internet) browses the web.
Stage 4 was built on 2026-10-13, and stage 5 -- the engine in a
content process per view, the network in its own -- the same day. **The choices were made the same day**: port NetSurf
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
/bin/wm/apps/browser           the window (stage 3); since stage 5 it links
                               none of the above (userland/browser/)
/bin/webcontent                the engine, one process per view (stage 5):
                               userland/webcontent/ over all of the above
/bin/webnet                    the network process (stage 5), MIT
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

### Stage 4 -- what makes it a browser -- BUILT 2026-10-13

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

**Built: bookmarks and history**, in a sidebar left of the page
(Ctrl+B, Ctrl+H), as Firefox has them. Bookmarks are NetSurf's hotlist,
kept in its own HTML file at `/var/lib/browser/bookmarks` -- started
EMPTY rather than with NetSurf's default links -- and history is its URL
database, the pages browsed to, newest first under a heading per day.
Both are drawn by a toolkit widget, `ui/uui_entrylist.h` (entries under
headings, a search, a remove button on the hovered row), rather than
NetSurf's tree windows, which would look like NetSurf. The address
field's star and Ctrl+D add or remove the page. Ctrl+H and Backspace are
the same key code, so the browser reads the Ctrl bit before taking it
as History.

**One popup at a time**, as a Wayland popup grab allows: opening the
menu, the lock's card or the Downloads flyout closes the other two, and
a click outside closes the one open. The flyout is a CONTAINER (its list
is a child), and the toolkit's router used to gate every press outside
a container before its own press was asked -- so a flyout that closes on
an outside click never heard one. `ui/uui_route.c` now lets an overlay
owner's own press see it.

**The I-beam over text.** NetSurf leaves plain text to the window's
default arrow (`BROWSER_POINTER_AUTO`); the page widget asks NetSurf's
box tree whether the box under the pointer holds text, as NetSurf's own
hit tests do, and shows the I-beam Chrome and Firefox show there.

**Built: downloads.** They go through NetSurf's own download path
(`userland/browser/ns_download.c`), which takes the response already
arriving -- a second fetch through `uhttp_download` would lose a POST's
result and a download that needs the session's cookie. A file is
written to `/home/Downloads/<name>.part` and renamed when whole; a
failed or cancelled one is deleted; a name already there becomes
"name (1).ext", as Chrome's does. The flyout under the down arrow opens
when a download starts, and its list is a toolkit widget,
`ui/uui_transfers.h` (a row per file: progress, status, and the one
action its state allows), so the next list of transfers is not drawn
again. NetSurf decides a fetch is a download from its first bytes (an
`application/octet-stream` is sniffed) and makes the download's window
only on data that arrives after that: a connection that breaks before a
second piece of data is a failed PAGE, NetSurf's error page, and leaves
no row in the flyout. That is upstream's `download_context_callback()`.

**Real pages, 2026-10-13: three bugs below the browser, none in it.**
Wikipedia was the first real site tried on the ASUS, and each fix
exposed the next:

- **The allocator was quadratic** -- `heap_core.c`'s first-fit walk over
  every block; an article never finished laying out. Fixed in the
  allocator (`docs/decisions/kernel.md`, "The heap keeps its free blocks
  in size classes").
- **`calloc(0, n)` answered NULL**, and NetSurf's flex layout read it as
  out of memory for any empty `display: flex` box -- Wikipedia's Main
  Page laid out as its header alone. tolibc now returns a block, as
  glibc does; `data/tests/web/flex_empty.html` keeps
  `browser_hostcheck.py` on it.
- **`box_coords()` dereferenced a float's missing container** when an
  image finished after layout (a slow network's order). A second
  link-time hook, `--wrap=box_coords` (`userland/browser/ns_boxfix.c`),
  takes the parent instead. The vendored source stays untouched, as for
  find. `tools/web_server.py`'s `image_delay` gives a test that order.

### Stage 5 -- `uui_webview`: the engine as a content process -- BUILT 2026-10-13

Chosen 2026-10-13 (the Browser stage 5 canvas and two question rounds).

**Three kinds of process, Chrome's and WebKit's shape:**

```
 Web Browser app (MIT)                 Help, System Update (MIT)
 tabs, toolbar, card, flyouts,         uui_webview + an "app:" scheme
 bookmarks, history                    handler -- NO network process
 uui_webview per tab ---------------.          |
      |  uchan "wv<app pid>"        |          | uchan "wv<app pid>"
      |  + a ubulk record ring      |          |
 /bin/webcontent (GPL) ...one per tab   /bin/webcontent (GPL)
 NetSurf: parse, lay out, paint        file:, resource:, data:, app:
 into a shm pixel buffer
      |  uchan "wn<app pid>" + ubulk
 /bin/webnet (MIT) ...one per browser, spawned by it
 sockets, TLS, cookies, certificate exceptions, downloads
```

- **The app links no NetSurf**, so the Web Browser, Help and System
  Update are MIT programs; `/bin/webcontent` is the one GPL binary.
- **Isolation here is CRASH ISOLATION, not containment.** Chrome's,
  Firefox's and WebKit's content processes are sandboxed; toy-os has no
  per-process sandbox, so a compromised content process can do what any
  process can. What the split buys is that a page that crashes NetSurf
  costs one tab, the licence boundary, and the shape a sandbox would
  later slot into. `umdf-design.md` says the same of its drivers.
- **The network leaves the content process** (WebKit's NetworkProcess,
  Chrome's Network Service). `/bin/webnet` owns the sockets, TLS, the
  connection pool, the cookie jar (`lib/ucookie.h`, RFC 6265, MIT --
  amending the earlier "cookies in the app" line), the certificate
  exceptions and the downloads, so a download outlives the tab that
  started it. Content processes and the app are its clients. A webview
  with no network process (Help, release notes) reaches only local
  schemes; a click on an http(s) link there is handed to the app.

**The surface: a pixel buffer and dirty rectangles**, WebKitGTK's
software path (`UpdateInfo`). The content process paints into a
viewport-sized shm buffer it created and granted to the app, then sends
FRAME (a scroll offset to apply, then the rectangles that changed). The
widget copies those rectangles into ITS OWN copy of the page and acks;
the content process does not paint again until the ack, so neither side
reads a half-drawn frame. A scroll is a blit of the old pixels plus a
paint of the exposed strip. The widget's copy is also the C3 view: a
crashed page's last frame is already in the app.

**The protocol: uchan as the doorbell, records in a ring.** The app is
the uchan server for its views (`uchan_server_scan()` reports a dead
content process for free) and a content process its client. Messages a
uchan slot cannot hold -- an 8 KiB URL, a title, a page's certificate --
travel as framed records in `lib/ubulk.h`: per client, one shm object
with a byte ring each way, created by the client and granted to the
server, the uchan message only saying "look". Everything rides the
records; the planned seqlock "state page" was not needed once the app
keeps the state the records describe.

**Input** goes to the content process in view coordinates, as Chrome's
browser process routes it. Keys the page might want (printable, editing,
arrows, paging, Tab, Enter, Esc, Ctrl+A/C/V/X/Z/Y) are forwarded and
claimed; everything else (Ctrl+T, F5, Alt+Left) stays the app's. What
NetSurf declines, the content process acts on itself -- scroll, or Esc
stopping a load -- since it owns the scroll offset.

**Lifetime:** a RAM-derived cap on live content processes; past it the
least recently shown background tab's process exits, the tab keeps its
URL and last frame, and it reloads when shown (Chrome's tab discarding,
Firefox's tab unloading).

**Bookmarks and history** move into the app as `lib/ubookmarks.h` and
`lib/uhistory.h`: one entry per line, tab-separated, imported once from
NetSurf's files. A content process reports each committed page
(VISITED); the app records it.

**Help and What's new** embed a webview with an `app:` scheme handler
(WebKit's `WKURLSchemeHandler`, Qt's `QWebEngineUrlSchemeHandler`): the
content process asks the embedding app for `app:` URLs, and the app
answers with HTML it made from its Markdown (`lib/umd.h` gains an HTML
writer) and a stylesheet from the theme.

**The crash view is C3**: the last frame kept, greyed, under a red bar --
"This page stopped." -- with Reload and View report (Crash Reports at
`/var/crash/webcontent-<pid>.crash`); the tab's title carries a
stopped-page icon.

What is per content process and therefore lost on a crash or an unload:
`:visited` colouring for pages visited in other tabs. The back/forward
list and HTTP logins were too, until the next section moved them out.

**What a test reads.** A page drawn in another process has no geometry
the app could report -- so the content process reports its LINKS (each
link box's rect in view pixels and its address, those in view first,
`WV_LINKS`), and the widget's `describe` op logs them with its pid,
whether it stopped, the stopped bar's buttons and its URL. An
accessibility tree's hyperlinks in miniature; `help_test.py` and
`sysupdate_test.py` click by it.

**Help and What's new, chosen 2026-10-13 (canvas H2 and N2).** Help's
pages take a document's look -- serif headings, a readable measure, a
page name written as its title and linked -- from a stylesheet the
widget makes from the live theme (`uui_webview_markdown_page()`), over
`lib/umd.h`'s HTML writer. NetSurf has no `<details>`, so What's new's
"Under the hood" fold became a LINK to a second page of its own, with a
Back link. A build without the engine (NOGPL=1) keeps both drawing
their Markdown with `uui_markdown`.

**Measured on the ASUS, 2026-10-13**: Wikipedia's "Operating system"
article holds its content process at 36 MiB, the browser app at 18 MiB,
`/bin/webnet` at 4 MiB. The live cap is (RAM - 512 MiB) / 160 MiB,
never under two -- every tab on a 2 GiB machine, since a window holds
eight.

**Two things below the browser it found:**

- **SHARED MEMORY WAS GRANTED TO A THREAD, NOT ITS PROCESS.**
  `kernel/mm/shm.c` checked an opener against `scheduler_current_pid()`
  -- the calling THREAD -- so a worker thread of a process that had been
  granted an object was refused it. webnet's fetch threads, waiting for
  room in a content process's ring, re-open its page to see that it
  still lives; refused, they took it for dead and ended the fetch. A
  page bigger than the 256 KiB ring (Wikipedia's article, never a test
  fixture) is the one that waits, so it never loaded. The check is the
  PROCESS's now (`scheduler_current_tgid()`), and `shm_test` has a
  thread open and grant.
- **A uchan service name with a dot cannot be found again**: a client
  finds its server by the part of its ring's name before the first dot.
  The names are `wv<pid>` and `wn<pid>`, and uchan refuses a dotted one.

### After stage 5 -- what outlives a content process -- BUILT 2026-10-16

Chosen 2026-10-16 (canvas A1/A2, H1/H2): stage 5 put each tab's engine
in a process, and three things still died with it or were missing.

**Back/forward is the app's list** (`uui_webview`'s `hist[]`), as
Chrome's NavigationController and Firefox's session history in the
parent keep it, not NetSurf's: a list in the engine dies with the
engine. The content process reports each page it commits (`WV_COMMIT`,
from a link-time wrap of NetSurf's `browser_window_history_add()`); Back
and Forward are `WV_GO`, which loads an entry without adding one and,
once that load has finished, scrolls to where the entry was left --
after NetSurf's own scroll restore, which would otherwise put it at the
top. The scroll noted for an entry is the last one reported while the
page was at REST: a navigation resets the scroll as it starts, before
its commit arrives. 50 entries, Chrome's `kMaxSessionHistoryEntries`. A
stopped or unloaded view goes Back by starting a process for the entry.

**The tabs survive a restart** (`browser/ns_session.c`, Firefox's
sessionstore): `/var/lib/browser/session.tsv`, every tab's list and
which was in front, rewritten two seconds after a title or address
changes and when the window closes. Restored tabs start unloaded but
the one in front; a page named on the command line opens in a tab of
its own beside them, as Chrome and Firefox do.

**HTTP logins are webnet's** (canvas A2), as Chrome's network service
and its LoginHandler hold them: a 401 with a Basic challenge is held,
not forwarded, while webnet asks the app (`WN_AUTH_ASK`), which shows
`uui_signin` over the tab whose content process asked; the answer goes
from the app to webnet and never through a content process, which drops
any Authorization it is sent and no longer runs NetSurf's in-page form
(whose strings were never installed: it showed `LoginTitle`). Fetches
for one origin and realm wait on one ask. The credential is memory-only,
per origin and realm, sent unasked within the directory it was given
for (RFC 7617's protection space); a refused one asks again with
"Wrong user name or password", three tries, and the lock card's Clear
forgets the host's logins with its cookies. Cancel releases the held
401 and the page shows its body. Basic only: Digest is on the roadmap.

**A typed host falls back to http** (canvas H1), Chrome's
HTTPS-Upgrades rather than its opt-in HTTPS-First page: only a host
typed without a scheme on the default port, only when https cannot be
reached at all -- no connection within 3 s (`uhttp_request`'s
`connect_timeout_ms`, set by webnet for a `WN_FETCH_UPGRADED` fetch), a
failed handshake, a refused certificate -- and never for a link or an
`https://` typed out. The content process swallows the failure before
NetSurf draws an error page and loads the http twin itself; the app
tries that host over http first for the rest of the session, and the
address field's red "Not secure" says what happened.

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
