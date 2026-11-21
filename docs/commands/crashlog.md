# crashlog

**a `/bin` program.**

**Category:** System information

## Synopsis

    crashlog [REPORT]
    crashlog --panics [BOOT]

## Description

Lists the crash reports in `/var/crash`, or prints one. A report is
written by the kernel when a ring-3 process faults and is torn down
(`kernel/proc/crash_report.c`): `<program>-<pid>.crash`, a text header
-- the program, pid, fault, registers, memory map and the tail of the
kernel log -- followed by the raw bytes of the process's stack.

A core-dumping signal's default action writes one too (`Killed by
SIGSEGV`). A process that set `prctl(PR_SET_DUMPABLE, 0)` -- a test
child faulting on purpose -- gets none, and no "closed unexpectedly"
card; `dmesg` says `crash: no report`. On the desktop the same reports are the Crash Reports app
(`/bin/wm/apps/crashreports`), which a crash notice's Details opens.

`crashlog <report>` prints the program's own **message** when it left
one (`set_abort_message()` before `abort()`: w16's 16-bit faults,
`assert()`, UBSAN), a summary, the **backtrace**, then the text header. The backtrace is recovered on the machine (`base/ucrash.h`, the
same reading the Crash Reports viewer shows): every word of the saved
stack from RSP up that points into code AND follows a call instruction,
named from that binary's own symbol table on disk --

    /bin/wm/apps/notepad (pid 18): Killed by SIGSEGV, sent by pid 20
    where: sys_futex_wait +0xa in notepad

    backtrace, found by scanning the stack:
      #0  0x80000081fa  sys_futex_wait +0xa  (notepad)
      #1  0x900005bce9  uapp_pump +0x249  (libui.so)
      #2  0x900005ca79  uapp_run +0x699  (libui.so)
      #3  0x8000007145  main +0x165  (notepad)

A scan can include a stale return address left in a local, and a binary
replaced since the crash names the wrong functions (it says so). Source
lines are the host's: `tools/lib/panic_resolve.py --crash <file>` resolves
RIP and the stack against the build's DWARF. Get the file off the
machine with `tools/dev/remote.py get`, or out of a QEMU disk image with
`tools/build/tfs3_writer.py read`.

## Options

- `--panics` -- KERNEL PANICS instead: the boots whose log in
  `/var/log/boot/` ends with a panic, one line each with its `PANIC:`
  line, newest first; `crashlog --panics BOOT` prints that boot's
  recovered lines (logd's `THIS BOOT PANICKED` marker to the end). BOOT
  is the number `log --list` shows.

      $ crashlog --panics
      boot 825   PANIC: General protection fault
      $ crashlog --panics 825
      [logd  ] logd: THIS BOOT PANICKED -- the rest was recovered from ...

A panic is a list of its own, never mixed into the reports:
`/var/crash` holds processes that faulted while the kernel carried on,
which is the system working, and a list that mixed the two would train
anyone to ignore it. A panic reaches a boot's log through the panic
store (`kernel/panic_store.h`) only when a warm restart kept RAM -- on a
machine where it does not, `panic_store=survey` finds a range that
survives (`docs/boot-flags.md`).

## Traps

A report is not written when a filesystem operation was in flight at
the fault -- a page fault inside a copy helper under a read, say -- and
the kernel log says so: `crash: no report -- a filesystem operation is
in flight`.
