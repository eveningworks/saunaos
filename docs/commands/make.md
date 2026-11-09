# make

**a `/usr/bin` program** -- GNU make, for building programs ON the machine with `cc` and `c++`.

**Category:** Programming

## Synopsis

    make [options] [target...] [variable=value...]

## Description

GNU make 4.4.1, built for SaunaOS unmodified (`docs/cc-design.md`,
stage 4): it reads `Makefile` in the current directory, works out from
the files' times which targets are older than what they are made from,
and runs only those recipes. Every option GNU make documents works as
documented -- `-C dir`, `-f file`, `-n`, `-k`, `-B`, `-j N`, `-p` --
and so do its built-in rules: with `CC` defaulting to `cc` and `CXX` to
`g++`, a Makefile that names only `prog: main.o util.o` builds both
objects and links them.

    # make
    # touch util.h && make      (rebuilds what includes util.h, nothing else)
    # make clean && make -j2

**A recipe with shell syntax in it runs under `/bin/sh`**, which is
dash (`docs/commands/dash.md`); a plain command (`cc -O2 -c main.c`) is
run directly, as GNU make does everywhere. `-j` works, through GNU
make's job server, which a sub-make (`$(MAKE) -C dir`) joins. The
machine has one CPU for now, so `-j2` overlaps waiting -- on the disk,
on a `sleep` -- rather than compiling twice as fast.

## What it deliberately does not do

- **It is not a way to rebuild SaunaOS** -- the tree's own Makefile
  wants `nasm`, Python and the ISO tools (`docs/commands/cc.md`).
- **No `load` directive and no Guile** -- configured off; `load` opens a
  shared object, and this make is static.
- **`-O` (`--output-sync`) does nothing but warn**: it serialises
  output with a file lock, there is no file locking in this system
  (`fcntl` refuses with ENOLCK), and make then prints "Cannot acquire
  output lock, disabling output sync" and runs unsynchronised.
- **No `-l` (load average)**: the system reports none, so make never
  holds a job back for one.

## The trap

**File times here are whole SECONDS**, and make rebuilds a target only
when something it depends on is strictly NEWER. A header edited in the
same second its object was built is not seen as newer, and that object
is not rebuilt -- wait a second, or `make -B` to rebuild everything.
Real systems with nanosecond times rarely meet this; FAT's two-second
times always did.

Like `cc`, it is on the disk only when the image was built after `make
native-toolchain` (`docs/commands/cc.md`, "The trap"), and it finds
`cc` on `PATH`, which every login session has.
