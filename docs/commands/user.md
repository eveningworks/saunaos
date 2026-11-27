# user

**a `/bin` program.**

**Category:** System administration

## Synopsis

    user [OPTION]... [COMMAND [NAME]...]

## Options

- `list` (or nothing) -- every person's account: its name, full name,
  type (administrator or standard) and home.
- `add <name>` -- add an account. Its home, `/home/<name>`, is made from
  `/etc/skel`; it is a standard account unless `-a`.
- `remove <name>` -- remove an account. Its home is kept unless `-r`.
- `set <name> admin` / `set <name> standard` -- make the account an
  administrator (a member of `wheel`) or a standard one.
- `set <name> name <full-name>` -- change the full name the login screen
  and Settings show.
- `set <name> password` -- set the account's password, asked twice with
  echo off; an empty one removes it. Unlike `passwd`, the old one is not
  asked: this is an administrator resetting it.
- `-c`, `--full-name <name>` -- with `add`: the person's full name.
- `-a`, `--admin` -- with `add`: an administrator, not a standard account.
- `-p`, `--password` -- with `add`: ask for the new account's password
  (twice). Without it the account signs in without one.
- `-r`, `--delete-files` -- with `remove`: delete the account's home too.
- `-h`, `--help` -- every command and option in one page.

A usage error exits 2; a refused or failed change exits 1.

## Description

The command line of **Settings > System > Users**, over the same `privd`
actions (`useradd`, `userdel`, `usermod`; `base/uusers.h`). **Every
change is an administrator's**, asked as `sudo` asks (`sudo.md`): once
your account has a password it is asked for here, on the terminal;
without one, `privd` asks on the desktop's prompt, or `user` asks y/N
when no desktop runs. Root is not asked. A Standard account is refused
here (`this account may not do that as administrator`): its
administrator signs on the desktop's prompt, from Settings > Users.

One command with verbs, as `component` and `part` are, rather than
shadow-utils' `useradd`/`userdel`/`usermod`, whose flags differ
between the three (`docs/users-design.md`, D3a). The files are Linux's
own: a line in `/etc/passwd`, a group of the same name in `/etc/group`,
`wheel` for an administrator, a line in `/etc/shadow`.

    $ user add -c "Anna Virtanen" anna
    [user add] password for user:
    user: added anna (standard), home /home/anna
    $ user
    NAME             FULL NAME                TYPE           HOME
    user             User                     administrator  /home/user
    anna             Anna Virtanen            standard       /home/anna
    $ user set anna admin
    $ user remove -r anna

**A NAME** is 1-32 lower-case ASCII letters, digits, `_` or `-`,
starting with a letter or `_` -- shadow-utils' rule, which every tool
reading `/etc/passwd` assumes. A new account's uid is one above every
person's uid and gid in use, so a removed account's files never fall to
the next one; **an account added again over a home that was kept takes
that home's old owner back**, and its files with it, when nobody else
has that id.

Refusals say why on stderr and exit 1: `that name is taken`, `the
account is signed in, or has programs running` (sign it out first),
`it is the last administrator` (removing it, or making it standard,
would leave nobody to change the machine), `no such account`, `the
password was wrong`, `not allowed`.
