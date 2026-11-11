# tolibc -- SaunaOS's C library

**The name is formed the way `tosh` was**: SaunaOS + `sh` gave `tosh`,
so SaunaOS + `libc` gives `tolibc`. It sits beside **Toykit**
(`userland/libs/ui/`, the GUI toolkit) as the second thing a ring-3 program
links, and beneath both of them is **libsys** (`userland/libs/rt/`), the
typed syscall layer.

**The archive is still `libc.a`, not `libtolibc.a`**, and deliberately:
a C library's archive has one conventional name, a linker expects to
find it under that name, and a ported build system that says `-lc`
should not have to know what this one is called. The NAME is for people;
the FILENAME is for tools. **The shared form follows the same rule**:
the same sources build `/lib/libc.so` (a second `-fpic` compile --
minus `pthread.c`, which stays static in `libc_nonshared.a` because
its `__thread` state is TLS a library here may not carry), and every
`/bin` and GUI program links it through `/lib/ld-toy.so`. See
`docs/dynlink-design.md`, and `docs/decisions.md`'s "tolibc stays" for
why this is tolibc and not musl.

## What is here

| File | What it holds |
|---|---|
| `stdio.c` | `FILE`, the buffering policy, `printf`, `sprintf`, `snprintf` |
| `scanf.c` | `sscanf`/`fscanf`/`scanf` over one scanner, scansets included |
| `strtod.c` | `strtod`/`strtof`/`strtold`/`atof`, correctly rounded: a first guess, then exact big-integer comparison against the midpoints |
| `stdlib.c` | `strtol`, the `long long` parsers, `div`/`ldiv`/`lldiv`, `realloc`, `qsort`, `exit`/`atexit`/`__cxa_atexit`/`_Exit`/`quick_exit` |
| `start.c` | what crt0 calls before `main()`: unwind-table registration, then the preinit, libraries' and init arrays; the fini arrays at exit |
| `inttypes.c` | `strtoimax`/`strtoumax`/`imaxabs`/`imaxdiv`. The PRI\*/SCN\* macros are deliberately absent -- see the header |
| `string.c` | the out-of-line `<string.h>`: the toolkit's functions under C's names (a hosted compiler calls them by name), and the ones it has no equivalent of |
| `time.c` | `struct tm`, `mktime`, `strftime`, `clock`, the POSIX clocks, `nanosleep`, `gettimeofday` |
| `math*.c` | C99's `<math.h>` in three types, split by how each can be wrong -- the list is the header's top comment; every function's accuracy is measured by `tools/tests/sweep/libc_diff.py --only math` |
| `fenv.c` | `<fenv.h>`: the rounding mode and exception flags of SSE and the x87 together |
| `ctype.c` | `<ctype.h>`, out of line because a hosted compiler calls `toupper` by name |
| `printf_float.c` | `%f`/`%e`/`%g`/`%a` and their `L` forms, with exact digits -- what the kernel's build cannot have |
| `signal.c` | POSIX's `struct sigaction` converted to the kernel's |
| `wait.c` | `waitpid()`, whose options pick one of four entry points |
| `termios.c` | POSIX's `struct termios` converted, `c_lflag` and `c_oflag` masked, and `tcgetsid()` |
| `fcntl.c` | `open()`'s variadic mode argument, accepted and discarded, and `open("/dev/tty")` made the controlling-terminal call (`SYS_OPEN_CTTY`); `fcntl()`, whose lock commands are refused |
| `pwd.c` | `<pwd.h>`: the user database's one entry, root |
| `getopt.c` | POSIX option parsing -- stops at the first operand |
| `strings.c` | `strncasecmp`, `bzero`, `bcopy` |
| `iconv.c` | `iconv_open`/`iconv`/`iconv_close` -- a small charset set, refused at open beyond it |
| `inet.c` | `inet_pton`/`inet_ntop` |
| `realpath.c` | `realpath`, over kernel/lib's `k_path_resolve()` |
| `pthread.c` | threads: the stack, the TLS block and the return value; mutexes, rwlocks, condition variables, keys, cancellation |
| `dlfcn.c` | `dlopen` and friends through ld-toy.so's vector, and `dl_iterate_phdr()` |
| `dirent.c`, `assert.c`, `cmem.c`, `heap_os.c`, `setjmp.S` | the rest |

