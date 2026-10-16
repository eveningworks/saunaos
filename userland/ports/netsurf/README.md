# netsurf -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/netsurf/patches/`, which the tool applies to the
checkout before every copy; or change the C library, or our front end in
`userland/backends/netsurf/`. `docs/browser-design.md` is the plan this
serves.

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
- **Patched, by toy-os**, from `userland/backends/netsurf/patches/` (applied to the checkout before the host build and the copy):
  - `0001-opacity-zero-form-control.patch` -- A form control at opacity 0 is not drawn (Wikipedia's Tools and menu checkboxes).
