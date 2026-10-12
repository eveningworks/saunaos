# NetSurf's toy-os side -- OURS, not vendored

`userland/ports/netsurf/` and its nine libraries are upstream's, byte for
byte (`tools/netsurf_vendor.py`). Everything NetSurf needs from toy-os
that upstream's tree cannot say is here, so the boundary between the
two is a directory, as it is for `userland/backends/doom/`.
`docs/browser-design.md` is the plan.

| File | Why it exists |
|---|---|
| `toyos_config.h` | Force-included into every NetSurf unit in place of its `utils/config.h`, which picks features by an `#if` per OS and has no toy-os branch. Each switch is a fact about tolibc. |
| `include/zlib.h`, `zlib_shim.c` | The corner of zlib NetSurf calls, over toy-os's own `lib/uinflate.h`. Not zlib: anything else NetSurf calls is a compile error. |
| `include/curl/curl.h` | A type NAME. Upstream's `content/fetch.c` includes the curl fetcher's header even when curl is not built. |
| `include/dom/bindings/hubbub/` | Forwarders: NetSurf includes libdom's HTML binding where libdom INSTALLS it; the vendored tree keeps it elsewhere. |
| `no_sockets.c` | A `socket()` that refuses, for `desktop/gui_factory.c`'s default opener, which only the (unbuilt) curl fetcher reaches. tolibc has no `socket()`. |

The front ends that use all this are programs elsewhere:
`userland/tests/nsmonkey.c` (the headless one, replacing upstream's
`frontends/monkey/main.c`).
