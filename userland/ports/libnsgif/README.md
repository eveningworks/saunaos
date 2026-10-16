# libnsgif -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/libnsgif/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

- **Upstream**: https://github.com/netsurf-browser/libnsgif
- **Commit**: `22e99eb6818b1284d0f3ff1b7f46159e87221220` (2026-08-06)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Licence**: `COPYING`, upstream's own file.
