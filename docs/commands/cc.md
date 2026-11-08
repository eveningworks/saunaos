# cc

**a `/usr/bin` program** -- GCC's C compiler driver; `gcc` is the same program.

**Category:** Programming

## Synopsis

    cc [options] file... [-o output]
    gcc [options] file... [-o output]

## Description

Compiles C on the machine: `cc hello.c -o hello && ./hello`. It is GCC
16.2 with binutils 2.47, built for SaunaOS (`docs/cc-design.md`, stage
3), and the same compiler the tree is built with -- every GCC option
works as GCC documents it, `-c`, `-S`, `-E`, `-O2`, `-g`, `-Wall`,
`-std=c23` included. `cc` runs `cc1`, then `as`, then `ld`, as child
processes, with temporary files in the scratch directory
(`storage.tmpdir`).

**THE LINK LINE IS THIS SYSTEM'S**, built into the driver's specs: a
static program at the base every `/bin` program uses, against the SDK in
`/usr/include` and `/usr/lib` -- the C library and libgcc, with the
stack protector on; the toolkit's four archives are there to name
(`-lui -lmedia -lnet -lbase`, in that order). A program built here runs as `./name` or under
`spawn`. No flag is needed for any of it, and the flags the tree passes
by hand (`-fpie`, `-mno-direct-extern-access`,
`-mstack-protector-guard=global`) are the defaults.

## What it deliberately does not do

- **It is not a way to rebuild SaunaOS.** The tree needs `nasm`, GNU
  make, Python and the ISO tools, none of which are here; the kernel's
  flags (`-mcmodel=kernel` and the rest) are a cross build's. This
  compiles programs, not the system (`docs/cc-design.md`, "This is NOT
  self-hosting").
- **Static programs only.** `-shared` and a program linked against
  `/lib/libc.so` are not offered by the specs.
- **No LTO, no plugins, no OpenMP, no sanitizers** -- configured off.
- **`-time` and `-ftime-report`'s child times** are not available:
  the system does not add a reaped child's CPU time to its parent, so
  `wait4()` refuses to report one.

## The trap

The compiler is on the disk only when the image was built after `make
native-toolchain`; a default image has no `/usr/bin/cc`. And it finds
its own `cc1` by looking its name up on `PATH`: a program started with
no environment (from the kernel's debug console, say) must be given
`PATH=/bin:/usr/bin`, which every login session already has.

It needs memory: `cc1` takes about 50 MB on a small C file, and `c++`
far more (`docs/commands/c++.md`). `cc -pipe` on a 256 MiB guest fails
to start `cc1` (`docs/bugs.md`).
