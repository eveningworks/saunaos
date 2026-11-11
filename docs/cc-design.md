# A C and C++ compiler that runs on SaunaOS: GCC

A staged plan, in the shape `docs/libc-design.md` and
`docs/dynlink-design.md` used. It answers one question -- **can you
write a C or C++ program on this machine, compile it on this machine, and run
it?** -- and it deliberately does not answer the larger one next to it.

**Status: stage 0 (the SDK), stage 1 (the cross toolchain), stage 2
(C++ for SaunaOS, cross-built) and stage 3 (GCC and binutils ON the
machine, opt-in) BUILT 2026-11-08; stage 4 (GNU make on the machine)
BUILT 2026-11-09; stage 5 (the corpus, compared) DONE 2026-11-11. THE
COMPILER IS GCC WITH BINUTILS** (chosen 2026-11-04), over TCC, because
only GCC gives C++. Pinned: GCC 16.2 and binutils 2.47, the host's own
(2026-11-08), so stage 5's comparison has no version to explain.
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

- **There was no assembler, no linker, and no `make` on the machine**
  (stage 3 put `as`, `ld` and `cc` in `/usr/bin`, on an image that asks
  for them, and stage 4 `make`). `/bin` has none of them. This is the single
  constraint that decides which compiler, because most small C compilers
  emit *assembly text* and shell out to `as` and `ld`.
- **The SDK ships (stage 0, 2026-11-08).** Before it, the disk carried
  no header and no static library, though the build made `libc.a` and
  `crt0.o`; `/usr/include` and `/usr/lib` now hold them -- stage 0
  below.
- **The kernel loads `ET_EXEC` only.** `kernel/proc/elf.c`'s `elf_load()`
  refuses anything else in as many words ("static, non-PIE only"), and
  handles `PT_INTERP` by loading `/lib/ld-toy.so`. That is exactly the
  classic non-PIE format a small compiler emits, so the OUTPUT format is
  not a problem -- it is the one thing that needs no work.
- **Userland links at a fixed base**, `0x8000000000` in
  `userland/libs/rt/link.ld`, and the real link line for a `/bin` program is
  `crt0.o + <prog>.o + sys.o + stack_chk.o + sigtramp.o + tls.o +
  libbase.a ... libui.a + libc.a`. An on-target compiler has to reproduce that, which
  is the job of GCC's specs (`gcc/config/saunaos.h`, stage 1).
- **tolibc has what a compiler needs**, checked symbol by symbol:
  `qsort`, `bsearch`, `strtod`, `strtoull`, `realloc`, `memmove`,
  `vfprintf`, `fflush`, `remove`, `rename`, `sscanf`, `atexit`, `mmap`,
  and a real `setjmp`/`longjmp` in assembly (`userland/libs/libc/setjmp.S`) --
  which matters, because a compiler's error path is usually a `longjmp`.
  Missing and needed: `ftruncate`, which was not even declared (stage 3
  added it, over a new `SYS_FTRUNCATE`).
- **The POSIX layer was `static inline` IN THE HEADERS**, over raw `sys_*`
  calls declared in `userland/libs/rt/sys.h` -- `lseek`, `getcwd`, `isatty`
  and friends had no out-of-line definition and appeared in no archive.
  Stage 3 moved them out of line (autoconf could not find them); the
  headers still include `rt/sys.h`, so the on-target include root must
  carry it as well as `userland/libs/libc/include/`, or they do not
  compile.
- **`errno` does not need TLS.** `<errno.h>` defines it as
  `(*__errno_location())`, a plain call. So a compiled program needs no
  thread-local storage at all, and a compiler with weak `__thread`
  support is not disqualified.
- **Vendoring at this scale is routine here.** mbedtls 203,755 lines
  against 566 of glue; doom 72,512 against 958; dash 20,418 against 600.
  The `userland/third_party/<name>/upstream/` + `userland/third_party/<name>/glue/` +
  `EXTRA_OBJS_<name>` pattern is established and unchanged.
- **GPL-family code is already vendored.** `userland/third_party/doom/upstream/` is
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
| Where the source lives | `userland/third_party/tcc/upstream/`, vendored like dash | fetched at build time with our patches -- a ~90 MB tarball does not go in the repo; that reaches the network, so it sits with `EXTRAS=1` |
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

