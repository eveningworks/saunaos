# toolchain/ -- the x86_64-saunaos cross compiler

GCC (C and C++) and binutils, taught a target named `x86_64-saunaos`, built
on the host (`docs/cc-design.md`, stages 1 and 2), and the same two built to
run on SaunaOS (stage 3). Only `patches/` and this file are tracked.

    make toolchain EXTRAS=1   # fetch the pinned tarballs (licence shown), then build
    make toolchain            # build offline; refuses if the tarballs are missing
    toolchain/local/bin/x86_64-saunaos-gcc hello.c -o hello
    toolchain/local/bin/x86_64-saunaos-g++ -std=c++17 hello.cpp -o hello
    make native-toolchain EXTRAS=1   # + GMP/MPFR/MPC; GCC and binutils that run ON SaunaOS
    make iso                         # ...which every image from here on carries in /usr

| Path | What | Tracked |
|---|---|---|
| `patches/binutils/*.patch`, `patches/gcc/*.patch` | our change: the target definition, `-p1`, applied in name order; the first line says what each does | yes |
| `tarballs/` | `gcc-16.2.0.tar.xz`, `binutils-2.47.tar.xz`, and for the native compiler `gmp-6.3.0.tar.bz2`, `mpfr-4.2.2.tar.bz2`, `mpc-1.3.1.tar.gz`, pinned in `tools/fetch/fetch_toolchain.py` | no |
| `build/` | `build.log` of the last build (the build trees are deleted once installed) | no |
| `build-native/` | `build.log` of the last `make native-toolchain` (`tools/build/build_native_toolchain.sh`; `--keep` keeps its tree) | no |
| `native/` | the compiler that runs ON SaunaOS, staged as the image's `/usr` (`docs/cc-design.md`, stage 3): `usr/bin/{cc,c++,gcc,g++,as,ld,...}`, `usr/libexec/gcc/x86_64-saunaos/16.2.0/{cc1,cc1plus,collect2}`, libgcc and libstdc++ in `usr/lib`, the C++ headers in `usr/include/c++`, the licences in `usr/share/licenses`. While it exists, every image this checkout seeds carries it; delete it and the next seed takes it off again | no |
| `local/` | the prefix: `bin/x86_64-saunaos-{gcc,g++}`, libstdc++ in `x86_64-saunaos/{lib,include/c++}`, and `x86_64-saunaos/sysroot`, a symlink to `../../../build/sysroot` (`make sdk`) | no |

All of it is OUTSIDE `build/`, so `make clean` keeps it. The patches are
GPL-3.0-or-later, as the files they change are (`LICENSE`, "Third-party code
that is FETCHED, not vendored"). `gcc/config/saunaos.h`, which the GCC patch
adds, holds the defaults and the link line; `docs/decisions/build.md` says why
they are what they are. Another checkout's toolchain is used with
`TOOLCHAIN=<that checkout>/toolchain/local`.
