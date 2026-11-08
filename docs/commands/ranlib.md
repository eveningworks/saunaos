# ranlib

**a `/usr/bin` program** -- an archive's index.

**Category:** Programming

## Synopsis

    ranlib archive...

## Description

Writes or refreshes an archive's symbol index -- the same as `ar s`, kept for build scripts that call it.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/ranlib`.
