# NetSurf's SaunaOS side -- OURS, not vendored

`userland/ports/netsurf/` and its libraries are upstream's
(`tools/netsurf_vendor.py`), plus the patches in `patches/` here, which
the tool applies -- never an edit made there, which the next re-vendor
deletes. Everything NetSurf needs from SaunaOS
that upstream's tree cannot say is here, so the boundary between the
two is a directory, as it is for `userland/backends/doom/`.
`docs/browser-design.md` is the plan.

| File | Why it exists |
|---|---|
| `saunaos_config.h` | Force-included into every NetSurf unit in place of its `utils/config.h`, which picks features by an `#if` per OS and has no SaunaOS branch. Each switch is a fact about tolibc. |
| `include/zlib.h`, `zlib_shim.c` | The corner of zlib NetSurf calls, over SaunaOS's own `lib/uinflate.h`. Not zlib: anything else NetSurf calls is a compile error. |
| `include/curl/curl.h` | A type NAME. Upstream's `content/fetch.c` includes the curl fetcher's header even when curl is not built. |
| `include/dom/bindings/hubbub/`, `include/dom/bindings/xml/` | Forwarders: NetSurf includes libdom's HTML binding, and libsvgtiny its XML one, where libdom INSTALLS them; the vendored tree keeps them elsewhere. |
| `patches/` | Our changes INSIDE NetSurf's own files, one `-p1` patch each, applied by `tools/netsurf_vendor.py` before the host build and the copy (`docs/conventions/build.md`). |
| `fetch_http.c`, `fetch_http.h` | The http: and https: fetcher, over `/lib/libhttp.so`: one thread per fetch, its results queued for NetSurf's poll on the main thread. Upstream's is curl; this is that fetcher's shape (`docs/browser-design.md`, stage 2). |
| `image_uimg.c`, `image_uimg.h` | PNG, JPEG and QOI content through `lib/uimg.h` in place of libpng and libjpeg: header on load, pixels on first draw into NetSurf's `image_cache`, a large image decoded in a thread. |
| `no_sockets.c` | A `socket()` that refuses, for `desktop/gui_factory.c`'s default opener, which only the (unbuilt) curl fetcher reaches. tolibc has no `socket()`. |

The front ends that use all this are programs elsewhere:
`userland/tests/nsmonkey.c` (the headless one, replacing upstream's
`frontends/monkey/main.c`) and the Web Browser, `userland/gui/apps/browser.c`
with its front end in `userland/browser/`.
