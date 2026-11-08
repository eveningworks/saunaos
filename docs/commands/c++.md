# c++

**a `/usr/bin` program** -- GCC's C++ compiler driver; `g++` is the same program.

**Category:** Programming

## Synopsis

    c++ [options] file... [-o output]
    g++ [options] file... [-o output]

## Description

`cc` for C++: the same driver, which runs `cc1plus` instead of `cc1`
and links libstdc++ (`/usr/lib/libstdc++.a`, headers in
`/usr/include/c++/16.2.0`). Exceptions, RTTI, `std::thread`,
iostreams and the rest of the standard library work, as they do for a
C++ program cross-built on the host. `import std` has its module
source installed (`/usr/lib/libstdc++.modules.json`) but has not been
tried on the machine. Everything `docs/commands/cc.md` says about the link line and
what is not offered applies here too.

## Memory

`cc1plus` is the largest program on the disk and the hungriest: about
115 MB for an iostream hello world at `-O2`, against `cc1`'s 50 or so
for a C file. A 256 MiB guest (`make run MEM=256`) still compiles it;
`docs/cc-design.md`, stage 3, has the measurements.
