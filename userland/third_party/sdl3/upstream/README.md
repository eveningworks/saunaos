# sdl3 -- vendored, not written here

**This directory is third-party source, copied by
`tools/fetch/sdl_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/third_party/sdl3/patches/`, which the tool applies to the copy.
SaunaOS's own SDL code -- the build configuration, the time and
filesystem modules, the drivers -- is in `userland/third_party/sdl3/glue/`.

- **Upstream**: https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
- **SHA-256 of the tarball**: `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`
- **Files**: the translation units in `sources.mk` (SDL's CMake globs, and one directory per driver SaunaOS picks), the headers they reach (`gcc -MM`), and the public `include/` tree whole.
- **Licence**: `LICENSE.txt`, upstream's own file (zlib).
- **Patched, by SaunaOS**, from `userland/third_party/sdl3/patches/`:
  - `0001-thread-no-per-thread-mask-or-priority.patch` -- SaunaOS: no per-thread signal mask and no thread priority on a private platform.
