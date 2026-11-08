# ar

**a `/usr/bin` program** -- the archiver.

**Category:** Programming

## Synopsis

    ar [options] archive file...

## Description

Makes and edits static libraries (`.a`): `ar rcs libfoo.a a.o b.o`. The `s` writes the symbol index `ld` needs, which is all `ranlib` adds.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/ar`.
