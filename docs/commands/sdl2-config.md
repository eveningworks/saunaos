# sdl2-config

**a `/usr/bin` script** -- the compiler and linker flags for a program written against SDL2.

**Category:** Programming

## Synopsis

    sdl2-config [--prefix] [--exec-prefix] [--version] [--cflags] [--libs]

## Description

What a port's build system asks for SDL2, as on Linux:
`cc game.c -o game $(sdl2-config --cflags --libs)`. `--cflags` is
`-I=/usr/include/SDL2 -D_REENTRANT` and `--libs` is `-lSDL2`; SDL2 is
sdl2-compat over SDL3 (`docs/sdl-design.md`), and the program needs
`/lib/libSDL2-2.0.so.0` when it runs. `--version` is sdl2-compat's,
`--prefix` and `--exec-prefix` are `/usr`.

The `=` in the include path means "the compiler's sysroot", so the same
answer serves `cc` here and the host's `x86_64-saunaos-gcc`, whose copy
of this script is the SDK's (`build/sysroot/usr/bin/sdl2-config`). SDL3
has no config script, as upstream: `pkg-config sdl3`
(`/usr/lib/pkgconfig/sdl3.pc`), or simply `-lSDL3`.

## What it deliberately does not do

- **`--static-libs` is refused**: SDL is a shared library here.

## The trap

It is on the disk only with the compiler (`docs/commands/cc.md`, "The
trap"): an image built after `make native-toolchain`.
