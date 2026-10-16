# libdom -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/libdom/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

- **Upstream**: https://github.com/netsurf-browser/libdom
- **Commit**: `f69781e1f062444b5af3f62d431d7d94018da53b` (2026-01-03)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Not taken**: `bindings/xml/libxml_xmlparser.c` -- the binding
  over libxml2. The expat one is taken, for libsvgtiny.
- **Licence**: `COPYING`, upstream's own file.
