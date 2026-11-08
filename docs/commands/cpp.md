# cpp

**a `/usr/bin` program** -- the C preprocessor.

**Category:** Programming

## Synopsis

    cpp [options] [input [output]]

## Description

Runs GCC's preprocessor alone: `#include`, `#define` and `#if`
expanded, the result printed -- what `cc -E` does. The include path is
the compiler's: `/usr/include`, then GCC's own headers, then the kernel
ABI headers in `/usr/include/saunaos`. Part of the native compiler
(`docs/commands/cc.md`).
