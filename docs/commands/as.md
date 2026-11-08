# as

**a `/usr/bin` program** -- the assembler.

**Category:** Programming

## Synopsis

    as [options] file.s... [-o file.o]

## Description

Assembles AT&T-syntax x86-64 into an ELF object. `cc` runs it for every C or C++ file, with `-mrelax-relocations=no` -- a program here links above 4 GiB, where a relaxed GOT load cannot hold the address -- so an object assembled by hand for a `cc` link wants the same flag.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/as`.
