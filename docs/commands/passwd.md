# passwd

**a `/bin` program.**

**Category:** System information

## Synopsis

    passwd [OPTION]... [ACCOUNT]

## Options

- `-d`, `--delete` -- remove the password: the account signs in without one.
- `-S`, `--status` -- say whether the account has a password (P) or not (NP).
- `-h`, `--help` -- the usage.

## Description

Sets or changes ACCOUNT's password -- your own when none is named. It
asks for the current password first (when the account has one), then
the new one twice, all with echo off. **It is not setuid**: `privd`
(`privd.md`) checks the current password and rewrites `/etc/shadow`,
which only root may read (`docs/users-design.md`, D2a).

Only root may name another account, and root is not asked for its
current password. An empty new password is refused: `passwd -d`
removes one. Setting a password turns automatic sign-in off for the
account; once it has one, `sudo` asks for it (`sudo.md`) and so does
the desktop's Authentication Required prompt.

Passwords are hashed with yescrypt (`$y$`), at the cost Linux
distributions use, so a line moves between `/etc/shadow` here and on a
Linux machine.

Refusals say why on stderr and exit 1: `the current password is wrong`
(after a two-second pause), `the passwords do not match`, `only root may
change another account's password`, `privd is not running`.

    $ passwd
    New password:
    Retype new password:
    passwd: user's password changed
    $ passwd -S
    user P