**What the runtime lacked, found by reading (2026-11-04), and is BUILT
since stage 2 (2026-11-08):** static constructors never ran (no
`.init_array` in `link.ld`, nothing called before `main`; C's
`__attribute__((constructor))` had the same hole), exceptions could not
unwind (no `.eh_frame` kept), and there was no `dl_iterate_phdr()`. The
C++ ABI's per-thread exception state sits in TLS, which tolibc has, and
libstdc++ found it (`HAVE_TLS`).

## The licences

**Both fit beside the MIT licence as separate programs** -- the
aggregation `userland/third_party/doom/upstream/` (GPL-2-or-later) already is. The
compiler's files keep their licence, our code keeps MIT, `LICENSE`
names GCC and binutils (`check_licenses.py` checks `userland/third_party/<port>/upstream/`
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
       +-- /usr/bin/cc, c++    GCC's own drivers (`cc` a copy of `gcc`,
       |                       where Linux has a link); their SPECS are
       |                       this OS's link line
       +-- cc1, cc1plus        the compilers, in GCC's libexec directory
       +-- as, ld              binutils
       |
       +-- /usr/include/       stage 0's SDK, plus libstdc++'s headers
       +-- /usr/lib/           libc.a, the toolkit's .a's, the crt/rt objects,
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

**BUILT 2026-11-08.** `make sdk` (part of `make all`) runs
`tools/build/stage_sdk.py` into `build/sysroot`; the seed step copies its
`usr/include` and `usr/lib` to the disk. **That one tree is also stage
1's sysroot**, which is the shape SerenityOS and Sortix have: the
cross compiler and the disk see the same bytes, so they cannot drift
apart. `docs/filesystem-layout.md` has a row per directory.

What reading the tree found, beyond the plan above:

- **The libc headers reach 25 kernel headers** (`kernel/include/api`,
  `abi`, `build/gen`), not only `rt/sys.h`. They ship flat in
  `/usr/include/saunaos/`, searched AFTER `/usr/include`
  (`-idirafter`) -- the tree's `-I` order, which is load-bearing because
  `string.h` and `errno.h` exist on both sides. Which ones is the
  compiler's own `-M` closure, so nothing keeps a list.
- **The toolkit's headers ship too** (`/usr/include/{base,net,media,ui}`),
  or its four archives in `/usr/lib` would be unusable.
- **`sync` is additive**, so a header the tree deletes would have stayed
  on every existing disk.img and still compiled. The seed step names
  `/usr/include` and `/usr/lib` in `seed/exact`, which
  `tfs3_writer.py`'s `sync` mirrors exactly.
- The drift check compares CONTENT (the open question below, answered):
  `tools/tests/gate/sdk_test.py` compares `build/sysroot` with a fresh
  staging and the image with `build/sysroot`, both directions, and
  builds a program from the sysroot alone with `-nostdinc`.

401 files, 8.4 MiB, most of it the archives (copied `--strip-debug`).
A program built that way with the host's own gcc ran on a guest.

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

**BUILT 2026-11-08.** `make toolchain EXTRAS=1` fetches the pinned
tarballs (`tools/fetch/fetch_toolchain.py`, the `toolchain` row of
`fetch_extras.py`, shown with its GPL-3 licence) and builds binutils and
a C-only GCC into `toolchain/local/` (`tools/build/build_toolchain.sh`;
`make toolchain` alone is offline and refuses without the tarballs).
On the 16-thread dev host it takes about 5.5 minutes (334 s) and
installs 106 MB (stripped); the tarballs are 130 MB, and the 3 GB build
tree is deleted once installed. `tools/tests/sweep/cross_gcc_test.py` is the exit
criterion: a program built with NO flags runs on a guest and its output
equals the host gcc's build of the same source, line for line.

What building it found:

- **No GCC or binutils LOGIC needed patching** -- the stop condition
  above did not fire. The patches are the lines every new OS target adds
  (`config.sub` twice, `bfd/config.bfd`, `gas/configure.tgt`,
  `ld/configure.tgt`, `gcc/config.gcc`, `libgcc/config.host`,
  `fixincludes/mkfixinc.sh`) and the new `gcc/config/saunaos.h`. The tm
  files are `x86_64-*-elf`'s with `glibc-stdint.h` for `newlib-stdint.h`
  (the host gcc that built `libc.a` uses glibc's `int_fast*` types) and
  `use_gcc_stdint=provide`, since tolibc ships no `<stdint.h>`.
- **`bfd/config.bfd`'s case labels are PARSED** (`targmatch.sed` turns
  them into a C table): a label continued onto a second line compiles to
  garbage, so the saunaos pattern shares the `x86_64-*-elf` line.
