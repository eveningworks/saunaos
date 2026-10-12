# libdom -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`. Do not edit it**: change the C library, or
our front end in `userland/backends/netsurf/`, or re-run the tool at a
new pin. `docs/browser-design.md` is the plan this serves.

- **Upstream**: https://github.com/netsurf-browser/libdom
- **Commit**: `f69781e1f062444b5af3f62d431d7d94018da53b` (2026-01-03)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Not taken**: `bindings/xml/` -- the XML parser binding, over
  expat, which NetSurf does not use in this configuration.
- **Licence**: `COPYING`, upstream's own file.