**The last six are the POSIX half, and none of them adds a syscall.**
Every one sits on a call that already existed; what they add is the
TRANSLATION between POSIX's shapes and this kernel's, which is the only
thing `userland/tests/posix_test.c` tests. The rule they follow is
`docs/libc-design.md`'s: **declare what can be honoured, and nothing
else** -- so `sigprocmask()` is absent, a non-empty `sa_mask` is
`EINVAL` rather than silently dropped, and `VMIN`/`VTIME` are
undefined so that a polling read is a compile error naming the line
rather than a program that hangs.

Public headers are `userland/libs/libc/include/`. Four more of tolibc's files are
not here at all: `string.c`, `knum.c`, `kfmt.c`, `heap_core.c` and
`caltime.c` live in `kernel/lib/` and are **compiled twice**, so a
ring-3 `strlen`, `snprintf`, `malloc` and calendar are the same code the
kernel runs rather than a second implementation.

## The rule about what goes in

**tolibc aims to be COMPLETE, not minimal.** If C specifies a function,
or POSIX specifies one that ported code reaches for constantly
(`strdup`, `strtok_r`, `isatty`), it belongs here even when nothing in
saunaos calls it yet -- because the alternative is a link error in
somebody else's source file, months from now, with no explanation
attached. That is the opposite of the bar the rest of this project holds
(`tools/`, Toykit and the kernel toolkit all require a second REAL
caller), and the difference is the audience: those serve code written
here, and this one serves code that has not been written yet.

**What is still absent is absent for a REASON, not for lack of time**,
and each says so in its header:

- ~~**`fork`/`exec`**~~ -- present since 2026-09-11 (`docs/fork-design.md`),
  built for a ported shell; `posix_spawn()` (`<spawn.h>`) over
  `SYS_SPAWN` is still what everything else should call.
- **locales and wide characters** -- listed as deliberately not pursued
  in `docs/roadmap-details.md`. The C locale's conversions are here
  (`mbrtowc`, `wcrtomb`, `mbtowc`, `mbstowcs` and the rest); the
  wide-string and wide-I/O families are not, which is why libstdc++ is
  built without `wchar_t` (`docs/cc-design.md`, stage 2). **Threads came
  off this list** (2026-08-26): `<pthread.h>` is here now, over the
  kernel's four thread syscalls, and what it still lacks is named in its
  own header.
- ~~**most of C99's `<math.h>`**~~ -- whole since 2026-11-08, in all
  three types and measured in ULPs (`docs/libc-design.md`, stage 13).
- ~~**`tmpfile()`**~~ -- works since 2026-11-08, over the read-write
  descriptor GCC's port needed (stage 14).
- ~~**`utime()`**~~ -- sets the modification time since 2026-11-09
  (`SYS_UTIME`; TFS3 and ramfs, ENOTSUP elsewhere), with `utimes()`.
- **File locks** (`fcntl`'s `F_SETLK` family) are NAMED and REFUSED
  with `ENOLCK` -- there is no file locking in this system.
- **`getaddrinfo()` and `alarm()`** -- names are resolved by libnet,
  above this library, and there is no per-process timer signal yet.

`docs/libc-design.md` is the full plan and the record of what each stage
found.

## Measuring it

`/tests/cjson_bench` puts tolibc under sustained load through cJSON --
parse and print between them hit `malloc`/`realloc` on every node,
`strtod` on every number, `sprintf` on every number written back, and
most of `<string.h>`. It is a load test of the allocator and the
formatter wearing a JSON hat.

**`spawn /tests/cjson_bench`, not `run`**: the legacy `run` loader has
no scheduler slot, so `clock()` reports "unavailable" and every CPU
figure collapses to zero. It says so rather than printing zeroes, and
the wall-clock numbers are still valid there. The report is written to
`/tmp/cjson_bench.out` as well as the console, because a spawned
program's output arrives while a harness is between commands.

**It is NOT in the gate, and must not be.** A timing number varies with
the host, and a benchmark in `preflight.sh` would make the gate flap for
reasons that have nothing to do with this OS. It is also why the numbers
are only worth reading as RATIOS -- the same build under `vm.py --kvm`
against the same build under TCG, or one commit against another on one
host. An absolute figure from an emulator measures the emulator.
