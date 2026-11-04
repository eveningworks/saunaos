# A C and C++ compiler that runs on SaunaOS: GCC

A staged plan, in the shape `docs/libc-design.md` and
`docs/dynlink-design.md` used. It answers one question -- **can you
write a C or C++ program on this machine, compile it on this machine, and run
it?** -- and it deliberately does not answer the larger one next to it.

**Status: PLANNED. Nothing below is built. THE COMPILER IS GCC WITH
BINUTILS** (chosen 2026-11-04), over TCC, because only GCC gives C++.
The comparison stays under "Two paths"; the decision is
`docs/decisions/build.md`'s "The compiler on the machine is GCC with
binutils, because only GCC gives C++".

## This is NOT self-hosting, and that distinction is the whole document

`docs/roadmap-details.md`'s out-of-scope list names **"a self-hosted C
compiler"** among the things SaunaOS deliberately does not chase, beside
an own bootloader and extra CPU architectures. That line stands. It
rules out *SaunaOS rebuilding SaunaOS*, which is a different and much
larger thing than *SaunaOS compiling a program*.

The measurement that settles it: this tree compiles with
`-mcmodel=kernel`, `-mstack-protector-guard=global`,
`-ftls-model=local-exec`, `-mno-direct-extern-access`,
`-fno-tree-loop-distribute-patterns` and `-Wframe-larger-than`. No small
compiler implements any of them, so "SaunaOS builds SaunaOS" means porting
GCC and Make, not porting a compiler. That is not this.

GCC changes one thing about that: it is the first third of the way to
self-hosting rather than none of it. The rest -- `nasm`,
the Python under `tools/`, `grub-mkrescue` and `xorriso` for the ISO --
is still not this document.

What IS in scope is the line below, on the machine's own console:

    # cc hello.c -o hello
    # ./hello

## The honest case against, first

Because it is real, and a plan that omits it is selling something.

- **Nobody has asked to program on the machine.** Every program in this
  tree is written in an editor on a Linux host with `clangd`, `bear`,
  `gdb` and a 2-second build. An on-target compiler competes with that
  and loses at every measurable thing. What it buys is not productivity;
  it is that the machine stops being a target and becomes a computer.
  That is a legitimate thing to want and a bad thing to pretend is
  practical.
- **It is the largest dependency the tree has ever taken, by far.**
  mbedtls is 203,755 lines and is a library nothing else can see; GCC
  and binutils are millions, which is why they are FETCHED rather than
  vendored. And a compiler is a *user-facing program* -- its bugs are
  reported as "SaunaOS miscompiled my code", which is a support surface
  this project has never had.
- **The SDK has to be right or the compiler is a toy.** Shipping headers
  and libraries on-target (stage 0) means the on-disk copy can drift
  from the tree that built the kernel. That is a new and silent failure
  class: a program compiled on the machine against stale headers, with
  nothing to say so.
- **A half-working compiler is worse than none.** "It compiles hello.c"
  is a demo. "It compiles anything you write" is a promise, and the
  distance between them is most of the work. Stage 5's exit criterion
  exists to keep that honest.

Against all of that: the pieces are unusually favourable here, and
stage 0 is worth doing on its own merits whatever happens after it.

## What exists today, measured

- **There is no assembler, no linker, and no `make` on the machine.**
  `/bin` has none of `as`, `ld`, `make`, `cc`. This is the single
  constraint that decides which compiler, because most small C compilers
  emit *assembly text* and shell out to `as` and `ld`.
- **No SDK ships.** `/lib` carries `libc.so`, `ld-toy.so` and four other
  shared libraries; there are **no headers anywhere on the disk** and no
  static library. The build already produces `build/userland/libc.a`
  (920,770 bytes, 385 exported symbols) and `build/userland/rt/crt0.o` --
  they are simply never staged.
- **The kernel loads `ET_EXEC` only.** `kernel/proc/elf.c`'s `elf_load()`
  refuses anything else in as many words ("static, non-PIE only"), and
  handles `PT_INTERP` by loading `/lib/ld-toy.so`. That is exactly the
  classic non-PIE format a small compiler emits, so the OUTPUT format is
  not a problem -- it is the one thing that needs no work.
