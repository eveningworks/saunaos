# netsurf -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`. Do not edit it**: change the C library, or
our front end in `userland/backends/netsurf/`, or re-run the tool at a
new pin. `docs/browser-design.md` is the plan this serves.

- **Upstream**: https://github.com/netsurf-browser/netsurf
- **Commit**: `39da3c3a40af4566d86500ff3052dfdc7f9a0378` (2026-02-19)
- **Files**: exactly what a host build of NetSurf's `monkey` front end
  compiled from this repository, read off the compiler's dependency
  files, plus the public `include/` tree and `COPYING`. `sources.mk`
  (the tool's) lists the translation units -- not every `.c` here is one. Generated
  sources are the host build's output, vendored with the rest.
- **Not taken**: `frontends/monkey/main.c` -- its loop waits in
  `select()`, which toy-os has not got; `userland/tests/nsmonkey.c`
  replaces it, and `content/fs_backing_store.c`, the disk cache only
  that main.c named. `generated/testament.h` is the tool's, not upstream's
  (upstream's records the build host). The four stylesheets under
  `resources/` and `generated/res/Messages` are run-time resources,
  staged on the image at `/usr/share/netsurf/`. `generated/duktape/` is
  nsgenbind's output on the host: the DOM bindings for Duktape, from
  the `.bnd` files and the WebIDL beside `dukky.c`.
- **Licence**: `COPYING`, upstream's own file.
