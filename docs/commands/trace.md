# trace

**a `/bin` program**

**Category:** System information

## Synopsis

    trace
    trace [-s RECORDS] start [GROUPS]
    trace stop
    trace [-n LINES] show
    trace dump FILE
    trace [-s RECORDS] run GROUPS PROGRAM [ARGS...]

## Options

- `-s`, `--size=RECORDS` -- with `start` or `run`: how many records each
  core's ring holds (the `kernel.trace_size` tunable), rounded up to a
  power of two; 64 up to a 32nd of RAM across every core. Takes effect
  at a start from stopped, so it is refused while a trace runs (`trace
  stop` first). Default 8192 (384 KiB a core).
- `-n`, `--lines=LINES` -- with `show`: only the newest LINES.
- `-h`, `--help` -- print the usage and exit.

A short option comes BEFORE the command, since what follows a command
is the command's own; the long form works anywhere.

## Commands

- `start [GROUPS]` -- record the groups named (comma-separated), or
  `all` (the default): `switch`, `wakeup`, `irq`, `fault`, `syscall`,
  `spin`, `mutex`. From stopped it empties every ring first; while
  running it changes only what is recorded.
- `stop` -- stop recording. The records stay to be read until the next
  `start`, and the kernel logs each core that lost some to a full ring.
- `show` -- print the records as text, every core's merged by time,
  newest last.
- `dump FILE` -- write the raw records to FILE, with a header naming the
  format version, the TSC rate and the cores, and the event, syscall and
  thread-name tables a reader needs -- for
  [`tools/dev/trace_view.py`](../tools/dev.md) on the host.
- `run GROUPS PROGRAM [ARGS...]` -- start, run PROGRAM until it exits,
  then stop: the trace covers that program's stretch and no more, as
  `trace-cmd record CMD` and `perf record -- CMD` do. Refused while a
  trace already runs, whose records and stop it would take over.

With no command, `trace` says what is armed and what each core holds.

## Description

The kernel event trace: the kernel writes a fixed-size binary record at
each of a few fixed points -- a context switch (both threads and why
the outgoing one left), a wakeup, an interrupt's entry and exit (its
vector), a page fault (address, error code, RIP), a syscall's entry
(number and three arguments) and exit (its return), a contended
spinlock's wait (the lock, how long, where), and a kmutex sleep and
hand-over -- into a ring PER CORE, the shape of Linux ftrace's ring
buffer and of Windows ETW's per-CPU buffers. `trace` arms it, reads it
back over `QUERY_KTRACE` and decodes it here, in ring 3.

A pipe round trip between `/tests/ktrace_load` (pid 31) and the child
it forked (32), from `trace run switch,wakeup,irq,fault,syscall
/tests/ktrace_load` on a two-core QEMU guest:

        0.012305 c1 [31] sys_enter nr=write a0=0x4 a1=0x807ff00f16 a2=0x1
        0.012478 c1 [31] sys_exit nr=write ret=1
        0.012520 c1 [31] sys_enter nr=read a0=0x5 a1=0x807ff00f17 a2=0x1
        0.012619 c1 [0] switch prev=31 prev_state=S next=32 next_tgid=32
        0.012735 c1 [32] sys_enter nr=read a0=0x3 a1=0x807ff00f17 a2=0x1
        0.012835 c1 [32] sys_exit nr=read ret=1
        0.012861 c1 [32] sys_enter nr=write a0=0x6 a1=0x807ff00f17 a2=0x1
        0.012868 c1 [32] wakeup tid=31 chan=0x1afce90 reason=2 tgid=31
        0.012875 c1 [32] sys_exit nr=write ret=1
        0.012878 c1 [32] sys_enter nr=read a0=0x3 a1=0x807ff00f17 a2=0x1
        0.012889 c1 [0] switch prev=32 prev_state=S next=31 next_tgid=31
        0.012929 c1 [31] sys_exit nr=read ret=-4095

A line is the seconds since the trace started, the core, the thread
that was current (`[0]`: the kernel, an idle core, or a thread that has
already let go of the core as it parks), the event and its arguments. A
switch's `prev_state` is `R` for a thread preempted while still
runnable, `S` for one that parked, `Z` for one that exited, `-` for the
kernel. A parked read's exit can return `-4095`: the "ask again" value
(`SYS_RETRY`) its wake wrote, which the C library retries.

**Off by default, and nearly free while off**: each trace point is one
load and a branch, its arguments not even computed. Armed, a record is a
TSC read and 48 bytes into the core's own ring with interrupts off --
no lock. A full ring overwrites its oldest records; `stop`, `show` and
the status say how many each core lost, and `-s` makes the rings bigger.

**The records name kernel addresses** (a lock, a RIP), and `SYS_QUERY`
answers any process, as it does for every other fact -- arm it on a
machine you are debugging, not as a matter of course.

At a kernel panic with the trace armed, the newest ~32 records of each
core are decoded into the panic record the next boot files with the dead
boot's log (`kernel/panic_store.h`), after the log itself.

## See also

[`strace`](strace.md) for one program's syscalls with their arguments
decoded, [`lockstat`](lockstat.md) for lock contention as totals,
`docs/trace-design.md` for the design.
