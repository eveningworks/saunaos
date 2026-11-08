# objcopy

**a `/usr/bin` program** -- copy and convert an object.

**Category:** Programming

## Synopsis

    objcopy [options] input [output]

## Description

Copies an object, changing it on the way: strip sections, add one from a file (`--add-section`), convert to raw binary (`-O binary`). `-p` cannot keep the times -- no filesystem here can be told one (`utime()` refuses) -- and says so.

It is GNU binutils 2.47, built for SaunaOS with the native compiler (`make native-toolchain`; `docs/cc-design.md`, stage 3), and takes every option binutils documents. An image built without that target has no `/usr/bin/objcopy`.
