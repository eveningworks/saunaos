# Syscall tracing in ring 3, and what a debugger needs beside it

A staged plan, in the shape `docs/netstack-design.md` used. It answers
"should `strace`'s decoding leave the kernel, what would it take, and
how much of it does a ring-3 debugger share?" -- and it RE-ARGUES a
decision rather than starting from nothing: `docs/decisions/shell.md`,
"`strace` is a `/bin` program, and the trace goes to the tracer's
terminal".

**STAGES 1-3 ARE BUILT (2026-10-09)**, and beside them **the kernel
EVENT ring (2026-11-21)** -- switches, interrupts, faults, syscalls and
lock waits on every core, a different question from one process's
syscalls; its own section is at the end. The stage markers are on the
headings.

## The finding that shapes the plan

**The 2026-08-23 decision kept the formatting in the kernel for one
concrete reason: "relaying would mean the kernel handing every line to
a ring-3 process through a channel that does not exist."** That is no
longer true. Since then the tree gained shared memory with an owner
(`SYS_SHM_GRANT`), a ring over it (`userland/libs/base/uchan.h`) and a wait
that consumes nothing (`SYS_WAKEWORD`); the compositor, sound and the
clipboard run on them. The netstack plan found the same thing.

**The second finding: `strace` and a debugger want DIFFERENT
mechanisms, and Linux's choice to share one is the expensive one.**
Linux's `strace` runs on `ptrace(2)`: the tracee STOPS at every syscall
entry and exit, and the tracer reads its registers and memory. That is
the debugger's mechanism, and it costs two context switches per call. A
tracer only needs to be TOLD what happened. A debugger needs the
process held still while it looks. So this plan builds the cheap one
for `strace` now, and leaves the stop for when there is a debugger to
use it -- sharing the parts that really are common: naming the target,
the decode table, and the record of a call.

## What real systems do

| system | syscall tracing | kernel events | debugging |
|---|---|---|---|
| **Linux** | `strace` over `ptrace` stops (decoding in ring 3); `perf trace` reads the `raw_syscalls` tracepoints from a kernel RING BUFFER instead, with no stop | **ftrace**: static tracepoints (`sched_switch`, `sched_wakeup`, `irq_handler_entry`, `page_fault_user`, ...) each a patched-out branch when off, writing binary events into a ring buffer PER CPU; `trace-cmd`/`perf` read the raw pages and decode in ring 3 | `ptrace`; `gdbserver` speaks the remote protocol over it |
| **FreeBSD** | **`ktrace(2)`**: the kernel appends binary records -- `KTR_SYSCALL`, `KTR_SYSRET`, `KTR_NAMEI` (a path, copied at the call), `KTR_GENIO` (I/O bytes) -- and **`kdump(1)` decodes them in ring 3**. `truss` is the ptrace-shaped alternative | **KTR** (`ktr(4)`): one circular buffer of fixed entries -- timestamp, CPU, thread, a format string and six arguments -- compiled in by option, masked at run time; `ktrdump` formats it. DTrace beside it | `ptrace` |
| **Windows** | ETW: the kernel logger writes system-call events into per-CPU buffers; decoding is the consumer's | **ETW's kernel logger**: context switches, DPCs and interrupts, page faults, per-CPU buffers flushed to an `.etl`; `xperf`/WPA decode it | debug objects (`NtCreateDebugObject`, `DbgUi`) -- a stop model of its own |
| **SaunaOS** | stages 1-3: records into a ring the TRACER owns, decoded by `/bin/strace` | the kernel event ring below: per-core binary rings, armed by `kernel.trace`, read over `QUERY_KTRACE`, decoded by `/bin/trace` and `trace_view.py` | none (`docs/kdebug-design.md` keeps user-space debugging out of the kernel stub) |

**FreeBSD's `ktrace`/`kdump` split is the shape for `strace`**: a record
stream the traced process never waits for, path strings copied at the
moment of the call (so a later write to that buffer cannot change what
the trace says), and every word of formatting in ring 3. **Linux's and
NT's stop model is the shape for the debugger**, and nobody ships the
debugger on the record stream.

## What exists, measured (2026-10-09)

