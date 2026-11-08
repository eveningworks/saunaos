# ld

**a `/usr/bin` program** -- the linker.

**Category:** Programming

## Synopsis

    ld [options] object... [-o output]

## Description

Links objects and archives into a program. `cc` calls it with this system's link line (the start code, `-T /usr/lib/saunaos.ld`, the C library), which a hand-written `ld` command must repeat -- `cc -v` prints the one it uses. Static links only.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/ld`.