- **Userland links at a fixed base**, `0x8000000000` in
  `userland/rt/link.ld`, and the real link line for a `/bin` program is
  `crt0.o + <prog>.o + sys.o + stack_chk.o + sigtramp.o + tls.o +
  libuapp.a + libc.a`. An on-target compiler has to reproduce that, which
  is the job of GCC's specs (`gcc/config/saunaos.h`, stage 1).
- **tolibc has what a compiler needs**, checked symbol by symbol:
  `qsort`, `bsearch`, `strtod`, `strtoull`, `realloc`, `memmove`,
  `vfprintf`, `fflush`, `remove`, `rename`, `sscanf`, `atexit`, `mmap`,
  and a real `setjmp`/`longjmp` in assembly (`userland/libc/setjmp.S`) --
  which matters, because a compiler's error path is usually a `longjmp`.
  Missing and needed: `ftruncate`, which is not even declared.
- **The POSIX layer is `static inline` IN THE HEADERS**, over raw `sys_*`
  calls declared in `userland/rt/sys.h` -- `lseek`, `getcwd`, `isatty`
  and friends have no out-of-line definition and appear in no archive.
  So the on-target include root must carry `rt/sys.h` as well as
  `userland/include/`, or the headers do not compile. This is the
  finding most likely to be rediscovered painfully.
- **`errno` does not need TLS.** `<errno.h>` defines it as
  `(*__errno_location())`, a plain call. So a compiled program needs no
  thread-local storage at all, and a compiler with weak `__thread`
  support is not disqualified.
- **Vendoring at this scale is routine here.** mbedtls 203,755 lines
  against 566 of glue; doom 72,512 against 958; dash 20,418 against 600.
  The `userland/ports/<name>/` + `userland/backends/<name>/` +
  `EXTRA_OBJS_<name>` pattern is established and unchanged.
- **GPL-family code is already vendored.** `userland/ports/doom/` is
  GPL-2-or-later inside an MIT repository, declared in `LICENSE` and
  enforced by `tools/checks/check_licenses.py`. An LGPL compiler is the same
  aggregation, not a new category.

## What real systems do

- **Classic Unix** split the job into three programs -- `cc` drove `cpp`,
  `as` and `ld`. That split is why most small compilers assume an
  assembler exists, and why adopting one here would mean writing two more
  tools.
- **Plan 9** wrote its own compact toolchain (kencc) rather than adopting
  GNU, and was self-hosted on it. It is the strongest precedent for
  "write our own", and also a reminder of what that costs: a compiler, an
  assembler, a linker and a librarian, per architecture.
- **SerenityOS** ported the GNU toolchain and reached self-hosting over
  years. It is the honest picture of where the larger goal leads.
- **TCC** (Fabrice Bellard, LGPL 2.1) is the hobby-OS answer, for one
  structural reason: preprocessor, compiler, **assembler and linker** are
  one binary that writes ELF directly, so it needs no binutils. `tccboot`
  famously compiled a Linux kernel at boot with it.

- **xcc** (tyfkda, MIT, active in 2026) is the one other small compiler
  with its own assembler and linker. Its linker rules it out
  (`src/ld/ld.c`, read 2026-11-04): three x86-64 relocation types
  (`64`, `PC32`, `PLT32`), an unknown type SILENTLY SKIPPED with its
  bytes left unpatched, a hard-coded base (`0x01000000`), no TLS, no
  `PT_INTERP`. tolibc is built `-fpie -mno-direct-extern-access`, so
  `libc.a` is full of GOT relocations -- linked by xcc, a program would
  be quietly broken. Using it means rebuilding libc with xcc or
  rewriting its linker.
- **slimcc, kefir, cproc** emit assembly text and need an `as` and an
  `ld` -- the three-projects problem below.

**Among the small compilers SaunaOS should follow TCC's shape and differ
from Plan 9's**, and the reason is the measurement above rather than
taste: with no `as` and no `ld` on the machine, a compiler that emits
assembly text is not one project but three. Writing our own remains the
more characteristic choice for this tree and is recorded as the rejected
alternative, not as an unconsidered one. **The other real candidate is
not small: GCC with binutils**, which brings its own `as` and `ld` --
and is the one chosen, next section.

