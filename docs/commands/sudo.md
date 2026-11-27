# sudo

**a `/bin` program.**

**Category:** System information

## Synopsis

    sudo COMMAND [ARG]...

## Options

- `-h`, `--help` -- the usage.

The first operand ends the options: `sudo ls -l` gives `-l` to `ls`.

## Description

Runs COMMAND as root, after the person at the machine allows it. **It
is not setuid sudo**: there is no sudoers file and no `-u`. It asks
`privd`, the root broker (`privd.md`), to let it start exactly this
command line elevated, and the command then runs as sudo's own child --
on its terminal, in its foreground group, with its exit status.

Who may ask: root (which just runs COMMAND), or an account in the
`wheel` group. A Standard account is refused before anyone is asked --
`sudo: aino is not an administrator` -- because its administrator signs
in the desktop's prompt, never on a terminal: a program it starts from
the desktop is asked there instead (`privd.md`).

**Once the account has a password** (`passwd.md`), sudo asks for it on
its own terminal -- `[sudo] password for user:`, with echo off -- and
privd checks it: the right password is the consent, from the console,
a VT, the desktop's Terminal or a telnet session alike. A wrong one
answers two seconds later with `Sorry, try again.`, three tries in all.

**Without a password**, the account must be AT the machine -- the
desktop's Terminal, the console shell, a VT shell; a telnet session is
refused -- and the person is asked:

- **with the desktop running**, its Authentication Required prompt
  opens over the desktop, whichever VT sudo was typed on;
- **on a text boot**, where no desktop ever ran, sudo asks on its own
  terminal -- `Run COMMAND as administrator? [y/N]`. That is a guard
  against a slip, not a lock: a program running as `user` there could
  answer it too, which is what a password ends.

COMMAND is looked up on `PATH` unless it names a directory. Refusals
say why on stderr and exit 1: `the password was wrong`, `this account
may not run commands as administrator`, `not allowed` (the person said Cancel), `no one to ask:
the desktop is not running` (a graphical boot whose desktop has gone),
`privd is not running`. A COMMAND that is not found exits 127.

    $ sudo mount 3 /mnt
    $ sudo service restart netd
