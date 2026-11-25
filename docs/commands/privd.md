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
  desktop's "run as administrator" launches use;
- **a password question** (users D2a): is this the account's password
  (`UPRIV_T_AUTH`) -- root may ask about any account, anyone else only
  about their own -- or set one (`passwd`, the action behind `passwd.md`).

Its policy, in order: a root caller is granted; an account in `wheel`
(read from `/etc/group`, as polkit reads `unix-group:`) whose process
holds the seat (`Seat=yes` in its service, inherited by everything it
starts) is ASKED -- for its PASSWORD once it has one; a `wheel` account
that sends its right password with the request (`sudo` on a terminal)
is granted, at the machine or not; anyone else is refused.

**privd is the only program that reads `/etc/shadow` or links a
password hash** (yescrypt; `docs/users-design.md`, D2a) -- LSASS's
shape, since there is no setuid `unix_chkpwd`. A wrong password is
answered two seconds later, without holding up anyone else's request,
and the desktop's prompt takes three tries before refusing.

The question is drawn by the desktop -- toywm registers as the agent,
and privd believes only `/bin/wm/system/toywm` holding the seat. With
no agent on a text boot, `sudo` asks on its own terminal; on a
graphical boot whose desktop has gone, requests are refused until it
returns.

Each decision is a line in its log (`/var/log/privd`).
