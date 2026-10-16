# chrt

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    chrt [-f|-r] PRIO PROGRAM [ARG]...
    chrt -l PROGRAM [ARG]...
    chrt [-f|-r] -p PRIO PID
    chrt -l -p PID
    chrt -p PID

## Description

Realtime for a program started by hand. Only `init` may make a process
realtime (`SCHED_FIFO`/`SCHED_RR`, which run before every ordinary
process), and a service gets it from its descriptor's
`CPUSchedulingPolicy=`. `chrt` **asks** init for it, the way PipeWire
asks rtkit on Linux. Init grants it only to a program that something
lists, at no more than the listed priority:

- **a service's own program** -- a loaded descriptor whose `Exec=` is
  that program and whose `CPUSchedulingPolicy=` is `fifo` or `rr`. So
  `/bin/snddrv` may have `SCHED_FIFO 20` however it was started.
- **an `/etc/realtime.d` entry**, for a program that is not a service
  (`data/etc/realtime.d/README.md` has the format).

What init checks is the PROGRAM the process is running, from the
kernel's record of what it was spawned or exec'd from. Nothing the
process says about itself counts. **An `exec` ends a granted class**, so
a listed program cannot pass realtime on to one that is not listed.
There are no users, so this keeps a program from going realtime by
accident; it is not a security boundary.

    $ chrt -p 41
    pid 41's current scheduling policy: SCHED_FIFO
    pid 41's current scheduling priority: 20
    $ chrt 20 /bin/snddrv --pci
    snddrv: hda serving pci 0 as ... at 16-bit, SCHED_FIFO 20
    $ chrt 30 /bin/snddrv --pci
    chrt: priority 30 is above what /bin/snddrv is allowed
    $ chrt 5 /bin/ls
    chrt: /bin/ls may not run realtime -- no service or /etc/realtime.d entry lists it

**The launch form spawns `PROGRAM` stopped** (`SPAWN_STOPPED`), asks,
and continues it only once init has answered, so a refused program
never runs. Exactly as Linux's `chrt` fails before its `exec`. Each
grant and refusal is a line in `dmesg` (`init: realtime ...`).

A program can also ask for itself through `lib/uinitctl.h`'s
`uinitctl_realtime()`, which a hand-started `snddrv` does.

## Options

- `-f`, `--fifo` -- ask for `SCHED_FIFO`.
- `-r`, `--rr` -- ask for `SCHED_RR`. Without `-f` or `-r` the policy
  is the one the listing gives (Linux defaults to `SCHED_RR`).
- `-p`, `--pid` -- act on the running process `PID` instead of starting
  `PROGRAM`. With only a `PID`, print its class instead.
- `-l`, `--listed` -- ask for exactly what the listing gives, policy and
  priority, with no `PRIO`.

Options end at `PRIO`, or at `PROGRAM` with `-l`: `chrt 20 snddrv -v`
gives `-v` to `snddrv`.

## Exit status

The program's own in the launch form. Otherwise 0; 1 when init refused
or could not be asked; 2 a usage error.

## Limits

**Realtime only.** A process drops ITSELF to `SCHED_OTHER` with
`sched_setscheduler()`; dropping another one, and Linux's `-o`, `-b`,
`-i`, `-a` and `-m`, are not here. A granted process is still under the
kernel's two realtime guards: `kernel.sched_rt_watchdog_ms` moves one
that runs without blocking back to `SCHED_OTHER`, and
`kernel.sched_rt_runtime_ms` caps all realtime together.

Not reachable from the kernel shell with a bare name; at a `#` prompt
use `spawn /bin/chrt ...`, where its output lands in `dmesg`.
