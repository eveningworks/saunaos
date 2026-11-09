# env

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    env [-i] [NAME=VALUE]... [PROGRAM [ARG]...]

## Options

- `-i`, `--ignore-environment` -- start from an empty environment.
- `-h`, `--help` -- print the usage and exit.

## Description

With no PROGRAM, prints the environment, one `NAME=VALUE` per line.
With one, runs it with each `NAME=VALUE` set first: `env PATH=/bin cc
hello.c` runs `cc` with only `/bin` searched. The first operand without
an `=` is the program, so its own options are its own (`env ls -l`).
PROGRAM is looked up on the changed PATH.

Every process starts with the environment init read at boot from
`/etc/environment` -- `NAME=value` lines, `#` comments, no `${}`
expansion (`docs/filesystem-layout.md`) -- and inherits it from there.
As shipped that is `PATH=/bin:/usr/bin` and `HOME=/`. Edit the file and
restart for a change to reach the desktop, the Terminal and what they
start.

The exit status is POSIX's: PROGRAM's own, 127 when it is not there,
126 when it is but could not be run, 125 when `env` itself failed.