| | |
|---|---|
| the hooks | three in `syscall_dispatch()` (`kernel/proc/syscall.c`): `strace_begin()` before the handler, `strace_end()` or `strace_end_noreturn()` after |
| what is formatted in ring 0 | everything: the name, three argument kinds (`A_PATH`, `A_BUF`, `A_OFLAGS` beside int/fd/hex), the return value, 14 errno names -- `kernel/proc/strace.c`, 507 lines |
| arguments | three registers -- ALL the ABI has (RDI, RSI, RDX; `userland/libs/rt/sys.c`): a call needing more passes a struct, and the struct's contents (`spawn_msg`, `setting_msg`) are what the trace cannot show |
| strings | copied from the tracee at entry (`vmm_copy_from_user()` with its pml4), 32 bytes before `...` |
| a call that parks | prints `= ?` and never its result -- a `read` on a pipe, `waitpid`, `SYS_WAIT_EVENT` |
| who is traced | ONE address space machine-wide (`g_traced_pml4`), named at the spawn (`SPAWN_TRACE`), followed across exec, not across fork |
| where it goes | the terminal the tracer's fd 1 names, and klog; `/bin/strace` (99 lines) spawns and waits, and prints nothing itself |
| what it cannot do | attach, follow children, filter (`-e`), count (`-c`), write to a file (`-o`) -- the decision lists them as "all of which want `ptrace`" |
| the decode data | the syscall table (`kernel/proc/syscall_table.c`) -- name, three arg kinds, DEC or HEX return -- kernel-only; ring 3 sees names only, through `kstack syscalls` |
| a debugger's raw material | NONE in ring 3: no cross-process memory read, no register access, no SIGTRAP, no stop-on-syscall. The kernel GDB stub can read a process's memory, but halts the whole machine to do it |

**Three of the "what it cannot do" items do not want `ptrace` at all.**
Filtering, counting and `-o` are things a ring-3 decoder does to a
stream it already has. Only attaching and the debugger need a stop.

## The choices

### 1. What the kernel hands over

| | |
|---|---|
| **A. A record stream (recommended)** | `ktrace`'s shape: a fixed-size record per syscall entry and exit -- pid, number, the three argument registers, the return value, plus the bytes of any path or buffer argument, copied at entry -- written into a ring the tracer owns. The traced process never stops for the tracer. |
| B. Stops | Linux `strace`'s shape: the tracee stops at entry and exit, and the tracer reads registers and memory. One mechanism for both tools, and the expensive one -- and it needs the whole debugger's machinery (stop, cross-process read, registers) before `strace` works again. |
| C. Nothing | Keep the 2026-08-23 decision: format in ring 0, extend it there. |

### 2. A full ring

The handler runs with interrupts off, so the record cannot simply wait
for space.

| | |
|---|---|
| **A. Stop the tracee on its way back to ring 3 (recommended)** | Mark it, and park it at the return edge -- one of the two places signals already deliver (`docs/decisions/kernel.md`, "Signals deliver on the way back to ring 3") -- until the tracer drains the ring and bumps its wakeword. Nothing is lost, which is what `ktrace` gets by writing synchronously. |
| B. Drop, and count | `perf`'s and ETW's choice: a LOST record says how many. Never slows the tracee, and a trace with holes in it is a worse `strace`. |

### 3. Where the decode table lives

| | |
|---|---|
| **A. Generated, compiled into both rings (recommended)** | The name and argument kinds become data in `kernel/include/abi/` that `syscall_table.c` and `/bin/strace` both read -- the "one source, two rings" rule, and `tools/gen/gen_signames.py`'s precedent for dash. A build where they disagree cannot exist. |
| B. Asked at run time | A query provider hands out the table, so an old `/bin/strace` decodes a new kernel. Strace is shipped with its kernel here (System Update replaces both), so the case it covers does not arise. |

## The stages

Each stage has a caller of its own before the next one needs it.

### Stage 1 -- records, beside the text -- BUILT 2026-10-09

`SPAWN_TRACE` gains a ring: the tracer creates an shm object, grants it
to nobody, and names it in the spawn message; the kernel writes entry
and exit records into it and bumps the tracer's wakeword. **The text
path keeps working unchanged**, so stage 1 can be checked against it
line for line. A call that parks gets its EXIT record when it finally
returns -- Linux's `<... read resumed>`, which `= ?` cannot say today.
The handler resumes inside `syscall_dispatch()` after the wake (a real
context switch), so the value is in RAX by then and the record is
written in the tracee's own context.

**A full ring stops the tracee BEFORE the call, not after it** (choice
2A, as built): at entry, with no room for an entry and an exit record,
the dispatcher rewinds RIP over the 2-byte `syscall`, sleeps the
tracee a millisecond and puts RAX back, so the call is re-issued once
the tracer has drained -- the signal path's SA_RESTART rewind. Nothing
has run, so nothing is lost, and nothing parks with interrupts off.

