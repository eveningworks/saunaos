# libnsbmp -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`. Do not edit it**: change the C library, or
our front end in `userland/backends/netsurf/`, or re-run the tool at a
new pin. `docs/browser-design.md` is the plan this serves.

- **Upstream**: https://github.com/netsurf-browser/libnsbmp
- **Commit**: `ea063c9f46acb43e90208da14073332b505ef7e7` (2026-01-05)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Licence**: `COPYING`, upstream's own file.
