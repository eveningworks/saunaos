# readelf

**a `/usr/bin` program** -- ELF structure.

**Category:** Programming

## Synopsis

    readelf [options] file...

## Description

Prints an ELF file's headers, segments, sections, symbols and dynamic section (`-a` for all of it), without the BFD layer `objdump` reads through.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/readelf`.
