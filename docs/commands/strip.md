# strip

**a `/usr/bin` program** -- remove symbols.

**Category:** Programming

## Synopsis

    strip [options] file...

## Description

Removes symbols and debug sections from an object or program, in place. `-p` cannot keep the times (see `objcopy`).

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/strip`.
