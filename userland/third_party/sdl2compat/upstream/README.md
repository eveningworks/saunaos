# sdl2compat -- vendored, not written here

**This directory is third-party source, copied by
`tools/fetch/sdl_vendor.py`, and the tool deletes and recopies it. Do not
edit it in place**: a change is a patch under
`userland/third_party/sdl2compat/patches/`, which the tool applies to the copy.
SaunaOS's own SDL code -- the build configuration, the time and
filesystem modules, the drivers -- is in `userland/third_party/sdl3/glue/`.

- **Upstream**: https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz
- **SHA-256 of the tarball**: `ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588`
- **Files**: `src/sdl2_compat.c` and `src/dynapi/SDL_dynapi.c`, the headers they reach, the public `include/` tree whole, and `include/SDL2/SDL_config_unix.h` as its CMake would write it (X11 off).
- **Licence**: `LICENSE.txt`, upstream's own file (zlib).
