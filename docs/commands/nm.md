# nm

**a `/usr/bin` program** -- an object's symbols.

**Category:** Programming

## Synopsis

    nm [options] file...

## Description

Lists the symbols an object, archive or program defines and needs: `T` code, `D` data, `U` undefined. A program on the disk keeps `.symtab`, so `nm /bin/ls` works.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/nm`.
