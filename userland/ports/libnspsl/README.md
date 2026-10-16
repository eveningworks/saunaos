# libnspsl -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/libnspsl/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

- **Upstream**: https://github.com/netsurf-browser/libnspsl
- **Commit**: `82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a` (2026-01-05)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Licence**: `COPYING`, upstream's own file.
