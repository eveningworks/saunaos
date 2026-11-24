# privd

**a `/bin` program.**

**Category:** System information

## Synopsis

    privd

## Description

`/bin/privd` is the root broker: the one way an ordinary account gets
administrator rights (`docs/users-design.md`, stage B -- polkit's and
UAC's shape, never a setuid program). init starts it as root from
`/etc/services.d/privd`; it takes no arguments, and serves the uchan
channel `privd` (`base/upriv.h`).

A request is ONE of:

- **an action** from a fixed table, which privd runs itself: set the
  clock (`settime`), load or unload a driver module (`modload`,
  `modunload`), signal a process of another account (`kill`);
- **an elevated spawn**: privd lets the asking process start exactly
  one path with exactly one argument vector as root, once, within 30
  seconds (`SYS_CRED_GRANT`, `SPAWN_ELEVATED`) -- what `sudo` and the
  desktop's "run as administrator" launches use.

Its policy, in order: a root caller is granted; an account in `wheel`
(read from `/etc/group`, as polkit reads `unix-group:`) whose process
holds the seat (`Seat=yes` in its service, inherited by everything it
starts) is ASKED; anyone else is refused.

The question is drawn by the desktop -- toywm registers as the agent,
and privd believes only `/bin/wm/system/toywm` holding the seat. With
no agent on a text boot, `sudo` asks on its own terminal; on a
graphical boot whose desktop has gone, requests are refused until it
returns.

Each decision is a line in its log (`/var/log/privd`).
