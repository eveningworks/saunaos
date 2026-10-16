# libnsutils -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/libnsutils/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

- **Upstream**: https://github.com/netsurf-browser/libnsutils
- **Commit**: `0bd39060740b6163bd50875326654a722df97eb2` (2026-01-05)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Not taken**: `src/unistd.c` -- `nsu_pread()`/`nsu_pwrite()`, whose
  only caller is NetSurf's disk cache (not taken either), over
  `pread()`, which toy-os has not got.
- **Licence**: `COPYING`, upstream's own file.
