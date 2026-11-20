# cpus

**a `/bin` program**, and a debug-console command of the same name.

**Category:** System information

## Synopsis

    cpus [-v]

## Options

- `-v`, `--verbose` -- also where each core's last tick landed and how it
  last took the kernel lock (the vector, the address, the pid).
- `-h`, `--help` -- print the usage and exit.

## Description

Each core's state, for a machine where one has stopped answering: what
runs there, its depth in the kernel lock and whether it holds it (a
depth without the lock is a core queued for it), how
long since its last timer interrupt and in which ring it landed, and its
cross-core calls and TLB shootdowns -- with one sent and not answered
said outright. A core that faulted says the vector; one stopped by
another core's panic says `stopped`. Every core the kernel knows is
listed, the ones that never started or failed included
(`QUERY_CPUSTATE`, `kernel/arch/x86_64/smp_query.c`).

    CPU 1 (APIC id 1): running, running pid 42
      kernel lock: depth 0
      last tick:   3 ms ago in ring 3
      calls:       2 run
      shootdowns:  117 answered

It is what had to be read through QEMU's gdbstub before it existed
(6282cb42). A snapshot: the other cores keep running while it reads,
so a busy one's fields may be a moment apart.

**When ring 3 cannot start a program at all**, the serial debug console
has its own `cpus` (`dbg> cpus`; `vm.py exec --raw cpus` -- plain `vm.py
exec cpus` runs this program through the kernel shell), which prints the
same record from the kernel, a line per core -- it needs only the boot
core's kernel context to be polling the console. The boot core's "last
tick" is its last clock event of any kind.

## See also

[`lscpu`](lscpu.md) for what the CPU is and the load on each core.
