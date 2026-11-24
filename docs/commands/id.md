# id

**a `/bin` program.**

**Category:** System information

## Synopsis

    id [OPTION]... [USER]

## Options

- `-u`, `--user` -- only the effective user id.
- `-g`, `--group` -- only the effective group id.
- `-G`, `--groups` -- only the group ids, every one, space-separated.
- `-n`, `--name` -- with `-u`, `-g` or `-G`: the name instead of the number.
- `-r`, `--real` -- with `-u`, `-g` or `-G`: the real id instead of the
  effective one.
- `-h`, `--help` -- the options.

## Description

The user and group ids of this process, or of the account USER, GNU
coreutils' way: `uid=1000(user) gid=1000(user) groups=1000(user),10(wheel)`
-- `euid=`/`egid=` follow only when they differ from the real ones. The
numbers come from `SYS_GETCRED` (`abi/cred_abi.h`); the names from
`/etc/passwd` and `/etc/group`. With USER, that account's line and every
group naming it, as a fresh process started as it would get.

The desktop, its apps and the shells run as `user` (uid 1000, in
`wheel`); services run as `root` (`docs/users-design.md`). Nothing is
refused on the strength of either yet.

A number with no name prints bare, and with `-n` is said on stderr and
exits 1.