## Two paths: TCC or the GCC suite -- GCC chosen

|  | TCC | GCC + binutils |
|---|---|---|
| What runs on the machine | one binary: cpp, cc, as, ld | `gcc`, `cc1`, `as`, `ld`, `collect2`, plus GNU make |
| Language | C99, most of C11, some GNU extensions | C to C23, C++, every GNU extension, inline asm as ported code writes it |
| Code quality | no optimiser to speak of | the same compiler the tree is built with |
| Links tolibc's `libc.a` | yes: GOT relocations, TLS, archives, `-Ttext` | yes |
| Where the source lives | `userland/ports/tcc/`, vendored like dash | fetched at build time with our patches -- a ~90 MB tarball does not go in the repo; that reaches the network, so it sits with `EXTRAS=1` |
| On disk, in memory (estimates, not measured) | a few hundred KB; small | `cc1` alone tens of MB, the suite ~100-200 MB; hundreds of MB of RAM at `-O2` |
| New libc demands | `ftruncate`, little else | libstdc++'s (wchar, locale, stdio) and GCC's support code's (`getrusage`, `sys/resource.h`, `mkstemp`, `utime`, `sysconf`, ...) |
| Rough size (estimate, 2026-11-04) | 3-5 sessions to `cc hello.c` | 6-12 sessions to a C compiler on the machine |
| Upstream | no release since 0.9.27; development is the open-push `mob` branch, so one commit is pinned | regular releases |
| Self-hosting | a dead end | the compiler third of it |

**Where GCC is better for the future**: C++ (most software worth porting
next needs it), ported code compiling unmodified rather than tripping a
missing extension, real optimisation, one compiler on both sides so the
stage 5 comparison has nothing to explain, and the first step towards
self-hosting if that ever becomes a goal. **Where TCC is better**: it is
done in a third of the time, fits any guest, lives in the repo with no
download, and compiles fast under TCG.

**Chosen 2026-11-04: GCC, because C++ needs it.** No small C++
compiler exists to pair with TCC, so taking TCC first would have meant
a second compiler later anyway, and TCC's port and driver would have
been thrown away when GCC's driver replaced them. What TCC's plan looked
like is kept under "The path not taken: TCC", below.

## C++

**This is why GCC was chosen.** TCC compiles C only, and no small C++
compiler is a serious option (C++ is a far larger language than C; the
real ones are GCC and Clang). With GCC, C++ is nearly free once stage 2
is done: `--enable-languages=c,c++` adds `cc1plus` and `g++`, and
libstdc++ is already there because GCC itself needs it. Two levels:

- **C++ programs FOR SaunaOS, cross-built on the host** -- arrives with
  stage 2. Enough to port C++ software as `/bin` programs.
- **`g++` ON SaunaOS** -- arrives with stage 3, at the cost of another
  `cc1plus`-sized binary on the disk.

**What the runtime lacks today, found by reading (2026-11-04)** -- each
is needed by C++ and by `cc1` itself, so it is stage 2's work on either level:

- **Static constructors never run.** `userland/rt/link.ld` places no
  `.init_array`/`.fini_array` and `crt0.asm` calls nothing before
  `main`; a C++ global with a constructor would stay unconstructed. (C's
  `__attribute__((constructor))` has the same hole; nothing uses it.)
- **Exceptions cannot unwind.** The linker script keeps no
  `.eh_frame`/`.eh_frame_hdr`, and neither `ld-toy.so` nor libc has
  `dl_iterate_phdr()`, which libgcc's unwinder uses to find a shared
  library's unwind tables.
- **The C++ ABI's per-thread exception state** (`__cxa_get_globals`)
  sits in TLS; tolibc has TLS (`userland/rt/tls.c`), so this is a check,
  not a build.

## The licences