- **binutils 2.47 has no configure switch for relax relocations**, so
  `-mrelax-relocations=no` is in the specs' `ASM_SPEC`; the guard's
  default is already `global` on a target that defines no
  `TARGET_THREAD_SSP_OFFSET`, and the spec states it anyway.
- **The tree's flags, and the two that are NOT defaults** (`-ffreestanding`,
  `-mno-red-zone`), with why: `docs/decisions/build.md`, "The cross
  compiler's defaults are the tree's ABI flags, and its sysroot is a
  symlink in its prefix". That entry also has why the sysroot is a
  symlink inside the prefix rather than `--with-sysroot=build/sysroot`.
- **STATIC ONLY.** A dynamic link (`saunaos-dyn.ld`,
  `-dynamic-linker /lib/ld-toy.so`) needs a driver option to ask for it
  (an `extra_options` `.opt` file) and `libc.so` in the sysroot, which
  stage 0 does not stage; deferred, not attempted.
- **The first comparison found a tolibc bug, not a compiler one**:
  `printf` ignored the `0` flag on `%f` -- and `+`, ` ` and `#` on
  every float conversion, fixed 2026-11-08 with a float matrix in
  `libc_diff.py`.

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

**BUILT 2026-11-08.** `make toolchain` now builds `x86_64-saunaos-g++` and
a static libstdc++/libsupc++ against tolibc, with the POSIX thread model,
in about 6.5 minutes (383 s, from scratch) on the 16-thread dev host; the prefix is 177 MB stripped, libstdc++'s headers 16 MB and its archives 6 MB of it.
`tools/tests/sweep/cross_gxx_test.py` is the exit criterion and more: two
translation units built with no flags, run on a guest, equal the host
g++'s build line for line -- a global constructed before `main` and
destroyed after it, a throw unwinding a destructor across two functions,
one caught across units by base class and rethrown, `std::string`,
`vector`, `map`, `unique_ptr`, a virtual call, `typeid`, iostream
formatting, four `std::thread`s on a `std::mutex` -- and every libc header
compiles as C++ on its own. It was seen red with `.eh_frame` discarded
and with `.init_array` dropped. That program is 657 KB (422 KB stripped); an iostream hello world is 620 KB (395 KB stripped), against 23 KB (19 KB) for C's puts.

What building it found:

- **No libstdc++ LOGIC needed patching** -- the stop condition did not
  fire. Its cross configuration has a case per OS and errors out on an
  unknown one, so the GCC patch adds `*-saunaos*` to the `*-linux*` line
  of `crossconfig.m4` and of its generated `configure` (the probe-by-link
  branch, which works because the sysroot links). The other new lines
  are target lines: `default_use_cxa_atexit`, the POSIX thread model,
  `t-gthr-noweak`, and the specs' `--eh-frame-hdr` and one link group.
- **The order and the split are glibc's, measured, and the tables are
  REGISTERED from libc** -- `crtbegin.o` cannot do it at this base, and
  libgcc offers the `dl_iterate_phdr()` path only to a fixed list of
  systems. Both choices are `docs/decisions/build.md`, "C++ runs on
  glibc's constructor order, registers its unwind tables from libc, and
  has POSIX threads".
- **A HOSTED COMPILER CALLS C NAMES BY ITSELF.** tolibc's `strlen`,
  `strcmp`, `snprintf` and friends were `static inline` over the toolkit
  -- safe under the tree's `-ffreestanding`, and an undefined reference
  the moment g++'s `<string>` called `__builtin_strlen()`. They are real
  symbols now (`docs/conventions/build.md`).
- **The headers had never been read by a C++ compiler**: no `extern "C"`
  anywhere but `<dlfcn.h>` (GCC 16 no longer adds it implicitly),
  `_Static_assert` at file scope in three `abi/` headers, and a parameter
  named `template`. All fixed; the toolkit's headers are still C-only.