### Stage 2 -- `/bin/strace` decodes, and the kernel's formatter goes -- BUILT 2026-10-09

`/bin/strace` prints the records to its stderr, as real strace does
(fd 2 is the terminal's), and `strace.c`'s formatting is deleted (the
arming, the hooks and the copy at the call stay). **What it gains for
free**: `-o FILE`, `-e trace=open,read`, `-c` for a count per call, and
errno names from libsys's one table (`sys_errname()`, glibc's
`strerrorname_np()`) rather than a 14-entry switch.

As built: choice 3A is an X-MACRO rather than a generator --
`abi/syscall_rows.h` holds each syscall's row once, the kernel expands
it with the handler and `base/utrace.c` without (an unused macro argument
is never looked up), so `check_syscalls.py`'s duplicate-number check
still has one list to read. `SPAWN_TRACE` without a ring is refused. A
path argument is copied to its terminator, a byte at a time, so the
dynamic loader's opens near the top of the stack decode (the text trace
showed them as pointers). The formatter KTESTs became
`/tests/utrace_test`, over records built in the test.

### Stage 3 -- more than one tracee -- BUILT 2026-10-09

One ring per tracer rather than one global: the tracer's pid owns the
ring, records carry the pid, and **fork follows** (the open roadmap item
"`strace` extended to follow a process's children once `fork()`
exists") -- a forked child writes into its parent's ring, as `strace -f`
shows.

As built: traces and traced address spaces in tables that grow
(`kernel/proc/strace.c`, kslots), bounded by the ring memory they pin --
a sixty-fourth of RAM; past it the spawn is `-EBUSY`, logged once, never
a child started untraced. Following is OPT-IN per trace
(`SPAWN_TRACE_FOLLOW`, Linux's `-f`) and covers both ways a child is
made -- spawn and fork. The ring header's `live` counts the address
spaces still writing, so the tracer knows when an orphaned grandchild
has finished too. `/bin/strace -f` prefixes another pid's lines
`[pid N] `, as Linux's does. Found on the way: the loaders claimed the
trace BEFORE the image could fail to load, leaving an entry for a
destroyed address space -- the claim is now their last step.

### Stage 4 -- the debugger's half, when there is a compiler

The roadmap item "A debugger for ring-3 programs" -- a `ptrace`-shaped
set: attach to a pid (the device claim's shape, one holder), stop it
(SIGSTOP and `SYS_WUNTRACED` already report a stopped child), read and
write its memory and registers, a breakpoint via SIGTRAP. `gdbserver`
in ring 3 speaks the protocol `kernel/debug/gdbstub.c` already speaks.
**It reuses from stages 1-3**: naming the target, the decode table (GDB's
`catch syscall`), and the return-edge stop from choice 2A. Not before
`/bin/cc` builds programs on the machine (`docs/cc-design.md`), because
until then every binary was built on a host that has GDB already.

## What does NOT move

- **The hooks and the string copy.** The kernel is the only place that
  sees every call, and the bytes of a path must be copied AT the call --
  a ring-3 tracer reading them later would race the tracee's next write,
  which is why `ktrace` copies too.
- **The arming at the spawn.** It has no race (the decision explains
  why), and a `ptrace`-style attach is stage 4's, not a replacement for
  it.

## What it buys

Not containment -- the formatter parses nothing a stranger sends; it
reads the tracee's own memory with bounds already checked. It buys
**the missing features without `ptrace`** (`-o`, `-e`, `-c`, children,
the result of a call that blocked), about 350 lines out of
ring 0, and a record format the debugger's `catch syscall` reuses.

## The case against

- **Nothing is broken, and `strace` is a diagnostic.** Nobody has asked
  for `-e` or `-c` yet; the decision said so in 2026-08.
- **A shm ring is more moving parts than a `tty_output()` call.** A
  tracer that dies leaves a ring nobody drains -- choice 2A must not
  stop the tracee for ever, so a ring whose owner has exited is
  abandoned and tracing ends.
- **Stage 1 alone fixes `= ?`** even if stage 2
  never happens -- the strongest point in favour, as stages 1-2 were
  for the netstack.

## Decided so far

Picked 2026-10-09: **1A, a record stream**, and **2A, a full ring
stops the tracee** -- before its call, by re-issue (Stage 1). Choice 3 was not put to the
maintainer and takes its recommendation, **3A, a generated table**,
unless reopened. The debugger's stage 4 is not scheduled.

## The kernel event ring -- BUILT 2026-11-21

A DIFFERENT QUESTION from the stages above: not "what did this program
ask the kernel" but "what was the machine doing" -- which thread ran on
which core, who woke whom, what interrupted it, where it faulted, which
lock it waited for. strace's ring belongs to one tracer and follows one
address space; this one is the kernel's, per core, and sees everything.
The maintainer's pick (2026-11-21): **armed rings plus a panic tail**.

**As built** (`kernel/include/kernel/ktrace.h`, `kernel/proc/ktrace.c`,
the format in `abi/ktrace_abi.h`):

- **Per-core rings of 48-byte records** -- TSC, core, thread, event id,
  four arguments -- ftrace's and ETW's shape, not KTR's one global
  buffer: a writer touches only its own core's ring (struct cpu's
  `ktrace`, one `%gs` load with interrupts off), so it takes no lock and
  no atomic beyond publishing its head. A full ring overwrites its
  oldest record (ftrace's overwrite mode); the stop logs each core that
  lost some.
- **Fixed trace points**: the switch (`switch_to()`, `switch_to_kernel()`:
  both threads and the outgoing one's state), the wakeup
  (`sched_delay_woken()`, which every way of making a parked slot READY
  passes), an interrupt's entry and exit (`isr_dispatch()`), a page fault
  (`isr_dispatch_body()`), a syscall's entry and exit
  (`syscall_dispatch()`), a contended spinlock's wait (`take()`'s
  contended path), a kmutex sleep and hand-over (`kmutex.c`).
- **Armed, off by default** (`kernel.trace` = a list of groups, or
  `off`): disarmed, a point is one load of `g_ktrace_mask` and a branch
  predicted not taken, its arguments not evaluated -- ftrace's static
  keys patch the branch out entirely, which this does not need yet.
  Measured below.
- **Allocated when armed**, `kernel.trace_size` records a core (8192 by
  default; up to a 32nd of RAM), and kept after the stop so the trace is
  read afterwards; the next start empties them, a size change replaces
  them. A ring is freed only after every core has run a cross-core call
  that moves its own pointer -- a call never lands inside a write, so
  none can still be writing through the old one (RCU-sched's shape).
- **Read over a QUERY, not an shm grant.** `QUERY_KTRACE_CPU` (a row a
  core) and `QUERY_KTRACE` (five records a call). A trace is read AFTER
  the interesting part, so throughput does not matter, and a query
  needed no mapping of kernel frames into a process and inherits the
  ABI's grow-append-only rule. Linux's `trace_pipe_raw` splices whole
  pages instead, which is the thing to copy if a live consumer ever
  needs it. A running ring moves between two reads, so each chunk
  carries its first record's number and the reader drops repeats.
- **Decoded in ring 3** (`/bin/trace`, `docs/commands/trace.md`), with
  the line formatter in `kernel/lib/ktrace_fmt.c` compiled into both
  rings (klineedit's rule), because the kernel decodes too -- once, at
  a panic. `trace dump` writes a file that DESCRIBES ITSELF (event names
  and argument kinds, syscall names, thread names, the TSC rate), the
  role of ftrace's `events/*/format` files, and
  `tools/dev/trace_view.py` turns it into Chrome Trace Event JSON for
  Perfetto: a track per core, a slice per stretch a thread ran.
- **The panic tail**: with the trace armed, `panic_store_save()` leaves
  12 KiB of the 32 KiB record for the newest records of each core (~32
  each on four cores), decoded after the log by the same formatter -- no
  lock, no heap, the other cores already stopped. A trace that was not
  running costs the record nothing.

**What it does not do**: filter by process (ftrace's `set_event_pid`),
stream while running, or trace NMIs (a point in an NMI could tear the
record being written on that core -- one record, never a fault). The
TSCs of two cores are assumed to agree, which QEMU's and every
invariant-TSC machine's do.

**Measured** (2026-11-21, `tools/bench/sys_bench.py --kvm --cpus 1`,
i7-11700, cycles, medians of 3-10 runs against the build before it):
disarmed, a `getpid` is 252 either way and a one-byte pipe write+read
1263 before and 1362 after, inside that row's own run-to-run spread (its
runs range 1045-3387). Armed for `syscall`, a getpid -- two records -- is
373 (+48%) and the pipe 1486 (+18%); armed for `all`, 379 and 1433.
