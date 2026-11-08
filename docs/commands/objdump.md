# objdump

**a `/usr/bin` program** -- an object's contents.

**Category:** Programming

## Synopsis

    objdump [options] file...

## Description

Disassembles (`-d`), and prints headers (`-h`, `-x`) and relocations (`-r`) of an ELF object or program.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/objdump`.
