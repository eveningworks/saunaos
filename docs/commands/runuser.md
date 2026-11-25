# runuser

**a `/bin` program.**

**Category:** System information

## Synopsis

    runuser -u USER COMMAND [ARG]...

## Options

- `-u USER`, `--user USER` -- the account to run COMMAND as.
- `-h`, `--help` -- the usage.

The first operand ends the options: `runuser -u user ls -l` gives `-l`
to `ls`.

## Description

Runs COMMAND as USER, as util-linux's `runuser` does: the account's
uid and primary group from `/etc/passwd`, every group `/etc/group` names
it in, and `HOME`, `USER`, `LOGNAME` and `SHELL` from its passwd line in
place of the caller's. The command keeps the caller's folder and
terminal, and runuser exits with its status.

**Root only, and it asks for nothing** -- the opposite direction from
`sudo` (`sudo.md`), which an account uses to run one command as root
after the person at the machine allows it. Another account gets
`runuser: may only be used by root`. The child has no seat mark
(`abi/cred_abi.h`'s `CRED_SEAT`): it owns files as USER, but the seat's
rights -- power, removable drives -- stay the desktop's.

An unknown USER is refused (`runuser: user X does not exist`), never run
as root. A COMMAND that is not found exits 127.

The GUI tests make their fixtures with it, through the debug console's
`as` (`docs/testing.md`), so a file an app changes belongs to the
account the app runs as.

    # runuser -u user touch /tmp/notes.txt
    # runuser -u user sh -c 'id; echo $HOME'
