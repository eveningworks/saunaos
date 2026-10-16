# expat -- vendored, not written here

**This directory is third-party source, copied by
`tools/netsurf_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/backends/expat/patches/`, which the tool applies to the
checkout before every copy. `expat_config.h` is ours, in `userland/backends/expat/`.

- **Upstream**: https://github.com/libexpat/libexpat
- **Release**: `R_2_9_0`, commit `e127b43ac1f514e0b320508893aa338ecd5e72be` (2026-10-05)
- **Files**: `expat/lib/`'s translation units that `lib/Makefile.am`
  lists unconditionally, plus `random_getentropy.c` (the entropy source
  toy-os's configuration picks), the headers they reach (`gcc -MM`), and
  `COPYING`. Under libdom's XML binding, for libsvgtiny.
- **Licence**: `COPYING`, upstream's own file.