- **What tolibc gained**, each with a check: the exit list grows past
  32 and holds `__cxa_atexit`/`__cxa_finalize`; `dl_iterate_phdr()`,
  `<link.h>`, `<elf.h>`; pthread keys, mutex types (recursive,
  error-checking), rwlocks, `pthread_cond_timedwait`, condition and
  rwlock attributes, deferred cancellation; `mblen`/`mbtowc`/`wctomb`/
  `mbstowcs`/`wcstombs`/`wcrtomb`, `MB_CUR_MAX`; `strtold` (double's
  precision); `fpclassify` and C99's quiet comparisons, `float_t`/
  `double_t`; POSIX's missing `errno` names; the remaining `DT_*`;
  empty `libm.a`/`libpthread.a`/`libdl.a`/`librt.a` in the SDK, as musl
  ships them, because g++ links `-lm`.
- **Configured away, and why**: `wchar_t` (configure turns it off --
  tolibc has no wide-string or wide-I/O family, genuinely large);
  `tmpfile()` is declared but always fails, because no descriptor here is
  read-write. Each is a `docs/roadmap.md` line. **C99's `<math.h>` was
  the third, and is BUILT (2026-11-08)**: libstdc++ takes the C99 set
  all or none (`_GLIBCXX_USE_C99_MATH_FUNCS`, one compile test naming
  every function in all three types, and `<fenv.h>` for
  `_GLIBCXX_USE_C99_FENV`), and without it `<cmath>` had no
  `std::round` or `std::cbrt` and the installed `bits/std.cc` -- the
  `import std` module's source -- was EMPTY. tolibc now has all of it,
  measured function by function (`docs/libc-design.md`, stage 13), and
  `cross_gxx_test.py` checks `<cmath>`'s C99 half and an `import std`
  program on a guest. Building the module found one more name a hosted
  compiler calls by itself: `std::format` calls `toupper()`, which was a
  `static inline`; all of `<ctype.h>` is out of line now. Still missing
  beside it: `<complex.h>`, a roadmap line (`%Lg` in printf, which made
  iostream print a `long double` as its format string, arrived
  2026-11-10).
- **The unwind tables cost about 14%** of `/bin`, `/lib` and `/tests`
  together (4.2 MB of 30.5, measured against the build before), because
  the scripts KEEP `.eh_frame` in every program, C ones included.
  **Trimmed the same day**: the tree's C compiles with
  `-fno-asynchronous-unwind-tables` except libc, which a C++ program
  throws THROUGH (a `qsort()` comparator; glibc keeps those tables for
  the same reason), and `rt/tls.o`, without which a dynamic program's
  `PT_GNU_EH_FRAME` is the empty one at address 0 -- 3.81 MB of `.eh_frame` down to 0.64 MB, the three
  directories 43.1 MB to 40.3 MB. `cross_gxx_test.py` throws out of a
  comparator, and its `--positive-control libc_eh_frame` stops there. The
  runtime adds about 1.1 MB more: the start code in every program, and
  `pthread.o`'s growth (about 6 KB) in every dynamic one, which links it
  whole.
- **libstdc++ is the toolchain's, not the SDK's**: under
  `toolchain/local/x86_64-saunaos/`, and on no disk until stage 3 puts a
  `g++` there.
