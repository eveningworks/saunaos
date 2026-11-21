# Lock-order checking (lockdep)

**Status: BUILT 2026-11-21, behind `option lockdep` (off by default, on
in `configs/debug.conf`).** `kernel/proc/lockdep.c`, its header
`kernel/include/kernel/lockdep.h`, its KTESTs `kernel/proc/lockdep_test.c`
(suite `lockdep`), the readout [`lockstat order`](commands/lockstat.md)
over `QUERY_LOCKDEP`.

SMP stage 7 (`docs/smp-design.md`) takes paths off the kernel lock one at
a time, and every path that leaves it starts relying on the ORDER its
spinlocks and kmutexes are taken in. A wrong order deadlocks only when two
cores interleave just so, which a TCG suite almost never shows. Lockdep
finds the order bug from ONE run that takes the locks both ways, at any
speed, on any number of cores.

## What it checks

- **A cycle in the lock order.** Every lock belongs to a CLASS. Taking
  class B while holding class A records the edge A -> B in a graph
  between classes; an edge that would close a cycle (B -> A seen before,
  or A -> B -> C before C -> A) is reported, naming both classes, where
  the current edge's locks were taken and where each edge of the old
  path was first recorded, with a stack scan of the current take. The
  edge is not added, so the graph stays acyclic and each later cycle is
  new; the pair is not reported again.
- **Two locks of one class held together** ("recursion"), unless the
  second is a different SUBCLASS (`kmutex_lock_nested()`) or both are
  under one NEST lock (`kmutex_lock_nest_lock()`). Re-taking the SAME
  kmutex is the lock's own business -- it is recursive by design -- and
  is not even seen.