**Both fit beside the MIT licence as separate programs** -- the
aggregation `userland/ports/doom/` (GPL-2-or-later) already is. The
compiler's files keep their licence, our code keeps MIT, `LICENSE`
names GCC and binutils (`check_licenses.py` checks `userland/ports/`
only, so a fetched toolchain needs a row it can see -- stage 1's work),
and the source travels with the image: the pinned tarball address plus
our patches.

- **TCC (the path not taken) is LGPL-2.1.** A relicensing to MIT is under way (its
  `RELICENSING` file; Bellard agreed) and NOT complete -- one
  contributor said no for `arm-gen.c`, others have not answered -- so
  treat it as LGPL. Its runtime `lib/libtcc1.c`, linked into every
  program it compiles, is GPL-2-or-later with a linking exception
  ("unlimited permission to link the compiled version of this file into
  combinations with other programs"), so a compiled program carries no
  TCC licence. **The trap**: linking `libtcc` (TCC as a library) into
  one of our own programs, a scripting JIT say, brings LGPL's
  relinking obligations to that program.
- **GCC and binutils are GPL-3.** libgcc and libstdc++, which compiled
  programs link, carry the GCC Runtime Library Exception, so a compiled
  program is the author's to license.

**Precedents**: SerenityOS, Sortix and managarm all carry a GCC with an
OS-specific target; OSDev's "OS Specific Toolchain" is the recipe.
`tccboot` and the many hobby systems on TCC are the other side.

## The shape

    hello.c / hello.cpp
       |
       +-- /bin/cc, /bin/c++   GCC's own drivers (`cc` a link to `gcc`,
       |                       as on every Linux); their SPECS are this
       |                       OS's link line
       +-- cc1, cc1plus        the compilers, in GCC's libexec directory
       +-- as, ld              binutils
       |
       +-- /usr/include/       stage 0's SDK, plus libstdc++'s headers
       +-- /usr/lib/           libc.a, libuapp.a, the crt/rt objects,
       |                       libgcc, libstdc++
       v
    hello                      ET_EXEC at 0x8000000000, runs under spawn

**What SaunaOS's link line is lives in ONE file**, the
`gcc/config/saunaos.h` our patches add: the crt and rt objects, `libc`,
`-dynamic-linker /lib/ld-toy.so`, the base address. There is no driver
of ours. The rest of the patches are the few lines that teach binutils
and GCC the `x86_64-saunaos` name -- the patch-file rule
`docs/decisions/build.md` already sets for ports ("Our changes to a
vendored port are patch files, applied before the host build").

## Staging

Each stage ships on its own and is verifiable on its own.

### Stage 0 -- ship the SDK

Stage `userland/include/` and `userland/rt/sys.h` to `/usr/include`, and
`libc.a`, `libuapp.a`, `crt0.o`, `sys.o`, `stack_chk.o`, `sigtramp.o`,
`tls.o` to `/usr/lib`. Rows in `docs/filesystem-layout.md` for both, so
`tools/checks/check_layout.py` enforces the contents in either direction.

**Worth doing whatever happens next**, which is why it is stage 0 rather
than part of stage 3: it makes the machine carry its own interface, and
it is the only stage that needs nothing fetched.

Exit: `/usr/include/stdio.h` and `/usr/lib/libc.a` are present, and a
test asserts the staged headers match the tree's (the drift named in the
case against).

### Stage 1 -- an `x86_64-saunaos` cross toolchain on the host

binutils and GCC taught a target named `x86_64-saunaos`: a few lines
each in `config.sub`, `bfd/config.bfd`, `ld/configure.tgt`,
`gcc/config.gcc` and `libgcc/config.host`, and one
`gcc/config/saunaos.h` whose specs ARE this OS's link line -- the crt
and rt objects, `libc`, `-dynamic-linker /lib/ld-toy.so`, the
`0x8000000000` base. The patches live in the repo; the tarballs are
fetched, pinned by SHA-256 like DOOM's game data, so building the
toolchain sits behind `EXTRAS=1`. Useful on its own: the userland build
could use a compiler that knows SaunaOS rather than a list of
freestanding flags, and whether it should is a later, separate choice.

Exit: the host's `x86_64-saunaos-gcc hello.c` produces a binary that
`spawn`s and prints on a booted guest.

### Stage 2 -- the C++ runtime and libstdc++ for SaunaOS

`cc1` is C++ since GCC 4.8, so a GCC that RUNS on SaunaOS needs a C++
runtime built for it. This is the real test of tolibc: libstdc++ wants
wchar, locale and much of stdio. Some of it configures away
(`--disable-wchar_t`, no threads); what is missing is found by building
it, which is a cheaper oracle than reading.

The runtime gaps under "C++" above are this stage's too:
`.init_array` run by crt0, `.eh_frame` kept, `dl_iterate_phdr()` in the
loader.

Exit: a C++ hello world that throws and catches, with a global whose
constructor prints, cross-built, runs on a guest.

### Stage 3 -- binutils and GCC built to run ON SaunaOS

A Canadian cross: build on Linux, host and target SaunaOS. GMP, MPFR
and MPC cross-built first (plain C). The gaps here are GCC's support
code's, not the compiler's.

Every program on `/bin` needs a `docs/commands/` page or a
`COMMAND_PAGE_EXEMPT` entry; `cc.md` says what it deliberately does not
do (no self-hosting). Programs it builds get the stack protector the
tree uses, through the specs -- a thing TCC could not have given.

Exit: `cc hello.c -o hello && ./hello` and the same with `c++` on the
machine, under `--kvm` for any statement about speed.

### Stage 4 -- GNU make on the machine

Over the `fork`/`exec`/pipes and dash already here.

Exit: a two-file program with a Makefile builds on the machine.

### Stage 5 -- the promise, tested

A tool that compiles a corpus ON the machine and runs it, named by a
runner. The corpus is the argument: not `hello.c`, but programs that use
structs, unions, function pointers, varargs, `setjmp`, floating point
and the standard headers, and C++ with exceptions, templates and the
standard library -- and whose OUTPUT is compared against the
same source cross-compiled by the host gcc. That comparison is the check
a "it produced a binary" test cannot make.

Exit: the corpus agrees byte for byte with the host build's output, or
each disagreement is a named, understood limitation.

## What would make us stop

Recorded now, while it is cheap to say:

- **Stage 1 needs more than a target definition** -- patches to GCC's
  or binutils' own logic rather than the lines every new OS target
  adds. That is a fork in all but name, and the next GCC pin would
  carry it.
- **Stage 3's binaries are too large or too slow to be usable** on
  a small guest (`make run MEM=512`; the default is larger). Measure
  before believing either way; TCG makes speed conclusions
  untrustworthy (CLAUDE.md), so measure under `--kvm`.
- **Stage 2's libstdc++ needs more of tolibc than is worth building.**
  Stopping after stage 1 still leaves the cross toolchain.
- **Stage 5's corpus disagrees in ways that are not enumerable.** A
  compiler that is wrong in a bounded, documented way is shippable; one
  that is wrong unpredictably is not, and shipping it would be the
  "half-working compiler" the case against warns about.

Stopping after stage 0, 1 or 2 leaves something useful behind -- the
SDK, the cross toolchain, C++ programs built on the host. That is
deliberate.

## The path not taken: TCC

Kept so the choice is not re-argued from nothing. TCC (LGPL-2.1, one
binary with its own assembler and linker) would have been vendored in
`userland/ports/tcc/`, proven on the host, then shipped as `/bin/tcc`
under a small `/bin/cc` driver of ours carrying the link line. Its
known costs: no stack protector in the programs it builds, `__thread`
unreliable, no optimiser, no release since 0.9.27 (development is the
open-push `mob` branch). Estimated at 3-5 sessions against GCC's 6-12.
It lost on C++ alone; had C++ not been wanted it would have won.



## Open questions

- **Does the toolchain go in the default image, or only an `EXTRAS=1`
  one?** Its build is fetched, so it is behind `EXTRAS=1` already; an
  image carrying it is the second question. Size argues for EXTRAS
  only, and the image then says it carries it.
- **Which GCC and binutils to pin**, and where their prefix tree lands
  (`/usr` with `cc` linked from `/bin`, or `/` itself) -- a
  `docs/filesystem-layout.md` row either way.
- **How much POSIX does tolibc lack?** `ftruncate` is the one found by
  reading, and stages 2 and 3 will find more; their builds are a
  cheaper oracle than any amount of reading.
- **Should stage 0's drift check compare content or just presence?**
  Content is stronger and couples the test to every header edit. Presence
  is weak enough to pass while the headers are stale -- which is the
  failure the check exists for.
