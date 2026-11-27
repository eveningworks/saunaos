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
starts) is ASKED -- for its PASSWORD once it has one, unless
`login.admin_prompt` lets Allow alone answer; a `wheel` account that
sends its right password with the request (`sudo` on a terminal) is
granted, at the machine or not; a STANDARD account (not in `wheel`) at
the machine is asked too, and an ADMINISTRATOR SIGNS it in the prompt --
an Administrator drop-down listing every `wheel` account with a
password, and the chosen one's password (polkit's `auth_admin`; users
D3b2b), never remembered; when no administrator has a password the
prompt only says so, with Close, and the request is refused; anyone
else -- a Standard account remotely, or with no desktop -- is refused.

**`login.admin_prompt`** (in root's `/etc/login.conf`; Settings > Users'
"When an app needs administrator rights") is what the desktop's prompt
asks of that account: `password` (the default) every time;
`remember5` / `remember15`, the password, then Allow alone for 5 or 15
minutes after the last right one; `consent`, Allow alone, as Windows
asks an administrator. Only the desktop's own prompt is affected --
`sudo` and remote sessions always need the password. A remembered
password is kept in privd's memory, per account, and forgotten when the
screen locks or comes back from the saver, at a sign-out, and at boot.
The `adminprompt` action changes the setting: to a choice that asks
LESS it is asked, with the password even under `consent`; to one that
asks more it is the seat's without asking.

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