- **STILL STATIC ONLY** -- `-shared` and a dynamic C++ program (whose
  libraries' unwind tables the loader would have to register) are
  roadmap lines.

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

**BUILT 2026-11-08.** `make native-toolchain` (after `make toolchain`;
`EXTRAS=1` fetches GMP, MPFR and MPC first) builds binutils and GCC's C
and C++ compilers as a Canadian cross -- build = the Linux host, host =
target = `x86_64-saunaos`, compiled by the cross toolchain against the
SDK -- into a staged `/usr` in `toolchain/native/`
(`tools/build/build_native_toolchain.sh`), and every image the checkout
seeds from then on carries it; a default build and a clean clone do not
(`docs/decisions/build.md`, "The compiler on the machine is opt-in,
staged as /usr, and never in a NOGPL image"). On the machine:

    # cc hello.c -o hello && ./hello
    # c++ hello.cpp -o hello && ./hello

`tools/tests/sweep/native_cc_test.py` is the exit criterion and more: a C
program, a two-file C program compiled with `-c` and linked by a third
`cc`, the C one again with `-pipe`, and a C++ one with iostream, a
virtual call, a `map` and an exception caught by base class through a
destructor -- all compiled, assembled and linked ON a guest, run there,
and equal line for line to the host gcc/g++'s build of the same
sources. It was seen red with `-nostartfiles` on every guest compile.

Measured:

- **Disk: 134 MiB** (1,032 files, counted in TFS3's 4 KiB blocks), on
  a system that is about 79 MiB without it -- the seeded tree goes from
  79 to 213 MiB. The staged tree: `cc1` 42 MB and `cc1plus`
  44 MB (static, stripped), the drivers and binutils 26 MB (`cc` and
  `c++` are hard links of `gcc` and `g++` since 2026-11-09, so 4.8 MB
  less than this figure, which counted them as copies), libstdc++'s
  headers 16 MB, libgcc and
  libstdc++ 9 MB. The default 9 GB `disk.img` holds it with room to
  spare; nothing was grown.
- **Memory**: the largest `cc1plus` the guest's `ps` showed on the C++
  hello at `-O2` was 115 MB (sampled once a second, so a lower bound on
  its peak), `cc1` 47 MB on the C one. Everything passes with `make run
  MEM=256`, 4 runs in 4 (2026-11-09); the default, 2048, has room to
  spare. **It did not, at first**: `cc -pipe` failed 4 runs in 6 at 256
  with "cannot execute cc1", because the kernel read a program into ONE
  kmalloc the size of the file -- a run of 10,000 contiguous frames for
  cc1's 40 MB, refused with 160 MB free -- and reported every failure to
  build an image as ENOENT. The image is read as it loads now, through a
  256 KiB window (`elf_load_from()`), and an exec says ENOMEM or ENOEXEC
  when that is what it was.
- **Time, under `--kvm`** on the dev host's i7-11700, by the guest's own
  monotonic clock: `cc -O2 hello.c` 0.14 s, the two-file build (three
  `cc` runs) 0.18 s, `cc -pipe` 0.15 s, and `c++ -O2` of the iostream
  hello 1.7 s.
- **Build**: 391 s on the 16-thread dev host, after `make toolchain`'s
  381.

What building it found -- **no GCC or binutils logic needed patching**;
the stop condition did not fire, and the only patches are stage 1's
target lines. The gaps were tolibc's and the kernel's, each filled with
a check (`docs/libc-design.md`, stage 14, has the C library's list):

- **A read-write descriptor**, in the kernel (`SYS_O_RDWR`): BFD writes
  every object through `fopen("w+b")`, and libiberty makes its temporary
  files `O_RDWR | O_CREAT | O_EXCL` -- so `mkstemp()` had never worked
  either. With it, update streams, `tmpfile()`, and the descriptor forms
  `ftruncate()` (`SYS_FTRUNCATE`) and `fchmod()` (`SYS_FCHMOD`).
- **Every POSIX name was `static inline`**, invisible to autoconf's link
  tests, so configure believed tolibc had no `getcwd()` and compiled
  libiberty's over it. Out of line now (`docs/conventions/build.md`).
- **`posix_spawn()` with file actions**, which is how the driver starts
  `cc1`, `as` and `collect2`; **a stream table that grows** past
  `FOPEN_MAX`, because a link keeps a dozen files open; **`putenv()`
  strings freed** by libc, which corrupted the driver's heap on every
  compile; and a dozen smaller names.
- **Three configure-time answers**, each named in the build script:
  `--disable-fixincludes` (it wants `alarm()`, and the target's headers
  are never fixed anyway), `-DCODY_NETWORKING=0` (no `getaddrinfo()`;
  the modules mapper is in-process by default), and gcc/configure's two
  no-PIE probes answered "no" -- `-fno-PIE` code cannot link above 4 GiB,
  and `--enable-host-pie` links `-pie`, an `ET_DYN` the kernel refuses.
  GMP takes `-std=gnu17`, its probes not being C23.
- **A seeded program needs its exec bit**: dash looks a command up by
  it, so `/usr/bin` and `/usr/libexec` joined `/bin` and `/tests` in
  the image writer's list. And **`PATH` is `/bin:/usr/bin`** now, from
  init: the driver finds its own prefix -- and so `cc1` -- by looking
  `argv[0]` up on `PATH`.

Still static only, as stage 1's are; no LTO, plugins or sanitizers; and
`cc -time`'s child times are refused (no reaped child's CPU time is kept).

### Stage 4 -- GNU make on the machine

Over the `fork`/`exec`/pipes and dash already here.

Exit: a two-file program with a Makefile builds on the machine.

**BUILT 2026-11-09.** GNU make 4.4.1 (the latest release; the `native-make`
fetch row, pinned from a `.sig` that verified against `gnu-keyring.gpg`)
is cross-built by the cross toolchain into the same staged tree as the
compiler, `toolchain/native/usr/bin/make`, by
`tools/build/build_native_make.sh`, which `make native-toolchain` runs
after the compiler's build; every image the checkout seeds carries it
beside `cc`. **Make is not patched**; its configure answers are named in
the script (`-std=gnu17`, `-DPOSIX`, `CXX=g++`, `--disable-load`). On
the machine:

    # make                 (a Makefile in the current directory)
    # touch util.h && make (rebuilds what includes util.h, nothing else)
    # make clean && make -j2

`tools/tests/sweep/native_make_test.py` is the exit criterion and more:
one Makefile with a two-file C program (a header dependency through
`main.o: main.c util.h msg.h`, objects by make's built-in rule), a C++
program and a `$(MAKE) -C sub`, driven through `make calc`, `make`,
`make` again, `touch util.h; make`, msg.h changed then `make`, `make
clean` twice and `make -j2` -- each step's printed commands equal to the
HOST's GNU make running the same step on the same files, and the
programs' output equal to the host build's, the changed message
included. `make -j2` of two one-second sleeps takes 1.0 s against the
serial run's 2.0 s. It was seen red with msg.h dropped from main.o's
rule, and, before `SYS_UTIME`, at `touch util.h` (nothing rebuilt).

Measured, under `--kvm` on the dev host's i7-11700, by the guest's own
clock:

- **Time**: `make calc` from nothing (two `cc -c`, one link) 0.17 s;
  then `make` of the C++ program and the sub-make's 1.4 s (the C++
  compile is nearly all of it); `touch util.h; make` (two compiles and a
  link) 0.18 s; the msg.h change (one compile, one link) 0.10 s.
- **Disk**: `/usr/bin/make` 301 KiB (static, stripped) and its licence
  texts 35 KiB, on top of the compiler's 134 MiB; `/bin/sh` is dash in
  EVERY image (BSD-licensed, so NOGPL keeps it) -- a 195 KiB copy then,
  a hard link costing nothing now.
- **Build**: 6 s, after the compiler's.

What building it found -- **no make logic needed patching**; the gaps
were tolibc's, the kernel's and two programs' (`docs/libc-design.md`,
stage 15, has the C library's list):

- **No way to set a file's time.** `/bin/touch` created a missing file
  and did nothing to an existing one, so `touch util.h && make` rebuilt
  nothing. `SYS_UTIME` sets `modified` (a backend operation, `set_mtime`,
  that TFS3 journals like a chmod and ramfs -- `/tmp` -- keeps; FAT32 and
  exFAT answer ENOTSUP), with `utime()`, `utimes()` and `touch` over it.
- **No `/bin/sh`.** make runs a recipe with shell syntax (`rm -f *.o`,
  `cd x && ...`) as `/bin/sh -c`; it is dash now -- a hard link of
  `/bin/dash` since 2026-11-09 -- in every image (`docs/decisions/shell.md`, "/bin/sh is dash, in every image"). A plain command
  make still runs directly.
- **`rm` had no `-f`**, which every `clean` rule passes; it takes `-r`
  and `-f` through `uargs` now, `-f` skipping a missing path.
- **`$(CXX)` defaulted to `x86_64-saunaos-g++`**, configure's cross
  compiler; `CXX=g++` at configure time, GNU make's default on a GNU
  system.
- **File times were whole SECONDS** (TFS3 stored epoch seconds): make
  rebuilds only what is strictly older, so a header changed in the same
  second its object was built was missed. Fixed 2026-11-09 with no
  format change: TFS3 keeps `modified`'s nanoseconds at inode byte 96
  (zero on every older volume, inside the checksum), ramfs keeps them
  too, `stat()` reports `st_mtim` and make, rebuilt, finds it at
  configure time. `utimensat()`/`futimens()` set them. The test's "fast"
  step edits a header within a second of its build.

`-O` (`--output-sync`) warned "Cannot acquire output lock" and ran
unsynchronised until `fcntl()`'s record locks were built (2026-11-09,
`kernel/fs/flock.c`); `native_make_test.py` runs `make -O -j2`. Still:
`-l` has no load average, and `-j` overlaps waiting rather than
compiling -- one CPU.

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

**BUILT 2026-11-10: `tools/tests/sweep/native_corpus_test.py`**, with
the oracle made stricter than the exit asks: every OBJECT the machine
writes is compared byte for byte with the cross compiler's (GCC
bootstrap's `make compare`), a linked program as loaded, and a program
that differs runs both ways on the machine; the host's gcc+glibc build
is an informational third column. The corpus: own C and C++ programs,
`gcc.c-torture/execute` and libstdc++'s `dg-do run` tests, read from the
GCC tarball (`tools/lib/gcc_testsuite.py`).

**DONE 2026-11-11 -- the whole corpus, on the ASUS and under `--kvm`,
and the two machines agree case for case.** 5,667 cases; 5,154 run (9
own, 1,681 torture, 3,464 libstdc++) and 513 skipped by their own
`dg-` lines; 4,666 build with the cross compiler.

- **The compilers agree: every object the machine wrote is
  byte-identical to the cross compiler's** -- 5,157 objects and
  libtestc++'s members -- and every program it linked is the same file
  (2,635) or loads the same bytes (1,939). Two `-g` objects differed
  only in `.debug_line_str`, the cross side's host directories; the
  cross build now maps them as it maps its headers.
- **39 cases failed, the same 39 on both machines, and each was named**
  in `cc_corpus/known.txt`. None was the compiler. **The follow-up round
  (2026-11-11 evening) fixed the causes of 18**, each case re-run under
  `--kvm`: the stack grows for any fault in its 8 MiB (`memcpy-1`,
  `pr28982b`), touching anonymous mmaps merge (the five mdspan builds),
  the cc1/cc1plus "SIGSEGV" was tolibc's `fputc()` writing past an
  exactly full stdio buffer (all three), and `/dev/[u]random`, an exact
  `strtold`, `uselocale()`, `link()`, ENOENT for an empty path and a
  directory renamed over an empty one cleared `random_device`,
  `std::stold`, `from_chars`, and five `std::filesystem` tests. **21
  remain named**: four nested-function trampolines on the
  non-executable stack, 12 wide-character facets libstdc++ was built
  without, `create_directories` (`..` resolves by name here), and four
  tests marked to fail that pass here as on glibc. The roadmap's
  Programming milestone has one item per cause.
- **What it found was the OS and its tools, not the compiler**, and
  each was fixed: a crashed child's status read as a failed wait
  (8fe2b334), arguments capped at 1 KiB (cd96c4cb), tolibc's
  printf/scanf/strtod gaps (f9e86f33), `ar` refusing every archive
  because `F_GETFL` never reported a write mode (a3777c34), a TFTP
  resend that skipped a window and damaged 63 of 1,671 fetched programs
  (155d355b), and a page read that failed cached as zeros (a7d276b7).
- **What a full run costs**: 10.7 hours on the ASUS. A KVM guest slows
  over hours of compiling (`docs/bugs.md`), so one is better run in
  parts -- `--skip-done` finishes a stopped run, `--collect` judges one
  that already ran.

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
  "half-working compiler" the case against warns about. **Measured
  2026-11-11: it did not** -- every object identical, every failure
  named and none of them the compiler's.

Stopping after stage 0, 1 or 2 leaves something useful behind -- the
SDK, the cross toolchain, C++ programs built on the host. That is
deliberate.

## The path not taken: TCC

Kept so the choice is not re-argued from nothing. TCC (LGPL-2.1, one
binary with its own assembler and linker) would have been vendored in
`userland/third_party/tcc/upstream/`, proven on the host, then shipped as `/bin/tcc`
under a small `/bin/cc` driver of ours carrying the link line. Its
known costs: no stack protector in the programs it builds, `__thread`
unreliable, no optimiser, no release since 0.9.27 (development is the
open-push `mob` branch). Estimated at 3-5 sessions against GCC's 6-12.
It lost on C++ alone; had C++ not been wanted it would have won.

## Open questions

- ~~**Does the toolchain go in the default image, or only an `EXTRAS=1`
  one?**~~ ANSWERED by stage 3: in NEITHER by default -- an image
  carries it when the checkout ran `make native-toolchain`, never under
  `NOGPL=1` (`docs/decisions/build.md`).
- ~~**Where the on-machine toolchain's prefix lands**~~ ANSWERED: `/usr`,
  a distribution's layout, with no `/bin` link (`docs/filesystem-layout.md`).
- ~~**How much POSIX does tolibc lack?**~~ Stages 2, 3 and 4 measured it
  by building (`docs/libc-design.md`, stages 13, 14 and 15), and the
  corpus (stage 5) measured the rest: wide characters, `std::filesystem`,
  a device file and `long double` parsing (`cc_corpus/known.txt`).

