# size

**a `/usr/bin` program** -- section sizes.

**Category:** Programming

## Synopsis

    size [options] file...

## Description

Prints the text, data and bss sizes of each object or program.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/size`.