- **IRQ safety, for spinlocks.** A class taken inside a hardware
  interrupt and also taken elsewhere with interrupts on is reported
  (Linux's `{IN-HARDIRQ} -> {HARDIRQ-ON}` "inconsistent lock state"): the
  interrupt can land on the core that holds it and spin forever. So is a
  direct edge from an irq-safe class to an irq-unsafe one.
- **A kmutex taken holding a spinlock** -- a sleep under one. kmutex.c's
  `might_sleep()` check already counts every take from atomic context;
  lockdep adds which spinlock was held.
- **A kmutex taken in a hardware interrupt**, **the kernel lock taken
  holding a spinlock**, and **a kmutex still held on the way back to
  ring 3**.
- **An embedded lock with no name**: reported once per call site and not
  checked (below).

A report is written to the log ONCE per pair (or per class, for the
one-class kinds) and kept in a table the query reads; nothing panics, and
checking carries on.

## Classes

- **A file-scope static lock is its own class**, keyed by its address:
  a lock inside the kernel image needs nothing. A `SPINLOCK_COUNTED()`
  lock is named by its counters (`heap`, `frames`); any other static is
  named `@function` after the function that first takes it.
- **A lock inside a struct is named where it is set up** --
  `kmutex_name(&m, "mount")`, `spin_lock_name(&l, "...")` -- and every
  lock given one name is ONE class: all mount locks, all AHCI drives'
  locks, every file description's position lock. The name is a string
  literal. Without `option lockdep` both are empty inlines.
- **An unnamed lock outside the image is not checked**: its first take
  is reported (`LOCKDEP_UNNAMED`, once per site), which is how a missing
  name shows. A static ARRAY of structs (`ahci.c`'s `g_drives[]`) is in
  the image, so it would silently be one class per element -- name it
  anyway.
- The named ones today: `mount` (`mount.c`, every take through
  `lock_slot()`), `mm` (`mmap.c`), `file position` (`fs_syscalls.c`),
  `ahci drive`, `usb-storage`, `xhci command`.

**Subclasses** are separate graph nodes ("mount/1"), so a fixed order
between two locks of one class is an edge, and the reverse an inversion.
The mount locks are taken as their DEPTH's subclass: `/` is mount/0,
`/boot` mount/1 -- the filesystem's "parent mount before child" rule,
which as one class would read as recursion. Depth is a conservative
stand-in for parenthood (two sibling mounts of one depth held together
would report) and clamps at 7. **`fs_exclusive_begin()` takes every
mount at once under `g_excl`** with `kmutex_lock_nest_lock()`: one held
entry with a count, no edges between them.

## Held locks, and where they live

- **Spinlocks per CORE** (`struct cpu`'s `ld`): a held spinlock raises
  the preemption guard, so it cannot move.
- **kmutexes per CONTEXT** (`struct sched_process`'s `ld`, or the core's
  own for slot -1 -- the boot core's kernel context, an AP's idle loop):
  a kmutex is held across sleeps and migrations.
- **The interrupt depth is per context too**, raised by `isr_dispatch()`
  around a hardware interrupt's body (vector 32 and up, not the syscall
  vector). A tick that switches contexts inside that body must not hand
  its depth to the context it resumes, which may be in a syscall -- a
  per-core counter would mark that syscall's locks as taken in an
  interrupt.
- **The check runs BEFORE the take** (Linux's `lock_acquire()`), so an
  inversion that really deadlocks is reported rather than hung on; a
  trylock is recorded only once it has the lock and adds no edges, since
  it cannot wait.
- Twelve kmutexes per context, eight spinlocks per core; deeper takes
  are not tracked and count in `unmatched`.

**The graph is fixed-size on purpose** -- 255 classes, 2048 sited edges,
64 reports: it is written from inside `spin_lock()`, under the heap's own
lock and in interrupts, where nothing may allocate (Linux's
`MAX_LOCKDEP_KEYS`, for the same reason). A full table logs once. The
fast path -- every edge from the held locks already known -- is one bit
test per held lock in an unlocked bitmap; the graph lock (a bare
test-and-set with interrupts off, never a checked lock) is taken only for
a class, edge or report seen for the first time. Reports are written to
the log only by a core holding the kernel lock (the log's own rule); one
made without it waits for the next lockdep event, or for the query.

## The kernel lock is not a class

`kernel/proc/bkl.c`'s lock is held WHENEVER the kernel runs: taken raw at
every entry, recursive, its depth moved with the context at every switch
(`bkl.h`). As a class it would sit before every other class -- an edge
from it to everything, which says nothing -- and it is never released
around a sleep in the ordinary sense, so its "held" state per context is
the depth bookkeeping bkl.c already checks (`bkl_faults()`). What CAN go
wrong with it is a fresh take while holding a spinlock (a spinner holding
something the kernel-lock holder needs), and that is checked:
`spin_lock_raw()` is the kernel lock's alone and reports a take with a
spinlock held on this core. It is the one lock left out, and the reason
is that it is the outermost lock by construction.

## What real systems do, and what was copied

**Linux's lockdep** (`kernel/locking/lockdep.c`, `CONFIG_PROVE_LOCKING`)
is the model. Copied: classes keyed by a static lock's address and by a
key given at init for a dynamic one; the dependency graph and the
breadth-first search for a path back on each NEW edge; the check made
before the wait; subclasses (`mutex_lock_nested()`) and nest locks
(`mutex_lock_nest_lock()`); trylocks recording a hold but no dependency;
the `IN-HARDIRQ`/`HARDIRQ-ON` usage bits and the irq-safe -> irq-unsafe
edge; `/proc/lockdep_stats` as the shape of the summary; the "non-static
key" warning for an unnamed dynamic lock.

Left out, and why:

- **Lock chains** (a hash of the whole held stack, so a known stack skips
  validation): with at most a handful held, a bit test per held lock is
  as cheap and needs no hash table.
- **Read/write locks, softirq states and the full transitive irq-safety
  search**: there are no rwlocks and no softirqs, and the irq-safety
  check covers the state and direct edges only -- an irq-safe class
  reaching an irq-unsafe one through a third class is not found.
- **A saved stack trace per dependency**: the kernel has no frame
  pointers, so an edge keeps its two call sites, and only the CURRENT
  take gets a stack scan.
- **Turning itself off after the first report** (`debug_locks_off()`):
  a hobby kernel's first report is often a known one, and switching off
  would hide the next. FreeBSD's way is taken instead.
- **Class keys per init site** (`static struct lock_class_key` in
  `spin_lock_init()`): a NAME is the key here, so two init sites that
  give one name share a class -- FreeBSD's way.
- **`lock_stat`'s integration**: `lockstat` already counts the named
  spinlocks; the two stay separate commands' halves of one program.

**FreeBSD's WITNESS** (`kern/subr_witness.c`) is the other shape and two
of its choices are taken: locks are grouped by their NAME (`mtx_init()`'s
name), and a lock order reversal is reported once per pair while the
system carries on. Its held-lock split is the same as here -- spin locks
per CPU (`pc_spinlocks`), sleep locks per thread (`td_sleeplocks`). Left
out: its static table of known orders (`witness_order_list`) and blessed
pairs -- the order is only ever learned -- and the run-time switch
(`debug.witness.watch`): it is a build option, so a kernel without it has
no hook on any lock path at all.

## What the real kernel reported

`make iso PROFILE=debug` (lockdep with KASAN, UBSAN and DEBUG_PREEMPT),
four vCPUs under TCG, 2026-11-21: the whole KTEST suite, the boot smoke
test and the ring-3 suite (`usertest_run.py`).

- **A REAL INVERSION, NOT FIXED HERE: `block.c`'s `g_wlock` and
  `block_part.c`'s `g_plock`.** `blk_part_create()` holds `g_plock` and
  takes `g_wlock` through `blk_track()`; `gone_as()` -- a disk removed or
  ejected -- holds `g_wlock` and takes `g_plock` through `reap()` ->
  `blk_part_release()`. A disk going away while a partition is being
  created (a USB stick pulled during another disk's rescan or a `part`
  write) deadlocks both. Found by the `blk-gone` KTESTs; `docs/bugs.md`.
- **Two false positives, fixed in the checker**: a process killed on its
  way back to ring 3 after a tick took `userfs_space_gone()`'s kmutex in
  what was counted as the tick's interrupt -- the interrupt depth now
  ends where the handler does (`isr_dispatch_body()`'s `trap_exit`), not
  after the return-to-user work; and the legacy loader
  (`process_run_ring3()`) runs ring 3 on slot -1 from inside a kernel
  call that holds an fs exclusion, so the held-on-return check skips
  slot -1.
- The boot and the ring-3 suite reported nothing, and no lock was left
  unnamed.

## Cost

`tools/bench/sys_bench.py --kvm --runs 3 --calls 2000000`, one vCPU on the
i7-11700 host, 2026-11-21; each build from a clean tree, the base an
export of the commit before:

| build | getpid | 1-byte pipe write+read | two-process round trip |
|---|---|---|---|
| before lockdep | 234-273 cycles | 1026-1080 | 82945-83208 |
| `lockdep` off | 230 | 1016-1071 | 83127-83486 |
| `lockdep` on | 239-242 (+4%) | 1196-1238 (+16%) | 84132-84776 (+1%) |

**Off is the same code**: `spin_lock()`, `spin_unlock()`, the irqsave
pair, `spin_trylock()`, `spin_lock_raw()`, `take()`, `kmutex_unlock()`,
`kmutex_trylock()`, `isr_dispatch()` and `mount_lock()` disassemble
identically to the base, and `kmutex_lock()` differs only in the operand
order of two compares. **On**, a hook avoids `popf` when interrupts were
already off (every irqsave take): the first version restored the flags
with `popf` four times per syscall and cost a getpid +48% and the pipe
+43%, and under `PROFILE=debug` made the KTEST suite take 759 s and
`fd_test`'s 6-second budget run out. Now that suite takes 588 s with
lockdep and 599 s without -- run-to-run noise.

## Testing

`ktest lockdep` (in every kernel; skipped without the option): an
A -> B / B -> A inversion between kmutexes and between spinlocks, a
three-class cycle with its path, a same-class nest with and without a
subclass and reversed, many of one class under a nest lock, a spinlock
taken in a simulated interrupt and then with interrupts on, a kmutex
under a spinlock and in an interrupt, and the running kernel having
classes and no unnamed lock. Each asserts ONE report naming the test's
classes, and forgets them after (`lockdep_forget()`), so the running
kernel's readout carries nothing from a test. Every check was seen red
with `tools/dev/mutate.py` breaking the detection it covers.
