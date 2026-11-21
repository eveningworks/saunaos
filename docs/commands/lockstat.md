# lockstat

**a `/bin` program**

**Category:** System information

## Synopsis

    lockstat [-i SECONDS] [-e [-a]]
    lockstat track on|off
    lockstat order

## Options

- `-i`, `--interval=SECONDS` -- count over the next SECONDS instead of
  since boot (1 to 86400).
- `-e`, `--entries` -- also who uses the kernel lock: each core, and the
  entries holding it longest.
- `-a`, `--all` -- with `-e`, every entry rather than the top 15.
- `-h`, `--help` -- print the usage and exit.

## Commands

- `track on|off` -- start or stop timing HOLDS (the `kernel.lockstat`
  tunable, off at boot). Starting zeroes the hold totals, so they cover
  one tracked stretch.
- `order` -- what lock-order checking found: a kernel built with
  `option lockdep` (`make iso PROFILE=debug` has it). See below.

## Description

How contended the kernel's counted spinlocks are, the shape of Linux's
`/proc/lock_stat`: for the kernel lock, the heap and the frame allocator,
how often each was taken, how many of those takes had to wait, the total
and longest wait and hold, and the share of the time it was held
(`QUERY_LOCKSTAT`, `kernel/proc/lockstat.c`).

    # lockstat track on; lockstat -i 8
    Over 8 s (the maxima are since boot):
    LOCK          TAKES    WAITED              WAIT   WAIT MAX       HOLD   HOLD MAX   HELD
    heap          17978         0   0.0%       0 ns       0 ns    41.6 ms   221.4 us   0.5%
    frames         4246         0   0.0%       0 ns       0 ns    36.3 ms    64.1 us   0.4%
    kernel        63731     31199  48.9%      1.1 s     3.2 ms      1.5 s   141.9 ms  19.2%

`-e` adds the kernel lock by core, then by ENTRY -- a syscall by name, a
page fault, an interrupt (`irq 14`, a handler such as `lapic_timer_isr`,
`core tick` for another core's tick and reschedule), or `kernel` for the
kernel itself taking it back after the idle halt -- longest held first
(`QUERY_BKLSTAT`). A hold is billed to the entry that took the lock; a
switch on that core may hand its release to another context.
`LOCKLESS` counts the syscalls answered WITHOUT taking it -- the rows
`abi/syscall_rows.h` marks MPSAFE (`getpid`, `gettid`, `ticks`,
`monotonic_ns`, `clock_page`), by core and by entry. The tunable
`kernel.lockless_syscalls` (on by default) sends them through the lock
again, for comparing the two.

**Hold times are off until `lockstat track on`**, the shape of
[`stalls`](stalls.md)' `track`: timing a hold reads the clock twice on
every take, which measured +20% on a `getpid` on one KVM core. Takes,
waits and wait times are always counted -- a take that did not wait reads
no clock, and one that did was spinning anyway -- so contention is
visible on any machine; the hold columns print `-` while it is off.

The counters run from boot and are never reset: `-i` samples twice and
prints the difference, as `vmstat 1` does, so two readers never disturb
each other. A maximum cannot be differenced, so with `-i` it is still
the boot's.

It is how SMP stage 7 picks what to take out of the kernel lock next
(`docs/smp-design.md`): a syscall that waits often and holds briefly is
one that needs none of what the lock protects.

## Lock order: `lockstat order`

On a kernel built with `option lockdep`, every spinlock and kmutex take
is checked against the order lock CLASSES have been taken in so far
(`docs/lockdep-design.md`, Linux's lockdep). `order` prints the summary,
the shape of Linux's `/proc/lockdep_stats`, then each finding, oldest
first (`QUERY_LOCKDEP`):

    # ktest blk-gone
    ...
    # lockstat order
    Lock-order checking: 15 classes [max 255], 18 dependencies [max 2048], 147112 takes checked
    Most held at once: 2 spinlocks on a core, 4 kmutexes in a context

    1 finding:
    1. lock order inversion
       now:    holding "@blk_register_over" (taken at gone_as+0x7e), took "@blk_part_create" at blk_part_release+0x22
       before: holding "@blk_part_create" (taken at blk_part_create+0x22), took the next at blk_track+0x29

That finding is real (`docs/bugs.md`). A class is a lock's NAME (`mount`, `heap`, `file position`), a subclass
after a slash (`mount/1`: a mount one level down), or `@function` for a
static lock, after the function that first took it. A finding is
reported once, in the kernel log as well (`dmesg`, with every edge of a
longer cycle and a stack scan of the take), and checking carries on. The
kinds: a lock order inversion (two orders, or a cycle through more
classes), two locks of one class held together, a spinlock taken in an
interrupt and with interrupts on, an irq-safe lock held while taking an
irq-unsafe one, a kmutex taken holding a spinlock, a kmutex taken in an
interrupt, the kernel lock taken holding a spinlock, a kmutex held on
the way back to ring 3, and an unnamed lock -- one inside a struct that
nobody named, which is therefore not checked.

A kernel without the option has no `QUERY_LOCKDEP`, and `order` says so
and exits 1.

## See also

[`cpus`](cpus.md) for each core's state now, [`lscpu`](lscpu.md) for the
load on each core.
